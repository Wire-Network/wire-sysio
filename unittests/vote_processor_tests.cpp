#include <boost/test/unit_test.hpp>

#include <sysio/chain/controller.hpp>
#include <sysio/chain/vote_processor.hpp>
#include <sysio/testing/tester.hpp>
#include <fc/bitutil.hpp>
#include <boost/signals2/signal.hpp>

#include <magic_enum/magic_enum.hpp>

#include <future>
#include <span>

namespace std {
std::ostream& operator<<(std::ostream& os, const sysio::chain::vote_message& v) {
   os << "vote_message{" << v.block_id << std::endl;
   return os;
}
std::ostream& operator<<(std::ostream& os, const sysio::chain::vote_result_t& v) {
   os << fc::reflector<sysio::chain::vote_result_t>::to_string(v) << std::endl;
   return os;
}
std::ostream& operator<<(std::ostream& os, const sysio::chain::vote_status_t& v) {
   os << magic_enum::enum_name(v);
   return os;
}
}

namespace {

using namespace sysio;
using namespace sysio::chain;

block_id_type make_block_id(uint32_t block_num) {
   block_id_type block_id;
   block_id._hash[0] &= 0xffffffff00000000;
   block_id._hash[0] += fc::endian_reverse_u32(block_num);
   return block_id;
}

bls_private_key bls_priv_key_0 = bls_private_key::generate();
bls_private_key bls_priv_key_1 = bls_private_key::generate();
bls_private_key bls_priv_key_2 = bls_private_key::generate();
std::vector<bls_private_key> bls_priv_keys{bls_priv_key_0, bls_priv_key_1, bls_priv_key_2};

auto create_genesis_block_state() { // block 2
   auto block = signed_block::create_mutable_block({});

   block->producer = config::system_account_name;
   auto pub_key = sysio::testing::base_tester::get_public_key( block->producer, "active" );

   std::vector<finalizer_authority> finalizers;
   finalizers.push_back(finalizer_authority{.description = "first", .weight = 1, .public_key = bls_priv_keys.at(0).get_public_key()});
   finalizers.push_back(finalizer_authority{.description = "second", .weight = 1, .public_key = bls_priv_keys.at(1).get_public_key()});
   finalizers.push_back(finalizer_authority{.description = "third", .weight = 1, .public_key = bls_priv_keys.at(2).get_public_key()});
   finalizer_policy new_finalizer_policy{.finalizers = finalizers};
   finalizer_policy_diff new_finalizer_policy_diff = finalizer_policy{}.create_diff(new_finalizer_policy);
   qc_claim_t initial_if_claim { .block_num = 2,
                                 .is_strong_qc = false };
   block->qc_claim = initial_if_claim;
   block->new_finalizer_policy_diff = new_finalizer_policy_diff;

   producer_authority_schedule schedule = { 0, { producer_authority{block->producer, block_signing_authority_v0{ 1, {{pub_key, 1}} } } } };
   auto genesis = std::make_shared<block_state>();
   block->previous = make_block_id(1);
   genesis->block = signed_block::create_signed_block(std::move(block));
   genesis->activated_protocol_features = std::make_shared<protocol_feature_activation_set>();
   genesis->active_finalizer_policy = std::make_shared<finalizer_policy>(new_finalizer_policy);
   genesis->active_proposer_policy = std::make_shared<proposer_policy>(proposer_policy{.proposer_schedule = schedule});
   genesis->core = finality_core::create_core_for_genesis_block(genesis->block_id, genesis->header.timestamp);
   genesis->block_id = genesis->block->calculate_id();
   return genesis;
}

auto create_test_block_state(const block_state_ptr& prev) {
   static block_timestamp_type timestamp;
   timestamp = timestamp.next(); // each test block state will be unique
   auto block = prev->block->clone();
   block->producer = config::system_account_name;
   block->previous = prev->id();
   block->timestamp = timestamp;

   auto priv_key = sysio::testing::base_tester::get_private_key( block->producer, "active" );
   auto pub_key  = sysio::testing::base_tester::get_public_key( block->producer, "active" );

   auto sig_digest = digest_type::hash("something");
   block->producer_signatures = {priv_key.sign( sig_digest )};

   vector<private_key_type> signing_keys;
   signing_keys.emplace_back( std::move( priv_key ) );

   auto signer = [&]( digest_type d ) {
      std::vector<signature_type> result;
      result.reserve(signing_keys.size());
      for (const auto& k: signing_keys)
         result.emplace_back(k.sign(d));
      return result;
   };
   block_header_state bhs = *prev;
   bhs.header = *block;
   bhs.header.timestamp = timestamp;
   bhs.header.previous = prev->id();
   // schedule_version removed - always Savanna mode
   bhs.block_id = block->calculate_id();

   auto bsp = std::make_shared<block_state>(bhs,
      deque<transaction_metadata_ptr>{},
      deque<transaction_receipt>{},
      std::optional<valid_t>{},
      std::optional<qc_t>{},
      signer,
      block_signing_authority_v0{ 1, {{pub_key, 1}} },
      digest_type{});

   return bsp;
}

vote_message_ptr make_empty_message(const block_id_type& id) {
   vote_message_ptr vm = std::make_shared<vote_message>();
   vm->block_id = id;
   return vm;
}

vote_message_ptr make_vote_message(const block_state_ptr& bsp) {
   vote_message_ptr vm = std::make_shared<vote_message>();
   vm->block_id = bsp->id();
   vm->strong = true;
   size_t i = bsp->block_num() % bls_priv_keys.size();
   vm->finalizer_key = bls_priv_keys.at(i).get_public_key();
   vm->sig = bls_priv_keys.at(i).sign_sha256(bsp->strong_digest);
   return vm;
}

/// A strong vote on bsp by the finalizer at key_index.
vote_message_ptr make_vote(const block_state_ptr& bsp, size_t key_index) {
   vote_message_ptr vm = std::make_shared<vote_message>();
   vm->block_id = bsp->id();
   vm->strong = true;
   vm->finalizer_key = bls_priv_keys.at(key_index).get_public_key();
   vm->sig = bls_priv_keys.at(key_index).sign_sha256(bsp->strong_digest);
   return vm;
}

/// A strong vote on bsp by the finalizer at key_index whose signature does not verify.
vote_message_ptr make_invalid_vote(const block_state_ptr& bsp, size_t key_index) {
   vote_message_ptr vm = make_vote(bsp, key_index);
   vm->sig = bls_priv_keys.at(key_index).sign_sha256(digest_type::hash("not the block"));
   return vm;
}

/// A strong vote on bsp by a key that is not in the block's finalizer policy.
vote_message_ptr make_unknown_key_vote(const block_state_ptr& bsp) {
   bls_private_key key = bls_private_key::generate();
   vote_message_ptr vm = std::make_shared<vote_message>();
   vm->block_id = bsp->id();
   vm->strong = true;
   vm->finalizer_key = key.get_public_key();
   vm->sig = key.sign_sha256(bsp->strong_digest);
   return vm;
}

/// Polls pred until it holds or a generous deadline passes; returns the final value of pred.
template <typename Pred>
bool wait_until(Pred&& pred) {
   for (size_t i = 0; i < 5000 && !pred(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
   }
   return pred();
}

/// A finalizer policy of one finalizer per key, each of weight one.
finalizer_policy_ptr make_policy(std::span<const bls_public_key> keys) {
   finalizer_policy policy{.threshold = keys.size() * 2 / 3 + 1};
   for (const bls_public_key& key : keys)
      policy.finalizers.push_back(finalizer_authority{.description = "f", .weight = 1, .public_key = key});
   return std::make_shared<finalizer_policy>(std::move(policy));
}

/// Runs a vote processor against a map standing in for the fork database and records every vote it reports.
/// A gate vote can hold the processor's single thread inside its report so that later votes stay queued.
struct vote_processor_fixture {
   static constexpr uint32_t gate_connection_id = 1000;

   struct emitted_t {
      uint32_t         connection_id;
      vote_result_t    status;
      vote_message_ptr msg;
   };

   std::mutex                               mtx;
   std::vector<emitted_t>                   emitted;
   std::map<block_id_type, block_state_ptr> fork_db;

   vote_message_ptr          gate_msg;
   std::promise<void>        gate_release;
   std::shared_future<void>  gate_released = gate_release.get_future().share();
   std::atomic<bool>         gate_entered{false};
   bool                      gate_opened = false;

   vote_processor_t vp;

   explicit vote_processor_fixture(const vote_processor_t::limits_t& limits = {}, size_t num_threads = 1)
      : vp{[this](const vote_signal_params& p) { on_vote(p); },
           [this](const block_id_type& id) -> block_state_ptr {
              std::lock_guard g(mtx);
              auto it = fork_db.find(id);
              return it == fork_db.end() ? block_state_ptr{} : it->second;
           },
           limits} {
      vp.start(num_threads, [](const fc::exception& e) {
         elog("{}", e.to_detail_string());
         BOOST_FAIL("exception in vote thread pool");
      });
   }

   ~vote_processor_fixture() { open_gate(); }

   void on_vote(const vote_signal_params& p) {
      const vote_message_ptr& msg = std::get<2>(p);
      {
         std::lock_guard g(mtx);
         emitted.push_back(emitted_t{std::get<0>(p), std::get<1>(p), msg});
      }
      if (gate_msg && msg == gate_msg) {
         gate_entered = true;
         gate_released.wait();
      }
   }

   void add_block(const block_state_ptr& bsp) {
      std::lock_guard g(mtx);
      fork_db[bsp->id()] = bsp;
   }

   /// Occupies the processor's thread with a valid vote on bsp, which must be in the fork database, until open_gate().
   void close_gate(const block_state_ptr& bsp, size_t key_index) {
      gate_msg = make_vote(bsp, key_index);
      vp.process_vote_message(gate_connection_id, gate_msg, async_t::yes);
      BOOST_REQUIRE(wait_until([&] { return gate_entered.load(); }));
   }

   void open_gate() {
      if (!gate_opened) {
         gate_opened = true;
         gate_release.set_value();
      }
   }

   size_t count(uint32_t connection_id, vote_result_t status) {
      std::lock_guard g(mtx);
      return std::ranges::count_if(emitted, [&](const emitted_t& e) {
         return e.connection_id == connection_id && e.status == status;
      });
   }

   size_t count(uint32_t connection_id) {
      std::lock_guard g(mtx);
      return std::ranges::count_if(emitted, [&](const emitted_t& e) { return e.connection_id == connection_id; });
   }
};

BOOST_AUTO_TEST_SUITE(vote_processor_tests)

BOOST_AUTO_TEST_CASE( vote_processor_test ) {
   vote_signal_t voted_block;

   uint32_t received_connection_id = 0;
   vote_result_t received_vote_status = vote_result_t::duplicate; // never reported
   vote_message_ptr received_vote_message{};

   std::atomic<size_t> signaled = 0;
   std::mutex                               fork_db_mtx;
   std::map<block_id_type, block_state_ptr> fork_db;
   auto add_to_fork_db = [&](const block_state_ptr& bsp) {
      std::lock_guard g(fork_db_mtx);
      fork_db[bsp->id()] = bsp;
   };

   voted_block.connect( [&]( const vote_signal_params& vote_signal ) {
      received_connection_id = std::get<0>(vote_signal);
      received_vote_status = std::get<1>(vote_signal);
      received_vote_message = std::get<2>(vote_signal);
      ++signaled;
   } );

   vote_processor_t vp{[&](const vote_signal_params& p) { voted_block(p); },
                       [&](const block_id_type& id) -> block_state_ptr {
                          std::lock_guard g(fork_db_mtx);
                          return fork_db[id];
                       }};
   vp.start(2, [](const fc::exception& e) {
      elog("{}", e.to_detail_string());
      BOOST_REQUIRE(false);
   });

   { // empty fork db, block never found, never signaled
      vote_message_ptr vm1 = make_empty_message(make_block_id(1));
      signaled = 0;
      vp.process_vote_message(1, vm1, async_t::yes);
      for (size_t i = 0; i < 50 && vp.parked_size() < 1; ++i) {
         std::this_thread::sleep_for(std::chrono::milliseconds{5});
      }
      BOOST_TEST(vp.parked_size() == 1u);
      // move lib past block
      vp.notify_lib(2);
      vp.notify_new_block(create_test_block_state(create_genesis_block_state()), async_t::yes);
      for (size_t i = 0; i < 50 && vp.parked_size() > 0; ++i) {
         std::this_thread::sleep_for(std::chrono::milliseconds{5});
      }
      BOOST_TEST(vp.parked_size() == 0u);
      BOOST_TEST(signaled.load() == 0u);
   }
   { // process a valid vote
      signaled = 0;
      auto gensis = create_genesis_block_state();
      auto bsp = create_test_block_state(gensis);
      BOOST_CHECK_EQUAL(bsp->block_num(), 3u);
      vote_message_ptr m1 = make_vote_message(bsp);
      add_to_fork_db(bsp);
      vp.process_vote_message(1, m1, async_t::yes);
      // duplicate ignored
      vp.process_vote_message(1, m1, async_t::yes);
      for (size_t i = 0; i < 50 && signaled.load() < 1; ++i) {
         std::this_thread::sleep_for(std::chrono::milliseconds{5});
      }
      BOOST_TEST(signaled.load() == 1u);
      BOOST_TEST(1u == received_connection_id);
      BOOST_TEST(vote_result_t::success == received_vote_status);
      BOOST_TEST(m1 == received_vote_message);
   }
   { // process an invalid signature vote
      signaled = 0;
      auto gensis = create_genesis_block_state();
      auto bsp = create_test_block_state(gensis);
      BOOST_CHECK_EQUAL(bsp->block_num(), 3u);
      vote_message_ptr m1 = make_vote_message(bsp);
      m1->strong = false; // signed with strong_digest
      add_to_fork_db(bsp);
      vp.process_vote_message(1, m1, async_t::yes);
      for (size_t i = 0; i < 50 && signaled.load() < 1; ++i) {
         std::this_thread::sleep_for(std::chrono::milliseconds{5});
      }
      BOOST_TEST(signaled.load() == 1u);
      BOOST_TEST(1u == received_connection_id);
      BOOST_TEST(vote_result_t::invalid_signature == received_vote_status);
      BOOST_TEST(m1 == received_vote_message);
   }
   { // process two diff block votes
      signaled = 0;
      auto gensis = create_genesis_block_state();
      auto bsp = create_test_block_state(gensis);
      auto bsp2 = create_test_block_state(bsp);
      vote_message_ptr m1 = make_vote_message(bsp);
      vote_message_ptr m2 = make_vote_message(bsp2);
      vp.process_vote_message(2, m1, async_t::yes);
      vp.process_vote_message(3, m2, async_t::yes);
      for (size_t i = 0; i < 5; ++i) {
         if (vp.parked_size() == 2) break;
         std::this_thread::sleep_for(std::chrono::milliseconds{5});
      }
      BOOST_TEST(vp.parked_size() == 2u);
      std::this_thread::sleep_for(std::chrono::milliseconds{5}); // no votes for awhile
      BOOST_TEST(signaled.load() == 0u);
      add_to_fork_db(bsp);
      vp.notify_new_block(bsp, async_t::yes);
      for (size_t i = 0; i < 50 && signaled.load() < 2; ++i) {
         std::this_thread::sleep_for(std::chrono::milliseconds{5});
      }
      BOOST_TEST(signaled.load() == 1u);
      BOOST_TEST(2u == received_connection_id);
      BOOST_TEST(vote_result_t::success == received_vote_status);
      BOOST_CHECK(m1 == received_vote_message);

      add_to_fork_db(bsp2);
      vp.notify_new_block(bsp2, async_t::yes);
      for (size_t i = 0; i < 50 && signaled.load() < 2; ++i) {
         std::this_thread::sleep_for(std::chrono::milliseconds{5});
      }
      BOOST_TEST(signaled.load() == 2u);
      BOOST_TEST(3u == received_connection_id);
      BOOST_TEST(vote_result_t::success == received_vote_status);
      BOOST_CHECK(m2 == received_vote_message);
   }
}

// A connection replaying an invalid vote is held to its queue limit, its signature is verified once, and votes from
// other connections are still aggregated.
BOOST_AUTO_TEST_CASE( invalid_vote_stream_is_bounded ) {
   constexpr uint32_t attacker = 1;
   constexpr uint32_t honest   = 2;
   constexpr uint32_t sentinel = 3;
   constexpr uint32_t per_connection = 8;
   constexpr uint32_t sent = 3 * per_connection;
   vote_processor_fixture f{vote_processor_t::limits_t{.max_votes_per_connection = per_connection}};

   block_state_ptr bsp = create_test_block_state(create_genesis_block_state());
   f.add_block(bsp);
   f.close_gate(bsp, 0);

   vote_message_ptr invalid = make_invalid_vote(bsp, 1);
   for (uint32_t i = 0; i < sent; ++i)
      f.vp.process_vote_message(attacker, invalid, async_t::yes);
   BOOST_TEST(f.vp.queued_size() == per_connection);
   BOOST_TEST(f.count(attacker, vote_result_t::max_exceeded) == sent - per_connection);

   f.open_gate();
   BOOST_TEST(wait_until([&] { return f.vp.queued_size() == 0; }));

   f.vp.process_vote_message(honest, make_vote(bsp, 1), async_t::yes);
   BOOST_TEST(wait_until([&] { return f.count(honest, vote_result_t::success) == 1; }));
   BOOST_TEST(f.count(attacker, vote_result_t::invalid_signature) == 1u);

   // until it closes, the connection's later votes are dropped without verification
   f.vp.process_vote_message(attacker, make_vote(bsp, 2), async_t::yes);
   f.vp.process_vote_message(sentinel, make_unknown_key_vote(bsp), async_t::yes); // reported after the attacker's vote
   BOOST_TEST(wait_until([&] { return f.count(sentinel) == 1; }));
   BOOST_TEST(f.count(attacker, vote_result_t::success) == 0u);
   BOOST_TEST(bsp->has_voted(bls_priv_keys.at(2).get_public_key()) == vote_status_t::not_voted);

   // a new session on the same connection id is processed
   f.vp.notify_connection_closed(attacker);
   f.vp.process_vote_message(attacker, make_vote(bsp, 2), async_t::yes);
   BOOST_TEST(wait_until([&] { return f.count(attacker, vote_result_t::success) == 1; }));
}

// Votes already queued by a connection whose vote failed verification are dropped without verification.
BOOST_AUTO_TEST_CASE( rejected_connection_skips_queued_votes ) {
   constexpr uint32_t bad_signature = 1;
   constexpr uint32_t unknown_key   = 2;
   constexpr uint32_t honest        = 3;
   vote_processor_fixture f;

   block_state_ptr bsp = create_test_block_state(create_genesis_block_state());
   f.add_block(bsp);
   f.close_gate(bsp, 0);

   f.vp.process_vote_message(bad_signature, make_invalid_vote(bsp, 1), async_t::yes);
   f.vp.process_vote_message(bad_signature, make_vote(bsp, 1), async_t::yes);
   f.vp.process_vote_message(unknown_key, make_unknown_key_vote(bsp), async_t::yes);
   f.vp.process_vote_message(unknown_key, make_vote(bsp, 2), async_t::yes);
   f.vp.process_vote_message(honest, make_vote(bsp, 1), async_t::yes);
   BOOST_TEST(f.vp.queued_size() == 5u);

   f.open_gate();
   BOOST_TEST(wait_until([&] { return f.count(honest, vote_result_t::success) == 1; }));
   BOOST_TEST(f.vp.queued_size() == 0u);
   BOOST_TEST(f.count(bad_signature, vote_result_t::invalid_signature) == 1u);
   BOOST_TEST(f.count(bad_signature) == 1u);
   BOOST_TEST(f.count(unknown_key, vote_result_t::unknown_public_key) == 1u);
   BOOST_TEST(f.count(unknown_key) == 1u);
   BOOST_TEST(bsp->has_voted(bls_priv_keys.at(2).get_public_key()) == vote_status_t::not_voted);
}

// Votes held across connections are capped; this node's own votes are exempt.
BOOST_AUTO_TEST_CASE( queued_votes_are_capped_globally ) {
   constexpr uint32_t per_connection = 4;
   constexpr uint32_t global         = 10;
   constexpr uint32_t local          = 0;
   constexpr uint32_t local_votes    = 2;
   vote_processor_fixture f{vote_processor_t::limits_t{.max_votes_per_connection = per_connection,
                                                       .max_queued_votes         = global}};

   block_state_ptr bsp = create_test_block_state(create_genesis_block_state());
   f.add_block(bsp);
   f.close_gate(bsp, 0);

   vote_message_ptr vote = make_vote(bsp, 1);
   for (uint32_t connection_id = 1; connection_id <= 3; ++connection_id) {
      for (uint32_t i = 0; i < per_connection; ++i)
         f.vp.process_vote_message(connection_id, vote, async_t::yes);
   }
   BOOST_TEST(f.vp.queued_size() == global);
   BOOST_TEST(f.count(3, vote_result_t::max_exceeded) == 0u);

   for (uint32_t i = 0; i < local_votes; ++i)
      f.vp.process_vote_message(local, vote, async_t::yes);
   BOOST_TEST(f.vp.queued_size() == global + local_votes);

   f.open_gate();
   BOOST_TEST(wait_until([&] { return f.vp.queued_size() == 0; }));
   BOOST_TEST(f.count(1, vote_result_t::success) == 1u);
}

// A parked vote is aggregated only when its own block is reported, not when votes arrive for other blocks.
BOOST_AUTO_TEST_CASE( parked_votes_drain_only_for_their_block ) {
   vote_processor_fixture f;

   block_state_ptr genesis = create_genesis_block_state();
   block_state_ptr bsp_a = create_test_block_state(genesis);
   block_state_ptr bsp_b = create_test_block_state(genesis);
   block_state_ptr bsp_c = create_test_block_state(genesis);

   f.vp.process_vote_message(1, make_vote(bsp_a, 0), async_t::yes);
   f.vp.process_vote_message(2, make_vote(bsp_b, 0), async_t::yes);
   BOOST_TEST(wait_until([&] { return f.vp.parked_size() == 2; }));

   // bsp_b is in the fork database but not reported; votes for another known block leave it parked
   f.add_block(bsp_b);
   f.add_block(bsp_c);
   f.vp.process_vote_message(3, make_vote(bsp_c, 0), async_t::yes);
   f.vp.process_vote_message(4, make_vote(bsp_c, 1), async_t::yes);
   BOOST_TEST(wait_until([&] { return f.count(4, vote_result_t::success) == 1; }));
   BOOST_TEST(f.vp.parked_size() == 2u);
   BOOST_TEST(f.count(2) == 0u);

   f.add_block(bsp_a);
   f.vp.notify_new_block(bsp_a, async_t::yes);
   BOOST_TEST(wait_until([&] { return f.count(1, vote_result_t::success) == 1; }));
   BOOST_TEST(f.vp.parked_size() == 1u);
   BOOST_TEST(f.count(2) == 0u);

   f.vp.notify_new_block(bsp_b, async_t::yes);
   BOOST_TEST(wait_until([&] { return f.count(2, vote_result_t::success) == 1; }));
   BOOST_TEST(f.vp.parked_size() == 0u);
}

// Parked votes whose block does not arrive before they expire, or before lib passes them, are discarded without a
// report and stop counting against their connection.
BOOST_AUTO_TEST_CASE( unmatched_parked_votes_are_discarded ) {
   constexpr uint32_t         parking  = 1;
   constexpr uint32_t         sentinel = 2;
   constexpr fc::microseconds expiry   = fc::milliseconds(50);
   vote_processor_fixture f{vote_processor_t::limits_t{.max_votes_per_connection = 1, .parked_vote_expiry = expiry}};

   block_state_ptr genesis       = create_genesis_block_state();
   block_state_ptr never_arrives = create_test_block_state(genesis);
   block_state_ptr passed_by_lib = create_test_block_state(genesis);
   block_state_ptr other         = create_test_block_state(genesis);
   block_state_ptr above_lib     = create_test_block_state(other);
   f.add_block(other);
   f.add_block(above_lib);

   f.vp.process_vote_message(parking, make_vote(never_arrives, 0), async_t::yes);
   BOOST_TEST(wait_until([&] { return f.vp.parked_size() == 1; }));
   std::this_thread::sleep_for(std::chrono::microseconds{2 * expiry.count()});
   f.vp.notify_new_block(other, async_t::yes);
   BOOST_TEST(wait_until([&] { return f.vp.parked_size() == 0; }));

   // the expired vote no longer counts against the connection's limit of one vote
   f.vp.process_vote_message(parking, make_vote(passed_by_lib, 0), async_t::yes);
   BOOST_TEST(wait_until([&] { return f.vp.parked_size() == 1; }));
   f.vp.notify_lib(passed_by_lib->block_num());
   f.vp.notify_new_block(other, async_t::yes);
   BOOST_TEST(wait_until([&] { return f.vp.parked_size() == 0; }));

   // reported after any report of the discarded votes
   f.vp.process_vote_message(sentinel, make_vote(above_lib, 1), async_t::yes);
   BOOST_TEST(wait_until([&] { return f.count(sentinel, vote_result_t::success) == 1; }));
   BOOST_TEST(f.count(parking) == 0u);
}

// Votes from peers for blocks beyond the parking lead are dropped without a report and without counting against the
// connection; this node's own votes are exempt.
BOOST_AUTO_TEST_CASE( votes_beyond_park_lead_are_dropped ) {
   constexpr block_num_type lead           = 10;
   constexpr uint32_t       per_connection = 2;
   constexpr uint32_t       peer           = 1;
   constexpr uint32_t       local          = 0;
   vote_processor_fixture f{vote_processor_t::limits_t{.max_votes_per_connection = per_connection,
                                                       .park_lead_blocks         = lead}};

   block_state_ptr bsp = create_test_block_state(create_genesis_block_state());
   f.add_block(bsp);
   f.vp.notify_new_block(bsp, async_t::yes);

   const block_id_type beyond_lead = make_block_id(bsp->block_num() + lead + 1);
   for (uint32_t i = 0; i <= per_connection; ++i)
      f.vp.process_vote_message(peer, make_empty_message(beyond_lead), async_t::yes);
   BOOST_TEST(f.vp.queued_size() == 0u);

   f.vp.process_vote_message(peer, make_vote(bsp, 1), async_t::yes);
   BOOST_TEST(wait_until([&] { return f.count(peer, vote_result_t::success) == 1; }));
   BOOST_TEST(f.count(peer) == 1u);
   BOOST_TEST(f.vp.parked_size() == 0u);

   f.vp.process_vote_message(local, make_empty_message(beyond_lead), async_t::yes);
   BOOST_TEST(wait_until([&] { return f.vp.parked_size() == 1; }));
}

// The parked set is capped; a vote beyond the cap is dropped without a report.
BOOST_AUTO_TEST_CASE( parked_votes_are_capped ) {
   constexpr block_num_type lead = 10;
   constexpr uint32_t max_parked = 3;
   vote_processor_fixture f{vote_processor_t::limits_t{.max_parked_votes = max_parked, .park_lead_blocks = lead}};

   block_state_ptr bsp = create_test_block_state(create_genesis_block_state());
   f.add_block(bsp);
   f.vp.notify_new_block(bsp, async_t::yes);

   // votes at the parking lead are parked
   const block_id_type within_lead = make_block_id(bsp->block_num() + lead);
   for (uint32_t connection_id = 1; connection_id <= max_parked; ++connection_id)
      f.vp.process_vote_message(connection_id, make_empty_message(within_lead), async_t::yes);
   BOOST_TEST(wait_until([&] { return f.vp.parked_size() == max_parked; }));

   // the parked set is full: dropped without penalizing the connection
   constexpr uint32_t over_cap = max_parked + 1;
   f.vp.process_vote_message(over_cap, make_empty_message(make_block_id(bsp->block_num() + 1)), async_t::yes);
   BOOST_TEST(wait_until([&] { return f.vp.queued_size() == 0; }));
   BOOST_TEST(f.vp.parked_size() == max_parked);
   BOOST_TEST(f.count(over_cap) == 0u);
}

// Closing a connection discards its parked votes; votes it already queued are aggregated but never parked.
BOOST_AUTO_TEST_CASE( closing_connection_purges_parked_votes ) {
   constexpr uint32_t closing = 1;
   constexpr uint32_t staying = 2;
   vote_processor_fixture f;

   block_state_ptr genesis = create_genesis_block_state();
   block_state_ptr known = create_test_block_state(genesis);
   block_state_ptr unknown = create_test_block_state(genesis);
   f.add_block(known);

   f.vp.process_vote_message(closing, make_vote(unknown, 0), async_t::yes);
   f.vp.process_vote_message(closing, make_vote(unknown, 1), async_t::yes);
   f.vp.process_vote_message(staying, make_vote(unknown, 2), async_t::yes);
   BOOST_TEST(wait_until([&] { return f.vp.parked_size() == 3; }));
   BOOST_TEST(f.vp.connection_count() == 2u);

   f.vp.notify_connection_closed(closing);
   BOOST_TEST(f.vp.parked_size() == 1u);
   BOOST_TEST(f.vp.connection_count() == 1u);

   f.add_block(unknown);
   f.vp.notify_new_block(unknown, async_t::yes);
   BOOST_TEST(wait_until([&] { return f.count(staying, vote_result_t::success) == 1; }));
   BOOST_TEST(f.vp.parked_size() == 0u);
   BOOST_TEST(f.count(closing) == 0u);
   BOOST_TEST(unknown->has_voted(bls_priv_keys.at(0).get_public_key()) == vote_status_t::not_voted);

   // close while votes are queued
   block_state_ptr later = create_test_block_state(genesis);
   f.close_gate(known, 0);
   f.vp.process_vote_message(closing, make_vote(known, 1), async_t::yes);
   f.vp.process_vote_message(closing, make_vote(later, 0), async_t::yes);
   f.vp.notify_connection_closed(closing);
   f.open_gate();
   BOOST_TEST(wait_until([&] { return f.count(closing, vote_result_t::success) == 1; }));
   BOOST_TEST(wait_until([&] { return f.vp.queued_size() == 0; }));
   BOOST_TEST(f.vp.parked_size() == 0u);
   BOOST_TEST(f.count(closing) == 1u);
   BOOST_TEST(f.vp.connection_count() == 2u); // the staying and gate connections remain open
}

// Unless fixed, the limits are sized for the vote threads and the distinct finalizers of the reported block's active
// and pending policies, never below vote_limits::min_finalizers.
BOOST_AUTO_TEST_CASE( limits_are_sized_from_policies ) {
   static_assert(vote_limits::park_lead_blocks == 12);
   static_assert(vote_limits::max_votes_per_connection(21) == 1008);
   static_assert(vote_limits::max_votes_per_connection(64) == 3072);
   static_assert(vote_limits::max_parked_votes(21) == 2016);
   static_assert(vote_limits::max_parked_votes(64) == 6144);
   static_assert(vote_limits::max_queued_votes(4) == 6400);

   constexpr size_t vote_threads = 2;
   vote_processor_t vp{[](const vote_signal_params&) {}, [](const block_id_type&) { return block_state_ptr{}; }};
   vp.start(vote_threads, [](const fc::exception& e) {
      elog("{}", e.to_detail_string());
      BOOST_FAIL("exception in vote thread pool");
   });

   vote_processor_t::limits_t limits = vp.current_limits();
   BOOST_TEST(limits.max_votes_per_connection == vote_limits::max_votes_per_connection(vote_limits::min_finalizers));
   BOOST_TEST(limits.max_parked_votes == vote_limits::max_parked_votes(vote_limits::min_finalizers));
   BOOST_TEST(limits.max_queued_votes == vote_limits::max_queued_votes(vote_threads));
   BOOST_TEST(limits.park_lead_blocks == vote_limits::park_lead_blocks);
   BOOST_CHECK(limits.parked_vote_expiry == vote_limits::parked_vote_expiry);

   constexpr size_t active_finalizers = 64;
   constexpr size_t pending_only      = 8;
   std::vector<bls_public_key> keys;
   for (size_t i = 0; i < active_finalizers + pending_only; ++i)
      keys.push_back(bls_private_key::generate().get_public_key());
   const std::span<const bls_public_key> all_keys{keys};

   block_state_ptr genesis = create_genesis_block_state();
   block_state_ptr bsp = create_test_block_state(genesis);
   bsp->active_finalizer_policy = make_policy(all_keys.first(active_finalizers));
   vp.notify_new_block(bsp, async_t::yes);
   limits = vp.current_limits();
   BOOST_TEST(limits.max_votes_per_connection == vote_limits::max_votes_per_connection(active_finalizers));
   BOOST_TEST(limits.max_parked_votes == vote_limits::max_parked_votes(active_finalizers));
   BOOST_TEST(limits.max_queued_votes == vote_limits::max_queued_votes(vote_threads));

   // a pending policy that adds pending_only keys to those of the active policy
   block_state_ptr next = create_test_block_state(bsp);
   next->pending_finalizer_policy = std::pair{next->block_num(), make_policy(all_keys.last(active_finalizers))};
   vp.notify_new_block(next, async_t::yes);
   limits = vp.current_limits();
   constexpr size_t distinct_finalizers = active_finalizers + pending_only;
   BOOST_TEST(limits.max_votes_per_connection == vote_limits::max_votes_per_connection(distinct_finalizers));
   BOOST_TEST(limits.max_parked_votes == vote_limits::max_parked_votes(distinct_finalizers));

   // fewer finalizers than the floor
   vp.notify_new_block(create_test_block_state(genesis), async_t::yes);
   limits = vp.current_limits();
   BOOST_TEST(limits.max_votes_per_connection == vote_limits::max_votes_per_connection(vote_limits::min_finalizers));
   BOOST_TEST(limits.max_parked_votes == vote_limits::max_parked_votes(vote_limits::min_finalizers));
}

BOOST_AUTO_TEST_SUITE_END()

}

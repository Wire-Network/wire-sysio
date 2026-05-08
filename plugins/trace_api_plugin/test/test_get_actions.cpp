#include <boost/test/unit_test.hpp>

#include <fc/filesystem.hpp>

#include <sysio/trace_api/abi_data_handler.hpp>
#include <sysio/trace_api/bloom_sidecar.hpp>
#include <sysio/trace_api/request_handler.hpp>
#include <sysio/trace_api/test_common.hpp>

using namespace sysio;
using namespace sysio::trace_api;
using namespace sysio::trace_api::test_common;

namespace {

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

struct get_actions_fixture {
   struct mock_logfile_provider {
      mock_logfile_provider(get_actions_fixture& f) : fixture(f) {}

      get_block_t get_block(uint32_t height) {
         auto it = fixture.blocks.find(height);
         if (it == fixture.blocks.end()) return {};
         return std::make_tuple(data_log_entry{it->second}, true /*irreversible*/);
      }

      // Stride/slice mapping is a fixture knob so tests can exercise the per-slice bloom skip path with a small
      // stride rather than the production default of 10,000 blocks.
      uint32_t slice_stride() const noexcept { return fixture.mock_slice_stride; }
      uint32_t slice_number(uint32_t block_num) const noexcept { return block_num / fixture.mock_slice_stride; }

      // Default: no sidecar -> invalid bloom_reader -> may_contain_* returns true -> caller scans as before.  Tests
      // that want to exercise skipping install a function that returns a valid reader for specific slices.
      bloom_reader get_bloom(uint32_t slice_number) const {
         return fixture.mock_get_bloom(slice_number);
      }

      get_actions_fixture& fixture;
   };

   struct mock_data_handler_provider {
      mock_data_handler_provider(get_actions_fixture& f) : fixture(f) {}

      std::tuple<fc::variant, std::optional<fc::variant>> serialize_to_variant(const action_trace_v0& a) {
         return fixture.mock_data_handler(a);
      }

      // Production shared_provider exposes decode(); the request_handler's
      // get_actions path calls decode() directly so a decode_error field can
      // be surfaced on failure.  The mock builds a decode_result from the
      // tuple returned by mock_data_handler -- decode never fails in tests.
      abi_data_handler::decode_result decode(const action_trace_v0& a) {
         auto [params, return_data] = fixture.mock_data_handler(a);
         abi_data_handler::decode_result r;
         r.params      = std::move(params);
         r.return_data = std::move(return_data);
         r.status      = r.params.is_null()
                           ? abi_data_handler::decode_status::not_attempted
                           : abi_data_handler::decode_status::ok;
         return r;
      }

      get_actions_fixture& fixture;
   };

   using impl_type = request_handler<mock_logfile_provider, mock_data_handler_provider>;

   get_actions_fixture()
   : impl(mock_logfile_provider(*this), mock_data_handler_provider(*this),
          [](const std::string& msg){ fc_dlog(fc::logger::default_logger(), "{}", msg); })
   {}

   actions_result get_actions(const action_query& query) {
      return impl.get_actions(query);
   }

   actions_result get_token_transfer_actions(const action_query& query) {
      return impl.get_token_transfer_actions(query);
   }

   // Default: no ABI decoding — params/return_data absent from result
   std::function<std::tuple<fc::variant, std::optional<fc::variant>>(const action_trace_v0&)>
   mock_data_handler = [](const action_trace_v0&) -> std::tuple<fc::variant, std::optional<fc::variant>> {
      return {};
   };

   std::map<uint32_t, block_trace_v0> blocks;
   uint32_t mock_slice_stride = 10;
   std::function<bloom_reader(uint32_t)> mock_get_bloom = [](uint32_t) { return bloom_reader{}; };
   impl_type impl;
};

// ---------------------------------------------------------------------------
// Builders
// ---------------------------------------------------------------------------

action_trace_v0 make_action(uint64_t seq, chain::name receiver, chain::name account,
                             chain::name act, chain::bytes data = {}) {
   action_trace_v0 a{};
   a.global_sequence = seq;
   a.receiver        = receiver;
   a.account         = account;
   a.action          = act;
   a.data            = std::move(data);
   return a;
}

transaction_trace_v0 make_trx(chain::transaction_id_type id, uint32_t block_num,
                               std::vector<action_trace_v0> actions) {
   transaction_trace_v0 trx;
   trx.id       = id;
   trx.actions  = std::move(actions);
   trx.block_num  = block_num;
   trx.block_time = chain::block_timestamp_type(0);
   return trx;
}

block_trace_v0 make_block(uint32_t num, std::vector<transaction_trace_v0> trxs) {
   block_trace_v0 blk;
   blk.number       = num;
   blk.producer     = "bp.one"_n;
   blk.transactions = std::move(trxs);
   return blk;
}

// Convenience: one action per block, sequential global_sequences
static const chain::transaction_id_type TRX1 =
   "0000000000000000000000000000000000000000000000000000000000000001"_h;
static const chain::transaction_id_type TRX2 =
   "0000000000000000000000000000000000000000000000000000000000000002"_h;
static const chain::transaction_id_type TRX3 =
   "0000000000000000000000000000000000000000000000000000000000000003"_h;

} // anonymous namespace

// ---------------------------------------------------------------------------
// Test suite
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_SUITE(get_actions_tests)

// No blocks in the queried range -> empty result
BOOST_FIXTURE_TEST_CASE(empty_range, get_actions_fixture)
{
   action_query q;
   q.block_num_start = 1;
   q.block_num_end   = 10;

   auto r = get_actions(q);

   BOOST_TEST(r.actions.empty());
}

// Filter that matches nothing returns empty result
BOOST_FIXTURE_TEST_CASE(no_matching_filter, get_actions_fixture)
{
   blocks[1] = make_block(1, {
      make_trx(TRX1, 1, { make_action(1, "sysio.token"_n, "sysio.token"_n, "transfer"_n) })
   });

   action_query q;
   q.block_num_start = 1;
   q.block_num_end   = 1;
   q.receiver        = "alice"_n;  // alice is not a receiver in this block

   auto r = get_actions(q);

   BOOST_TEST(r.actions.empty());
}

// receiver filter: keeps only actions where receiver matches
BOOST_FIXTURE_TEST_CASE(filter_by_receiver, get_actions_fixture)
{
   // Two actions: original transfer (receiver=sysio.token) + inline notification (receiver=bob)
   blocks[1] = make_block(1, {
      make_trx(TRX1, 1, {
         make_action(1, "sysio.token"_n, "sysio.token"_n, "transfer"_n),
         make_action(2, "bob"_n,         "sysio.token"_n, "transfer"_n)
      })
   });

   action_query q;
   q.block_num_start = 1;
   q.block_num_end   = 1;
   q.receiver        = "sysio.token"_n;

   auto r = get_actions(q);

   BOOST_REQUIRE_EQUAL(r.actions.size(), 1u);
   BOOST_TEST(r.actions[0].get_object()["receiver"].as_string() == "sysio.token");
   BOOST_TEST(r.actions[0].get_object()["global_sequence"].as_uint64() == 1u);
}

// account filter: keeps only actions where account (code) matches
BOOST_FIXTURE_TEST_CASE(filter_by_account, get_actions_fixture)
{
   blocks[1] = make_block(1, {
      make_trx(TRX1, 1, {
         make_action(1, "alice"_n, "sysio.token"_n, "transfer"_n),
         make_action(2, "alice"_n, "mycontract"_n,  "foo"_n)
      })
   });

   action_query q;
   q.block_num_start = 1;
   q.block_num_end   = 1;
   q.account         = "sysio.token"_n;

   auto r = get_actions(q);

   BOOST_REQUIRE_EQUAL(r.actions.size(), 1u);
   BOOST_TEST(r.actions[0].get_object()["account"].as_string() == "sysio.token");
   BOOST_TEST(r.actions[0].get_object()["global_sequence"].as_uint64() == 1u);
}

// action name filter: keeps only the named action
BOOST_FIXTURE_TEST_CASE(filter_by_action_name, get_actions_fixture)
{
   blocks[1] = make_block(1, {
      make_trx(TRX1, 1, {
         make_action(1, "sysio.token"_n, "sysio.token"_n, "transfer"_n),
         make_action(2, "sysio"_n,       "sysio"_n,       "newaccount"_n)
      })
   });

   action_query q;
   q.block_num_start = 1;
   q.block_num_end   = 1;
   q.action          = "transfer"_n;

   auto r = get_actions(q);

   BOOST_REQUIRE_EQUAL(r.actions.size(), 1u);
   BOOST_TEST(r.actions[0].get_object()["name"].as_string() == "transfer");
}

// Actions are returned across multiple blocks; missing blocks in the log are skipped
BOOST_FIXTURE_TEST_CASE(multi_block_scan, get_actions_fixture)
{
   blocks[1] = make_block(1, { make_trx(TRX1, 1, { make_action(1, "a"_n, "tok"_n, "transfer"_n) }) });
   blocks[2] = make_block(2, { make_trx(TRX2, 2, { make_action(2, "a"_n, "tok"_n, "transfer"_n) }) });
   // block 3 is missing (gap in trace log)
   blocks[4] = make_block(4, { make_trx(TRX3, 4, { make_action(3, "a"_n, "tok"_n, "transfer"_n) }) });

   action_query q;
   q.block_num_start = 1;
   q.block_num_end   = 5;

   auto r = get_actions(q);

   BOOST_REQUIRE_EQUAL(r.actions.size(), 3u);
   BOOST_TEST(r.actions[0].get_object()["block_num"].as<uint32_t>() == 1u);
   BOOST_TEST(r.actions[1].get_object()["block_num"].as<uint32_t>() == 2u);
   BOOST_TEST(r.actions[2].get_object()["block_num"].as<uint32_t>() == 4u);
}

// Per-trx cpu / net totals are emitted on every action variant in the full
// (get_actions) shape, alongside trx_id / block_num / block_time /
// producer_block_id, so callers can attribute resource usage to the parent
// transaction without a separate lookup.  Deliberately distinct from
// action-level cpu_usage_us / net_usage which are per-action and in different
// units (action net_usage is bytes; trx net_usage_words is ceil/8).
BOOST_FIXTURE_TEST_CASE(emits_trx_resource_totals_in_full_shape, get_actions_fixture)
{
   transaction_trace_v0 trx = make_trx(TRX1, 1, {
      make_action(1, "sysio.token"_n, "sysio.token"_n, "transfer"_n),
      make_action(2, "bob"_n,         "sysio.token"_n, "transfer"_n)
   });
   trx.cpu_usage_us    = 1234;
   trx.net_usage_words = fc::unsigned_int{56};
   blocks[1] = make_block(1, { std::move(trx) });

   action_query q;
   q.block_num_start = 1;
   q.block_num_end   = 1;

   auto r = get_actions(q);

   BOOST_REQUIRE_EQUAL(r.actions.size(), 2u);
   for (const auto& a : r.actions) {
      const auto& obj = a.get_object();
      BOOST_TEST(obj["trx_cpu_usage_us"].as<uint32_t>()    == 1234u);
      BOOST_TEST(obj["trx_net_usage_words"].as<uint32_t>() == 56u);
   }
}

// Slim (get_token_transfers) omits all resource fields - both action-level
// cpu_usage_us / net_usage and the trx-level totals.  Per-trx context
// (trx_id, block_num, etc.) still appears so transfers can be located in the
// chain.
BOOST_FIXTURE_TEST_CASE(slim_shape_omits_trx_resource_totals, get_actions_fixture)
{
   transaction_trace_v0 trx = make_trx(TRX1, 1, {
      make_action(1, "sysio.token"_n, "sysio.token"_n, "transfer"_n)
   });
   trx.cpu_usage_us    = 1234;
   trx.net_usage_words = fc::unsigned_int{56};
   blocks[1] = make_block(1, { std::move(trx) });

   action_query q;
   q.block_num_start = 1;
   q.block_num_end   = 1;
   q.receiver        = "sysio.token"_n;
   q.account         = "sysio.token"_n;
   q.action          = "transfer"_n;

   auto r = get_token_transfer_actions(q);

   BOOST_REQUIRE_EQUAL(r.actions.size(), 1u);
   const auto& obj = r.actions[0].get_object();
   BOOST_TEST(obj.contains("trx_id"));
   BOOST_TEST(!obj.contains("trx_cpu_usage_us"));
   BOOST_TEST(!obj.contains("trx_net_usage_words"));
   BOOST_TEST(!obj.contains("cpu_usage_us"));
   BOOST_TEST(!obj.contains("net_usage"));
}

// ABI-decoded params are included in the result when the data handler returns them
BOOST_FIXTURE_TEST_CASE(abi_decoded_params_included, get_actions_fixture)
{
   blocks[1] = make_block(1, {
      make_trx(TRX1, 1, {
         make_action(1, "sysio.token"_n, "sysio.token"_n, "transfer"_n, {0x01, 0x02})
      })
   });

   mock_data_handler = [](const action_trace_v0&) -> std::tuple<fc::variant, std::optional<fc::variant>> {
      return { fc::mutable_variant_object()("amount", 100), std::nullopt };
   };

   action_query q;
   q.block_num_start = 1;
   q.block_num_end   = 1;

   auto r = get_actions(q);

   BOOST_REQUIRE_EQUAL(r.actions.size(), 1u);
   const auto& obj = r.actions[0].get_object();
   BOOST_REQUIRE(obj.contains("params"));
   BOOST_TEST(obj["params"].get_object()["amount"].as<int>() == 100);
   BOOST_TEST(!obj.contains("return_data"));
}

// When the data handler returns null, no params field is emitted
BOOST_FIXTURE_TEST_CASE(no_params_when_handler_returns_null, get_actions_fixture)
{
   blocks[1] = make_block(1, {
      make_trx(TRX1, 1, { make_action(1, "alice"_n, "contract"_n, "foo"_n, {static_cast<char>(0xAB)}) })
   });

   action_query q;
   q.block_num_start = 1;
   q.block_num_end   = 1;

   auto r = get_actions(q);

   BOOST_REQUIRE_EQUAL(r.actions.size(), 1u);
   const auto& obj = r.actions[0].get_object();
   BOOST_TEST(!obj.contains("params"));
   BOOST_TEST(obj["data"].as_string() == "ab");  // raw hex always present
}

// Verifies the receiver+account+action filter used by get_token_transfers:
// exactly one result per transfer (the original; notification copy is excluded)
BOOST_FIXTURE_TEST_CASE(token_transfer_filter_excludes_notifications, get_actions_fixture)
{
   blocks[1] = make_block(1, {
      make_trx(TRX1, 1, {
         make_action(1, "sysio.token"_n, "sysio.token"_n, "transfer"_n),  // original
         make_action(2, "bob"_n,         "sysio.token"_n, "transfer"_n)   // inline notification
      })
   });

   // This is the filter preset used by POST /v1/trace_api/get_token_transfers
   action_query q;
   q.block_num_start = 1;
   q.block_num_end   = 1;
   q.receiver        = "sysio.token"_n;
   q.account         = "sysio.token"_n;
   q.action          = "transfer"_n;

   auto r = get_actions(q);

   BOOST_REQUIRE_EQUAL(r.actions.size(), 1u);
   BOOST_TEST(r.actions[0].get_object()["global_sequence"].as_uint64() == 1u);
   BOOST_TEST(r.actions[0].get_object()["receiver"].as_string() == "sysio.token");
}

// Actions within a transaction are returned sorted by global_sequence (execution order),
// not the schedule order in which the chain stored them.  The divergence matters when
// an action queues both an inline AND a require_recipient notification — notifications
// execute before inlines, so the inline's global_sequence is higher than later-scheduled
// notifications'.  Sorting gives clients a consistent execution view matching what
// chain_plugin's push_transaction does and what the legacy get_block response returned.
BOOST_FIXTURE_TEST_CASE(actions_sorted_by_global_sequence, get_actions_fixture)
{
   blocks[1] = make_block(1, {
      make_trx(TRX1, 1, {
         make_action(5, "a"_n, "tok"_n, "transfer"_n),  // schedule order: 5
         make_action(1, "a"_n, "tok"_n, "transfer"_n),  // schedule order: 1
         make_action(3, "a"_n, "tok"_n, "transfer"_n)   // schedule order: 3
      })
   });

   action_query q;
   q.block_num_start = 1;
   q.block_num_end   = 1;

   auto r = get_actions(q);

   BOOST_REQUIRE_EQUAL(r.actions.size(), 3u);
   BOOST_TEST(r.actions[0].get_object()["global_sequence"].as_uint64() == 1u);
   BOOST_TEST(r.actions[1].get_object()["global_sequence"].as_uint64() == 3u);
   BOOST_TEST(r.actions[2].get_object()["global_sequence"].as_uint64() == 5u);
}

// Realistic scenario exercising top-level actions, notifications (receiver != account),
// inline actions triggered by a notification handler, and the inline's own notifications.
//
// Scenario: alice transfers 1 SYS to bob.contract via sysio.token::transfer.
//   bob.contract has a notif handler that fires an inline logger::log action.
//   logger::log has an audit account notification handler.
//
// Expected global_sequence order (chain-assigned, monotonic):
//   100: sysio.token::transfer     (receiver=sysio.token, account=sysio.token)  <- original
//   101: sysio.token::transfer     (receiver=alice,       account=sysio.token)  <- notif to sender
//   102: sysio.token::transfer     (receiver=bob.contract,account=sysio.token)  <- notif to recipient
//   103: logger::log               (receiver=logger,      account=logger)       <- inline from 102
//   104: logger::log               (receiver=audit,       account=logger)       <- notif from 103
BOOST_FIXTURE_TEST_CASE(complex_inline_and_notification_ordering, get_actions_fixture)
{
   blocks[1] = make_block(1, {
      make_trx(TRX1, 1, {
         make_action(100, "sysio.token"_n,  "sysio.token"_n, "transfer"_n),
         make_action(101, "alice"_n,        "sysio.token"_n, "transfer"_n),
         make_action(102, "bob.contract"_n, "sysio.token"_n, "transfer"_n),
         make_action(103, "logger"_n,       "logger"_n,      "log"_n),
         make_action(104, "audit"_n,        "logger"_n,      "log"_n)
      })
   });

   // No filter: all 5 actions in global_sequence order.
   {
      action_query q;
      q.block_num_start = 1;
      q.block_num_end   = 1;

      auto r = get_actions(q);

      BOOST_REQUIRE_EQUAL(r.actions.size(), 5u);
      BOOST_TEST(r.actions[0].get_object()["global_sequence"].as_uint64() == 100u);
      BOOST_TEST(r.actions[1].get_object()["global_sequence"].as_uint64() == 101u);
      BOOST_TEST(r.actions[2].get_object()["global_sequence"].as_uint64() == 102u);
      BOOST_TEST(r.actions[3].get_object()["global_sequence"].as_uint64() == 103u);
      BOOST_TEST(r.actions[4].get_object()["global_sequence"].as_uint64() == 104u);
   }

   // receiver=sysio.token: only the original execution; notifications to alice/bob excluded.
   {
      action_query q;
      q.block_num_start = 1;
      q.block_num_end   = 1;
      q.receiver        = "sysio.token"_n;

      auto r = get_actions(q);

      BOOST_REQUIRE_EQUAL(r.actions.size(), 1u);
      BOOST_TEST(r.actions[0].get_object()["global_sequence"].as_uint64() == 100u);
      BOOST_TEST(r.actions[0].get_object()["receiver"].as_string() == "sysio.token");
   }

   // account=sysio.token: original + both notifications (3 rows), ordered by global_seq.
   {
      action_query q;
      q.block_num_start = 1;
      q.block_num_end   = 1;
      q.account         = "sysio.token"_n;

      auto r = get_actions(q);

      BOOST_REQUIRE_EQUAL(r.actions.size(), 3u);
      BOOST_TEST(r.actions[0].get_object()["global_sequence"].as_uint64() == 100u);
      BOOST_TEST(r.actions[1].get_object()["global_sequence"].as_uint64() == 101u);
      BOOST_TEST(r.actions[2].get_object()["global_sequence"].as_uint64() == 102u);
      BOOST_TEST(r.actions[0].get_object()["receiver"].as_string() == "sysio.token");
      BOOST_TEST(r.actions[1].get_object()["receiver"].as_string() == "alice");
      BOOST_TEST(r.actions[2].get_object()["receiver"].as_string() == "bob.contract");
   }

   // account=logger: the inline from bob.contract's notif handler + its notification to audit.
   {
      action_query q;
      q.block_num_start = 1;
      q.block_num_end   = 1;
      q.account         = "logger"_n;

      auto r = get_actions(q);

      BOOST_REQUIRE_EQUAL(r.actions.size(), 2u);
      BOOST_TEST(r.actions[0].get_object()["global_sequence"].as_uint64() == 103u);
      BOOST_TEST(r.actions[1].get_object()["global_sequence"].as_uint64() == 104u);
      BOOST_TEST(r.actions[0].get_object()["receiver"].as_string() == "logger");
      BOOST_TEST(r.actions[1].get_object()["receiver"].as_string() == "audit");
   }

   // receiver=bob.contract + action=transfer: exactly the one notification to the recipient.
   {
      action_query q;
      q.block_num_start = 1;
      q.block_num_end   = 1;
      q.receiver        = "bob.contract"_n;
      q.action          = "transfer"_n;

      auto r = get_actions(q);

      BOOST_REQUIRE_EQUAL(r.actions.size(), 1u);
      BOOST_TEST(r.actions[0].get_object()["global_sequence"].as_uint64() == 102u);
   }
}

// Per-slice bloom skip: a valid bloom that does not contain the queried receiver causes get_actions_impl to advance
// past the entire slice without scanning any of its blocks.  The fixture observes "no scan" by having get_block
// return a single well-known action in every block; if the scan ran, the result would include that action.
BOOST_FIXTURE_TEST_CASE(bloom_skips_entire_slice_when_receiver_absent, get_actions_fixture) {
   fc::temp_directory tempdir;

   // Three slices of 10 blocks each; populate every block so a non-skipped scan would always find the single action.
   mock_slice_stride = 10;
   for (uint32_t n = 1; n < 30; ++n) {
      blocks[n] = make_block(n, { make_trx(TRX1, n, { make_action(n, "alice"_n, "alice"_n, "transfer"_n) }) });
   }

   // Build bloom sidecars for slices 0, 1, 2.  Slice 1 is the only one that contains alice; slices 0 and 2 have no
   // receivers at all (empty blooms -> every probe misses).
   auto bloom_for = [&tempdir](std::size_t idx, bool with_alice) {
      bloom_builder b;
      if (with_alice) {
         action_trace_v0 a{};
         a.receiver = "alice"_n;
         a.account  = "alice"_n;
         a.action   = "transfer"_n;
         b.add_action(a);
      }
      const auto path = tempdir.path() / ("bloom_slice_" + std::to_string(idx) + ".log");
      b.finalize_and_write(path);
      return path;
   };
   const auto slice0_path = bloom_for(0, /*with_alice=*/false);
   const auto slice1_path = bloom_for(1, /*with_alice=*/true);
   const auto slice2_path = bloom_for(2, /*with_alice=*/false);

   mock_get_bloom = [slice0_path, slice1_path, slice2_path](uint32_t slice) -> bloom_reader {
      switch (slice) {
         case 0: return bloom_reader{slice0_path};
         case 1: return bloom_reader{slice1_path};
         case 2: return bloom_reader{slice2_path};
         default: return bloom_reader{};
      }
   };

   action_query q;
   q.block_num_start = 1;
   q.block_num_end   = 29;
   q.receiver        = "alice"_n;

   auto r = get_actions(q);

   // All hits come from slice 1 (blocks 10..19).  Slices 0 and 2 were bloom-skipped without any get_block call.
   BOOST_REQUIRE_EQUAL(r.actions.size(), 10u);
   for (const auto& a : r.actions) {
      const auto block_num = a.get_object()["block_num"].as_uint64();
      BOOST_TEST(block_num >= 10u);
      BOOST_TEST(block_num <= 19u);
   }
}

// Sanity check that a query with no filter cannot bloom-skip: even if the mock would return an empty bloom for
// every slice, we still scan because there's nothing to probe against.  Without this behaviour callers would see
// empty results on unfiltered queries once sidecars exist.
BOOST_FIXTURE_TEST_CASE(bloom_not_consulted_when_no_filter, get_actions_fixture) {
   mock_slice_stride = 10;
   for (uint32_t n = 1; n < 15; ++n) {
      blocks[n] = make_block(n, { make_trx(TRX1, n, { make_action(n, "alice"_n, "alice"_n, "transfer"_n) }) });
   }
   // If the handler ever calls get_bloom under this configuration, fail loudly.
   mock_get_bloom = [](uint32_t) -> bloom_reader {
      BOOST_FAIL("get_bloom should not be called when no filter is set");
      return bloom_reader{};
   };

   action_query q;
   q.block_num_start = 1;
   q.block_num_end   = 14;
   auto r = get_actions(q);
   BOOST_CHECK_EQUAL(r.actions.size(), 14u);
}

BOOST_AUTO_TEST_SUITE_END()
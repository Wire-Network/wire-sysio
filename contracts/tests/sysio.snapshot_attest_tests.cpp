#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#include <boost/test/unit_test.hpp>
#pragma GCC diagnostic pop

#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/resource_limits.hpp>
#include <sysio/protocol/snapshot_attestation.hpp>
#include <sysio/testing/tester.hpp>

#include <fc/exception/exception.hpp>
#include <fc/variant_object.hpp>

#include "sysio.system_tester.hpp"
#include "finalizer_test_keys.hpp"

using namespace sysio_system;

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------
class snapshot_attest_tester : public sysio_system_tester {
public:
   /// The five producers every test in this fixture delegates snapshot providers from.
   static std::vector<account_name> fixture_producers() {
      return {"producer1"_n, "producer2"_n, "producer3"_n, "producer4"_n, "producer5"_n};
   }

   snapshot_attest_tester() : snapshot_attest_tester(std::vector<account_name>{}, 0) {}

   /**
    * @param extra_producers producers a single test needs beyond the fixture's five. They are
    *   created HERE rather than in the test because every registered finalizer key has to be in
    *   the node's voting set from the start -- see `register_schedulable_finalizer_keys`.
    * @param cadence_periods how many `block_spacing` periods of history to build before anything
    *   else. 0 -- the default -- costs nothing, and is what every registration test uses: it
    *   deliberately does NOT advance, because the advance costs 25 000 blocks and permanently
    *   forfeits the validating controller. 1 reaches the first attestable height
    *   (`snapshot_voting_tester`); 2 reaches the second, for the purging tests that need two
    *   scheduled heights live at once (`snapshot_multi_height_tester`). A test pays for the
    *   periods its assertions actually require and no more.
    */
   explicit snapshot_attest_tester(const std::vector<account_name>& extra_producers,
                                   uint32_t cadence_periods = 0)
      // A cadence-advancing fixture stops ONE level short of `full`, so the chain reaches the
      // attestable height with only `sysio.bios` on the system account. bios declares no
      // `onblock`, so those blocks execute no contract code at all — where under `sysio.system`
      // every one of them runs the interpreted `onblock` (blockinfo write, round attribution,
      // and a schedule rebuild every 120 slots). That difference, times `block_spacing`
      // (25 000) blocks times two dozen tests, is the whole cost of this suite.
      : sysio_system_tester(cadence_periods > 0 ? setup_level::core_token : setup_level::full) {
      if (cadence_periods > 0) {
         advance_to_attestation_cadence(cadence_periods);
         // The rest of what `setup_level::full` would have done, now that the expensive part of
         // the chain's history is behind us.
         initialize_multisig();
         deploy_contract();
         remaining_setup();
      }
      produce_blocks();

      std::vector<account_name> producers = fixture_producers();
      producers.insert(producers.end(), extra_producers.begin(), extra_producers.end());
      setup_producer_accounts(producers);

      // Create snap provider accounts with resources
      const std::vector<account_name> snap_accounts = {
         "snapprov1"_n, "snapprov2"_n, "snapprov3"_n,
         "snapprov4"_n, "snapprov5"_n
      };
      setup_producer_accounts(snap_accounts);

      produce_blocks();

      // Register producers
      for (const auto& p : producers) {
         regproducer(p);
      }
      produce_blocks();

      // Snapshot-provider eligibility is POSITION among schedulable producers, not a stored rank
      // governance hands out. A producer is schedulable only as an ACTIVE PRODUCER operator in
      // sysio.opreg carrying an active finalizer key, so the fixture must supply both. With equal
      // scores the index orders by account name, so producer1..producer5 take positions 1..5 --
      // all inside max_snap_provider_rank.
      deploy_opreg_once();
      register_producer_operators(std::vector<name>(producers.begin(), producers.end()));
      produce_blocks();
      // Reach the attestation cadence BEFORE registering finalizer keys, i.e. while no producer
      // is schedulable yet. Ordering is the whole point: once they are, `update_ranked_producers`
      // publishes a policy of ALL of them, and the node then signs and verifies one vote per
      // finalizer on EVERY block. Paying that across a `block_spacing` (25 000) block advance is
      // what took this suite from minutes to over an hour and blew CI's 1000 s ctest timeout.
      // Advancing first leaves the cheap single-finalizer genesis policy in force for those
      // 25 000 blocks, and the larger policy applies only to the handful of blocks a test
      // produces afterwards.
      register_schedulable_finalizer_keys(std::vector<name>(producers.begin(), producers.end()));
      produce_blocks();
   }

   /**
    * Produce blocks until the head reaches the given number of `block_spacing` periods.
    *
    * `votesnaphash` rejects a height above the head, and only multiples of `block_spacing` are
    * scheduled, so a test that votes needs the chain at least one period along -- and a test that
    * needs two scheduled heights live at once needs two.
    *
    * Called from the constructor BEFORE `sysio.system` is deployed and BEFORE any finalizer key is
    * registered, which is what makes it affordable: those blocks then run no contract code and
    * carry the single-finalizer genesis policy. The same advance performed later -- mid-test, with
    * the system contract live and five finalizers voting -- costs several times as much per block,
    * which is why the period count is a constructor decision and not something a test can reach
    * for on its own.
    *
    * It skips duplicate validation too, and those flags STAY set: the validating controller never
    * received these blocks, so re-enabling it afterwards makes the very next block unlinkable
    * against a node tens of thousands of blocks behind.
    *
    * @param periods how many `block_spacing` periods of history to build.
    */
   void advance_to_attestation_cadence(uint32_t periods) {
      const uint32_t target = scheduled_height(periods);
      if (control->head().block_num() >= target) return;

      // Flush anything the setup above left pending FIRST: the empty-block advance deliberately
      // skips pending transactions, so a transaction queued before it would instead be applied
      // after -- by which point the chain has jumped hours of block time and it has expired.
      produce_block();
      skip_validate           = true;
      primary_only_production = true;
      produce_blocks(target - control->head().block_num(), true);
   }

   /// Give each name an active finalizer key -- required for a rank position -- and configure the
   /// node to vote with every one of them. `get_bls_key` derives a distinct key per account name,
   /// so there is no fixed key table to run out of and regfinkey's global uniqueness check is
   /// satisfied by construction.
   ///
   /// `set_node_finalizers` is not optional here, and the reason is easy to miss: once these
   /// producers are schedulable, `onblock`'s throttled rebuild proposes a finalizer policy built
   /// from exactly these keys. A policy this node cannot vote for freezes LIB -- and these tests
   /// then advance `block_spacing` (25 000) blocks to reach an attestable height, so an unpruned
   /// fork database grows the whole way and the chainbase segment is exhausted long before the
   /// test finishes. It is called ONCE, over every key any test in this fixture will register,
   /// which is why `extra_producers` is a constructor parameter rather than test-local setup.
   void register_schedulable_finalizer_keys(const std::vector<name>& names) {
      for (const auto& p : names) {
         push_action(config::system_account_name, "setacctram"_n,
                     mvo()("account", p)("ram_bytes", int64_t(1'000'000)));
      }
      produce_blocks();
      for (const auto& p : names) {
         auto [privkey, pubkey, pop, sig_provider] = sysio::testing::get_bls_key(p);
         BOOST_REQUIRE_EQUAL(success(),
            push_action(p, "regfinkey"_n, mvo()
               ("finalizer_name", p)
               ("finalizer_key", pubkey.to_string())
               ("proof_of_possession", pop.to_string())));
      }
      produce_blocks();
      set_node_finalizers(names);
   }

   /** Produce a block with traces, skipping duplicate validation only after cadence mode begins. */
   produce_block_result_t produce_block_ex(fc::microseconds skip_time = default_skip_time,
                                           bool no_throw = false) override {
      if (primary_only_production) {
         return _produce_block(skip_time, false, no_throw);
      }
      return sysio_system_tester::produce_block_ex(skip_time, no_throw);
   }

   /** Produce a block, skipping duplicate validation only after cadence mode begins. */
   signed_block_ptr produce_block(fc::microseconds skip_time = default_skip_time,
                                  bool no_throw = false) override {
      return produce_block_ex(skip_time, no_throw).block;
   }

   /** Produce an empty block and preserve aborted transactions in either validation mode. */
   signed_block_ptr produce_empty_block(fc::microseconds skip_time = default_skip_time) override {
      if (!primary_only_production) {
         return sysio_system_tester::produce_empty_block(skip_time);
      }
      unapplied_transactions.add_aborted(control->abort_block());
      return _produce_block(skip_time, true);
   }

   /// Register a snapshot provider.
   action_result regsnapprov(name producer, name snap_account) {
      return push_action(producer, "regsnapprov"_n, mvo()
         ("producer", producer)
         ("snap_account", snap_account));
   }

   /// Deactivate a producer while retaining its producer-table row.
   action_result unregproducer(name producer) {
      return push_action(producer, "unregprod"_n, mvo()
         ("producer", producer));
   }


   /// Vote on a snapshot hash.
   action_result votesnaphash(name snap_account, const fc::sha256& block_id, const fc::sha256& snapshot_hash) {
      return push_action(snap_account, "votesnaphash"_n, mvo()
         ("snap_account", snap_account)
         ("block_id", block_id)
         ("snapshot_hash", snapshot_hash));
   }

   /// Set snapshot attestation configuration.
   action_result setsnpcfg(uint32_t min_providers) {
      return push_action(config::system_account_name, "setsnpcfg"_n, mvo()
         ("min_providers", min_providers));
   }

   /// Return an attested snapshot record from the contract table.
   fc::variant getsnaphash(uint32_t block_num) {
      vector<char> data = get_row_by_account(
         config::system_account_name, config::system_account_name,
         "snaprecords"_n, name(block_num));
      return data.empty() ? fc::variant() : abi_ser.binary_to_variant(
         "snap_record", data, abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   /// Return a registered snapshot provider from the contract table.
   fc::variant get_snap_provider(name snap_account) {
      vector<char> data = get_row_by_account(
         config::system_account_name, config::system_account_name,
         "snapprovs"_n, snap_account);
      return data.empty() ? fc::variant() : abi_ser.binary_to_variant(
         "snap_provider", data, abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   /// Count pending snapshot tuple rows across the scheduled tally space.
   uint32_t snapshot_vote_count() {
      constexpr uint64_t maximum_vote_rows = 64;
      uint32_t           count             = 0;
      for (uint64_t id = 0; id < maximum_vote_rows; ++id) {
         const auto data = get_row_by_account(
            config::system_account_name, config::system_account_name, "snapvotes"_n, name{id});
         if (!data.empty()) {
            ++count;
         }
      }
      return count;
   }

   /// The height a given `block_spacing` period falls on.
   ///
   /// Usable WITHOUT advancing: `votesnaphash` runs its two height checks -- is this a scheduled
   /// multiple, and is it at or below the head -- before it reads any table, so a test asserting on
   /// either of those needs no chain history at all and belongs on the base fixture.
   static constexpr uint32_t scheduled_height(uint32_t period) {
      return period * sysio::protocol::snapshot_attestation::block_spacing;
   }

   /// The latest attestable height at or below the head.
   ///
   /// This does NOT advance. How far the chain runs is a constructor decision (`cadence_periods`)
   /// because that is the only point at which the advance is affordable; a fixture that did not ask
   /// for one has no attestable height and says so, rather than silently buying one mid-test at
   /// several times the price.
   uint32_t vote_block_num() {
      const uint32_t spacing = sysio::protocol::snapshot_attestation::block_spacing;
      // Commit registrations and configuration the test queued before reading the height.
      produce_block();
      const uint32_t head_block_num = control->head().block_num();
      BOOST_REQUIRE_MESSAGE(head_block_num >= spacing,
                            "fixture never advanced to a cadence boundary -- construct it with "
                            "cadence_periods >= 1 (see snapshot_voting_tester) to vote");
      return head_block_num / spacing * spacing;
   }

   /**
    * Make a synthetic block id with a specific block number embedded in big-endian form.
    *
    * `fork` differentiates ids at the same height to emulate competing forks.
    */
   static fc::sha256 make_block_id(uint32_t block_num, uint8_t fork = 0) {
      fc::sha256 id;
      memset(id.data(), 0, id.data_size());
      auto* data = id.data();
      // block_num in big-endian in first 4 bytes
      data[0] = static_cast<char>((block_num >> 24) & 0xFF);
      data[1] = static_cast<char>((block_num >> 16) & 0xFF);
      data[2] = static_cast<char>((block_num >> 8) & 0xFF);
      data[3] = static_cast<char>(block_num & 0xFF);
      data[8] = static_cast<char>(fork);
      return id;
   }

   /// Make a synthetic snapshot hash.
   static fc::sha256 make_snap_hash(uint32_t seed) {
      fc::sha256 hash;
      memset(hash.data(), 0, hash.data_size());
      auto* data = hash.data();
      // Put seed at end to differentiate from block_id
      data[28] = static_cast<char>((seed >> 24) & 0xFF);
      data[29] = static_cast<char>((seed >> 16) & 0xFF);
      data[30] = static_cast<char>((seed >> 8) & 0xFF);
      data[31] = static_cast<char>(seed & 0xFF);
      return hash;
   }

private:
   /// True after a cadence-bound vote case begins its expensive empty-block advance.
   bool primary_only_production = false;
};

// ===========================================================================
/**
 * Fixture for the VOTING tests -- the ones whose assertions need a real attestable height.
 *
 * It builds one `block_spacing` (25 000) block period BEFORE the system contract is deployed and
 * BEFORE any finalizer key is registered, which is the only cheap order: once the producers are
 * schedulable, `update_ranked_producers` publishes a policy of all of them and the node signs plus
 * verifies one vote per finalizer on EVERY block. Paying that across the advance took this suite
 * from minutes to over an hour.
 *
 * The registration tests, and the two height-precondition negatives whose checks fire before the
 * contract reads any table, use the base fixture and never advance at all.
 */
struct snapshot_voting_tester : public snapshot_attest_tester {
   snapshot_voting_tester()
      : snapshot_attest_tester(std::vector<account_name>{}, /*cadence_periods*/ 1) {}
};

/**
 * Fixture for the purging tests, which need TWO scheduled heights live at once.
 *
 * Both periods are built in the constructor, on the cheap side of the system-contract deploy. The
 * alternative -- advance one period, then produce the second from inside the test -- is what this
 * replaces: those blocks ran the interpreted `onblock` and carried a five-finalizer policy, making
 * that second period several times more expensive than the first.
 */
struct snapshot_multi_height_tester : public snapshot_attest_tester {
   snapshot_multi_height_tester()
      : snapshot_attest_tester(std::vector<account_name>{}, /*cadence_periods*/ 2) {}
};

BOOST_AUTO_TEST_SUITE(sysio_snapshot_attest_tests)

// ---------------------------------------------------------------------------
// regsnapprov tests
// ---------------------------------------------------------------------------
BOOST_FIXTURE_TEST_CASE(regsnapprov_basic, snapshot_attest_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));

   auto prov = get_snap_provider("snapprov1"_n);
   BOOST_REQUIRE_EQUAL(false, prov.is_null());
   BOOST_REQUIRE_EQUAL("snapprov1", prov["snap_account"].as_string());
   BOOST_REQUIRE_EQUAL("producer1", prov["producer"].as_string());
} FC_LOG_AND_RETHROW() }

// The provider table is bounded by the RANK BAND, not by its own capacity check.
//
// `max_snap_providers` is defined as `max_snap_provider_rank`, and a producer holds at most one
// mapping at a time, so at most `max_snap_provider_rank` producers can ever hold one. Once rank
// became POSITION among schedulable producers -- necessarily distinct, where the stored ordinal it
// replaced could repeat -- a 31st rank-eligible producer stopped existing, and with it the only way
// to reach `maximum registered snapshot providers reached`. The `check` stays as the guard that
// keeps the table bounded if the two constants ever diverge; the reachable rejection at a full
// table is now the rank gate, and the reachable RECOVERY is the stale-mapping prune.
/// Fixture for the capacity case: twenty-six producers beyond the fixture's five.
///
/// They are constructor-supplied rather than created inside the test because every finalizer key
/// the chain knows about has to be in the node's voting set BEFORE `onblock` proposes a policy
/// from them -- see `snapshot_attest_tester::register_schedulable_finalizer_keys`.
///
/// The `z` prefix is load-bearing: rank is POSITION in the score-ordered index, and equal scores
/// order by account name. A `capprov*` name sorts BEFORE `producer*`, which would push the
/// fixture's own five past max_snap_provider_rank and break their registrations.
struct snapshot_capacity_tester : public snapshot_attest_tester {
   snapshot_capacity_tester() : snapshot_attest_tester(capacity_producers()) {}

   static std::vector<account_name> capacity_producers() {
      return {
         "zcapprova"_n, "zcapprovb"_n, "zcapprovc"_n, "zcapprovd"_n, "zcapprove"_n,
         "zcapprovf"_n, "zcapprovg"_n, "zcapprovh"_n, "zcapprovi"_n, "zcapprovj"_n,
         "zcapprovk"_n, "zcapprovl"_n, "zcapprovm"_n, "zcapprovn"_n, "zcapprovo"_n,
         "zcapprovp"_n, "zcapprovq"_n, "zcapprovr"_n, "zcapprovs"_n, "zcapprovt"_n,
         "zcapprovu"_n, "zcapprovv"_n, "zcapprovw"_n, "zcapprovx"_n, "zcapprovy"_n,
         "zcapprovz"_n,
      };
   }
};

BOOST_FIXTURE_TEST_CASE(regsnapprov_rank_band_bounds_provider_table, snapshot_capacity_tester) { try {
   constexpr uint32_t max_registered_snapshot_providers = 30;
   constexpr uint32_t fixture_snapshot_providers        = 5;
   constexpr uint32_t additional_providers_to_fill_cap =
      max_registered_snapshot_providers - fixture_snapshot_providers;

   // Twenty-five fill the remaining slots; the twenty-sixth is the rank-ineligible newcomer.
   const auto capacity_producers = snapshot_capacity_tester::capacity_producers();
   BOOST_REQUIRE_EQUAL(additional_providers_to_fill_cap + 1, capacity_producers.size());

   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer2"_n, "snapprov2"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer3"_n, "snapprov3"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer4"_n, "snapprov4"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer5"_n, "snapprov5"_n));
   for (uint32_t index = 0; index < additional_providers_to_fill_cap; ++index) {
      // A producer may delegate snapshot signing to itself; this minimizes setup while still
      // exercising the public registration action and its provider-table capacity check.
      BOOST_REQUIRE_EQUAL(success(), regsnapprov(capacity_producers[index], capacity_producers[index]));
   }

   // Thirty producers now hold every mapping AND every rank position. The 31st producer is
   // position 31, so it is turned away by the rank gate -- the capacity check below it is never
   // reached, because a full table and a rank-eligible newcomer cannot coexist.
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("producer rank exceeds maximum for snapshot providers"),
                        regsnapprov(capacity_producers.back(), capacity_producers.back()));

   // Deactivating a holder frees the rank position the 31st producer was waiting on -- and leaves
   // that holder's mapping stale. A full-table registration lazily removes stale mappings before
   // enforcing the cap, but a registration that already conflicts must fail without pruning
   // unrelated rows.
   BOOST_REQUIRE_EQUAL(success(), unregproducer("producer1"_n));
   BOOST_REQUIRE(!get_snap_provider("snapprov1"_n).is_null());
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("snap_account is already registered as a provider"),
                       regsnapprov(capacity_producers.back(), "snapprov2"_n));
   BOOST_REQUIRE(!get_snap_provider("snapprov1"_n).is_null());
   BOOST_REQUIRE_EQUAL(success(),
                       regsnapprov(capacity_producers.back(), capacity_producers.back()));
   // The stale mapping was consumed to make room, so the table held at max_snap_providers rather
   // than growing past it -- the bound holds without the capacity check ever firing.
   BOOST_REQUIRE(get_snap_provider("snapprov1"_n).is_null());
   BOOST_REQUIRE(!get_snap_provider(capacity_producers.back()).is_null());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(regsnapprov_is_idempotent_and_rotates_provider, snapshot_attest_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov2"_n));

   BOOST_REQUIRE(get_snap_provider("snapprov1"_n).is_null());
   const auto rotated_provider = get_snap_provider("snapprov2"_n);
   BOOST_REQUIRE(!rotated_provider.is_null());
   BOOST_REQUIRE_EQUAL("producer1", rotated_provider["producer"].as_string());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(regsnapprov_rejects_provider_owned_by_another_producer,
                        snapshot_attest_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("snap_account is already registered as a provider"),
                        regsnapprov("producer2"_n, "snapprov1"_n));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(regsnapprov_wrong_auth, snapshot_attest_tester) { try {
   // snapprov1 tries to register but action requires producer1's auth
   BOOST_REQUIRE_EQUAL(error("missing authority of producer1"),
                        push_action("snapprov1"_n, "regsnapprov"_n, mvo()
                           ("producer", "producer1")
                           ("snap_account", "snapprov1")));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(regsnapprov_unregistered_producer, snapshot_attest_tester) { try {
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("producer is not registered"),
                        regsnapprov("alice1111111"_n, "snapprov1"_n));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(regsnapprov_rank_too_high, snapshot_attest_tester) { try {
   create_account("highrank"_n, config::system_account_name, false, false, true, true);
   produce_blocks();
   regproducer("highrank"_n);
   // No opreg operator row and no finalizer key, so it occupies no rank position at all -- which
   // is exactly the "outside the eligible band" case this rejects.
   produce_blocks();

   BOOST_REQUIRE_EQUAL(wasm_assert_msg("producer rank exceeds maximum for snapshot providers"),
                        regsnapprov("highrank"_n, "snapprov1"_n));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(regsnapprov_rejects_inactive_producer, snapshot_attest_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), unregproducer("producer1"_n));

   BOOST_REQUIRE_EQUAL(wasm_assert_msg("producer is not active"),
                        regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE(get_snap_provider("snapprov1"_n).is_null());
} FC_LOG_AND_RETHROW() }

// ---------------------------------------------------------------------------
// setsnpcfg tests
// ---------------------------------------------------------------------------
BOOST_FIXTURE_TEST_CASE(setsnpcfg_basic, snapshot_attest_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(3));

   // Only sysio can call — producer1 should fail
   BOOST_REQUIRE_EQUAL(error("missing authority of sysio"),
                        push_action("producer1"_n, "setsnpcfg"_n, mvo()
                           ("min_providers", 2)));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(setsnpcfg_validation, snapshot_attest_tester) { try {
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("min_providers must be at least 1"),
                        setsnpcfg(0));
   // min_providers cannot exceed the provider-table ceiling (max_snap_provider_rank
   // == 30): above it, quorum is unreachable no matter how many providers register.
   // The boundary value is accepted; one past it is rejected.
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(30));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("min_providers exceeds the maximum registrable providers"),
                        setsnpcfg(31));
} FC_LOG_AND_RETHROW() }

// ---------------------------------------------------------------------------
// votesnaphash tests
// ---------------------------------------------------------------------------
BOOST_FIXTURE_TEST_CASE(votesnaphash_unregistered, snapshot_voting_tester) { try {
   auto bid = make_block_id(vote_block_num());
   auto shash = make_snap_hash(1);
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("snap_account is not a registered snapshot provider"),
                        votesnaphash("snapprov1"_n, bid, shash));
} FC_LOG_AND_RETHROW() }

/// Producer eligibility is a registration gate; later lifecycle churn does not retract authority or votes.
BOOST_FIXTURE_TEST_CASE(votesnaphash_preserves_registered_authority_after_producer_churn, snapshot_voting_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer2"_n, "snapprov2"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(2));
   BOOST_REQUIRE_EQUAL(success(), unregproducer("producer1"_n));
   BOOST_REQUIRE(!get_snap_provider("snapprov1"_n).is_null());

   const auto block_num = vote_block_num();
   const auto block_id = make_block_id(block_num);
   const auto snapshot_hash = make_snap_hash(9);
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, block_id, snapshot_hash));
   BOOST_REQUIRE(getsnaphash(block_num).is_null());
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov2"_n, block_id, snapshot_hash));
   BOOST_REQUIRE(!getsnaphash(block_num).is_null());
} FC_LOG_AND_RETHROW() }

/// A governance change applies to pending votes, and an exact retry can finalize the existing tuple.
BOOST_FIXTURE_TEST_CASE(votesnaphash_uses_current_fixed_k_for_pending_votes, snapshot_voting_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer2"_n, "snapprov2"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer3"_n, "snapprov3"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(3));

   const auto block_num = vote_block_num();
   const auto block_id = make_block_id(block_num);
   const auto snapshot_hash = make_snap_hash(10);
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, block_id, snapshot_hash));
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov2"_n, block_id, snapshot_hash));
   BOOST_REQUIRE(getsnaphash(block_num).is_null());

   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(2));
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, block_id, snapshot_hash));
   BOOST_REQUIRE(!getsnaphash(block_num).is_null());
} FC_LOG_AND_RETHROW() }

/// Every competing tuple at one height is measured against the same current governance-set K.
BOOST_FIXTURE_TEST_CASE(votesnaphash_uses_current_fixed_k_for_competing_tuples, snapshot_voting_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer2"_n, "snapprov2"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer3"_n, "snapprov3"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(3));

   const auto block_num = vote_block_num();
   const auto block_id_a = make_block_id(block_num);
   const auto block_id_b = make_block_id(block_num, 1);
   const auto hash_a = make_snap_hash(11);
   const auto hash_b = make_snap_hash(12);
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, block_id_a, hash_a));
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov2"_n, block_id_b, hash_b));
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov3"_n, block_id_b, hash_b));
   BOOST_REQUIRE(getsnaphash(block_num).is_null());

   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(2));
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov2"_n, block_id_b, hash_b));
   BOOST_REQUIRE(!getsnaphash(block_num).is_null());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(votesnaphash_single_no_quorum, snapshot_voting_tester) { try {
   // Fixed K is two, so a single vote remains pending regardless of registration count.
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer2"_n, "snapprov2"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer3"_n, "snapprov3"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(2));
   produce_blocks();

   const auto block_num = vote_block_num();
   auto bid = make_block_id(block_num);
   auto shash = make_snap_hash(1);

   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, bid, shash));

   // No attested record yet
   auto rec = getsnaphash(block_num);
   BOOST_REQUIRE_EQUAL(true, rec.is_null());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(votesnaphash_quorum_reached, snapshot_voting_tester) { try {
   // Fixed K is two, so the second distinct producer finalizes the tuple.
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer2"_n, "snapprov2"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer3"_n, "snapprov3"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(2));
   produce_blocks();

   const auto block_num = vote_block_num();
   auto bid = make_block_id(block_num);
   auto shash = make_snap_hash(1);

   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, bid, shash));
   // One vote is below K=2, so the tuple is not attested yet.
   BOOST_REQUIRE_EQUAL(true, getsnaphash(block_num).is_null());

   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov2"_n, bid, shash));

   // K reached — the attested record should exist.
   auto rec = getsnaphash(block_num);
   BOOST_REQUIRE_EQUAL(false, rec.is_null());
   BOOST_REQUIRE_EQUAL(block_num, rec["block_num"].as_uint64());
} FC_LOG_AND_RETHROW() }

// Snapshot service is a SCORING factor, and the only per-producer history it can be scored from is
// this counter: the vote rows that name the voters are purged the moment a record finalizes, so
// without crediting at quorum there would be nothing left to score. Registration is free and
// therefore worthless as a signal; reaching quorum is not, so only the producers whose votes
// carried the record are credited.
BOOST_FIXTURE_TEST_CASE(votesnaphash_quorum_credits_voting_producers, snapshot_voting_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer2"_n, "snapprov2"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer3"_n, "snapprov3"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(2));
   produce_blocks();

   const auto attestations_of = [this](account_name producer) {
      return get_producer_info(producer)["snapshot_attestations"].as<uint32_t>();
   };
   const auto key_of = [this](account_name producer) {
      return get_producer_info(producer)["rank_score"].as<uint64_t>();
   };
   for (const auto& p : {"producer1"_n, "producer2"_n, "producer3"_n}) {
      BOOST_REQUIRE_EQUAL(0u, attestations_of(p));
   }
   const uint64_t key1 = key_of("producer1"_n);
   const uint64_t key2 = key_of("producer2"_n);
   const uint64_t key3 = key_of("producer3"_n);

   const auto block_num = vote_block_num();
   auto       bid       = make_block_id(block_num);
   auto       shash     = make_snap_hash(1);

   // Below quorum nothing is credited: an unfinalized tuple is not service rendered.
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, bid, shash));
   BOOST_REQUIRE_EQUAL(0u, attestations_of("producer1"_n));

   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov2"_n, bid, shash));
   BOOST_REQUIRE(!getsnaphash(block_num).is_null());

   BOOST_REQUIRE_EQUAL(1u, attestations_of("producer1"_n));
   BOOST_REQUIRE_EQUAL(1u, attestations_of("producer2"_n));
   // producer3 registered a provider but never voted, so it earned nothing.
   BOOST_REQUIRE_EQUAL(0u, attestations_of("producer3"_n));

   // The credit is a SCORING factor and it reaches the index at once: a higher composite is a
   // numerically LOWER key. producer3 earned nothing, so its key is untouched.
   BOOST_REQUIRE_LT(key_of("producer1"_n), key1);
   BOOST_REQUIRE_LT(key_of("producer2"_n), key2);
   BOOST_REQUIRE_EQUAL(key3, key_of("producer3"_n));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(votesnaphash_same_tuple_retry_is_idempotent, snapshot_voting_tester) { try {
   // Need 2 providers, min_providers=2 so single vote won't attest and purge
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer2"_n, "snapprov2"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(2));
   produce_blocks();

   const auto block_num = vote_block_num();
   auto bid = make_block_id(block_num);
   auto shash = make_snap_hash(1);

   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, bid, shash));
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, bid, shash));
   BOOST_REQUIRE(getsnaphash(block_num).is_null());
} FC_LOG_AND_RETHROW() }

/// An exact retry remains idempotent after finalization and subsequent eligibility removal.
BOOST_FIXTURE_TEST_CASE(votesnaphash_final_tuple_retry_is_idempotent, snapshot_voting_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(1));

   const auto block_num = vote_block_num();
   const auto block_id = make_block_id(block_num);
   const auto snapshot_hash = make_snap_hash(103);
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, block_id, snapshot_hash));
   BOOST_REQUIRE(!getsnaphash(block_num).is_null());
   BOOST_REQUIRE_EQUAL(0u, snapshot_vote_count());

   BOOST_REQUIRE_EQUAL(success(), unregproducer("producer1"_n));
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, block_id, snapshot_hash));
   BOOST_REQUIRE_EQUAL(0u, snapshot_vote_count());
} FC_LOG_AND_RETHROW() }

/// Voting is disabled until governance explicitly chooses a nonzero fixed K.
BOOST_FIXTURE_TEST_CASE(votesnaphash_rejects_unconfigured_quorum, snapshot_voting_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));

   const auto block_num = vote_block_num();
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("snapshot attestation configuration has not been set"),
                       votesnaphash("snapprov1"_n, make_block_id(block_num), make_snap_hash(101)));
   BOOST_REQUIRE(getsnaphash(block_num).is_null());
} FC_LOG_AND_RETHROW() }

/// A provider cannot pre-attest a tuple for a block height the chain has not reached.
///
/// On the base fixture: the head is far below the first scheduled height, so that height is itself
/// in the future. The check runs before `votesnaphash` reads any table, so proving it needs no
/// chain history -- only a height the chain has not reached, which is every scheduled height here.
BOOST_FIXTURE_TEST_CASE(votesnaphash_rejects_future_block_height, snapshot_attest_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(1));

   const uint32_t future_block_num = scheduled_height(1);
   BOOST_REQUIRE(control->head().block_num() < future_block_num);
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("snapshot block cannot be in the future"),
                       votesnaphash("snapprov1"_n,
                                    make_block_id(future_block_num),
                                    make_snap_hash(102)));
} FC_LOG_AND_RETHROW() }

/// A manual snapshot height cannot enter the bounded on-chain tally space.
///
/// On the base fixture: the scheduled-multiple check is the FIRST thing `votesnaphash` evaluates
/// after decoding the height -- ahead of the future-height check and every table read -- so an
/// off-cadence height is rejected for being off-cadence no matter where the head sits.
BOOST_FIXTURE_TEST_CASE(votesnaphash_rejects_unscheduled_block_height, snapshot_attest_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(1));

   const uint32_t unscheduled_block_num = scheduled_height(1) + 1;
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("snapshot block is not a scheduled attestation height"),
                       votesnaphash("snapprov1"_n,
                                    make_block_id(unscheduled_block_num),
                                    make_snap_hash(104)));
} FC_LOG_AND_RETHROW() }

// ---------------------------------------------------------------------------
// fixed-K tests
// ---------------------------------------------------------------------------
BOOST_FIXTURE_TEST_CASE(fixed_k_can_be_reached_after_more_providers_register, snapshot_voting_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(2));
   produce_blocks();

   const auto block_num = vote_block_num();
   auto bid = make_block_id(block_num);
   auto shash = make_snap_hash(2);

   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, bid, shash));
   BOOST_REQUIRE(getsnaphash(block_num).is_null());
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer2"_n, "snapprov2"_n));
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov2"_n, bid, shash));
   BOOST_REQUIRE(!getsnaphash(block_num).is_null());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(fixed_k_is_independent_of_registration_count, snapshot_voting_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer2"_n, "snapprov2"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer3"_n, "snapprov3"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer4"_n, "snapprov4"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer5"_n, "snapprov5"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(4));
   produce_blocks();

   const auto block_num = vote_block_num();
   auto bid = make_block_id(block_num);
   auto shash = make_snap_hash(3);

   // Three votes do not meet K even though the registration population is five.
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, bid, shash));
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov2"_n, bid, shash));
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov3"_n, bid, shash));
   BOOST_REQUIRE_EQUAL(true, getsnaphash(block_num).is_null());

   // The fourth distinct producer reaches the governance-set K.
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov4"_n, bid, shash));
   BOOST_REQUIRE_EQUAL(false, getsnaphash(block_num).is_null());
} FC_LOG_AND_RETHROW() }

// ---------------------------------------------------------------------------
// disagreement tests
// ---------------------------------------------------------------------------
BOOST_FIXTURE_TEST_CASE(disagreement_detection, snapshot_voting_tester) { try {
   // K=1 finalizes on the first vote regardless of the two registered mappings.
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer2"_n, "snapprov2"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(1));
   produce_blocks();

   const auto block_num = vote_block_num();
   auto bid = make_block_id(block_num);
   auto shash = make_snap_hash(4);

   // Attest with one vote because K=1.
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, bid, shash));
   BOOST_REQUIRE_EQUAL(false, getsnaphash(block_num).is_null());

   // Second provider votes with different hash for same block — disagreement
   auto bad_hash = make_snap_hash(999);
   // snap_hash_disagreement_error = 9001 (defined in snapshot_attest.hpp)
   BOOST_REQUIRE_EQUAL(wasm_assert_code(9001),
                        votesnaphash("snapprov2"_n, bid, bad_hash));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(blockid_mismatch_votes_not_aggregated, snapshot_voting_tester) { try {
   // Three providers are registered, but the fixed K remains two.
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer2"_n, "snapprov2"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer3"_n, "snapprov3"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(2));
   produce_blocks();

   // Same height and same snapshot hash, but different block ids (competing forks).
   const auto block_num = vote_block_num();
   auto bid_a  = make_block_id(block_num);
   auto bid_b  = make_block_id(block_num, 1);
   auto shash  = make_snap_hash(8);

   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, bid_a, shash));
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov2"_n, bid_b, shash));

   // The two votes agree on the hash but not the block id, so they must NOT
   // jointly reach K=2.
   BOOST_REQUIRE_EQUAL(true, getsnaphash(block_num).is_null());

   // A distinct producer may join the first tuple to reach K.
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov3"_n, bid_a, shash));
   auto rec = getsnaphash(block_num);
   BOOST_REQUIRE_EQUAL(false, rec.is_null());
   BOOST_REQUIRE_EQUAL(bid_a.str(), rec["block_id"].as_string());
   BOOST_REQUIRE_EQUAL(shash.str(), rec["snapshot_hash"].as_string());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(votesnaphash_rejects_producer_equivocation_across_hashes, snapshot_voting_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer2"_n, "snapprov2"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(2));

   const auto bid = make_block_id(vote_block_num());
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, bid, make_snap_hash(80)));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("producer already voted a different snapshot tuple for this height"),
                        votesnaphash("snapprov1"_n, bid, make_snap_hash(81)));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(votesnaphash_reports_disagreement_before_eligibility_failure, snapshot_voting_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer2"_n, "snapprov2"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(1));

   const auto bid = make_block_id(vote_block_num());
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, bid, make_snap_hash(83)));
   BOOST_REQUIRE_EQUAL(success(), unregproducer("producer2"_n));
   BOOST_REQUIRE_EQUAL(wasm_assert_code(9001),
                        votesnaphash("snapprov2"_n, bid, make_snap_hash(84)));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(record_blockid_disagreement, snapshot_voting_tester) { try {
   // K=1 finalizes on the first vote regardless of the two registered mappings.
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer2"_n, "snapprov2"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(1));
   produce_blocks();

   const auto block_num = vote_block_num();
   auto bid_a = make_block_id(block_num);
   auto bid_b = make_block_id(block_num, 1);
   auto shash = make_snap_hash(9);

   // Attest with one vote because K=1.
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, bid_a, shash));
   BOOST_REQUIRE_EQUAL(false, getsnaphash(block_num).is_null());

   // Same snapshot hash under a different block id disagrees with the attested record
   BOOST_REQUIRE_EQUAL(wasm_assert_code(9001),
                        votesnaphash("snapprov2"_n, bid_b, shash));
} FC_LOG_AND_RETHROW() }

// ---------------------------------------------------------------------------
// purging tests
// ---------------------------------------------------------------------------
/// Votes at different scheduled heights coexist until a final record purges older pending rows.
BOOST_FIXTURE_TEST_CASE(votesnaphash_keeps_scheduled_heights_independent_until_finalization, snapshot_multi_height_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer2"_n, "snapprov2"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(2));

   // Both heights are already behind the head -- the fixture built two cadence periods up front.
   const uint32_t newer_block_num = vote_block_num();
   const uint32_t older_block_num = newer_block_num - sysio::protocol::snapshot_attestation::block_spacing;
   const auto older_block_id = make_block_id(older_block_num);
   const auto older_hash = make_snap_hash(5);
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, older_block_id, older_hash));

   const auto newer_block_id = make_block_id(newer_block_num);
   const auto newer_hash = make_snap_hash(6);
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, newer_block_id, newer_hash));
   BOOST_REQUIRE_EQUAL(2u, snapshot_vote_count());

   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov2"_n, older_block_id, older_hash));
   BOOST_REQUIRE(!getsnaphash(older_block_num).is_null());
   BOOST_REQUIRE_EQUAL(1u, snapshot_vote_count());

   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov2"_n, newer_block_id, newer_hash));
   BOOST_REQUIRE(!getsnaphash(newer_block_num).is_null());
   BOOST_REQUIRE_EQUAL(0u, snapshot_vote_count());
} FC_LOG_AND_RETHROW() }

/// A newer finalization permanently closes older heights whose unfinished rows were purged.
BOOST_FIXTURE_TEST_CASE(votesnaphash_rejects_reopening_purged_historical_height, snapshot_multi_height_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer2"_n, "snapprov2"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(2));

   // Both heights are already behind the head -- the fixture built two cadence periods up front.
   const uint32_t newer_block_num = vote_block_num();
   const uint32_t older_block_num = newer_block_num - sysio::protocol::snapshot_attestation::block_spacing;
   const auto older_block_id = make_block_id(older_block_num);
   const auto older_hash = make_snap_hash(25);
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, older_block_id, older_hash));

   const auto newer_block_id = make_block_id(newer_block_num);
   const auto newer_hash = make_snap_hash(26);
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, newer_block_id, newer_hash));
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov2"_n, newer_block_id, newer_hash));
   BOOST_REQUIRE(!getsnaphash(newer_block_num).is_null());
   BOOST_REQUIRE(getsnaphash(older_block_num).is_null());
   BOOST_REQUIRE_EQUAL(0u, snapshot_vote_count());

   BOOST_REQUIRE_EQUAL(wasm_assert_msg("snapshot block is older than latest attested snapshot height"),
                       votesnaphash("snapprov2"_n, older_block_id, older_hash));
   BOOST_REQUIRE_EQUAL(0u, snapshot_vote_count());
} FC_LOG_AND_RETHROW() }

/// Registration churn cannot erase pending votes cast by other producers.
BOOST_FIXTURE_TEST_CASE(votesnaphash_registration_churn_preserves_other_votes, snapshot_voting_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer2"_n, "snapprov2"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer3"_n, "snapprov3"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer4"_n, "snapprov4"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(3));

   const auto block_num = vote_block_num();
   const auto block_id = make_block_id(block_num);
   const auto snapshot_hash = make_snap_hash(114);
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, block_id, snapshot_hash));
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov2"_n, block_id, snapshot_hash));

   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer3"_n, "snapprov5"_n));
   BOOST_REQUIRE_EQUAL(1u, snapshot_vote_count());

   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov4"_n, block_id, snapshot_hash));
   BOOST_REQUIRE(!getsnaphash(block_num).is_null());
} FC_LOG_AND_RETHROW() }

// ---------------------------------------------------------------------------
// getsnaphash tests
// ---------------------------------------------------------------------------
BOOST_FIXTURE_TEST_CASE(getsnaphash_not_found, snapshot_attest_tester) { try {
   auto rec = getsnaphash(99999);
   BOOST_REQUIRE_EQUAL(true, rec.is_null());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(getsnaphash_action_not_found, snapshot_attest_tester) { try {
   // Exercise the getsnaphash action's check() assertion for missing records
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("no attested snapshot record for this block number"),
                        push_action(config::system_account_name, "getsnaphash"_n, mvo()
                           ("block_num", 99999)));
} FC_LOG_AND_RETHROW() }

/// Governance owns the fixed-K tradeoff; K=1 deliberately permits one of many providers to attest.
BOOST_FIXTURE_TEST_CASE(votesnaphash_honors_governance_fixed_k, snapshot_voting_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer2"_n, "snapprov2"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer3"_n, "snapprov3"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(1));

   const auto block_num = vote_block_num();
   const auto bid  = make_block_id(block_num);
   const auto hash = make_snap_hash(1);

   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, bid, hash));
   BOOST_REQUIRE(!getsnaphash(block_num).is_null());
} FC_LOG_AND_RETHROW() }

/// Rotating a snapshot account cannot add Sybil weight and makes exact retries idempotent.
BOOST_FIXTURE_TEST_CASE(votesnaphash_snap_account_rotation_does_not_add_sybil_weight, snapshot_voting_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer2"_n, "snapprov2"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer3"_n, "snapprov3"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(2));

   const auto block_num = vote_block_num();
   const auto block_id = make_block_id(block_num);
   const auto snapshot_hash = make_snap_hash(7);
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, block_id, snapshot_hash));
   BOOST_REQUIRE_EQUAL(1u, snapshot_vote_count());

   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov5"_n));
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov5"_n, block_id, snapshot_hash));
   BOOST_REQUIRE_EQUAL(1u, snapshot_vote_count());
   BOOST_REQUIRE(getsnaphash(block_num).is_null());

   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov2"_n, block_id, snapshot_hash));
   BOOST_REQUIRE(!getsnaphash(block_num).is_null());
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()

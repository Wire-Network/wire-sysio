#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#include <boost/test/unit_test.hpp>
#pragma GCC diagnostic pop

#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/resource_limits.hpp>
#include <sysio.system/snapshot_attest_constants.hpp>
#include <sysio/testing/tester.hpp>

#include <fc/exception/exception.hpp>
#include <fc/variant_object.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <map>

#include "sysio.system_tester.hpp"
#include "finalizer_test_keys.hpp"

using namespace sysio_system;

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------
class snapshot_attest_tester : public sysio_system_tester {
public:
   /// Small configured cadence used by contract tests that need real scheduled heights.
   static constexpr uint32_t test_block_spacing = 250;

   /// The five producers every test in this fixture ranks first, so each of them can vote.
   static std::vector<account_name> fixture_producers() {
      return {"producer1"_n, "producer2"_n, "producer3"_n, "producer4"_n, "producer5"_n};
   }

   /// The contract's `max_snap_providers`, which a host test cannot include.
   static constexpr uint32_t max_snapshot_providers = 30;

   /// The producer permission a provider node signs `votesnaphash` with.
   static inline const permission_name snapshot_permission{sysiosystem::snapshot_attestation::permission_snapshot};

   snapshot_attest_tester() : snapshot_attest_tester(std::vector<account_name>{}, 0) {}

   /**
    * @param extra_producers producers a single test needs beyond the fixture's five. They are
    *   created HERE rather than in the test because every registered finalizer key has to be in
    *   the node's voting set from the start -- see `register_schedulable_finalizer_keys`.
    * @param cadence_periods how many test-block-spacing periods of history to build before anything
    *   else. 0 -- the default -- costs nothing, and is what every configuration test uses: it
    *   deliberately does NOT advance, because the advance permanently
    *   forfeits the validating controller. 1 reaches the first attestable height
    *   (`snapshot_voting_tester`); 2 reaches the second, for the purging tests that need two
    *   scheduled heights live at once (`snapshot_multi_height_tester`). A test pays for the
    *   periods its assertions actually require and no more.
    * @param bonded_producers producers registered as NON-bootstrapped operators bonded at exactly `producer_min_bond`,
    *   rather than bootstrapped like the rest; they sort ahead of the bootstrapped tier. Supplied here for the same
    *   finalizer-key reason as `extra_producers`. Empty, the default, installs no collateral config.
    */
   explicit snapshot_attest_tester(const std::vector<account_name>& extra_producers,
                                   uint32_t cadence_periods = 0,
                                   const std::vector<account_name>& bonded_producers = {})
      // A cadence-advancing fixture stops ONE level short of `full`, so the chain reaches the
      // attestable height with only `sysio.bios` on the system account. bios declares no
      // `onblock`, so those blocks execute no contract code at all — where under `sysio.system`
      // every one of them runs the interpreted `onblock` (blockinfo write, round attribution,
      // and a schedule rebuild every 120 slots). Keeping that work on the cheap side of deployment
      // still matters even with the deliberately small test cadence.
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
      setup_producer_accounts(bonded_producers);

      produce_blocks();

      // Snapshot voting eligibility is POSITION among schedulable producers, not a stored rank
      // governance hands out. A producer is schedulable only as an ACTIVE PRODUCER operator in
      // sysio.opreg carrying an active finalizer key. The ACTIVE operator row must now exist before
      // regproducer is admitted, so install the bootstrapped fixture operators first.
      deploy_opreg_once();
      register_producer_operators(std::vector<name>(producers.begin(), producers.end()));
      register_bonded_producer_operators(bonded_producers);
      // Past opreg registration a bonded producer is set up like any other.
      producers.insert(producers.end(), bonded_producers.begin(), bonded_producers.end());
      for (const auto& p : producers) {
         regproducer(p);
      }
      produce_blocks();
      // With equal scores the index orders by account name, so producer1..producer5 take positions
      // 1..5 -- all inside max_snap_provider_rank.
      // Reach the attestation cadence BEFORE registering finalizer keys, i.e. while no producer
      // is schedulable yet. Ordering is the whole point: once they are, `update_ranked_producers`
      // publishes a policy of ALL of them, and the node then signs and verifies one vote per
      // finalizer on EVERY block. Advancing first leaves the cheap single-finalizer genesis policy
      // in force for the configured history, and the larger policy applies only to the handful of
      // blocks a test produces afterwards.
      register_schedulable_finalizer_keys(std::vector<name>(producers.begin(), producers.end()));
      produce_blocks();
   }

   /**
    * Produce blocks until the head reaches the given number of test-cadence periods.
    *
    * `votesnaphash` rejects a height above the head, and only multiples of the configured cadence are
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
    * against the validating controller's older head.
    *
    * @param periods how many test-cadence periods of history to build.
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
   /// then advance to a scheduled height, so an unpruned
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

   /// Chain and token of the one producer collateral pair `set_producer_min_bond` requires.
   static constexpr std::string_view bond_chain = "WIRE";
   static constexpr std::string_view bond_token = "WIRE";

   /// The depot-native collateral denomination, separate from the fixture's SYS resource token.
   static inline const symbol bond_symbol = symbol(9, "WIRE");

   /// The producer minimum `register_bonded_producer_operators` installs, and the bond each bonded producer posts.
   static constexpr uint64_t producer_min_bond = 1'000'000;

   /// Replace sysio.opreg's producer collateral requirement with `min_bond` on the one required pair.
   ///
   /// `setconfig` re-evaluates no operator, so a status earned under the previous minimum stays stored.
   void set_producer_min_bond(uint64_t min_bond) {
      base_tester::push_action("sysio.opreg"_n, "setconfig"_n, "sysio.opreg"_n, mvo()
         ("terminate_prune_delay_ms",         uint64_t{600'000})
         ("terminate_max_consecutive_misses", uint32_t{5})
         ("terminate_max_pct_misses_24h",     uint32_t{5})
         ("terminate_window_ms",              uint64_t{24ULL * 60 * 60 * 1000})
         ("req_prod_collat",                  fc::variants{fc::variant(mvo()
            ("chain_code",          bond_chain)
            ("token_code",          bond_token)
            ("min_bond",            min_bond)
            ("config_timestamp_ms", uint64_t{0}))})
         ("req_batchop_collat",               fc::variants{})
         ("req_uw_collat",                    fc::variants{}));
      produce_block();
   }

   /// Fund and deposit real depot-native collateral under the producer's authority. opreg
   /// re-evaluates the operator's status on the balance change.
   void credit_producer_bond(name producer, uint64_t amount) {
      issue_and_transfer(producer, asset(static_cast<int64_t>(amount), bond_symbol));
      base_tester::push_action("sysio.opreg"_n, "deposit"_n, producer, mvo()
         ("account",    producer)
         ("token_code", bond_token)
         ("amount",     amount));
      produce_block();
   }

   /// Register each name as a NON-bootstrapped PRODUCER operator bonded at exactly `producer_min_bond`. The deposit
   /// that clears the minimum is what makes opreg promote it to ACTIVE, which `regproducer` then admits.
   void register_bonded_producer_operators(const std::vector<name>& names) {
      if (names.empty()) return;
      create_currency("sysio.token"_n, config::system_account_name, asset(asset::max_amount, bond_symbol));
      set_producer_min_bond(producer_min_bond);
      for (const auto& p : names) {
         base_tester::push_action("sysio.opreg"_n, "regoperator"_n, "sysio.opreg"_n, mvo()
            ("account", p)
            ("type", sysio::opp::types::OperatorType::OPERATOR_TYPE_PRODUCER)
            ("is_bootstrapped", false));
         credit_producer_bond(p, producer_min_bond);
      }
   }

   /// The status sysio.opreg stores on `account`'s operator row.
   sysio::opp::types::OperatorStatus opreg_status(name account) {
      const auto data = get_row_by_account("sysio.opreg"_n, "sysio.opreg"_n, "operators"_n, account);
      BOOST_REQUIRE(!data.empty());
      const auto opreg_abi = get_resolver()("sysio.opreg"_n);
      BOOST_REQUIRE(opreg_abi.has_value());
      const auto row = opreg_abi->binary_to_variant(
         "operator_entry", data, fixture_abi_yield_function());
      return row["status"].as<sysio::opp::types::OperatorStatus>();
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

   /// Deactivate a producer while retaining its producer-table row.
   action_result unregproducer(name producer) {
      return push_action(producer, "unregprod"_n, mvo()
         ("producer", producer));
   }

   /// Vote on a snapshot hash as `producer@active`.
   action_result votesnaphash(name producer, const fc::sha256& block_id, const fc::sha256& snapshot_hash) {
      return push_action(producer, "votesnaphash"_n, mvo()
         ("producer", producer)
         ("block_id", block_id)
         ("snapshot_hash", snapshot_hash));
   }

   /// Give `producer` a snapshot permission holding `key` under active, linked to `votesnaphash`, as an operator would.
   void link_snapshot_permission(name producer, const public_key_type& key) {
      set_authority(producer, snapshot_permission, authority(key), config::active_name);
      link_authority(producer, config::system_account_name, snapshot_permission, "votesnaphash"_n);
      produce_block();
   }

   /// Push the sysio action `action_type` for `producer` declaring only `producer@permission`, signed with `key` alone.
   action_result push_signed_as(name producer, permission_name permission, const private_key_type& key,
                                action_name action_type, const variant_object& data) {
      action act;
      act.account       = config::system_account_name;
      act.name          = action_type;
      act.authorization = {{producer, permission}};
      act.data          = abi_ser.variant_to_binary(abi_ser.get_action_type(action_type), data,
                                                    fixture_abi_yield_function());

      signed_transaction trx;
      trx.actions.emplace_back(std::move(act));
      set_transaction_headers(trx);
      trx.sign(key, control->get_chain_id());
      try {
         push_transaction(trx);
      } catch (const fc::exception& ex) {
         return error(ex.top_message());
      }
      produce_block();
      return success();
   }

   /// Vote on a snapshot hash as `producer@permission`, signed with `key` alone, the way a provider node signs.
   action_result votesnaphash_signed_as(name producer, permission_name permission, const private_key_type& key,
                                        const fc::sha256& block_id, const fc::sha256& snapshot_hash) {
      return push_signed_as(producer, permission, key, "votesnaphash"_n, mvo()
         ("producer", producer)
         ("block_id", block_id)
         ("snapshot_hash", snapshot_hash));
   }

   /// Set snapshot attestation configuration, using the small test cadence by default.
   action_result setsnpcfg(uint32_t min_providers, uint32_t block_spacing = test_block_spacing) {
      return push_action(config::system_account_name, "setsnpcfg"_n, mvo()
         ("min_providers", min_providers)
         ("block_spacing", block_spacing));
   }

   /// Return the singleton snapshot-attestation configuration row.
   fc::variant get_snap_config() {
      const vector<char> data = get_row_by_account(
         config::system_account_name, config::system_account_name,
         "snapconfig"_n, "snapconfig"_n);
      return data.empty() ? fc::variant() : abi_ser.binary_to_variant(
         "snap_config", data, fixture_abi_yield_function());
   }

   /// Return an attested snapshot record from the contract table.
   fc::variant getsnaphash(uint32_t block_num) {
      vector<char> data = get_row_by_account(
         config::system_account_name, config::system_account_name,
         "snaprecords"_n, name(block_num));
      return data.empty() ? fc::variant() : abi_ser.binary_to_variant(
         "snap_record", data, fixture_abi_yield_function());
   }

   /// RAM `account` currently uses.
   int64_t ram_usage(account_name account) {
      return control->get_resource_limits_manager().get_account_ram_usage(account);
   }

   /// Cut `account` down to what `sysio.roa::newuser` grants: newaccount_ram and no CPU/NET.
   void limit_to_new_user_resources(account_name account) {
      constexpr int64_t no_weight = 0;
      BOOST_REQUIRE_LE(ram_usage(account), static_cast<int64_t>(newaccount_ram));
      BOOST_REQUIRE_EQUAL(success(), push_action(config::system_account_name, "setacctram"_n, mvo()
         ("account", account)("ram_bytes", static_cast<int64_t>(newaccount_ram))));
      BOOST_REQUIRE_EQUAL(success(), push_action(config::system_account_name, "setacctnet"_n, mvo()
         ("account", account)("net_weight", no_weight)));
      BOOST_REQUIRE_EQUAL(success(), push_action(config::system_account_name, "setacctcpu"_n, mvo()
         ("account", account)("cpu_weight", no_weight)));
   }

   /// Produce blocks until the head is a multiple of `block_spacing`, i.e. a scheduled height.
   void align_head_to(uint32_t block_spacing) {
      const uint32_t remainder = control->head().block_num() % block_spacing;
      if (remainder != 0) {
         produce_blocks(block_spacing - remainder);
      }
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

   /// The height a given test-cadence period falls on.
   ///
   /// Usable without advancing when a test only needs to construct a candidate height.
   static constexpr uint32_t scheduled_height(uint32_t period) {
      return period * test_block_spacing;
   }

   /// The latest attestable height at or below the head.
   ///
   /// This does NOT advance. How far the chain runs is a constructor decision (`cadence_periods`)
   /// because that is the only point at which the advance is affordable; a fixture that did not ask
   /// for one has no attestable height and says so, rather than silently buying one mid-test at
   /// several times the price.
   uint32_t vote_block_num() {
      constexpr uint32_t spacing = test_block_spacing;
      // Commit setup and configuration the test queued before reading the height.
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
 * It builds one small configured-cadence period BEFORE the system contract is deployed and
 * BEFORE any finalizer key is registered, which is the only cheap order: once the producers are
 * schedulable, `update_ranked_producers` publishes a policy of all of them and the node signs plus
 * verifies one vote per finalizer on EVERY block. Paying that across the advance took this suite
 * from minutes to over an hour.
 *
 * The configuration and permission-scope tests, and the two height-precondition negatives whose checks
 * fire before the contract reads any table, use the base fixture and never advance at all.
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

/// Voting fixture with one producer bonded as a NON-bootstrapped operator at exactly the producer minimum, beside the
/// fixture's five bootstrapped ones: the only kind of producer a collateral minimum can apply to.
struct snapshot_bonded_voting_tester : public snapshot_attest_tester {
   static constexpr account_name bonded_producer = "bondedprod"_n;

   snapshot_bonded_voting_tester()
      : snapshot_attest_tester(std::vector<account_name>{}, /*cadence_periods*/ 1, {bonded_producer}) {}
};

BOOST_AUTO_TEST_SUITE(sysio_snapshot_attest_tests)

// ---------------------------------------------------------------------------
// vote eligibility tests
// ---------------------------------------------------------------------------
/// Any producer holding one of the first max_snap_provider_rank positions can vote, with no setup.
BOOST_FIXTURE_TEST_CASE(votesnaphash_accepts_a_top_ranked_producer, snapshot_voting_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(1));

   const auto block_num = vote_block_num();
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("producer1"_n, make_block_id(block_num), make_snap_hash(1)));
   BOOST_REQUIRE(!getsnaphash(block_num).is_null());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(votesnaphash_rejects_an_account_that_is_not_a_producer, snapshot_voting_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(1));

   const auto block_num = vote_block_num();
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("producer rank exceeds maximum for snapshot providers"),
                        votesnaphash("alice1111111"_n, make_block_id(block_num), make_snap_hash(2)));
   BOOST_REQUIRE(getsnaphash(block_num).is_null());
} FC_LOG_AND_RETHROW() }

/// A parked producer holds no rank position, so it cannot vote until it registers as a producer again.
BOOST_FIXTURE_TEST_CASE(votesnaphash_rejects_a_parked_producer, snapshot_voting_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(1));
   BOOST_REQUIRE_EQUAL(success(), unregproducer("producer1"_n));

   const auto block_num = vote_block_num();
   const auto block_id  = make_block_id(block_num);
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("producer rank exceeds maximum for snapshot providers"),
                        votesnaphash("producer1"_n, block_id, make_snap_hash(3)));

   regproducer("producer1"_n);
   produce_block();
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("producer1"_n, block_id, make_snap_hash(3)));
   BOOST_REQUIRE(!getsnaphash(block_num).is_null());
} FC_LOG_AND_RETHROW() }

/// Fixture for the rank-band case: twenty-six producers beyond the fixture's five, so one sits at position 31.
///
/// They are constructor-supplied rather than created inside the test because every finalizer key
/// the chain knows about has to be in the node's voting set BEFORE `onblock` proposes a policy
/// from them -- see `snapshot_attest_tester::register_schedulable_finalizer_keys`.
///
/// The `z` prefix is load-bearing: rank is POSITION in the score-ordered index, and equal scores
/// order by account name. A `bandprod*` name sorts BEFORE `producer*`, which would push the
/// fixture's own five past max_snap_provider_rank.
struct snapshot_rank_band_tester : public snapshot_attest_tester {
   snapshot_rank_band_tester() : snapshot_attest_tester(band_producers(), /*cadence_periods*/ 1) {}

   static std::vector<account_name> band_producers() {
      return {
         "zbandproda"_n, "zbandprodb"_n, "zbandprodc"_n, "zbandprodd"_n, "zbandprode"_n,
         "zbandprodf"_n, "zbandprodg"_n, "zbandprodh"_n, "zbandprodi"_n, "zbandprodj"_n,
         "zbandprodk"_n, "zbandprodl"_n, "zbandprodm"_n, "zbandprodn"_n, "zbandprodo"_n,
         "zbandprodp"_n, "zbandprodq"_n, "zbandprodr"_n, "zbandprods"_n, "zbandprodt"_n,
         "zbandprodu"_n, "zbandprodv"_n, "zbandprodw"_n, "zbandprodx"_n, "zbandprody"_n,
         "zbandprodz"_n,
      };
   }
};

/// Rank is read at each vote: position 31 is refused, and moves into the band the moment a producer above it parks.
BOOST_FIXTURE_TEST_CASE(votesnaphash_follows_the_live_rank_band, snapshot_rank_band_tester) { try {
   const auto band = band_producers();
   BOOST_REQUIRE_EQUAL(max_snapshot_providers + 1, fixture_producers().size() + band.size());
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(1));

   const auto block_num = vote_block_num();
   const auto block_id  = make_block_id(block_num);
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("producer rank exceeds maximum for snapshot providers"),
                        votesnaphash(band.back(), block_id, make_snap_hash(4)));

   BOOST_REQUIRE_EQUAL(success(), unregproducer("producer1"_n));
   BOOST_REQUIRE_EQUAL(success(), votesnaphash(band.back(), block_id, make_snap_hash(4)));
   BOOST_REQUIRE(!getsnaphash(block_num).is_null());
} FC_LOG_AND_RETHROW() }

// ---------------------------------------------------------------------------
// snapshot permission tests
// ---------------------------------------------------------------------------
/// A provider node holds only the producer's snapshot key: linked to `votesnaphash`, that key casts the
/// vote alone, and the vote counts for the producer.
BOOST_FIXTURE_TEST_CASE(votesnaphash_accepts_the_linked_snapshot_permission, snapshot_voting_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(1));
   const auto snapshot_key = get_private_key("producer1"_n, snapshot_permission.to_string());
   link_snapshot_permission("producer1"_n, snapshot_key.get_public_key());

   const auto block_num = vote_block_num();
   BOOST_REQUIRE_EQUAL(success(), votesnaphash_signed_as("producer1"_n, snapshot_permission, snapshot_key,
                                                         make_block_id(block_num), make_snap_hash(140)));
   BOOST_REQUIRE(!getsnaphash(block_num).is_null());
   BOOST_REQUIRE_EQUAL(1u, get_producer_info("producer1"_n)["snapshot_attestations"].as<uint32_t>());
} FC_LOG_AND_RETHROW() }

/// Until the producer links it, the snapshot permission authorizes nothing: `votesnaphash` falls back to
/// requiring active.
BOOST_FIXTURE_TEST_CASE(votesnaphash_rejects_an_unlinked_snapshot_permission, snapshot_voting_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(1));
   const auto snapshot_key = get_private_key("producer1"_n, snapshot_permission.to_string());
   set_authority("producer1"_n, snapshot_permission, authority(snapshot_key.get_public_key()), config::active_name);
   produce_block();

   const auto block_num = vote_block_num();
   const auto result    = votesnaphash_signed_as("producer1"_n, snapshot_permission, snapshot_key,
                                                 make_block_id(block_num), make_snap_hash(141));
   BOOST_REQUIRE_MESSAGE(result.find("irrelevant authority") != std::string::npos, result);
   BOOST_REQUIRE(getsnaphash(block_num).is_null());
} FC_LOG_AND_RETHROW() }

/// The snapshot key is scoped to votes: it can neither park the producer nor claim its pay.
BOOST_FIXTURE_TEST_CASE(snapshot_permission_authorizes_only_votes, snapshot_attest_tester) { try {
   const auto snapshot_key = get_private_key("producer1"_n, snapshot_permission.to_string());
   link_snapshot_permission("producer1"_n, snapshot_key.get_public_key());

   const auto park = push_signed_as("producer1"_n, snapshot_permission, snapshot_key, "unregprod"_n,
                                    mvo()("producer", "producer1"));
   BOOST_REQUIRE_MESSAGE(park.find("irrelevant authority") != std::string::npos, park);
   BOOST_REQUIRE(get_producer_info("producer1"_n)["is_active"].as<bool>());

   const auto claim = push_signed_as("producer1"_n, snapshot_permission, snapshot_key, "claimpay"_n,
                                     mvo()("account_name", "producer1"));
   BOOST_REQUIRE_MESSAGE(claim.find("irrelevant authority") != std::string::npos, claim);
} FC_LOG_AND_RETHROW() }

/// Rotating the snapshot key is a native `updateauth`: the old key stops voting and the new one starts.
BOOST_FIXTURE_TEST_CASE(votesnaphash_follows_a_rotated_snapshot_key, snapshot_voting_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(1));
   const auto old_key = get_private_key("producer1"_n, snapshot_permission.to_string());
   const auto new_key = get_private_key("producer1"_n, "rotated");
   link_snapshot_permission("producer1"_n, old_key.get_public_key());
   set_authority("producer1"_n, snapshot_permission, authority(new_key.get_public_key()), config::active_name);
   produce_block();

   const auto block_num     = vote_block_num();
   const auto block_id      = make_block_id(block_num);
   const auto snapshot_hash = make_snap_hash(142);
   const auto stale         = votesnaphash_signed_as("producer1"_n, snapshot_permission, old_key, block_id,
                                                     snapshot_hash);
   BOOST_REQUIRE_MESSAGE(stale.find("does not have signatures") != std::string::npos, stale);
   BOOST_REQUIRE(getsnaphash(block_num).is_null());

   BOOST_REQUIRE_EQUAL(success(), votesnaphash_signed_as("producer1"_n, snapshot_permission, new_key, block_id,
                                                         snapshot_hash));
   BOOST_REQUIRE(!getsnaphash(block_num).is_null());
} FC_LOG_AND_RETHROW() }

/// One producer cannot vote in another's name.
BOOST_FIXTURE_TEST_CASE(votesnaphash_requires_the_producers_authority, snapshot_voting_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(1));

   const auto block_num = vote_block_num();
   BOOST_REQUIRE_EQUAL(error("missing authority of producer1"),
                        push_action("producer2"_n, "votesnaphash"_n, mvo()
                           ("producer", "producer1")
                           ("block_id", make_block_id(block_num))
                           ("snapshot_hash", make_snap_hash(143))));
   BOOST_REQUIRE(getsnaphash(block_num).is_null());
} FC_LOG_AND_RETHROW() }

// ---------------------------------------------------------------------------
// setsnpcfg tests
// ---------------------------------------------------------------------------
BOOST_FIXTURE_TEST_CASE(setsnpcfg_basic, snapshot_attest_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(3));

   // Only sysio can call — producer1 should fail
   BOOST_REQUIRE_EQUAL(error("missing authority of sysio"),
                        push_action("producer1"_n, "setsnpcfg"_n, mvo()
                           ("min_providers", 2)
                           ("block_spacing", test_block_spacing)));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(setsnpcfg_validation, snapshot_attest_tester) { try {
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("min_providers must be at least 1"),
                        setsnpcfg(0));
   // min_providers cannot exceed max_snap_provider_rank (30): above it, quorum is
   // unreachable because no more producers can vote.
   // One past each supported boundary is rejected without initializing the singleton.
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("min_providers exceeds the number of producers that can vote"),
                        setsnpcfg(31));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("block_spacing must be greater than 0"),
                       setsnpcfg(1, 0));
   BOOST_REQUIRE(get_snap_config().is_null());
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(30));
   BOOST_REQUIRE_EQUAL(30u, get_snap_config()["min_providers"].as_uint64());
} FC_LOG_AND_RETHROW() }

/** Governance can select every positive uint32 cadence, including single-block test spacing. */
BOOST_FIXTURE_TEST_CASE(setsnpcfg_accepts_every_positive_block_spacing, snapshot_attest_tester) { try {
   const std::array spacings{
      uint32_t{1}, sysiosystem::snapshot_attestation::default_block_spacing + 1,
      std::numeric_limits<uint32_t>::max()};
   for (const auto spacing : spacings) {
      BOOST_REQUIRE_EQUAL(success(), setsnpcfg(1, spacing));
      const auto config = get_snap_config();
      BOOST_REQUIRE(!config.is_null());
      BOOST_REQUIRE_EQUAL(1u, config["min_providers"].as_uint64());
      BOOST_REQUIRE_EQUAL(spacing, config["block_spacing"].as_uint64());
   }
} FC_LOG_AND_RETHROW() }

/** Quorum and cadence can change together; a rejected zero leaves the previous values intact. */
BOOST_FIXTURE_TEST_CASE(setsnpcfg_updates_cadence_and_quorum, snapshot_attest_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(3));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(2, test_block_spacing + 1));
   auto config = get_snap_config();
   BOOST_REQUIRE_EQUAL(2u, config["min_providers"].as_uint64());
   BOOST_REQUIRE_EQUAL(test_block_spacing + 1, config["block_spacing"].as_uint64());

   BOOST_REQUIRE_EQUAL(wasm_assert_msg("block_spacing must be greater than 0"),
                       setsnpcfg(1, 0));
   config = get_snap_config();
   BOOST_REQUIRE_EQUAL(2u, config["min_providers"].as_uint64());
   BOOST_REQUIRE_EQUAL(test_block_spacing + 1, config["block_spacing"].as_uint64());
} FC_LOG_AND_RETHROW() }

/** Height eligibility accepts any positive cadence and grace deliberately rounds down. */
BOOST_AUTO_TEST_CASE(snapshot_attestation_cadence_helpers_enforce_boundaries) {
   namespace cadence = sysiosystem::snapshot_attestation;
   constexpr uint32_t small_spacing = 4;
   constexpr uint32_t odd_spacing = 5;
   constexpr auto largest_spacing = std::numeric_limits<uint32_t>::max();
   BOOST_CHECK(!cadence::is_valid_block_spacing(0));
   BOOST_CHECK(cadence::is_valid_block_spacing(1));
   BOOST_CHECK(cadence::is_valid_block_spacing(cadence::default_block_spacing + 1));
   BOOST_CHECK(cadence::is_valid_block_spacing(largest_spacing));

   BOOST_CHECK(!cadence::is_scheduled_block(0, small_spacing));
   BOOST_CHECK(!cadence::is_scheduled_block(small_spacing - 1, small_spacing));
   BOOST_CHECK(cadence::is_scheduled_block(small_spacing, small_spacing));
   BOOST_CHECK(!cadence::is_scheduled_block(small_spacing + 1, small_spacing));
   BOOST_CHECK(cadence::is_scheduled_block(largest_spacing, largest_spacing));
   BOOST_CHECK(!cadence::is_scheduled_block(largest_spacing, 0));
   BOOST_CHECK(cadence::is_scheduled_block(1, 1));

   BOOST_CHECK_EQUAL(0u, cadence::snapshot_attestation_grace_blocks(1));
   BOOST_CHECK_EQUAL(2u, cadence::snapshot_attestation_grace_blocks(odd_spacing));
   BOOST_CHECK_EQUAL(12'500u,
                     cadence::snapshot_attestation_grace_blocks(cadence::default_block_spacing));

   // The pending window opens at the first scheduled height, then holds exactly the most recent
   // max_pending_vote_heights scheduled heights at every block.
   constexpr uint32_t checked_periods = 3;
   constexpr uint32_t window_blocks   = cadence::max_pending_vote_heights * small_spacing;
   BOOST_CHECK_EQUAL(0u, cadence::pending_vote_floor(window_blocks - 1, small_spacing));
   BOOST_CHECK_EQUAL(1u, cadence::pending_vote_floor(window_blocks, small_spacing));
   for (uint32_t current = window_blocks; current < checked_periods * window_blocks; ++current) {
      const uint64_t floor        = cadence::pending_vote_floor(current, small_spacing);
      uint32_t       open_heights = 0;
      for (uint32_t height = small_spacing; height <= current; height += small_spacing) {
         if (height >= floor) ++open_heights;
      }
      BOOST_CHECK_EQUAL(cadence::max_pending_vote_heights, open_heights);
   }
   // A cadence near the uint32 limit cannot wrap the floor past a valid height.
   BOOST_CHECK_EQUAL(0u, cadence::pending_vote_floor(largest_spacing, largest_spacing));

   // A vote landing at the end of the node-side verification grace still finds its height open.
   for (const uint32_t spacing : {uint32_t{1}, small_spacing, odd_spacing, cadence::default_block_spacing}) {
      const uint32_t height = checked_periods * spacing;
      BOOST_CHECK_LE(cadence::pending_vote_floor(height + cadence::snapshot_attestation_grace_blocks(spacing),
                                                 spacing),
                     height);
   }
}

// ---------------------------------------------------------------------------
// votesnaphash tests
// ---------------------------------------------------------------------------
/** A cadence update changes eligible heights immediately in the contract. */
BOOST_FIXTURE_TEST_CASE(votesnaphash_uses_updated_cadence, snapshot_voting_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(1));
   const uint32_t formerly_unscheduled_height = vote_block_num() - 1;
   const auto formerly_unscheduled = make_block_id(formerly_unscheduled_height);
   const auto snapshot_hash = make_snap_hash(1);
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("snapshot block is not a scheduled attestation height"),
                       votesnaphash("producer1"_n, formerly_unscheduled, snapshot_hash));
   // The height becomes its own cadence's first, so it is also inside the pending window.
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(1, formerly_unscheduled_height));
   BOOST_REQUIRE_EQUAL(success(),
                       votesnaphash("producer1"_n, formerly_unscheduled, snapshot_hash));
} FC_LOG_AND_RETHROW() }

/// A vote survives its producer leaving: votes are keyed by producer identity, so parking the producer
/// that cast one neither retracts it nor stops the tuple it belongs to from finalizing.
BOOST_FIXTURE_TEST_CASE(votesnaphash_keeps_the_vote_of_a_producer_that_parks, snapshot_voting_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(2));

   const auto block_num     = vote_block_num();
   const auto block_id      = make_block_id(block_num);
   const auto snapshot_hash = make_snap_hash(11);

   BOOST_REQUIRE_EQUAL(success(), votesnaphash("producer1"_n, block_id, snapshot_hash));
   BOOST_REQUIRE(getsnaphash(block_num).is_null());

   // producer1 parks after voting. Its vote is already recorded under the producer identity.
   BOOST_REQUIRE_EQUAL(success(), unregproducer("producer1"_n));
   BOOST_REQUIRE_EQUAL(1u, snapshot_vote_count());

   // The parked producer is refused while the tuple is still PENDING.
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("producer rank exceeds maximum for snapshot providers"),
                        votesnaphash("producer1"_n, block_id, snapshot_hash));

   // The parked producer's vote still counts toward K, so producer2 alone completes the quorum.
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("producer2"_n, block_id, snapshot_hash));
   BOOST_REQUIRE(!getsnaphash(block_num).is_null());

   // ...and once FINAL, where the tuple matches the record.
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("producer rank exceeds maximum for snapshot providers"),
                        votesnaphash("producer1"_n, block_id, snapshot_hash));
} FC_LOG_AND_RETHROW() }

/// A governance change applies to pending votes, and an exact retry can finalize the existing tuple.
BOOST_FIXTURE_TEST_CASE(votesnaphash_uses_current_fixed_k_for_pending_votes, snapshot_voting_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(3));

   const auto block_num = vote_block_num();
   const auto block_id = make_block_id(block_num);
   const auto snapshot_hash = make_snap_hash(10);
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("producer1"_n, block_id, snapshot_hash));
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("producer2"_n, block_id, snapshot_hash));
   BOOST_REQUIRE(getsnaphash(block_num).is_null());

   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(2));
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("producer1"_n, block_id, snapshot_hash));
   BOOST_REQUIRE(!getsnaphash(block_num).is_null());
} FC_LOG_AND_RETHROW() }

/// Every competing tuple at one height is measured against the same current governance-set K.
BOOST_FIXTURE_TEST_CASE(votesnaphash_uses_current_fixed_k_for_competing_tuples, snapshot_voting_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(3));

   const auto block_num = vote_block_num();
   const auto block_id_a = make_block_id(block_num);
   const auto block_id_b = make_block_id(block_num, 1);
   const auto hash_a = make_snap_hash(11);
   const auto hash_b = make_snap_hash(12);
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("producer1"_n, block_id_a, hash_a));
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("producer2"_n, block_id_b, hash_b));
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("producer3"_n, block_id_b, hash_b));
   BOOST_REQUIRE(getsnaphash(block_num).is_null());

   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(2));
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("producer2"_n, block_id_b, hash_b));
   BOOST_REQUIRE(!getsnaphash(block_num).is_null());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(votesnaphash_single_no_quorum, snapshot_voting_tester) { try {
   // Fixed K is two, so a single vote remains pending however many producers can vote.
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(2));
   produce_blocks();

   const auto block_num = vote_block_num();
   auto bid = make_block_id(block_num);
   auto shash = make_snap_hash(1);

   BOOST_REQUIRE_EQUAL(success(), votesnaphash("producer1"_n, bid, shash));

   // No attested record yet
   auto rec = getsnaphash(block_num);
   BOOST_REQUIRE_EQUAL(true, rec.is_null());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(votesnaphash_quorum_reached, snapshot_voting_tester) { try {
   // Fixed K is two, so the second distinct producer finalizes the tuple.
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(2));
   produce_blocks();

   const auto block_num = vote_block_num();
   auto bid = make_block_id(block_num);
   auto shash = make_snap_hash(1);

   BOOST_REQUIRE_EQUAL(success(), votesnaphash("producer1"_n, bid, shash));
   // One vote is below K=2, so the tuple is not attested yet.
   BOOST_REQUIRE_EQUAL(true, getsnaphash(block_num).is_null());

   BOOST_REQUIRE_EQUAL(success(), votesnaphash("producer2"_n, bid, shash));

   // K reached — the attested record should exist.
   auto rec = getsnaphash(block_num);
   BOOST_REQUIRE_EQUAL(false, rec.is_null());
   BOOST_REQUIRE_EQUAL(block_num, rec["block_num"].as_uint64());
} FC_LOG_AND_RETHROW() }

// Snapshot service is a SCORING factor, and the only per-producer history it can be scored from is
// this counter: the vote rows that name the voters are purged the moment a record finalizes, so
// without crediting at quorum there would be nothing left to score. Being able to vote costs
// nothing and signals nothing; reaching quorum does, so only the producers whose votes carried
// the record are credited.
BOOST_FIXTURE_TEST_CASE(votesnaphash_quorum_credits_voting_producers, snapshot_voting_tester) { try {
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
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("producer1"_n, bid, shash));
   BOOST_REQUIRE_EQUAL(0u, attestations_of("producer1"_n));

   BOOST_REQUIRE_EQUAL(success(), votesnaphash("producer2"_n, bid, shash));
   BOOST_REQUIRE(!getsnaphash(block_num).is_null());

   BOOST_REQUIRE_EQUAL(1u, attestations_of("producer1"_n));
   BOOST_REQUIRE_EQUAL(1u, attestations_of("producer2"_n));
   // producer3 could vote but never did, so it earned nothing.
   BOOST_REQUIRE_EQUAL(0u, attestations_of("producer3"_n));

   // The credit is a SCORING factor and it reaches the index at once: a higher composite is a
   // numerically LOWER key. producer3 earned nothing, so its key is untouched.
   BOOST_REQUIRE_LT(key_of("producer1"_n), key1);
   BOOST_REQUIRE_LT(key_of("producer2"_n), key2);
   BOOST_REQUIRE_EQUAL(key3, key_of("producer3"_n));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(votesnaphash_same_tuple_retry_is_idempotent, snapshot_voting_tester) { try {
   // Need 2 providers, min_providers=2 so single vote won't attest and purge
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(2));
   produce_blocks();

   const auto block_num = vote_block_num();
   auto bid = make_block_id(block_num);
   auto shash = make_snap_hash(1);

   BOOST_REQUIRE_EQUAL(success(), votesnaphash("producer1"_n, bid, shash));
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("producer1"_n, bid, shash));
   BOOST_REQUIRE(getsnaphash(block_num).is_null());
} FC_LOG_AND_RETHROW() }

/// An exact retry of a final tuple is a no-op while the producer holds its rank, and is refused once it parks.
BOOST_FIXTURE_TEST_CASE(votesnaphash_final_tuple_retry_is_idempotent, snapshot_voting_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(1));

   const auto block_num = vote_block_num();
   const auto block_id = make_block_id(block_num);
   const auto snapshot_hash = make_snap_hash(103);
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("producer1"_n, block_id, snapshot_hash));
   BOOST_REQUIRE(!getsnaphash(block_num).is_null());
   BOOST_REQUIRE_EQUAL(0u, snapshot_vote_count());

   BOOST_REQUIRE_EQUAL(success(), votesnaphash("producer1"_n, block_id, snapshot_hash));
   BOOST_REQUIRE_EQUAL(0u, snapshot_vote_count());

   BOOST_REQUIRE_EQUAL(success(), unregproducer("producer1"_n));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("producer rank exceeds maximum for snapshot providers"),
                        votesnaphash("producer1"_n, block_id, snapshot_hash));
} FC_LOG_AND_RETHROW() }

/// An account outside the rank cannot land a no-op vote on a final tuple, though a mismatch with the
/// record is still reported to it as a disagreement.
BOOST_FIXTURE_TEST_CASE(votesnaphash_final_tuple_rejects_an_unranked_account, snapshot_voting_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(1));

   const auto block_num     = vote_block_num();
   const auto block_id      = make_block_id(block_num);
   const auto snapshot_hash = make_snap_hash(106);
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("producer1"_n, block_id, snapshot_hash));
   BOOST_REQUIRE(!getsnaphash(block_num).is_null());

   BOOST_REQUIRE_EQUAL(wasm_assert_msg("producer rank exceeds maximum for snapshot providers"),
                        votesnaphash("alice1111111"_n, block_id, snapshot_hash));
   // snap_hash_disagreement_error = 9001 (defined in snapshot_attest.hpp)
   BOOST_REQUIRE_EQUAL(wasm_assert_code(9001),
                        votesnaphash("alice1111111"_n, block_id, make_snap_hash(107)));
} FC_LOG_AND_RETHROW() }

/// A ranked producer that did not vote before quorum may still vote the final tuple: a no-op that
/// writes no vote row and earns no attestation credit.
BOOST_FIXTURE_TEST_CASE(votesnaphash_final_tuple_accepts_late_producer_vote, snapshot_voting_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(1));

   const auto block_num     = vote_block_num();
   const auto block_id      = make_block_id(block_num);
   const auto snapshot_hash = make_snap_hash(108);
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("producer1"_n, block_id, snapshot_hash));
   BOOST_REQUIRE(!getsnaphash(block_num).is_null());

   BOOST_REQUIRE_EQUAL(success(), votesnaphash("producer2"_n, block_id, snapshot_hash));
   BOOST_REQUIRE_EQUAL(0u, snapshot_vote_count());
   BOOST_REQUIRE_EQUAL(0u, get_producer_info("producer2"_n)["snapshot_attestations"].as<uint32_t>());
} FC_LOG_AND_RETHROW() }

/// Standing gates only a vote that can count toward a quorum, so a ranked producer whose bond fell below a raised
/// collateral minimum may still vote a final tuple.
BOOST_FIXTURE_TEST_CASE(votesnaphash_final_tuple_skips_standing_check, snapshot_bonded_voting_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(1));
   set_producer_min_bond(producer_min_bond * 2);

   const auto block_num     = vote_block_num();
   const auto block_id      = make_block_id(block_num);
   const auto snapshot_hash = make_snap_hash(109);
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("producer1"_n, block_id, snapshot_hash));
   BOOST_REQUIRE(!getsnaphash(block_num).is_null());

   BOOST_REQUIRE_EQUAL(success(), votesnaphash(bonded_producer, block_id, snapshot_hash));
   BOOST_REQUIRE_EQUAL(0u, snapshot_vote_count());
} FC_LOG_AND_RETHROW() }

/// Voting is disabled until governance explicitly chooses a nonzero fixed K.
BOOST_FIXTURE_TEST_CASE(votesnaphash_rejects_unconfigured_quorum, snapshot_voting_tester) { try {

   const auto block_num = vote_block_num();
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("snapshot attestation configuration has not been set"),
                       votesnaphash("producer1"_n, make_block_id(block_num), make_snap_hash(101)));
   BOOST_REQUIRE(getsnaphash(block_num).is_null());
} FC_LOG_AND_RETHROW() }

/// A producer cannot pre-attest a tuple for a block height the chain has not reached.
///
/// The candidate is computed from the live head so this remains a future-height test even when
/// fixture setup grows or the configured test cadence changes.
BOOST_FIXTURE_TEST_CASE(votesnaphash_rejects_future_block_height, snapshot_attest_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(1));
   produce_block();

   const uint32_t current_block_num = control->head().block_num();
   const uint32_t future_block_num =
      (current_block_num / test_block_spacing + 2) * test_block_spacing;
   BOOST_REQUIRE(control->head().block_num() < future_block_num);
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("snapshot block cannot be in the future"),
                       votesnaphash("producer1"_n,
                                    make_block_id(future_block_num),
                                    make_snap_hash(102)));
} FC_LOG_AND_RETHROW() }

/// A manual snapshot height cannot enter the bounded on-chain tally space.
///
/// The configured-cadence check precedes the future-height check, so an off-cadence height is
/// rejected for being off-cadence no matter where the head sits.
BOOST_FIXTURE_TEST_CASE(votesnaphash_rejects_unscheduled_block_height, snapshot_attest_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(1));

   const uint32_t unscheduled_block_num = scheduled_height(1) + 1;
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("snapshot block is not a scheduled attestation height"),
                       votesnaphash("producer1"_n,
                                    make_block_id(unscheduled_block_num),
                                    make_snap_hash(104)));
} FC_LOG_AND_RETHROW() }

/// The contract accepts the exact configured boundary and rejects both adjacent heights.
BOOST_FIXTURE_TEST_CASE(votesnaphash_enforces_configured_cadence_edges,
                        snapshot_multi_height_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(2));
   BOOST_REQUIRE_GE(vote_block_num(), test_block_spacing + 1);

   const auto snapshot_hash = make_snap_hash(105);
   BOOST_REQUIRE_EQUAL(
      wasm_assert_msg("snapshot block is not a scheduled attestation height"),
      votesnaphash("producer1"_n, make_block_id(test_block_spacing - 1), snapshot_hash));
   BOOST_REQUIRE_EQUAL(success(),
                       votesnaphash("producer1"_n,
                                    make_block_id(test_block_spacing), snapshot_hash));
   BOOST_REQUIRE_EQUAL(
      wasm_assert_msg("snapshot block is not a scheduled attestation height"),
      votesnaphash("producer1"_n, make_block_id(test_block_spacing + 1), snapshot_hash));
} FC_LOG_AND_RETHROW() }

// ---------------------------------------------------------------------------
// fixed-K tests
// ---------------------------------------------------------------------------
BOOST_FIXTURE_TEST_CASE(fixed_k_is_reached_by_the_second_producer, snapshot_voting_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(2));
   produce_blocks();

   const auto block_num = vote_block_num();
   auto bid = make_block_id(block_num);
   auto shash = make_snap_hash(2);

   BOOST_REQUIRE_EQUAL(success(), votesnaphash("producer1"_n, bid, shash));
   BOOST_REQUIRE(getsnaphash(block_num).is_null());
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("producer2"_n, bid, shash));
   BOOST_REQUIRE(!getsnaphash(block_num).is_null());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(fixed_k_is_independent_of_the_ranked_producer_count, snapshot_voting_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(4));
   produce_blocks();

   const auto block_num = vote_block_num();
   auto bid = make_block_id(block_num);
   auto shash = make_snap_hash(3);

   // Three votes do not meet K even though five producers can vote.
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("producer1"_n, bid, shash));
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("producer2"_n, bid, shash));
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("producer3"_n, bid, shash));
   BOOST_REQUIRE_EQUAL(true, getsnaphash(block_num).is_null());

   // The fourth distinct producer reaches the governance-set K.
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("producer4"_n, bid, shash));
   BOOST_REQUIRE_EQUAL(false, getsnaphash(block_num).is_null());
} FC_LOG_AND_RETHROW() }

// ---------------------------------------------------------------------------
// disagreement tests
// ---------------------------------------------------------------------------
BOOST_FIXTURE_TEST_CASE(disagreement_detection, snapshot_voting_tester) { try {
   // K=1 finalizes on the first vote.
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(1));
   produce_blocks();

   const auto block_num = vote_block_num();
   auto bid = make_block_id(block_num);
   auto shash = make_snap_hash(4);

   // Attest with one vote because K=1.
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("producer1"_n, bid, shash));
   BOOST_REQUIRE_EQUAL(false, getsnaphash(block_num).is_null());

   // A second producer votes a different hash for the same block — disagreement
   auto bad_hash = make_snap_hash(999);
   // snap_hash_disagreement_error = 9001 (defined in snapshot_attest.hpp)
   BOOST_REQUIRE_EQUAL(wasm_assert_code(9001),
                        votesnaphash("producer2"_n, bid, bad_hash));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(blockid_mismatch_votes_not_aggregated, snapshot_voting_tester) { try {
   // Three producers vote, but the fixed K remains two.
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(2));
   produce_blocks();

   // Same height and same snapshot hash, but different block ids (competing forks).
   const auto block_num = vote_block_num();
   auto bid_a  = make_block_id(block_num);
   auto bid_b  = make_block_id(block_num, 1);
   auto shash  = make_snap_hash(8);

   BOOST_REQUIRE_EQUAL(success(), votesnaphash("producer1"_n, bid_a, shash));
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("producer2"_n, bid_b, shash));

   // The two votes agree on the hash but not the block id, so they must NOT
   // jointly reach K=2.
   BOOST_REQUIRE_EQUAL(true, getsnaphash(block_num).is_null());

   // A distinct producer may join the first tuple to reach K.
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("producer3"_n, bid_a, shash));
   auto rec = getsnaphash(block_num);
   BOOST_REQUIRE_EQUAL(false, rec.is_null());
   BOOST_REQUIRE_EQUAL(bid_a.str(), rec["block_id"].as_string());
   BOOST_REQUIRE_EQUAL(shash.str(), rec["snapshot_hash"].as_string());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(votesnaphash_rejects_producer_equivocation_across_hashes, snapshot_voting_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(2));

   const auto bid = make_block_id(vote_block_num());
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("producer1"_n, bid, make_snap_hash(80)));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("producer already voted a different snapshot tuple for this height"),
                        votesnaphash("producer1"_n, bid, make_snap_hash(81)));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(votesnaphash_reports_disagreement_before_eligibility_failure, snapshot_voting_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(1));

   const auto bid = make_block_id(vote_block_num());
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("producer1"_n, bid, make_snap_hash(83)));
   // Losing operator standing drops producer2 out of the rank; the disagreement is still reported first.
   terminate_operator("producer2"_n);
   BOOST_REQUIRE_EQUAL(wasm_assert_code(9001),
                        votesnaphash("producer2"_n, bid, make_snap_hash(84)));
} FC_LOG_AND_RETHROW() }

/// Rank is read live on every vote: a producer whose sysio.opreg row left ACTIVE is no longer schedulable, so it
/// drops out of the rank and cannot vote.
BOOST_FIXTURE_TEST_CASE(votesnaphash_rejects_producer_without_operator_standing, snapshot_voting_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(1));
   terminate_operator("producer1"_n);

   const auto block_num = vote_block_num();
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("producer rank exceeds maximum for snapshot providers"),
                        votesnaphash("producer1"_n, make_block_id(block_num), make_snap_hash(85)));
   BOOST_REQUIRE(getsnaphash(block_num).is_null());
   BOOST_REQUIRE_EQUAL(0u, snapshot_vote_count());
} FC_LOG_AND_RETHROW() }

/// The standing check gates new votes only: a vote accepted while the producer was bonded still counts
/// toward K after its operator row leaves ACTIVE.
BOOST_FIXTURE_TEST_CASE(votesnaphash_keeps_votes_cast_before_operator_lost_standing, snapshot_voting_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(2));

   const auto block_num     = vote_block_num();
   const auto block_id      = make_block_id(block_num);
   const auto snapshot_hash = make_snap_hash(86);
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("producer1"_n, block_id, snapshot_hash));
   terminate_operator("producer1"_n);

   // Even an exact retry of its own pending vote is refused once the producer drops out of the rank.
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("producer rank exceeds maximum for snapshot providers"),
                        votesnaphash("producer1"_n, block_id, snapshot_hash));
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("producer2"_n, block_id, snapshot_hash));
   BOOST_REQUIRE(!getsnaphash(block_num).is_null());
} FC_LOG_AND_RETHROW() }

/// A raised collateral minimum reaches the vote gate without any sweep. `sysio.opreg::setconfig` re-evaluates no
/// stored status, so a producer left short by the raise is still ACTIVE in opreg; the gate measures its bond against
/// the live requirement instead. Bootstrapped producers are exempt, and restoring the bond restores the vote.
BOOST_FIXTURE_TEST_CASE(votesnaphash_rejects_producer_below_raised_collateral_minimum,
                        snapshot_bonded_voting_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(2));

   set_producer_min_bond(producer_min_bond * 2);
   BOOST_REQUIRE_EQUAL(sysio::opp::types::OperatorStatus::OPERATOR_STATUS_ACTIVE, opreg_status(bonded_producer));

   const auto block_num     = vote_block_num();
   const auto block_id      = make_block_id(block_num);
   const auto snapshot_hash = make_snap_hash(87);
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("producer does not meet the live PRODUCER operator requirements"),
                       votesnaphash(bonded_producer, block_id, snapshot_hash));
   BOOST_REQUIRE_EQUAL(0u, snapshot_vote_count());

   BOOST_REQUIRE_EQUAL(success(), votesnaphash("producer1"_n, block_id, snapshot_hash));
   BOOST_REQUIRE(getsnaphash(block_num).is_null());

   // Topping the bond up to the raised minimum restores the vote, which completes the quorum.
   credit_producer_bond(bonded_producer, producer_min_bond);
   BOOST_REQUIRE_EQUAL(success(), votesnaphash(bonded_producer, block_id, snapshot_hash));
   BOOST_REQUIRE(!getsnaphash(block_num).is_null());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(record_blockid_disagreement, snapshot_voting_tester) { try {
   // K=1 finalizes on the first vote.
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(1));
   produce_blocks();

   const auto block_num = vote_block_num();
   auto bid_a = make_block_id(block_num);
   auto bid_b = make_block_id(block_num, 1);
   auto shash = make_snap_hash(9);

   // Attest with one vote because K=1.
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("producer1"_n, bid_a, shash));
   BOOST_REQUIRE_EQUAL(false, getsnaphash(block_num).is_null());

   // Same snapshot hash under a different block id disagrees with the attested record
   BOOST_REQUIRE_EQUAL(wasm_assert_code(9001),
                        votesnaphash("producer2"_n, bid_b, shash));
} FC_LOG_AND_RETHROW() }

// ---------------------------------------------------------------------------
// purging tests
// ---------------------------------------------------------------------------
/// Votes at different scheduled heights coexist until a final record purges older pending rows.
BOOST_FIXTURE_TEST_CASE(votesnaphash_keeps_scheduled_heights_independent_until_finalization, snapshot_multi_height_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(2));

   // Both heights are already behind the head -- the fixture built two cadence periods up front.
   const uint32_t newer_block_num = vote_block_num();
   const uint32_t older_block_num = newer_block_num - test_block_spacing;
   const auto older_block_id = make_block_id(older_block_num);
   const auto older_hash = make_snap_hash(5);
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("producer1"_n, older_block_id, older_hash));

   const auto newer_block_id = make_block_id(newer_block_num);
   const auto newer_hash = make_snap_hash(6);
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("producer1"_n, newer_block_id, newer_hash));
   BOOST_REQUIRE_EQUAL(2u, snapshot_vote_count());

   BOOST_REQUIRE_EQUAL(success(), votesnaphash("producer2"_n, older_block_id, older_hash));
   BOOST_REQUIRE(!getsnaphash(older_block_num).is_null());
   BOOST_REQUIRE_EQUAL(1u, snapshot_vote_count());

   BOOST_REQUIRE_EQUAL(success(), votesnaphash("producer2"_n, newer_block_id, newer_hash));
   BOOST_REQUIRE(!getsnaphash(newer_block_num).is_null());
   BOOST_REQUIRE_EQUAL(0u, snapshot_vote_count());
} FC_LOG_AND_RETHROW() }

/// A newer finalization permanently closes older heights whose unfinished rows were purged.
BOOST_FIXTURE_TEST_CASE(votesnaphash_rejects_reopening_purged_historical_height, snapshot_multi_height_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(2));

   // Both heights are already behind the head -- the fixture built two cadence periods up front.
   const uint32_t newer_block_num = vote_block_num();
   const uint32_t older_block_num = newer_block_num - test_block_spacing;
   const auto older_block_id = make_block_id(older_block_num);
   const auto older_hash = make_snap_hash(25);
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("producer1"_n, older_block_id, older_hash));

   const auto newer_block_id = make_block_id(newer_block_num);
   const auto newer_hash = make_snap_hash(26);
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("producer1"_n, newer_block_id, newer_hash));
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("producer2"_n, newer_block_id, newer_hash));
   BOOST_REQUIRE(!getsnaphash(newer_block_num).is_null());
   BOOST_REQUIRE(getsnaphash(older_block_num).is_null());
   BOOST_REQUIRE_EQUAL(0u, snapshot_vote_count());

   BOOST_REQUIRE_EQUAL(wasm_assert_msg("snapshot block is older than latest attested snapshot height"),
                       votesnaphash("producer2"_n, older_block_id, older_hash));
   BOOST_REQUIRE_EQUAL(0u, snapshot_vote_count());
} FC_LOG_AND_RETHROW() }

/// Without quorum, only the last max_pending_vote_heights scheduled heights keep rows: each vote purges
/// the rows that fell below the window, and any vote below it, even an exact retry, is refused.
BOOST_FIXTURE_TEST_CASE(votesnaphash_bounds_pending_votes_without_quorum, snapshot_voting_tester) { try {
   namespace cadence = sysiosystem::snapshot_attestation;
   constexpr uint32_t window = cadence::max_pending_vote_heights;
   // Every successful push produces a block, so one retry plus a round of votes must fit in a period.
   constexpr uint32_t spacing = 10;
   constexpr uint32_t rounds  = window + 2;
   const auto producers = fixture_producers();
   BOOST_REQUIRE_LT(producers.size() + 1, spacing);
   // Each producer votes its own tuple, so none reaches K and every height stays pending.
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(2, spacing));
   align_head_to(spacing);

   const auto tuple_hash = [&](uint32_t round, size_t voter) {
      return make_snap_hash(round * static_cast<uint32_t>(producers.size()) + static_cast<uint32_t>(voter));
   };
   std::vector<uint32_t> heights;
   for (uint32_t round = 0; round < rounds; ++round) {
      const uint32_t height = control->head().block_num();
      BOOST_REQUIRE(cadence::is_scheduled_block(height, spacing));
      heights.push_back(height);

      if (round >= window) {
         // The closed height's rows still exist, but the window refuses them before the tally.
         const uint32_t closed_round = round - window;
         BOOST_REQUIRE_EQUAL(wasm_assert_msg("snapshot block is older than the pending vote window"),
                             votesnaphash(producers[0], make_block_id(heights[closed_round]),
                                          tuple_hash(closed_round, 0)));
         // The oldest open height still takes an idempotent retry, which purges the closed rows.
         const uint32_t oldest_open_round = closed_round + 1;
         BOOST_REQUIRE_EQUAL(success(), votesnaphash(producers[0], make_block_id(heights[oldest_open_round]),
                                                     tuple_hash(oldest_open_round, 0)));
      }
      for (size_t i = 0; i < producers.size(); ++i) {
         BOOST_REQUIRE_EQUAL(success(), votesnaphash(producers[i], make_block_id(height), tuple_hash(round, i)));
      }
      BOOST_REQUIRE(getsnaphash(height).is_null());

      const uint32_t open_heights = std::min(round + 1, window);
      BOOST_REQUIRE_EQUAL(producers.size() * open_heights, snapshot_vote_count());
      BOOST_REQUIRE_LE(snapshot_vote_count(), max_snapshot_providers * window);
      produce_blocks(height + spacing - control->head().block_num());
   }

   // Shrinking the cadence strands the old heights below the narrower window; the next vote purges them.
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(2, 1));
   const uint32_t height = control->head().block_num();
   BOOST_REQUIRE_EQUAL(success(), votesnaphash(producers[0], make_block_id(height), tuple_hash(rounds, 0)));
   BOOST_REQUIRE_EQUAL(1u, snapshot_vote_count());
} FC_LOG_AND_RETHROW() }

/// The window gates only the pending tally: a final record still answers late votes below it, and
/// rows it closes stay until an open vote purges them.
BOOST_FIXTURE_TEST_CASE(votesnaphash_pending_window_preserves_final_record_checks, snapshot_voting_tester) { try {
   namespace cadence = sysiosystem::snapshot_attestation;
   constexpr uint32_t spacing = 2;
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(1, spacing));

   const uint32_t final_height = control->head().block_num() / spacing * spacing;
   const auto     final_id     = make_block_id(final_height);
   const auto     final_hash   = make_snap_hash(120);
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("producer1"_n, final_id, final_hash));
   BOOST_REQUIRE(!getsnaphash(final_height).is_null());

   // A later height that never reaches K.
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(2, spacing));
   const uint32_t pending_height = control->head().block_num() / spacing * spacing;
   BOOST_REQUIRE_GT(pending_height, final_height);
   const auto pending_id   = make_block_id(pending_height);
   const auto pending_hash = make_snap_hash(121);
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("producer1"_n, pending_id, pending_hash));
   BOOST_REQUIRE_EQUAL(1u, snapshot_vote_count());

   produce_blocks(cadence::max_pending_vote_heights * spacing);
   BOOST_REQUIRE_GT(cadence::pending_vote_floor(control->head().block_num() + 1, spacing), pending_height);

   // A ranked producer's matching late vote is still a no-op, and a mismatch still reports 9001.
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("producer2"_n, final_id, final_hash));
   BOOST_REQUIRE_EQUAL(wasm_assert_code(9001), votesnaphash("producer2"_n, final_id, make_snap_hash(122)));

   // The unfinished height is closed to new votes and to retries alike.
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("snapshot block is older than the pending vote window"),
                       votesnaphash("producer2"_n, pending_id, pending_hash));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("snapshot block is older than the pending vote window"),
                       votesnaphash("producer1"_n, pending_id, pending_hash));
   BOOST_REQUIRE_EQUAL(1u, snapshot_vote_count());

   // An open vote purges the closed row and leaves only its own.
   const uint32_t open_height = control->head().block_num() / spacing * spacing;
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("producer2"_n, make_block_id(open_height), make_snap_hash(123)));
   BOOST_REQUIRE_EQUAL(1u, snapshot_vote_count());
} FC_LOG_AND_RETHROW() }

/// A producer parking and returning cannot erase pending votes cast by other producers.
BOOST_FIXTURE_TEST_CASE(votesnaphash_producer_churn_preserves_other_votes, snapshot_voting_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(3));

   const auto block_num = vote_block_num();
   const auto block_id = make_block_id(block_num);
   const auto snapshot_hash = make_snap_hash(114);
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("producer1"_n, block_id, snapshot_hash));
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("producer2"_n, block_id, snapshot_hash));

   BOOST_REQUIRE_EQUAL(success(), unregproducer("producer3"_n));
   regproducer("producer3"_n);
   produce_block();
   BOOST_REQUIRE_EQUAL(1u, snapshot_vote_count());

   BOOST_REQUIRE_EQUAL(success(), votesnaphash("producer4"_n, block_id, snapshot_hash));
   BOOST_REQUIRE(!getsnaphash(block_num).is_null());
} FC_LOG_AND_RETHROW() }

// ---------------------------------------------------------------------------
// RAM billing tests
// ---------------------------------------------------------------------------
/// Vote rows bill sysio, so producers holding only what `sysio.roa::newuser` grants can vote without
/// their RAM usage moving.
BOOST_FIXTURE_TEST_CASE(snapshot_rows_bill_the_system_account, snapshot_voting_tester) { try {
   const auto producers = fixture_producers();
   constexpr uint32_t voters = 3;
   const std::vector<account_name> limited(producers.begin(), producers.begin() + voters);
   for (const auto& account : limited) {
      limit_to_new_user_resources(account);
   }
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(voters));

   std::map<account_name, int64_t> usage_before;
   for (const auto& account : limited) {
      usage_before[account] = ram_usage(account);
   }
   const auto require_unbilled = [&]() {
      for (const auto& account : limited) {
         BOOST_REQUIRE_EQUAL(usage_before[account], ram_usage(account));
      }
   };

   const auto    block_num     = vote_block_num();
   const auto    block_id      = make_block_id(block_num);
   const auto    snapshot_hash = make_snap_hash(130);
   const int64_t system_before = ram_usage(config::system_account_name);
   // The first vote creates the pending row and the second grows it; neither voter pays.
   BOOST_REQUIRE_EQUAL(success(), votesnaphash(producers[0], block_id, snapshot_hash));
   BOOST_REQUIRE_EQUAL(success(), votesnaphash(producers[1], block_id, snapshot_hash));
   BOOST_REQUIRE_EQUAL(1u, snapshot_vote_count());
   require_unbilled();
   BOOST_REQUIRE_GT(ram_usage(config::system_account_name), system_before);

   BOOST_REQUIRE_EQUAL(success(), votesnaphash(producers[2], block_id, snapshot_hash));
   BOOST_REQUIRE(!getsnaphash(block_num).is_null());
   require_unbilled();
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

/// Governance owns the fixed-K tradeoff; K=1 deliberately permits one of many producers to attest.
BOOST_FIXTURE_TEST_CASE(votesnaphash_honors_governance_fixed_k, snapshot_voting_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(1));

   const auto block_num = vote_block_num();
   const auto bid  = make_block_id(block_num);
   const auto hash = make_snap_hash(1);

   BOOST_REQUIRE_EQUAL(success(), votesnaphash("producer1"_n, bid, hash));
   BOOST_REQUIRE(!getsnaphash(block_num).is_null());
} FC_LOG_AND_RETHROW() }

/// Votes count per producer, whichever of its permissions signs: a second vote through the snapshot key is an
/// exact retry, not a second voter.
BOOST_FIXTURE_TEST_CASE(votesnaphash_counts_a_producer_once_whatever_permission_signs, snapshot_voting_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(2));
   const auto snapshot_key = get_private_key("producer1"_n, snapshot_permission.to_string());
   link_snapshot_permission("producer1"_n, snapshot_key.get_public_key());

   const auto block_num = vote_block_num();
   const auto block_id = make_block_id(block_num);
   const auto snapshot_hash = make_snap_hash(7);
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("producer1"_n, block_id, snapshot_hash));
   BOOST_REQUIRE_EQUAL(1u, snapshot_vote_count());

   BOOST_REQUIRE_EQUAL(success(), votesnaphash_signed_as("producer1"_n, snapshot_permission, snapshot_key,
                                                         block_id, snapshot_hash));
   BOOST_REQUIRE_EQUAL(1u, snapshot_vote_count());
   BOOST_REQUIRE(getsnaphash(block_num).is_null());

   BOOST_REQUIRE_EQUAL(success(), votesnaphash("producer2"_n, block_id, snapshot_hash));
   BOOST_REQUIRE(!getsnaphash(block_num).is_null());
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()

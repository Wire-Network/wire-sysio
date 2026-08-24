#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#include <boost/test/unit_test.hpp>
#pragma GCC diagnostic pop

#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/resource_limits.hpp>
#include <sysio/testing/tester.hpp>

#include <fc/exception/exception.hpp>
#include <fc/variant_object.hpp>

#include "sysio.system_tester.hpp"

using namespace sysio_system;

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------
class snapshot_attest_tester : public sysio_system_tester {
public:
   snapshot_attest_tester() : sysio_system_tester(setup_level::full) {
      produce_blocks();

      // Create producer accounts (setup_producer_accounts gives them resources)
      const std::vector<account_name> producers = {
         "producer1"_n, "producer2"_n, "producer3"_n,
         "producer4"_n, "producer5"_n
      };
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

      // Set ranks for producers (all within max_snap_provider_rank = 30)
      for (uint32_t i = 0; i < producers.size(); ++i) {
         BOOST_REQUIRE_EQUAL(success(), setrank(producers[i], i + 1));
      }
      produce_blocks();
   }

   /// Register a snapshot provider.
   action_result regsnapprov(name producer, name snap_account) {
      return push_action(producer, "regsnapprov"_n, mvo()
         ("producer", producer)
         ("snap_account", snap_account));
   }

   /// Unregister a snapshot provider.
   action_result delsnapprov(name account) {
      return push_action(account, "delsnapprov"_n, mvo()
         ("account", account));
   }

   /// Deactivate a producer while retaining its producer-table row.
   action_result unregproducer(name producer) {
      return push_action(producer, "unregprod"_n, mvo()
         ("producer", producer));
   }

   /// Publish a producer-key schedule through the rank-assignment action path.
   action_result set_producer_schedule(const std::vector<name>& producers) {
      std::vector<fc::variant> schedule;
      schedule.reserve(producers.size());
      for (const auto producer : producers) {
         schedule.push_back(mvo()
            ("producer_name", producer)
            ("block_signing_key", get_public_key(producer, "active")));
      }
      return push_action(config::system_account_name, "setprodkeys"_n, mvo()
         ("schedule", schedule));
   }

   /// Remove a producer through the governance-authorized lifecycle action.
   action_result remove_producer(name producer) {
      return push_action(config::system_account_name, "rmvproducer"_n, mvo()
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
   action_result setsnpcfg(uint32_t min_providers, uint32_t threshold_pct) {
      return push_action(config::system_account_name, "setsnpcfg"_n, mvo()
         ("min_providers", min_providers)
         ("threshold_pct", threshold_pct));
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

   /// Count pending snapshot tuple rows across the bounded live committee.
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

   /// Return a final block height at or before the current head for synthetic vote tuples.
   uint32_t vote_block_num(uint32_t blocks_before_head = 0) const {
      return control->head().block_num() - blocks_before_head;
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
};

// ===========================================================================
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

BOOST_FIXTURE_TEST_CASE(regsnapprov_rejects_provider_beyond_maximum, snapshot_attest_tester) { try {
   constexpr uint32_t max_registered_snapshot_providers = 30;
   constexpr uint32_t fixture_snapshot_providers        = 5;
   constexpr uint32_t additional_providers_to_fill_cap =
      max_registered_snapshot_providers - fixture_snapshot_providers;

   // The fixture provides five producers. Add 26 producers so 25 can fill the
   // remaining slots and the final, rank-eligible producer can exercise the rejection path.
   const std::vector<account_name> capacity_producers = {
      "capprova"_n, "capprovb"_n, "capprovc"_n, "capprovd"_n, "capprove"_n,
      "capprovf"_n, "capprovg"_n, "capprovh"_n, "capprovi"_n, "capprovj"_n,
      "capprovk"_n, "capprovl"_n, "capprovm"_n, "capprovn"_n, "capprovo"_n,
      "capprovp"_n, "capprovq"_n, "capprovr"_n, "capprovs"_n, "capprovt"_n,
      "capprovu"_n, "capprovv"_n, "capprovw"_n, "capprovx"_n, "capprovy"_n,
      "capprovz"_n,
   };
   BOOST_REQUIRE_EQUAL(additional_providers_to_fill_cap + 1, capacity_producers.size());

   setup_producer_accounts(capacity_producers);
   produce_blocks();
   for (const auto& producer : capacity_producers) {
      regproducer(producer);
   }
   produce_blocks();
   for (uint32_t index = 0; index < capacity_producers.size(); ++index) {
      const uint32_t rank = index < additional_providers_to_fill_cap
         ? fixture_snapshot_providers + index + 1
         : max_registered_snapshot_providers;
      BOOST_REQUIRE_EQUAL(success(), setrank(capacity_producers[index], rank));
   }
   produce_blocks();

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

   BOOST_REQUIRE_EQUAL(wasm_assert_msg("maximum registered snapshot providers reached"),
                        regsnapprov(capacity_producers.back(), capacity_producers.back()));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(regsnapprov_duplicate_rejected, snapshot_attest_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));

   // Same snap_account should fail
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("snap_account is already registered as a provider"),
                        regsnapprov("producer1"_n, "snapprov1"_n));

   // Same producer different snap_account should fail
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("producer already has a registered snapshot provider"),
                        regsnapprov("producer1"_n, "snapprov2"_n));
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
   BOOST_REQUIRE_EQUAL(success(), setrank("highrank"_n, 31));
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
// delsnapprov tests
// ---------------------------------------------------------------------------
BOOST_FIXTURE_TEST_CASE(delsnapprov_by_snap_account, snapshot_attest_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), delsnapprov("snapprov1"_n));

   auto prov = get_snap_provider("snapprov1"_n);
   BOOST_REQUIRE_EQUAL(true, prov.is_null());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(delsnapprov_by_producer, snapshot_attest_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), delsnapprov("producer1"_n));

   auto prov = get_snap_provider("snapprov1"_n);
   BOOST_REQUIRE_EQUAL(true, prov.is_null());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(delsnapprov_not_found, snapshot_attest_tester) { try {
   BOOST_REQUIRE_EQUAL(
      wasm_assert_msg("account is not registered as a snapshot provider or producer"),
      delsnapprov("snapprov1"_n));
} FC_LOG_AND_RETHROW() }

// ---------------------------------------------------------------------------
// setsnpcfg tests
// ---------------------------------------------------------------------------
BOOST_FIXTURE_TEST_CASE(setsnpcfg_basic, snapshot_attest_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(3, 80));

   // Only sysio can call — producer1 should fail
   BOOST_REQUIRE_EQUAL(error("missing authority of sysio"),
                        push_action("producer1"_n, "setsnpcfg"_n, mvo()
                           ("min_providers", 2)
                           ("threshold_pct", 50)));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(setsnpcfg_validation, snapshot_attest_tester) { try {
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("threshold_pct must be between 1 and 100"),
                        setsnpcfg(1, 0));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("threshold_pct must be between 1 and 100"),
                        setsnpcfg(1, 101));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("min_providers must be at least 1"),
                        setsnpcfg(0, 67));
   // min_providers cannot exceed the provider-table ceiling (max_snap_provider_rank
   // == 30): above it, quorum is unreachable no matter how many providers register.
   // The boundary value is accepted; one past it is rejected.
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(30, 80));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("min_providers exceeds the maximum registrable providers"),
                        setsnpcfg(31, 80));
} FC_LOG_AND_RETHROW() }

// ---------------------------------------------------------------------------
// votesnaphash tests
// ---------------------------------------------------------------------------
BOOST_FIXTURE_TEST_CASE(votesnaphash_unregistered, snapshot_attest_tester) { try {
   auto bid = make_block_id(vote_block_num());
   auto shash = make_snap_hash(1);
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("snap_account is not a registered snapshot provider"),
                        votesnaphash("snapprov1"_n, bid, shash));
} FC_LOG_AND_RETHROW() }

/// CertiK WNS-17 / WIRE-350: unregprod removes the provider mapping with voting authority.
BOOST_FIXTURE_TEST_CASE(votesnaphash_rejects_provider_after_producer_unregistered, snapshot_attest_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), unregproducer("producer1"_n));

   const auto block_num = vote_block_num();
   const auto bid = make_block_id(block_num);
   const auto hash = make_snap_hash(9);
   BOOST_REQUIRE(get_snap_provider("snapprov1"_n).is_null());
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("snap_account is not a registered snapshot provider"),
                        votesnaphash("snapprov1"_n, bid, hash));
   BOOST_REQUIRE(getsnaphash(block_num).is_null());
} FC_LOG_AND_RETHROW() }

/// A rank demotion removes the provider mapping at the rank-mutation hook.
BOOST_FIXTURE_TEST_CASE(votesnaphash_rejects_provider_after_producer_rank_demotion, snapshot_attest_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), setrank("producer1"_n, 31));

   const auto block_num = vote_block_num();
   const auto bid = make_block_id(block_num);
   const auto hash = make_snap_hash(10);
   BOOST_REQUIRE(get_snap_provider("snapprov1"_n).is_null());
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("snap_account is not a registered snapshot provider"),
                        votesnaphash("snapprov1"_n, bid, hash));
   BOOST_REQUIRE(getsnaphash(block_num).is_null());
} FC_LOG_AND_RETHROW() }

/// Automatic schedule churn reconciles mappings after assign_producer_ranks demotes a producer.
BOOST_FIXTURE_TEST_CASE(votesnaphash_reconciles_assigned_schedule_rank_demotion, snapshot_attest_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), setrank("producer3"_n, 10));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer3"_n, "snapprov3"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(2, 67));

   const auto block_num = vote_block_num();
   BOOST_REQUIRE_EQUAL(success(), votesnaphash(
      "snapprov3"_n, make_block_id(block_num), make_snap_hash(110)));
   BOOST_REQUIRE_EQUAL(1u, snapshot_vote_count());

   // Excluding rank-10 producer3 adds max_producers (21), making its rank 31.
   BOOST_REQUIRE_EQUAL(success(), set_producer_schedule({"producer1"_n, "producer2"_n}));
   BOOST_REQUIRE(get_snap_provider("snapprov3"_n).is_null());
   BOOST_REQUIRE_EQUAL(0u, snapshot_vote_count());
} FC_LOG_AND_RETHROW() }

/// Governance producer removal reconciles the delegated mapping and its pending vote.
BOOST_FIXTURE_TEST_CASE(votesnaphash_reconciles_governance_producer_removal, snapshot_attest_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer2"_n, "snapprov2"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(2, 67));

   const auto block_num = vote_block_num();
   BOOST_REQUIRE_EQUAL(success(), votesnaphash(
      "snapprov2"_n, make_block_id(block_num), make_snap_hash(111)));
   BOOST_REQUIRE_EQUAL(1u, snapshot_vote_count());

   BOOST_REQUIRE_EQUAL(success(), remove_producer("producer2"_n));
   BOOST_REQUIRE(get_snap_provider("snapprov2"_n).is_null());
   BOOST_REQUIRE_EQUAL(0u, snapshot_vote_count());
} FC_LOG_AND_RETHROW() }

/// Quorum numerator and denominator both use the remaining live registration set.
BOOST_FIXTURE_TEST_CASE(votesnaphash_uses_live_provider_set_after_unregistration, snapshot_attest_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer2"_n, "snapprov2"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer3"_n, "snapprov3"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(2, 50));
   BOOST_REQUIRE_EQUAL(success(), unregproducer("producer3"_n));

   const auto block_num = vote_block_num();
   const auto bid = make_block_id(block_num);
   const auto hash = make_snap_hash(11);
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, bid, hash));
   BOOST_REQUIRE(getsnaphash(block_num).is_null());

   // The two remaining mappings supply both the denominator and the two-vote configuration floor.
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov2"_n, bid, hash));
   BOOST_REQUIRE(!getsnaphash(block_num).is_null());
} FC_LOG_AND_RETHROW() }

/// Rank reconciliation preserves the explicit security floor while shrinking the live denominator.
BOOST_FIXTURE_TEST_CASE(votesnaphash_uses_live_provider_set_after_rank_demotion, snapshot_attest_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer2"_n, "snapprov2"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer3"_n, "snapprov3"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(2, 50));

   BOOST_REQUIRE_EQUAL(success(), setrank("producer3"_n, 31));

   const auto block_num = vote_block_num();
   const auto bid = make_block_id(block_num);
   const auto hash = make_snap_hash(13);
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, bid, hash));
   BOOST_REQUIRE(getsnaphash(block_num).is_null());
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov2"_n, bid, hash));
   BOOST_REQUIRE(!getsnaphash(block_num).is_null());
} FC_LOG_AND_RETHROW() }

/// A producer eligibility change removes only that producer's pending weight.
BOOST_FIXTURE_TEST_CASE(votesnaphash_prunes_removed_producer_vote, snapshot_attest_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer2"_n, "snapprov2"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer3"_n, "snapprov3"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(2, 1));

   const auto block_num = vote_block_num();
   const auto bid = make_block_id(block_num);
   const auto hash = make_snap_hash(12);
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, bid, hash));
   BOOST_REQUIRE_EQUAL(1u, snapshot_vote_count());
   BOOST_REQUIRE_EQUAL(success(), unregproducer("producer1"_n));
   BOOST_REQUIRE_EQUAL(0u, snapshot_vote_count());

   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov2"_n, bid, hash));
   BOOST_REQUIRE(getsnaphash(block_num).is_null());

   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov3"_n, bid, hash));
   BOOST_REQUIRE(!getsnaphash(block_num).is_null());
} FC_LOG_AND_RETHROW() }

/// Removing a non-voter immediately finalizes a tuple that meets the smaller live-set quorum.
BOOST_FIXTURE_TEST_CASE(votesnaphash_rechecks_quorum_after_nonvoter_removal, snapshot_attest_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer2"_n, "snapprov2"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer3"_n, "snapprov3"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer4"_n, "snapprov4"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer5"_n, "snapprov5"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(3, 67));

   const auto block_num = vote_block_num();
   const auto block_id = make_block_id(block_num);
   const auto snapshot_hash = make_snap_hash(115);
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, block_id, snapshot_hash));
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov2"_n, block_id, snapshot_hash));
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov3"_n, block_id, snapshot_hash));
   BOOST_REQUIRE(getsnaphash(block_num).is_null());

   // Five providers require four votes. Deregistering non-voter producer5 leaves four providers and
   // lowers the quorum to three, so the existing tuple must finalize without a vote resubmission.
   BOOST_REQUIRE_EQUAL(success(), delsnapprov("snapprov5"_n));
   BOOST_REQUIRE(!getsnaphash(block_num).is_null());
   BOOST_REQUIRE_EQUAL(0u, snapshot_vote_count());
} FC_LOG_AND_RETHROW() }

/// Lowering governance's quorum immediately evaluates already-collected live votes.
BOOST_FIXTURE_TEST_CASE(votesnaphash_rechecks_quorum_after_config_change, snapshot_attest_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer2"_n, "snapprov2"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer3"_n, "snapprov3"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer4"_n, "snapprov4"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(1, 100));

   const auto block_num = vote_block_num();
   const auto block_id = make_block_id(block_num);
   const auto snapshot_hash = make_snap_hash(116);
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, block_id, snapshot_hash));
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov2"_n, block_id, snapshot_hash));
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov3"_n, block_id, snapshot_hash));
   BOOST_REQUIRE(getsnaphash(block_num).is_null());

   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(1, 67));
   BOOST_REQUIRE(!getsnaphash(block_num).is_null());
   BOOST_REQUIRE_EQUAL(0u, snapshot_vote_count());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(votesnaphash_single_no_quorum, snapshot_attest_tester) { try {
   // 3 providers, 50% threshold -> ceil(3*50/100) = 2, min_providers=2
   // quorum = max(2, 2) = 2; single vote not enough
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer2"_n, "snapprov2"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer3"_n, "snapprov3"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(2, 50));
   produce_blocks();

   const auto block_num = vote_block_num();
   auto bid = make_block_id(block_num);
   auto shash = make_snap_hash(1);

   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, bid, shash));

   // No attested record yet
   auto rec = getsnaphash(block_num);
   BOOST_REQUIRE_EQUAL(true, rec.is_null());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(votesnaphash_quorum_reached, snapshot_attest_tester) { try {
   // 3 providers, 50% threshold -> ceil(3*50/100) = ceil(1.5) = 2
   // quorum = max(2, 2) = 2
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer2"_n, "snapprov2"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer3"_n, "snapprov3"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(2, 50));
   produce_blocks();

   const auto block_num = vote_block_num();
   auto bid = make_block_id(block_num);
   auto shash = make_snap_hash(1);

   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, bid, shash));
   // 1 vote, quorum=2, not attested yet
   BOOST_REQUIRE_EQUAL(true, getsnaphash(block_num).is_null());

   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov2"_n, bid, shash));

   // Quorum reached — attested record should exist
   auto rec = getsnaphash(block_num);
   BOOST_REQUIRE_EQUAL(false, rec.is_null());
   BOOST_REQUIRE_EQUAL(block_num, rec["block_num"].as_uint64());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(votesnaphash_same_tuple_retry_is_idempotent, snapshot_attest_tester) { try {
   // Need 2 providers, min_providers=2 so single vote won't attest and purge
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer2"_n, "snapprov2"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(2, 67));
   produce_blocks();

   const auto block_num = vote_block_num();
   auto bid = make_block_id(block_num);
   auto shash = make_snap_hash(1);

   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, bid, shash));
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, bid, shash));
   BOOST_REQUIRE(getsnaphash(block_num).is_null());
} FC_LOG_AND_RETHROW() }

/// An exact retry remains idempotent after finalization and subsequent eligibility removal.
BOOST_FIXTURE_TEST_CASE(votesnaphash_final_tuple_retry_is_idempotent, snapshot_attest_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(1, 67));

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

/// Voting is disabled until governance explicitly chooses a nonzero provider floor.
BOOST_FIXTURE_TEST_CASE(votesnaphash_rejects_unconfigured_quorum, snapshot_attest_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));

   const auto block_num = vote_block_num();
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("snapshot attestation configuration has not been set"),
                       votesnaphash("snapprov1"_n, make_block_id(block_num), make_snap_hash(101)));
   BOOST_REQUIRE(getsnaphash(block_num).is_null());
} FC_LOG_AND_RETHROW() }

/// A provider cannot pre-attest a tuple for a block height the chain has not reached.
BOOST_FIXTURE_TEST_CASE(votesnaphash_rejects_future_block_height, snapshot_attest_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(1, 67));

   // The action executes in the pending block at head + 1, so head + 2 is the first
   // height that is strictly beyond current_block_number().
   const auto future_block_num = vote_block_num() + 2;
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("snapshot block cannot be in the future"),
                       votesnaphash("snapprov1"_n,
                                    make_block_id(future_block_num),
                                    make_snap_hash(102)));
} FC_LOG_AND_RETHROW() }

// ---------------------------------------------------------------------------
// threshold / min_providers tests
// ---------------------------------------------------------------------------
BOOST_FIXTURE_TEST_CASE(threshold_min_providers_floor, snapshot_attest_tester) { try {
   // A live committee smaller than min_providers is rejected before quorum calculation.
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(2, 67));
   produce_blocks();

   const auto block_num = vote_block_num();
   auto bid = make_block_id(block_num);
   auto shash = make_snap_hash(2);

   BOOST_REQUIRE_EQUAL(wasm_assert_msg("registered snapshot providers are below min_providers"),
                       votesnaphash("snapprov1"_n, bid, shash));

   // Not attested because min_providers floor is 2
   auto rec = getsnaphash(block_num);
   BOOST_REQUIRE_EQUAL(true, rec.is_null());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(threshold_percentage_calculation, snapshot_attest_tester) { try {
   // 5 providers, 67% threshold -> ceil(5*67/100) = ceil(3.35) = 4
   // quorum = max(1, 4) = 4
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer2"_n, "snapprov2"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer3"_n, "snapprov3"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer4"_n, "snapprov4"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer5"_n, "snapprov5"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(1, 67));
   produce_blocks();

   const auto block_num = vote_block_num();
   auto bid = make_block_id(block_num);
   auto shash = make_snap_hash(3);

   // 3 votes should NOT be enough (need 4)
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, bid, shash));
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov2"_n, bid, shash));
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov3"_n, bid, shash));
   BOOST_REQUIRE_EQUAL(true, getsnaphash(block_num).is_null());

   // 4th vote reaches quorum
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov4"_n, bid, shash));
   BOOST_REQUIRE_EQUAL(false, getsnaphash(block_num).is_null());
} FC_LOG_AND_RETHROW() }

// ---------------------------------------------------------------------------
// disagreement tests
// ---------------------------------------------------------------------------
BOOST_FIXTURE_TEST_CASE(disagreement_detection, snapshot_attest_tester) { try {
   // 2 providers, quorum = max(1, ceil(2*50/100)) = 1
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer2"_n, "snapprov2"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(1, 50));
   produce_blocks();

   const auto block_num = vote_block_num();
   auto bid = make_block_id(block_num);
   auto shash = make_snap_hash(4);

   // Attest with one vote (quorum=1)
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, bid, shash));
   BOOST_REQUIRE_EQUAL(false, getsnaphash(block_num).is_null());

   // Second provider votes with different hash for same block — disagreement
   auto bad_hash = make_snap_hash(999);
   // snap_hash_disagreement_error = 9001 (defined in snapshot_attest.hpp)
   BOOST_REQUIRE_EQUAL(wasm_assert_code(9001),
                        votesnaphash("snapprov2"_n, bid, bad_hash));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(blockid_mismatch_votes_not_aggregated, snapshot_attest_tester) { try {
   // 3 providers, quorum = max(2, ceil(3*50/100)) = 2.
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer2"_n, "snapprov2"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer3"_n, "snapprov3"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(2, 50));
   produce_blocks();

   // Same height and same snapshot hash, but different block ids (competing forks).
   const auto block_num = vote_block_num();
   auto bid_a  = make_block_id(block_num);
   auto bid_b  = make_block_id(block_num, 1);
   auto shash  = make_snap_hash(8);

   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, bid_a, shash));
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov2"_n, bid_b, shash));

   // The two votes agree on the hash but not the block id, so they must NOT
   // jointly reach the quorum of 2.
   BOOST_REQUIRE_EQUAL(true, getsnaphash(block_num).is_null());

   // A distinct producer may join the first tuple to reach quorum.
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov3"_n, bid_a, shash));
   auto rec = getsnaphash(block_num);
   BOOST_REQUIRE_EQUAL(false, rec.is_null());
   BOOST_REQUIRE_EQUAL(bid_a.str(), rec["block_id"].as_string());
   BOOST_REQUIRE_EQUAL(shash.str(), rec["snapshot_hash"].as_string());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(votesnaphash_rejects_producer_equivocation_across_hashes, snapshot_attest_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer2"_n, "snapprov2"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(2, 50));

   const auto bid = make_block_id(vote_block_num());
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, bid, make_snap_hash(80)));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("producer already voted a different snapshot tuple for this height"),
                        votesnaphash("snapprov1"_n, bid, make_snap_hash(81)));
} FC_LOG_AND_RETHROW() }

/// Re-registering an eligible producer starts from an empty pending set rather than reviving stale weight.
BOOST_FIXTURE_TEST_CASE(votesnaphash_reregistration_does_not_restore_pending_vote, snapshot_attest_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer2"_n, "snapprov2"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer3"_n, "snapprov3"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(2, 50));

   const auto block_num = vote_block_num();
   const auto bid  = make_block_id(block_num);
   const auto hash = make_snap_hash(82);
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov3"_n, bid, hash));
   BOOST_REQUIRE_EQUAL(1u, snapshot_vote_count());
   BOOST_REQUIRE_EQUAL(success(), unregproducer("producer3"_n));
   BOOST_REQUIRE_EQUAL(0u, snapshot_vote_count());

   BOOST_REQUIRE_EQUAL(success(), regproducer("producer3"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer3"_n, "snapprov3"_n));
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov3"_n, bid, hash));
   BOOST_REQUIRE(getsnaphash(block_num).is_null());
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, bid, hash));
   BOOST_REQUIRE(!getsnaphash(block_num).is_null());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(votesnaphash_reports_disagreement_before_eligibility_failure, snapshot_attest_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer2"_n, "snapprov2"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(1, 50));

   const auto bid = make_block_id(vote_block_num());
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, bid, make_snap_hash(83)));
   BOOST_REQUIRE_EQUAL(success(), unregproducer("producer2"_n));
   BOOST_REQUIRE_EQUAL(wasm_assert_code(9001),
                        votesnaphash("snapprov2"_n, bid, make_snap_hash(84)));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(record_blockid_disagreement, snapshot_attest_tester) { try {
   // 2 providers, quorum = max(1, ceil(2*50/100)) = 1
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer2"_n, "snapprov2"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(1, 50));
   produce_blocks();

   const auto block_num = vote_block_num();
   auto bid_a = make_block_id(block_num);
   auto bid_b = make_block_id(block_num, 1);
   auto shash = make_snap_hash(9);

   // Attest with one vote (quorum=1)
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, bid_a, shash));
   BOOST_REQUIRE_EQUAL(false, getsnaphash(block_num).is_null());

   // Same snapshot hash under a different block id disagrees with the attested record
   BOOST_REQUIRE_EQUAL(wasm_assert_code(9001),
                        votesnaphash("snapprov2"_n, bid_b, shash));
} FC_LOG_AND_RETHROW() }

// ---------------------------------------------------------------------------
// purging tests
// ---------------------------------------------------------------------------
BOOST_FIXTURE_TEST_CASE(vote_retention_is_bounded_per_producer, snapshot_attest_tester) { try {
   // Register 2 providers, min_providers=2
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer2"_n, "snapprov2"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(2, 50));
   produce_blocks();

   const auto older_block_num = vote_block_num(2);
   const auto middle_block_num = vote_block_num(1);
   const auto latest_block_num = vote_block_num();

   // Vote at an older height (won't reach quorum with just 1 vote).
   auto bid1 = make_block_id(older_block_num);
   auto shash1 = make_snap_hash(5);
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, bid1, shash1));
   BOOST_REQUIRE_EQUAL(1u, snapshot_vote_count());

   // Moving to a newer height drops only producer1's obsolete vote. Because that vote was
   // the old tuple's sole weight, its empty row is erased.
   auto bid2 = make_block_id(middle_block_num);
   auto shash2 = make_snap_hash(6);
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, bid2, shash2));
   BOOST_REQUIRE_EQUAL(1u, snapshot_vote_count());

   // producer2 may still vote at the older height; this cannot erase producer1's newer vote.
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov2"_n, bid1, shash1));
   BOOST_REQUIRE_EQUAL(2u, snapshot_vote_count());

   // Moving producer2 to the middle height erases the now-empty old row and reaches quorum.
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov2"_n, bid2, shash2));

   BOOST_REQUIRE_EQUAL(false, getsnaphash(middle_block_num).is_null());
   BOOST_REQUIRE_EQUAL(0u, snapshot_vote_count());

   // Verify system works for subsequent attestations
   auto bid3 = make_block_id(latest_block_num);
   auto shash3 = make_snap_hash(7);
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, bid3, shash3));
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov2"_n, bid3, shash3));
   BOOST_REQUIRE_EQUAL(false, getsnaphash(latest_block_num).is_null());
} FC_LOG_AND_RETHROW() }

/// A provider voting at a newer height cannot erase honest weight at an older height.
BOOST_FIXTURE_TEST_CASE(votesnaphash_newer_vote_cannot_censor_older_tuple, snapshot_attest_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer2"_n, "snapprov2"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer3"_n, "snapprov3"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer4"_n, "snapprov4"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(3, 67));

   const auto newer_block_num = vote_block_num();
   const auto older_block_num = newer_block_num - 1;
   const auto older_block_id = make_block_id(older_block_num);
   const auto older_hash = make_snap_hash(112);
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, older_block_id, older_hash));
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov2"_n, older_block_id, older_hash));

   BOOST_REQUIRE_EQUAL(success(), votesnaphash(
      "snapprov3"_n, make_block_id(newer_block_num), make_snap_hash(113)));
   BOOST_REQUIRE_EQUAL(2u, snapshot_vote_count());

   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov4"_n, older_block_id, older_hash));
   BOOST_REQUIRE(!getsnaphash(older_block_num).is_null());
   BOOST_REQUIRE_EQUAL(1u, snapshot_vote_count());
} FC_LOG_AND_RETHROW() }

/// Registration churn cannot erase pending votes cast by other live producers.
BOOST_FIXTURE_TEST_CASE(votesnaphash_registration_churn_preserves_other_votes, snapshot_attest_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer2"_n, "snapprov2"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer3"_n, "snapprov3"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer4"_n, "snapprov4"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(3, 67));

   const auto block_num = vote_block_num();
   const auto block_id = make_block_id(block_num);
   const auto snapshot_hash = make_snap_hash(114);
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, block_id, snapshot_hash));
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov2"_n, block_id, snapshot_hash));

   BOOST_REQUIRE_EQUAL(success(), delsnapprov("snapprov3"_n));
   BOOST_REQUIRE_EQUAL(1u, snapshot_vote_count());
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

// A misconfigured low quorum (min_providers=1) must NOT let a single provider attest an arbitrary
// (block_id, snapshot_hash): votesnaphash enforces a Byzantine quorum floor (provider_count/3 + 1)
// independently of the governance config, so a fault minority cannot attest on its own (which would
// drive honest providers whose hash differs to self-shutdown).
BOOST_FIXTURE_TEST_CASE(votesnaphash_enforces_byzantine_quorum_floor, snapshot_attest_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer2"_n, "snapprov2"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer3"_n, "snapprov3"_n));
   // Low/misconfigured quorum — without the floor, one vote would attest.
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(1, 1));

   const auto block_num = vote_block_num();
   const auto bid  = make_block_id(block_num);
   const auto hash = make_snap_hash(1);

   // 3 providers -> floor = 3/3 + 1 = 2. A single vote must NOT create an attested record.
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, bid, hash));
   BOOST_REQUIRE(getsnaphash(block_num).is_null());

   // A second agreeing vote reaches the floor and attests.
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov2"_n, bid, hash));
   BOOST_REQUIRE(!getsnaphash(block_num).is_null());
} FC_LOG_AND_RETHROW() }

/// Rotating a snapshot account prunes only that producer's old pending weight.
BOOST_FIXTURE_TEST_CASE(votesnaphash_invalidates_pending_votes_on_snap_account_rotation, snapshot_attest_tester) { try {
   // 4 providers -> floor = 4/3 + 1 = 2 is the binding quorum (threshold 1% rounds to 1).
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer2"_n, "snapprov2"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer3"_n, "snapprov3"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer4"_n, "snapprov4"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(1, 1));
   produce_blocks();

   const auto block_num = vote_block_num();
   const auto bid  = make_block_id(block_num);
   const auto hash = make_snap_hash(7);

   // producer1 votes via its first snap_account.
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, bid, hash));

   // producer1 rotates its snap_account: drop snapprov1, register a fresh snapprov5.
   BOOST_REQUIRE_EQUAL(success(), delsnapprov("snapprov1"_n));
   BOOST_REQUIRE_EQUAL(0u, snapshot_vote_count());
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov5"_n));
   produce_blocks();

   // The rotated account starts a fresh pending tuple; one producer cannot reach the floor of 2.
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov5"_n, bid, hash));
   BOOST_REQUIRE(getsnaphash(block_num).is_null());

   // A genuinely distinct producer supplies the second, quorum-reaching vote.
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov2"_n, bid, hash));
   BOOST_REQUIRE(!getsnaphash(block_num).is_null());
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()

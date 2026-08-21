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

   // Helper: register a snapshot provider
   action_result regsnapprov(name producer, name snap_account) {
      return push_action(producer, "regsnapprov"_n, mvo()
         ("producer", producer)
         ("snap_account", snap_account));
   }

   // Helper: unregister a snapshot provider
   action_result delsnapprov(name account) {
      return push_action(account, "delsnapprov"_n, mvo()
         ("account", account));
   }

   // Helper: deactivate a producer while retaining its producer-table row.
   action_result unregproducer(name producer) {
      return push_action(producer, "unregprod"_n, mvo()
         ("producer", producer));
   }

   // Helper: vote on a snapshot hash
   action_result votesnaphash(name snap_account, const fc::sha256& block_id, const fc::sha256& snapshot_hash) {
      return push_action(snap_account, "votesnaphash"_n, mvo()
         ("snap_account", snap_account)
         ("block_id", block_id)
         ("snapshot_hash", snapshot_hash));
   }

   // Helper: set snapshot config
   action_result setsnpcfg(uint32_t min_providers, uint32_t threshold_pct) {
      return push_action(config::system_account_name, "setsnpcfg"_n, mvo()
         ("min_providers", min_providers)
         ("threshold_pct", threshold_pct));
   }

   // Helper: get attested snapshot record from table
   fc::variant getsnaphash(uint32_t block_num) {
      vector<char> data = get_row_by_account(
         config::system_account_name, config::system_account_name,
         "snaprecords"_n, name(block_num));
      return data.empty() ? fc::variant() : abi_ser.binary_to_variant(
         "snap_record", data, abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   // Helper: get snap provider
   fc::variant get_snap_provider(name snap_account) {
      vector<char> data = get_row_by_account(
         config::system_account_name, config::system_account_name,
         "snapprovs"_n, snap_account);
      return data.empty() ? fc::variant() : abi_ser.binary_to_variant(
         "snap_provider", data, abi_serializer::create_yield_function(abi_serializer_max_time));
   }

   // Make a fake block_id with a specific block number embedded in big-endian.
   // `fork` differentiates block ids that share the same height, emulating blocks
   // from competing forks (same first-4-byte height prefix, different remainder).
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

   // Make a fake snapshot hash
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
   auto bid = make_block_id(1000);
   auto shash = make_snap_hash(1);
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("snap_account is not a registered snapshot provider"),
                        votesnaphash("snapprov1"_n, bid, shash));
} FC_LOG_AND_RETHROW() }

// CertiK WNS-17 / WIRE-350: unregprod retains the producer row and its provider mapping, but the
// mapping must not retain voting authority after the producer is inactive.
BOOST_FIXTURE_TEST_CASE(votesnaphash_rejects_provider_after_producer_unregistered, snapshot_attest_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), unregproducer("producer1"_n));

   const auto bid = make_block_id(9000);
   const auto hash = make_snap_hash(9);
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("producer is not active"),
                        votesnaphash("snapprov1"_n, bid, hash));
   BOOST_REQUIRE(getsnaphash(9000).is_null());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(votesnaphash_rejects_provider_after_producer_rank_demotion, snapshot_attest_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), setrank("producer1"_n, 31));

   const auto bid = make_block_id(9001);
   const auto hash = make_snap_hash(10);
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("producer rank exceeds maximum for snapshot providers"),
                        votesnaphash("snapprov1"_n, bid, hash));
   BOOST_REQUIRE(getsnaphash(9001).is_null());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(votesnaphash_excludes_inactive_providers_from_quorum, snapshot_attest_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer2"_n, "snapprov2"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer3"_n, "snapprov3"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(1, 50));
   BOOST_REQUIRE_EQUAL(success(), unregproducer("producer3"_n));

   const auto bid = make_block_id(9002);
   const auto hash = make_snap_hash(11);
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, bid, hash));
   BOOST_REQUIRE(getsnaphash(9002).is_null());

   // The three registered mappings require two votes at 50%; producer3's inactive status must not
   // lower that denominator or let producer1 attest on its own.
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov2"_n, bid, hash));
   BOOST_REQUIRE(!getsnaphash(9002).is_null());
} FC_LOG_AND_RETHROW() }

// CertiK WNS-17 / WIRE-350: producer eligibility controls who may cast a vote, but rank churn must
// not lower the stable registered-provider quorum for an irreversible attestation record.
BOOST_FIXTURE_TEST_CASE(votesnaphash_preserves_registered_provider_quorum_after_rank_demotion, snapshot_attest_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer2"_n, "snapprov2"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer3"_n, "snapprov3"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer4"_n, "snapprov4"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer5"_n, "snapprov5"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(1, 50));

   BOOST_REQUIRE_EQUAL(success(), setrank("producer2"_n, 31));
   BOOST_REQUIRE_EQUAL(success(), setrank("producer3"_n, 31));
   BOOST_REQUIRE_EQUAL(success(), setrank("producer4"_n, 31));
   BOOST_REQUIRE_EQUAL(success(), setrank("producer5"_n, 31));

   const auto bid = make_block_id(9004);
   const auto hash = make_snap_hash(13);
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, bid, hash));
   BOOST_REQUIRE(getsnaphash(9004).is_null());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(votesnaphash_excludes_inactive_pending_voters_from_quorum, snapshot_attest_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer2"_n, "snapprov2"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer3"_n, "snapprov3"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(2, 1));

   const auto bid = make_block_id(9003);
   const auto hash = make_snap_hash(12);
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, bid, hash));
   BOOST_REQUIRE_EQUAL(success(), unregproducer("producer1"_n));

   // producer1's pending vote is stale, so producer2 alone must not attest the snapshot.
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov2"_n, bid, hash));
   BOOST_REQUIRE(getsnaphash(9003).is_null());

   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov3"_n, bid, hash));
   BOOST_REQUIRE(!getsnaphash(9003).is_null());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(votesnaphash_single_no_quorum, snapshot_attest_tester) { try {
   // 3 providers, 50% threshold -> ceil(3*50/100) = 2, min_providers=2
   // quorum = max(2, 2) = 2; single vote not enough
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer2"_n, "snapprov2"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer3"_n, "snapprov3"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(2, 50));
   produce_blocks();

   auto bid = make_block_id(1000);
   auto shash = make_snap_hash(1);

   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, bid, shash));

   // No attested record yet
   auto rec = getsnaphash(1000);
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

   auto bid = make_block_id(1000);
   auto shash = make_snap_hash(1);

   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, bid, shash));
   // 1 vote, quorum=2, not attested yet
   BOOST_REQUIRE_EQUAL(true, getsnaphash(1000).is_null());

   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov2"_n, bid, shash));

   // Quorum reached — attested record should exist
   auto rec = getsnaphash(1000);
   BOOST_REQUIRE_EQUAL(false, rec.is_null());
   BOOST_REQUIRE_EQUAL(1000u, rec["block_num"].as_uint64());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(votesnaphash_same_tuple_retry_is_idempotent, snapshot_attest_tester) { try {
   // Need 2 providers, min_providers=2 so single vote won't attest and purge
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer2"_n, "snapprov2"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(2, 67));
   produce_blocks();

   auto bid = make_block_id(1000);
   auto shash = make_snap_hash(1);

   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, bid, shash));
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, bid, shash));
   BOOST_REQUIRE(getsnaphash(1000).is_null());
} FC_LOG_AND_RETHROW() }

// ---------------------------------------------------------------------------
// threshold / min_providers tests
// ---------------------------------------------------------------------------
BOOST_FIXTURE_TEST_CASE(threshold_min_providers_floor, snapshot_attest_tester) { try {
   // Single provider, but min_providers=2 means quorum=2, no attestation with 1 vote
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(2, 67));
   produce_blocks();

   auto bid = make_block_id(2000);
   auto shash = make_snap_hash(2);

   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, bid, shash));

   // Not attested because min_providers floor is 2
   auto rec = getsnaphash(2000);
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

   auto bid = make_block_id(3000);
   auto shash = make_snap_hash(3);

   // 3 votes should NOT be enough (need 4)
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, bid, shash));
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov2"_n, bid, shash));
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov3"_n, bid, shash));
   BOOST_REQUIRE_EQUAL(true, getsnaphash(3000).is_null());

   // 4th vote reaches quorum
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov4"_n, bid, shash));
   BOOST_REQUIRE_EQUAL(false, getsnaphash(3000).is_null());
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

   auto bid = make_block_id(4000);
   auto shash = make_snap_hash(4);

   // Attest with one vote (quorum=1)
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, bid, shash));
   BOOST_REQUIRE_EQUAL(false, getsnaphash(4000).is_null());

   // Second provider votes with different hash for same block — disagreement
   auto bad_hash = make_snap_hash(999);
   // snap_hash_disagreement_error = 9001 (defined in snapshot_attest.hpp)
   BOOST_REQUIRE_EQUAL(wasm_assert_code(9001),
                        votesnaphash("snapprov2"_n, bid, bad_hash));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(blockid_mismatch_votes_not_aggregated, snapshot_attest_tester) { try {
   // 2 providers, quorum = max(2, ceil(2*50/100)) = 2
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer2"_n, "snapprov2"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(2, 50));
   produce_blocks();

   // Same height and same snapshot hash, but different block ids (competing forks).
   auto bid_a  = make_block_id(8000);
   auto bid_b  = make_block_id(8000, 1);
   auto shash  = make_snap_hash(8);

   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, bid_a, shash));
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov2"_n, bid_b, shash));

   // The two votes agree on the hash but not the block id, so they must NOT
   // jointly reach the quorum of 2.
   BOOST_REQUIRE_EQUAL(true, getsnaphash(8000).is_null());

   // A distinct producer may join the first tuple to reach quorum; producer2 cannot vote twice
   // across the competing tuples at the same height.
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer3"_n, "snapprov3"_n));
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov3"_n, bid_a, shash));
   auto rec = getsnaphash(8000);
   BOOST_REQUIRE_EQUAL(false, rec.is_null());
   BOOST_REQUIRE_EQUAL(bid_a.str(), rec["block_id"].as_string());
   BOOST_REQUIRE_EQUAL(shash.str(), rec["snapshot_hash"].as_string());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(votesnaphash_rejects_producer_equivocation_across_hashes, snapshot_attest_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer2"_n, "snapprov2"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(2, 50));

   const auto bid = make_block_id(8001);
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, bid, make_snap_hash(80)));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("producer has already voted for this snapshot"),
                        votesnaphash("snapprov1"_n, bid, make_snap_hash(81)));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(votesnaphash_retries_pending_vote_after_eligibility_restored, snapshot_attest_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer2"_n, "snapprov2"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer3"_n, "snapprov3"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(3, 50));

   const auto bid  = make_block_id(8002);
   const auto hash = make_snap_hash(82);
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov3"_n, bid, hash));
   BOOST_REQUIRE_EQUAL(success(), unregproducer("producer3"_n));
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, bid, hash));
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov2"_n, bid, hash));
   BOOST_REQUIRE(getsnaphash(8002).is_null());

   BOOST_REQUIRE_EQUAL(success(), regproducer("producer3"_n));
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov3"_n, bid, hash));
   BOOST_REQUIRE(!getsnaphash(8002).is_null());
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(votesnaphash_reports_disagreement_before_eligibility_failure, snapshot_attest_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer2"_n, "snapprov2"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(1, 50));

   const auto bid = make_block_id(8003);
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

   auto bid_a = make_block_id(9000);
   auto bid_b = make_block_id(9000, 1);
   auto shash = make_snap_hash(9);

   // Attest with one vote (quorum=1)
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, bid_a, shash));
   BOOST_REQUIRE_EQUAL(false, getsnaphash(9000).is_null());

   // Same snapshot hash under a different block id disagrees with the attested record
   BOOST_REQUIRE_EQUAL(wasm_assert_code(9001),
                        votesnaphash("snapprov2"_n, bid_b, shash));
} FC_LOG_AND_RETHROW() }

// ---------------------------------------------------------------------------
// purging tests
// ---------------------------------------------------------------------------
BOOST_FIXTURE_TEST_CASE(vote_purging_on_attestation, snapshot_attest_tester) { try {
   // Register 2 providers, min_providers=2
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer2"_n, "snapprov2"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(2, 50));
   produce_blocks();

   // Vote on block 5000 (won't reach quorum with just 1 vote)
   auto bid1 = make_block_id(5000);
   auto shash1 = make_snap_hash(5);
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, bid1, shash1));

   // Vote on block 6000 — both providers vote, reaching quorum
   auto bid2 = make_block_id(6000);
   auto shash2 = make_snap_hash(6);
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, bid2, shash2));
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov2"_n, bid2, shash2));

   BOOST_REQUIRE_EQUAL(false, getsnaphash(6000).is_null());

   // Verify system works for subsequent attestations
   auto bid3 = make_block_id(7000);
   auto shash3 = make_snap_hash(7);
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, bid3, shash3));
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov2"_n, bid3, shash3));
   BOOST_REQUIRE_EQUAL(false, getsnaphash(7000).is_null());
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

   const auto bid  = make_block_id(1000);
   const auto hash = make_snap_hash(1);

   // 3 providers -> floor = 3/3 + 1 = 2. A single vote must NOT create an attested record.
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, bid, hash));
   BOOST_REQUIRE(getsnaphash(1000).is_null());

   // A second agreeing vote reaches the floor and attests.
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov2"_n, bid, hash));
   BOOST_REQUIRE(!getsnaphash(1000).is_null());
} FC_LOG_AND_RETHROW() }

// A single producer must not be able to clear the Byzantine floor by rotating
// snap_accounts: regsnapprov(A) -> vote -> delsnapprov(A) -> regsnapprov(B) -> retry the same
// (block_id, snapshot_hash). The retry is idempotent by stable producer identity, so it adds no
// second weight and the lone producer cannot reach quorum.
BOOST_FIXTURE_TEST_CASE(votesnaphash_rejects_snap_account_rotation_sybil, snapshot_attest_tester) { try {
   // 4 providers -> floor = 4/3 + 1 = 2 is the binding quorum (threshold 1% rounds to 1).
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer2"_n, "snapprov2"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer3"_n, "snapprov3"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer4"_n, "snapprov4"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(1, 1));
   produce_blocks();

   const auto bid  = make_block_id(7000);
   const auto hash = make_snap_hash(7);

   // producer1 votes via its first snap_account.
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, bid, hash));

   // producer1 rotates its snap_account: drop snapprov1, register a fresh snapprov5.
   // provider_count is unchanged (still 4), so the floor stays 2.
   BOOST_REQUIRE_EQUAL(success(), delsnapprov("snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov5"_n));
   produce_blocks();

   // The rotated snap_account resolves to the same producer, so its idempotent retry adds no
   // second weight and the snapshot is still NOT attested (one producer cannot reach the floor of 2).
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov5"_n, bid, hash));
   BOOST_REQUIRE(getsnaphash(7000).is_null());

   // A genuinely distinct producer supplies the second, quorum-reaching vote.
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov2"_n, bid, hash));
   BOOST_REQUIRE(!getsnaphash(7000).is_null());
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()

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

   // Make a fake block_id with a specific block number embedded in big-endian
   static fc::sha256 make_block_id(uint32_t block_num) {
      fc::sha256 id;
      auto* data = id.data();
      // block_num in big-endian in first 4 bytes
      data[0] = static_cast<char>((block_num >> 24) & 0xFF);
      data[1] = static_cast<char>((block_num >> 16) & 0xFF);
      data[2] = static_cast<char>((block_num >> 8) & 0xFF);
      data[3] = static_cast<char>(block_num & 0xFF);
      return id;
   }

   // Make a fake snapshot hash
   static fc::sha256 make_snap_hash(uint32_t seed) {
      fc::sha256 hash;
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

BOOST_FIXTURE_TEST_CASE(votesnaphash_duplicate_vote, snapshot_attest_tester) { try {
   // Need 2 providers, min_providers=2 so single vote won't attest and purge
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer2"_n, "snapprov2"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(2, 67));
   produce_blocks();

   auto bid = make_block_id(1000);
   auto shash = make_snap_hash(1);

   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, bid, shash));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("snap_account has already voted for this snapshot"),
                        votesnaphash("snapprov1"_n, bid, shash));
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
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("snapshot hash disagrees with attested record for this block"),
                        votesnaphash("snapprov2"_n, bid, bad_hash));
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

BOOST_AUTO_TEST_SUITE_END()

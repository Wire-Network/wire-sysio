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

   /// Advance lazily to the first cadence boundary and return the latest scheduled height.
   uint32_t vote_block_num() {
      const uint32_t spacing = sysio::protocol::snapshot_attestation::block_spacing;
      uint32_t       head_block_num = control->head().block_num();
      if (head_block_num < spacing) {
         // Commit registrations and configuration queued by the test before empty cadence blocks
         // intentionally skip pending transactions.
         produce_block();
         head_block_num = control->head().block_num();

         // Only the expensive empty-block advance skips duplicate validation. Fast registration
         // and configuration cases retain normal validating-controller coverage.
         skip_validate           = true;
         primary_only_production = true;
         produce_blocks(spacing - head_block_num, true);
      }
      return control->head().block_num() / spacing * spacing;
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

   // Eligibility changes do not touch the normal lifecycle path. A full-table registration lazily
   // removes stale mappings before enforcing the cap, but a registration that already conflicts
   // must fail without pruning unrelated rows.
   BOOST_REQUIRE_EQUAL(success(), unregproducer("producer1"_n));
   BOOST_REQUIRE(!get_snap_provider("snapprov1"_n).is_null());
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("snap_account is already registered as a provider"),
                       regsnapprov(capacity_producers.back(), "snapprov2"_n));
   BOOST_REQUIRE(!get_snap_provider("snapprov1"_n).is_null());
   BOOST_REQUIRE_EQUAL(success(),
                       regsnapprov(capacity_producers.back(), capacity_producers.back()));
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
BOOST_FIXTURE_TEST_CASE(votesnaphash_unregistered, snapshot_attest_tester) { try {
   auto bid = make_block_id(vote_block_num());
   auto shash = make_snap_hash(1);
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("snap_account is not a registered snapshot provider"),
                        votesnaphash("snapprov1"_n, bid, shash));
} FC_LOG_AND_RETHROW() }

/// Producer eligibility is a registration gate; later lifecycle churn does not retract authority or votes.
BOOST_FIXTURE_TEST_CASE(votesnaphash_preserves_registered_authority_after_producer_churn,
                        snapshot_attest_tester) { try {
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
BOOST_FIXTURE_TEST_CASE(votesnaphash_uses_current_fixed_k_for_pending_votes, snapshot_attest_tester) { try {
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
BOOST_FIXTURE_TEST_CASE(votesnaphash_uses_current_fixed_k_for_competing_tuples,
                        snapshot_attest_tester) { try {
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

BOOST_FIXTURE_TEST_CASE(votesnaphash_single_no_quorum, snapshot_attest_tester) { try {
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

BOOST_FIXTURE_TEST_CASE(votesnaphash_quorum_reached, snapshot_attest_tester) { try {
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

BOOST_FIXTURE_TEST_CASE(votesnaphash_same_tuple_retry_is_idempotent, snapshot_attest_tester) { try {
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
BOOST_FIXTURE_TEST_CASE(votesnaphash_final_tuple_retry_is_idempotent, snapshot_attest_tester) { try {
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
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(1));

   const uint32_t future_block_num =
      vote_block_num() + sysio::protocol::snapshot_attestation::block_spacing;
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("snapshot block cannot be in the future"),
                       votesnaphash("snapprov1"_n,
                                    make_block_id(future_block_num),
                                    make_snap_hash(102)));
} FC_LOG_AND_RETHROW() }

/// A manual snapshot height cannot enter the bounded on-chain tally space.
BOOST_FIXTURE_TEST_CASE(votesnaphash_rejects_unscheduled_block_height, snapshot_attest_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(1));

   const uint32_t unscheduled_block_num = vote_block_num() + 1;
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("snapshot block is not a scheduled attestation height"),
                       votesnaphash("snapprov1"_n,
                                    make_block_id(unscheduled_block_num),
                                    make_snap_hash(104)));
} FC_LOG_AND_RETHROW() }

// ---------------------------------------------------------------------------
// fixed-K tests
// ---------------------------------------------------------------------------
BOOST_FIXTURE_TEST_CASE(fixed_k_can_be_reached_after_more_providers_register, snapshot_attest_tester) { try {
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

BOOST_FIXTURE_TEST_CASE(fixed_k_is_independent_of_registration_count, snapshot_attest_tester) { try {
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
BOOST_FIXTURE_TEST_CASE(disagreement_detection, snapshot_attest_tester) { try {
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

BOOST_FIXTURE_TEST_CASE(blockid_mismatch_votes_not_aggregated, snapshot_attest_tester) { try {
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

BOOST_FIXTURE_TEST_CASE(votesnaphash_rejects_producer_equivocation_across_hashes, snapshot_attest_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer2"_n, "snapprov2"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(2));

   const auto bid = make_block_id(vote_block_num());
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, bid, make_snap_hash(80)));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("producer already voted a different snapshot tuple for this height"),
                        votesnaphash("snapprov1"_n, bid, make_snap_hash(81)));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(votesnaphash_reports_disagreement_before_eligibility_failure, snapshot_attest_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer2"_n, "snapprov2"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(1));

   const auto bid = make_block_id(vote_block_num());
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, bid, make_snap_hash(83)));
   BOOST_REQUIRE_EQUAL(success(), unregproducer("producer2"_n));
   BOOST_REQUIRE_EQUAL(wasm_assert_code(9001),
                        votesnaphash("snapprov2"_n, bid, make_snap_hash(84)));
} FC_LOG_AND_RETHROW() }

BOOST_FIXTURE_TEST_CASE(record_blockid_disagreement, snapshot_attest_tester) { try {
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
BOOST_FIXTURE_TEST_CASE(votesnaphash_keeps_scheduled_heights_independent_until_finalization,
                        snapshot_attest_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer2"_n, "snapprov2"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(2));

   const uint32_t older_block_num = vote_block_num();
   const auto older_block_id = make_block_id(older_block_num);
   const auto older_hash = make_snap_hash(5);
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, older_block_id, older_hash));

   const uint32_t newer_block_num =
      older_block_num + sysio::protocol::snapshot_attestation::block_spacing;
   produce_blocks(newer_block_num - control->head().block_num(), true);
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
BOOST_FIXTURE_TEST_CASE(votesnaphash_rejects_reopening_purged_historical_height,
                        snapshot_attest_tester) { try {
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer1"_n, "snapprov1"_n));
   BOOST_REQUIRE_EQUAL(success(), regsnapprov("producer2"_n, "snapprov2"_n));
   BOOST_REQUIRE_EQUAL(success(), setsnpcfg(2));

   const uint32_t older_block_num = vote_block_num();
   const auto older_block_id = make_block_id(older_block_num);
   const auto older_hash = make_snap_hash(25);
   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov1"_n, older_block_id, older_hash));

   const uint32_t newer_block_num =
      older_block_num + sysio::protocol::snapshot_attestation::block_spacing;
   produce_blocks(newer_block_num - control->head().block_num(), true);
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
BOOST_FIXTURE_TEST_CASE(votesnaphash_registration_churn_preserves_other_votes, snapshot_attest_tester) { try {
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
BOOST_FIXTURE_TEST_CASE(votesnaphash_honors_governance_fixed_k, snapshot_attest_tester) { try {
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

/// Rotating a snapshot account preserves the producer's monotonic vote and makes retries idempotent.
BOOST_FIXTURE_TEST_CASE(votesnaphash_preserves_pending_vote_on_snap_account_rotation,
                        snapshot_attest_tester) { try {
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

   BOOST_REQUIRE_EQUAL(success(), votesnaphash("snapprov2"_n, block_id, snapshot_hash));
   BOOST_REQUIRE(!getsnaphash(block_num).is_null());
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()

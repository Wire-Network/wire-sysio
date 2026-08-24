#pragma once

#include <sysio/chain/name.hpp>
#include <sysio/chain/snapshot.hpp>
#include <sysio/protocol/snapshot_attestation.hpp>

#include <sysio_system_tester.hpp>

#include <boost/test/unit_test.hpp>

#include <cstring>
#include <vector>

namespace snapshot_attest_test_support {

using namespace sysio;
using namespace sysio::chain;
using namespace sysio::testing;
using mvo = fc::mutable_variant_object;
namespace snapshot_attestation = sysio::protocol::snapshot_attestation;

/// Producer registered for the shared snapshot-attestation tests.
inline constexpr auto producer_account = "producer1"_n;

/// Provider registered for the shared snapshot-attestation tests.
inline constexpr auto snapshot_provider_account = "snapprov1"_n;

/// One-provider quorum floor used by tests that finalize with one vote.
inline constexpr uint32_t single_provider_minimum = 1;

/// Unanimous voting threshold used by the one-provider test configuration.
inline constexpr uint32_t unanimous_threshold_pct = 100;

/// Distance from a scheduled height to the immediately preceding block.
inline constexpr uint32_t preceding_block_offset = 1;

namespace system_contract {

/// Producer rank assignment action used during fixture bootstrap.
inline constexpr auto action_setrank = "setrank"_n;

/// Producer rank field in the rank-assignment action.
inline constexpr auto field_rank = "rank";

/// Top eligible producer rank assigned by the fixture.
inline constexpr uint32_t producer_rank = 1;

} // namespace system_contract

/** Convert a BLAKE3 snapshot root to the checksum representation stored by the contract. */
inline fc::sha256 to_contract_snapshot_hash(const fc::crypto::blake3& snapshot_root) {
   static_assert(fc::sha256::byte_size == fc::crypto::blake3::byte_size);
   fc::sha256 contract_hash;
   std::memcpy(contract_hash.data(), snapshot_root.data(), contract_hash.data_size());
   return contract_hash;
}

/**
 * Real-contract fixture for scheduled snapshot-attestation and bootstrap tests.
 *
 * The fixture deploys the current token and system contracts, registers one producer/provider
 * pair, and stops immediately before the first 25,000-block production cadence height.
 */
class snapshot_attest_fixture : public sysio_system::sysio_system_tester {
public:
   /** Deploy and initialize the contracts and provider mapping used by each test. */
   snapshot_attest_fixture() {
      create_accounts({producer_account, snapshot_provider_account});
      produce_blocks();

      regproducer(producer_account);
      produce_blocks();

      BOOST_REQUIRE_EQUAL(
         success(),
         push_action(
            config::system_account_name, system_contract::action_setrank,
            mvo()(snapshot_attestation::field::producer, producer_account)
               (system_contract::field_rank, system_contract::producer_rank)));
      produce_blocks();

      BOOST_REQUIRE_EQUAL(
         success(),
         push_action(
            producer_account, action_name{snapshot_attestation::action_regsnapprov},
            mvo()(snapshot_attestation::field::producer, producer_account)
               (snapshot_attestation::field::snap_account, snapshot_provider_account)));
      produce_blocks();

      const uint32_t block_before_first_snapshot =
         snapshot_attestation::block_spacing - preceding_block_offset;
      if (control->head().block_num() < block_before_first_snapshot) {
         produce_blocks(block_before_first_snapshot - control->head().block_num(), true);
      }
   }

   /** Submit one provider snapshot-hash vote. */
   void vote_snapshot(const account_name& snap_account, const block_id_type& block_id,
                      const fc::sha256& snapshot_hash) {
      const auto result = push_action(
         snap_account, action_name{snapshot_attestation::action_votesnaphash},
         mvo()(snapshot_attestation::field::snap_account, snap_account)
            (snapshot_attestation::field::block_id, block_id)
            (snapshot_attestation::field::snapshot_hash, snapshot_hash));
      BOOST_REQUIRE_EQUAL(success(), result);
   }

   /** Configure the governance-controlled quorum parameters and commit the action in a new block. */
   void set_snap_config(uint32_t min_providers, uint32_t threshold_pct) {
      const auto result = push_action(
         config::system_account_name, action_name{snapshot_attestation::action_setsnapcfg},
         mvo()(snapshot_attestation::field::min_providers, min_providers)
            (snapshot_attestation::field::threshold_pct, threshold_pct));
      BOOST_REQUIRE_EQUAL(success(), result);
   }

   /** Read and ABI-decode one final snapshot record, returning whether it exists. */
   bool read_snap_record(const base_tester& tester, uint32_t block_num,
                         fc::sha256& on_chain_hash) {
      const vector<char> data = tester.get_row_by_id(
         config::system_account_name, config::system_account_name,
         name{snapshot_attestation::table_snaprecords},
         static_cast<uint64_t>(block_num));
      if (data.empty()) {
         return false;
      }

      const auto row = abi_ser.binary_to_variant(
         snapshot_attestation::type_snap_record, data,
         abi_serializer::create_yield_function(abi_serializer_max_time));
      const auto hash_bytes = row[snapshot_attestation::field::snapshot_hash].as<bytes>();
      if (hash_bytes.size() != on_chain_hash.data_size()) {
         return false;
      }
      std::memcpy(on_chain_hash.data(), hash_bytes.data(), hash_bytes.size());
      return true;
   }

   /** Validate producer/validator parity once, then close safely for block-log replay. */
   void validate_and_close() {
      BOOST_REQUIRE(validate());
      skip_validate = true;
      close();
   }
};

} // namespace snapshot_attest_test_support

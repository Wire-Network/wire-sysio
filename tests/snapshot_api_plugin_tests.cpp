#include <sysio/snapshot_api_plugin/snapshot_catalog.hpp>

#include <boost/test/unit_test.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

namespace snapshot_attest = sysio::protocol::snapshot_attestation;

BOOST_AUTO_TEST_SUITE(snapshot_api_plugin_tests)

/// Seed used to construct the expected block identifier.
constexpr auto expected_block_seed = "expected-block";

/// Seed used to construct a mismatched block identifier.
constexpr auto other_block_seed = "other-block";

/// Non-object value used to exercise fail-closed row decoding.
constexpr auto malformed_record_value = "malformed";

/// Hex digit used to construct a nonzero mismatched BLAKE3 value.
constexpr char other_hash_hex_digit = '1';

/// Number of hexadecimal characters required to encode one byte.
constexpr std::size_t hex_characters_per_byte = 2;

/// Finality distance used by the synthetic attestation row.
constexpr uint32_t attestation_delay_blocks = 10;

/** Exact scheduled serving accepts only complete, irreversible attestation rows. */
BOOST_AUTO_TEST_CASE(servable_snapshot_attestation_requires_final_exact_tuple) {
   constexpr uint32_t snapshot_block_num = snapshot_attest::block_spacing;
   constexpr uint32_t other_snapshot_block_num = snapshot_block_num + snapshot_attest::block_spacing;
   constexpr uint32_t attestation_block_num = snapshot_block_num + attestation_delay_blocks;
   const auto expected_block_id = sysio::chain::block_id_type::hash(expected_block_seed);
   const auto other_block_id = sysio::chain::block_id_type::hash(other_block_seed);
   const fc::crypto::blake3 expected_hash;
   const fc::crypto::blake3 other_hash{
      std::string(fc::crypto::blake3::byte_size * hex_characters_per_byte, other_hash_hex_digit)};

   const auto make_record = [&](uint32_t block_num,
                                const sysio::chain::block_id_type& block_id,
                                const fc::crypto::blake3& snapshot_hash,
                                uint32_t attested_at_block) {
      return fc::variant(fc::mutable_variant_object()
         (snapshot_attest::field::block_num, block_num)
         (snapshot_attest::field::block_id, block_id)
         (snapshot_attest::field::snapshot_hash, snapshot_hash.str())
         (snapshot_attest::field::attested_at_block, attested_at_block));
   };

   const auto exact_record = make_record(
      snapshot_block_num, expected_block_id, expected_hash, attestation_block_num);
   BOOST_CHECK(sysio::snapshot_api::is_servable_snapshot_attestation(
      exact_record, snapshot_block_num, expected_block_id, expected_hash,
      attestation_block_num));
   BOOST_CHECK(!sysio::snapshot_api::is_servable_snapshot_attestation(
      exact_record, snapshot_block_num, expected_block_id, expected_hash,
      attestation_block_num - 1));
   BOOST_CHECK(!sysio::snapshot_api::is_servable_snapshot_attestation(
      make_record(other_snapshot_block_num, expected_block_id, expected_hash, attestation_block_num),
      snapshot_block_num, expected_block_id, expected_hash, attestation_block_num));
   BOOST_CHECK(!sysio::snapshot_api::is_servable_snapshot_attestation(
      make_record(snapshot_block_num, other_block_id, expected_hash, attestation_block_num),
      snapshot_block_num, expected_block_id, expected_hash, attestation_block_num));
   BOOST_CHECK(!sysio::snapshot_api::is_servable_snapshot_attestation(
      make_record(snapshot_block_num, expected_block_id, other_hash, attestation_block_num),
      snapshot_block_num, expected_block_id, expected_hash, attestation_block_num));
   BOOST_CHECK(!sysio::snapshot_api::is_servable_snapshot_attestation(
      fc::variant(malformed_record_value), snapshot_block_num, expected_block_id, expected_hash,
      attestation_block_num));
   BOOST_CHECK(!sysio::snapshot_api::is_servable_snapshot_attestation(
      fc::variant(fc::mutable_variant_object()
         (snapshot_attest::field::block_num, snapshot_block_num)),
      snapshot_block_num, expected_block_id, expected_hash, attestation_block_num));
}

/** A newer manual snapshot cannot hide the newest scheduled snapshot from discovery. */
BOOST_AUTO_TEST_CASE(latest_scheduled_snapshot_skips_newer_manual_entries) {
   constexpr uint32_t first_scheduled_block = snapshot_attest::block_spacing;
   constexpr uint32_t pending_scheduled_block = snapshot_attest::block_spacing * 2;
   constexpr uint32_t newer_manual_block = pending_scheduled_block + 1;

   std::map<uint32_t, uint32_t> catalog{
      {first_scheduled_block, first_scheduled_block},
      {pending_scheduled_block, pending_scheduled_block},
      {newer_manual_block, newer_manual_block},
   };

   const auto is_attested = [](uint32_t block_num) {
      return block_num == first_scheduled_block;
   };
   auto latest = sysio::snapshot_api::find_latest_scheduled_snapshot(catalog);
   BOOST_REQUIRE(latest != catalog.rend());
   BOOST_CHECK_EQUAL(latest->first, pending_scheduled_block);

   BOOST_CHECK(sysio::snapshot_api::is_snapshot_servable(
      first_scheduled_block, first_scheduled_block, is_attested));
   BOOST_CHECK(!sysio::snapshot_api::is_snapshot_servable(
      pending_scheduled_block, pending_scheduled_block, is_attested));
   BOOST_CHECK(sysio::snapshot_api::is_snapshot_servable(
      newer_manual_block, newer_manual_block, is_attested));

   catalog.erase(first_scheduled_block);
   catalog.erase(pending_scheduled_block);
   latest = sysio::snapshot_api::find_latest_scheduled_snapshot(catalog);
   BOOST_CHECK(latest == catalog.rend());
}

BOOST_AUTO_TEST_SUITE_END()

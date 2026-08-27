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

/** Minimal catalog value used to exercise attestation-driven discovery. */
struct test_snapshot_entry {
   uint32_t                    block_num;
   sysio::chain::block_id_type block_id;
   fc::crypto::blake3          root_hash;
   bool                        available;
};

/** Build one decoded snapshot-attestation row for selection and validation tests. */
fc::variant make_attestation_record(uint32_t block_num,
                                    const sysio::chain::block_id_type& block_id,
                                    const fc::crypto::blake3& snapshot_hash,
                                    uint32_t attested_at_block) {
   return fc::variant(fc::mutable_variant_object()
      (snapshot_attest::field::block_num, block_num)
      (snapshot_attest::field::block_id, block_id)
      (snapshot_attest::field::snapshot_hash, snapshot_hash.str())
      (snapshot_attest::field::attested_at_block, attested_at_block));
}

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

   const auto exact_record = make_attestation_record(
      snapshot_block_num, expected_block_id, expected_hash, attestation_block_num);
   BOOST_CHECK(sysio::snapshot_api::is_servable_snapshot_attestation(
      exact_record, snapshot_block_num, expected_block_id, expected_hash,
      attestation_block_num));
   BOOST_CHECK(!sysio::snapshot_api::is_servable_snapshot_attestation(
      exact_record, snapshot_block_num, expected_block_id, expected_hash,
      attestation_block_num - 1));
   BOOST_CHECK(!sysio::snapshot_api::is_servable_snapshot_attestation(
      make_attestation_record(other_snapshot_block_num, expected_block_id, expected_hash, attestation_block_num),
      snapshot_block_num, expected_block_id, expected_hash, attestation_block_num));
   BOOST_CHECK(!sysio::snapshot_api::is_servable_snapshot_attestation(
      make_attestation_record(snapshot_block_num, other_block_id, expected_hash, attestation_block_num),
      snapshot_block_num, expected_block_id, expected_hash, attestation_block_num));
   BOOST_CHECK(!sysio::snapshot_api::is_servable_snapshot_attestation(
      make_attestation_record(snapshot_block_num, expected_block_id, other_hash, attestation_block_num),
      snapshot_block_num, expected_block_id, expected_hash, attestation_block_num));
   BOOST_CHECK(!sysio::snapshot_api::is_servable_snapshot_attestation(
      fc::variant(malformed_record_value), snapshot_block_num, expected_block_id, expected_hash,
      attestation_block_num));
   BOOST_CHECK(!sysio::snapshot_api::is_servable_snapshot_attestation(
      fc::variant(fc::mutable_variant_object()
         (snapshot_attest::field::block_num, snapshot_block_num)),
      snapshot_block_num, expected_block_id, expected_hash, attestation_block_num));
}

/** Discovery skips newer unattested, stale-file, and manual catalog entries. */
BOOST_AUTO_TEST_CASE(latest_servable_scheduled_snapshot_skips_unusable_entries) {
   constexpr uint32_t first_scheduled_block = snapshot_attest::block_spacing;
   constexpr uint32_t pending_scheduled_block = snapshot_attest::block_spacing * 2;
   constexpr uint32_t stale_scheduled_block = snapshot_attest::block_spacing * 3;
   constexpr uint32_t newer_manual_block = stale_scheduled_block + 1;
   constexpr uint32_t last_irreversible_block = stale_scheduled_block + attestation_delay_blocks;
   const auto first_block_id = sysio::chain::block_id_type::hash(expected_block_seed);
   const auto stale_block_id = sysio::chain::block_id_type::hash(other_block_seed);
   const fc::crypto::blake3 first_hash;
   const fc::crypto::blake3 stale_hash{
      std::string(fc::crypto::blake3::byte_size * hex_characters_per_byte, other_hash_hex_digit)};

   std::map<uint32_t, test_snapshot_entry> catalog{
      {first_scheduled_block,
       {first_scheduled_block, first_block_id, first_hash, true}},
      {pending_scheduled_block,
       {pending_scheduled_block, stale_block_id, stale_hash, true}},
      {stale_scheduled_block,
       {stale_scheduled_block, stale_block_id, stale_hash, false}},
      {newer_manual_block,
       {newer_manual_block, stale_block_id, stale_hash, true}},
   };
   const fc::variants attestations{
      make_attestation_record(stale_scheduled_block, stale_block_id, stale_hash,
                              stale_scheduled_block + attestation_delay_blocks),
      make_attestation_record(first_scheduled_block, first_block_id, first_hash,
                              first_scheduled_block + attestation_delay_blocks),
   };

   const auto is_attested = [](const test_snapshot_entry& snapshot) {
      const uint32_t block_num = snapshot.block_num;
      return block_num == first_scheduled_block;
   };
   const auto is_available = [](const test_snapshot_entry& snapshot) {
      return snapshot.available;
   };
   auto latest = sysio::snapshot_api::find_latest_servable_scheduled_snapshot(
      catalog, attestations, last_irreversible_block, is_available);
   BOOST_REQUIRE(latest != catalog.end());
   BOOST_CHECK_EQUAL(latest->first, first_scheduled_block);

   BOOST_CHECK(sysio::snapshot_api::is_snapshot_servable(
      first_scheduled_block, catalog.at(first_scheduled_block), is_attested));
   BOOST_CHECK(!sysio::snapshot_api::is_snapshot_servable(
      pending_scheduled_block, catalog.at(pending_scheduled_block), is_attested));
   BOOST_CHECK(sysio::snapshot_api::is_snapshot_servable(
      newer_manual_block, catalog.at(newer_manual_block), is_attested));

   catalog.at(first_scheduled_block).available = false;
   latest = sysio::snapshot_api::find_latest_servable_scheduled_snapshot(
      catalog, attestations, last_irreversible_block, is_available);
   BOOST_CHECK(latest == catalog.end());
}

BOOST_AUTO_TEST_SUITE_END()

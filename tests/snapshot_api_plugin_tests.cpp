#include <sysio/snapshot_api_plugin/snapshot_catalog.hpp>

#include <boost/test/unit_test.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>

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

/// Number of table pages read by the continuation scenario.
constexpr std::size_t expected_discovery_page_count = 2;

/** Minimal catalog value used to exercise attestation-driven discovery. */
struct test_snapshot_entry {
   uint32_t                    block_num;
   sysio::chain::block_id_type block_id;
   fc::crypto::blake3          root_hash;
   bool                        available;
};

/** Immutable catalog snapshot used by discovery-driver tests. */
using test_snapshot_catalog = std::map<uint32_t, test_snapshot_entry>;

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

/** Discovery continues after a filtered-empty newest page and selects the older matching row. */
BOOST_AUTO_TEST_CASE(latest_servable_scheduled_snapshot_paginates_filtered_empty_page) {
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

   const auto catalog = std::make_shared<const test_snapshot_catalog>(test_snapshot_catalog{
      {first_scheduled_block,
       {first_scheduled_block, first_block_id, first_hash, true}},
      {pending_scheduled_block,
       {pending_scheduled_block, stale_block_id, stale_hash, true}},
      {stale_scheduled_block,
       {stale_scheduled_block, stale_block_id, stale_hash, false}},
      {newer_manual_block,
       {newer_manual_block, stale_block_id, stale_hash, true}},
   });
   const auto is_attested = [](const test_snapshot_entry& snapshot) {
      const uint32_t block_num = snapshot.block_num;
      return block_num == first_scheduled_block;
   };
   const auto is_available = [](const test_snapshot_entry& snapshot) {
      return snapshot.available;
   };
   const auto is_servable_attestation = [last_irreversible_block, is_available](
                                           const auto& candidate_catalog,
                                           const fc::variant& row) {
      return sysio::snapshot_api::is_servable_catalog_snapshot_attestation(
         candidate_catalog, row, last_irreversible_block, is_available);
   };

   const auto first_page_cursor = sysio::make_snapshot_attestation_record_query(
      pending_scheduled_block).lower_bound;
   const auto expected_lower_bound = sysio::make_snapshot_attestation_record_query(
      first_scheduled_block).lower_bound;
   const auto expected_upper_bound = fc::json::to_string(
      fc::mutable_variant_object()
      (snapshot_attest::field::block_num, static_cast<uint64_t>(pending_scheduled_block) + 1),
      fc::time_point::maximum());
   std::size_t page_count = 0;
   const auto read_page = [&](const sysio::chain_apis::read_only::get_table_rows_params& params)
      -> std::optional<sysio::chain_apis::read_only::get_table_rows_result> {
      ++page_count;
      BOOST_CHECK_EQUAL(params.lower_bound, expected_lower_bound);
      BOOST_CHECK_EQUAL(params.limit, sysio::snapshot_api::snapshot_attestation_discovery_page_size);
      BOOST_REQUIRE(params.reverse);
      BOOST_CHECK(*params.reverse);
      BOOST_REQUIRE(params.filter);

      sysio::chain_apis::read_only::get_table_rows_result page;
      if (page_count == 1) {
         BOOST_CHECK_EQUAL(params.upper_bound, expected_upper_bound);
         const auto pending_record = make_attestation_record(
            pending_scheduled_block, first_block_id, first_hash,
            pending_scheduled_block + attestation_delay_blocks);
         if ((*params.filter)(pending_record)) {
            page.rows.push_back(pending_record);
         }
         page.more = true;
         page.next_key = first_page_cursor;
         return page;
      }

      BOOST_CHECK_EQUAL(params.upper_bound, first_page_cursor);
      const auto matching_record = make_attestation_record(
         first_scheduled_block, first_block_id, first_hash,
         first_scheduled_block + attestation_delay_blocks);
      if ((*params.filter)(matching_record)) {
         page.rows.push_back(matching_record);
      }
      return page;
   };
   const auto deadline_reached = []() { return false; };

   const auto latest = sysio::snapshot_api::discover_latest_servable_scheduled_snapshot(
      catalog, is_available, is_servable_attestation, read_page, deadline_reached);
   BOOST_CHECK(latest.status == sysio::snapshot_api::snapshot_discovery_status::found);
   BOOST_REQUIRE(latest.snapshot);
   BOOST_CHECK_EQUAL(latest.snapshot->block_num, first_scheduled_block);
   BOOST_CHECK_EQUAL(page_count, expected_discovery_page_count);

   BOOST_CHECK(sysio::snapshot_api::is_snapshot_servable(
      first_scheduled_block, catalog->at(first_scheduled_block), is_attested));
   BOOST_CHECK(!sysio::snapshot_api::is_snapshot_servable(
      pending_scheduled_block, catalog->at(pending_scheduled_block), is_attested));
   BOOST_CHECK(sysio::snapshot_api::is_snapshot_servable(
      newer_manual_block, catalog->at(newer_manual_block), is_attested));
}

/** Discovery distinguishes exhausted scans from read and deadline failures. */
BOOST_AUTO_TEST_CASE(latest_servable_scheduled_snapshot_reports_discovery_outcome) {
   constexpr uint32_t scheduled_block = snapshot_attest::block_spacing;
   constexpr uint32_t last_irreversible_block = scheduled_block + attestation_delay_blocks;
   const auto block_id = sysio::chain::block_id_type::hash(expected_block_seed);
   const fc::crypto::blake3 snapshot_hash;
   const auto catalog = std::make_shared<const test_snapshot_catalog>(test_snapshot_catalog{
      {scheduled_block, {scheduled_block, block_id, snapshot_hash, true}},
   });
   const auto is_available = [](const test_snapshot_entry& snapshot) {
      return snapshot.available;
   };
   const auto is_servable_attestation = [last_irreversible_block, is_available](
                                           const auto& candidate_catalog,
                                           const fc::variant& row) {
      return sysio::snapshot_api::is_servable_catalog_snapshot_attestation(
         candidate_catalog, row, last_irreversible_block, is_available);
   };
   const auto deadline_not_reached = []() { return false; };

   const auto exhausted_read = [](const auto&) {
      return std::optional{sysio::chain_apis::read_only::get_table_rows_result{}};
   };
   const auto exhausted = sysio::snapshot_api::discover_latest_servable_scheduled_snapshot(
      catalog, is_available, is_servable_attestation, exhausted_read,
      deadline_not_reached);
   BOOST_CHECK(exhausted.status == sysio::snapshot_api::snapshot_discovery_status::not_found);
   BOOST_CHECK(!exhausted.snapshot);

   const auto failed_read = [](const auto&)
      -> std::optional<sysio::chain_apis::read_only::get_table_rows_result> {
      return std::nullopt;
   };
   const auto failed = sysio::snapshot_api::discover_latest_servable_scheduled_snapshot(
      catalog, is_available, is_servable_attestation, failed_read,
      deadline_not_reached);
   BOOST_CHECK(failed.status == sysio::snapshot_api::snapshot_discovery_status::unavailable);
   BOOST_CHECK(!failed.snapshot);

   std::size_t deadline_read_count = 0;
   const auto unread_page = [&deadline_read_count](const auto&) {
      ++deadline_read_count;
      return std::optional{sysio::chain_apis::read_only::get_table_rows_result{}};
   };
   const auto deadline_reached = []() { return true; };
   const auto timed_out = sysio::snapshot_api::discover_latest_servable_scheduled_snapshot(
      catalog, is_available, is_servable_attestation, unread_page, deadline_reached);
   BOOST_CHECK(timed_out.status == sysio::snapshot_api::snapshot_discovery_status::unavailable);
   BOOST_CHECK(!timed_out.snapshot);
   BOOST_CHECK_EQUAL(deadline_read_count, 0U);
}

/** A copied table request owns its catalog and predicate after discovery returns. */
BOOST_AUTO_TEST_CASE(latest_servable_scheduled_snapshot_filter_owns_abandoned_request_state) {
   constexpr uint32_t scheduled_block = snapshot_attest::block_spacing;
   constexpr uint32_t last_irreversible_block = scheduled_block + attestation_delay_blocks;
   const auto block_id = sysio::chain::block_id_type::hash(expected_block_seed);
   const fc::crypto::blake3 snapshot_hash;
   auto catalog = std::make_shared<const test_snapshot_catalog>(test_snapshot_catalog{
      {scheduled_block, {scheduled_block, block_id, snapshot_hash, true}},
   });
   std::weak_ptr<const test_snapshot_catalog> catalog_lifetime = catalog;
   auto predicate_lifetime = std::make_shared<const bool>(true);
   std::weak_ptr<const bool> predicate_lifetime_observer = predicate_lifetime;
   const auto is_available = [](const test_snapshot_entry& snapshot) {
      return snapshot.available;
   };
   auto is_servable_attestation = [last_irreversible_block, is_available, predicate_lifetime](
                                     const auto& candidate_catalog,
                                     const fc::variant& row) {
      return *predicate_lifetime
             && sysio::snapshot_api::is_servable_catalog_snapshot_attestation(
                candidate_catalog, row, last_irreversible_block, is_available);
   };
   std::optional<sysio::chain_apis::read_only::get_table_rows_params> abandoned_request;
   const auto abandon_read = [&abandoned_request](const auto& params)
      -> std::optional<sysio::chain_apis::read_only::get_table_rows_result> {
      abandoned_request = params;
      return std::nullopt;
   };

   const auto result = sysio::snapshot_api::discover_latest_servable_scheduled_snapshot(
      catalog, is_available, std::move(is_servable_attestation), abandon_read,
      []() { return false; });
   BOOST_CHECK(result.status == sysio::snapshot_api::snapshot_discovery_status::unavailable);
   BOOST_REQUIRE(abandoned_request);
   BOOST_REQUIRE(abandoned_request->filter);

   catalog.reset();
   predicate_lifetime.reset();
   BOOST_CHECK(!catalog_lifetime.expired());
   BOOST_CHECK(!predicate_lifetime_observer.expired());
   BOOST_CHECK((*abandoned_request->filter)(make_attestation_record(
      scheduled_block, block_id, snapshot_hash, last_irreversible_block)));

   abandoned_request.reset();
   BOOST_CHECK(catalog_lifetime.expired());
   BOOST_CHECK(predicate_lifetime_observer.expired());
}

BOOST_AUTO_TEST_SUITE_END()

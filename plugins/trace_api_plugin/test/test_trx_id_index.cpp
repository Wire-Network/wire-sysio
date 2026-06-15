#include <boost/test/unit_test.hpp>
#include <fc/io/cfile.hpp>
#include <fc/io/raw.hpp>
#include <fc/crypto/sha256.hpp>

#include <sysio/trace_api/trx_id_index.hpp>
#include <sysio/trace_api/test_common.hpp>

using namespace sysio;
using namespace sysio::trace_api;
using namespace sysio::trace_api::test_common;

namespace {

// Build a sha256 whose first 8 bytes (the prefix64) equal the given value.
// On little-endian x86 this is the little-endian encoding written into bytes 0-7.
chain::transaction_id_type make_trx_id(uint64_t prefix64, uint8_t disambiguator = 0) {
   chain::transaction_id_type id;
   std::memcpy(id.data(), &prefix64, sizeof(prefix64));
   // vary the last byte so ids with the same prefix64 are still distinct objects
   id.data()[id.data_size() - 1] = disambiguator;
   return id;
}

struct trx_id_index_fixture {
   fc::temp_directory tempdir;

   std::filesystem::path index_path() const {
      return tempdir.path() / "test_trx_idx.log";
   }
};

} // namespace

BOOST_AUTO_TEST_SUITE(trx_id_index_tests)

// ---------------------------------------------------------------------------
// Writer / Reader round-trip
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(empty_writer_is_valid, trx_id_index_fixture) {
   trx_id_index_writer w;
   BOOST_REQUIRE_EQUAL(w.entry_count(), 0u);
   w.write(index_path());

   trx_id_index_reader r(index_path());
   BOOST_CHECK(r.valid());
   // empty index: any lookup should return nullopt
   BOOST_CHECK(!r.lookup(make_trx_id(0)));
   BOOST_CHECK(!r.lookup(make_trx_id(42)));
}

BOOST_FIXTURE_TEST_CASE(single_entry_round_trip, trx_id_index_fixture) {
   auto id = make_trx_id(0xDEADBEEFCAFEBABEULL);
   const uint32_t block = 12345;

   trx_id_index_writer w;
   w.add(id, block);
   w.write(index_path());

   trx_id_index_reader r(index_path());
   BOOST_REQUIRE(r.valid());
   auto result = r.lookup(id);
   BOOST_REQUIRE(result.has_value());
   BOOST_CHECK_EQUAL(*result, block);
}

BOOST_FIXTURE_TEST_CASE(multiple_entries_round_trip, trx_id_index_fixture) {
   // 20 distinct entries with distinct prefix64 values, varied block numbers
   const int N = 20;
   std::vector<std::pair<chain::transaction_id_type, uint32_t>> entries;
   trx_id_index_writer w;
   for (int i = 0; i < N; ++i) {
      auto id = make_trx_id(static_cast<uint64_t>(i) * 0x0101010101010101ULL + i, static_cast<uint8_t>(i));
      uint32_t block = 1000 + i;
      w.add(id, block);
      entries.emplace_back(id, block);
   }
   BOOST_REQUIRE_EQUAL(w.entry_count(), static_cast<size_t>(N));
   w.write(index_path());

   trx_id_index_reader r(index_path());
   BOOST_REQUIRE(r.valid());
   for (const auto& [id, expected_block] : entries) {
      auto result = r.lookup(id);
      BOOST_REQUIRE_MESSAGE(result.has_value(), "entry not found for block " << expected_block);
      BOOST_CHECK_EQUAL(*result, expected_block);
   }
}

BOOST_FIXTURE_TEST_CASE(not_found_returns_nullopt, trx_id_index_fixture) {
   trx_id_index_writer w;
   w.add(make_trx_id(0xAAAAAAAAAAAAAAAAULL), 100);
   w.write(index_path());

   trx_id_index_reader r(index_path());
   BOOST_REQUIRE(r.valid());
   // id not in the index
   BOOST_CHECK(!r.lookup(make_trx_id(0xBBBBBBBBBBBBBBBBULL)));
}

// ---------------------------------------------------------------------------
// Linear probing: two entries that hash to the same initial slot
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(linear_probing_on_prefix_collision, trx_id_index_fixture) {
   // With 2 entries, bucket_count = bit_ceil(2*2+1) = bit_ceil(5) = 8, mask = 7.
   // Both ids below have uint32_t(prefix64) & 7 == 0, so they start at slot 0.
   // The second must be linearly probed to slot 1.
   const uint64_t prefix_A = 0x0000000000000000ULL; // slot = 0 & 7 = 0
   const uint64_t prefix_B = 0x0000000000000008ULL; // slot = 8 & 7 = 0
   auto id_A = make_trx_id(prefix_A, 1);
   auto id_B = make_trx_id(prefix_B, 2);

   trx_id_index_writer w;
   w.add(id_A, 101);
   w.add(id_B, 202);
   w.write(index_path());

   trx_id_index_reader r(index_path());
   BOOST_REQUIRE(r.valid());

   auto result_A = r.lookup(id_A);
   BOOST_REQUIRE(result_A.has_value());
   BOOST_CHECK_EQUAL(*result_A, 101u);

   auto result_B = r.lookup(id_B);
   BOOST_REQUIRE(result_B.has_value());
   BOOST_CHECK_EQUAL(*result_B, 202u);
}

// ---------------------------------------------------------------------------
// Last-write-wins for duplicate prefix64
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(duplicate_prefix64_last_write_wins, trx_id_index_fixture) {
   // Two adds with the same prefix64 should result in the second's block_num
   // overwriting the first.  Combined with build_trx_id_index's per-block_num
   // dedup, this gives lookups the same "latest fork wins" semantic as the
   // linear scan in get_trx_block_number.
   const uint64_t shared_prefix = 0xCAFEBABEDEADBEEFULL;
   auto id1 = make_trx_id(shared_prefix, 1);
   auto id2 = make_trx_id(shared_prefix, 2); // same prefix64, different tail

   trx_id_index_writer w;
   w.add(id1, 10);
   w.add(id2, 20);
   w.write(index_path());

   trx_id_index_reader r(index_path());
   BOOST_REQUIRE(r.valid());

   // Both id1 and id2 share the prefix, so a lookup of either resolves the
   // single bucket — and that bucket holds the LATEST value (20).
   auto result1 = r.lookup(id1);
   BOOST_REQUIRE(result1.has_value());
   BOOST_CHECK_EQUAL(*result1, 20u);

   auto result2 = r.lookup(id2);
   BOOST_REQUIRE(result2.has_value());
   BOOST_CHECK_EQUAL(*result2, 20u);
}

// ---------------------------------------------------------------------------
// Error paths
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(missing_file_is_invalid, trx_id_index_fixture) {
   trx_id_index_reader r(tempdir.path() / "nonexistent.log");
   BOOST_CHECK(!r.valid());
}

BOOST_FIXTURE_TEST_CASE(bad_magic_is_invalid, trx_id_index_fixture) {
   // Write a file with a bad magic value
   fc::cfile f;
   f.set_file_path(index_path());
   f.open(fc::cfile::create_or_update_rw_mode);
   trx_id_index_header hdr;
   hdr.magic = 0xDEADBEEF; // wrong magic
   hdr.version = trx_id_index_header::current_version;
   hdr.bucket_count = 0;
   auto data = fc::raw::pack(hdr);
   f.write(data.data(), data.size());
   f.flush();
   f.close();

   trx_id_index_reader r(index_path());
   BOOST_CHECK(!r.valid());
}

BOOST_FIXTURE_TEST_CASE(bad_version_is_invalid, trx_id_index_fixture) {
   fc::cfile f;
   f.set_file_path(index_path());
   f.open(fc::cfile::create_or_update_rw_mode);
   trx_id_index_header hdr;
   hdr.magic = trx_id_index_header::magic_value;
   hdr.version = 99; // unsupported version
   hdr.bucket_count = 0;
   auto data = fc::raw::pack(hdr);
   f.write(data.data(), data.size());
   f.flush();
   f.close();

   trx_id_index_reader r(index_path());
   BOOST_CHECK(!r.valid());
}

// ---------------------------------------------------------------------------
// Load factor / large table
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(large_entry_set_all_found, trx_id_index_fixture) {
   // Write 200 entries. bucket_count = bit_ceil(201*2) = bit_ceil(401) = 512.
   // Load factor = 200/512 ~ 0.39 — well under 0.5.
   const int N = 200;
   std::vector<std::pair<chain::transaction_id_type, uint32_t>> entries;
   trx_id_index_writer w;
   for (int i = 0; i < N; ++i) {
      // Spread prefix64 values across the full range
      uint64_t prefix = static_cast<uint64_t>(i) * 0x9E3779B97F4A7C15ULL; // Fibonacci hashing
      auto id = make_trx_id(prefix, static_cast<uint8_t>(i & 0xFF));
      uint32_t block = 100000 + static_cast<uint32_t>(i);
      w.add(id, block);
      entries.emplace_back(id, block);
   }
   BOOST_REQUIRE_EQUAL(w.entry_count(), static_cast<size_t>(N));
   w.write(index_path());

   trx_id_index_reader r(index_path());
   BOOST_REQUIRE(r.valid());
   for (const auto& [id, expected_block] : entries) {
      auto result = r.lookup(id);
      BOOST_REQUIRE_MESSAGE(result.has_value(), "entry not found for block " << expected_block);
      BOOST_CHECK_EQUAL(*result, expected_block);
   }
}

// ---------------------------------------------------------------------------
// Defensive validation of corrupt / hostile index files
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(bucket_count_not_power_of_two_is_invalid, trx_id_index_fixture) {
   fc::cfile f;
   f.set_file_path(index_path());
   f.open(fc::cfile::create_or_update_rw_mode);
   trx_id_index_header hdr;
   hdr.bucket_count = 5; // not a power of two
   auto data = fc::raw::pack(hdr);
   f.write(data.data(), data.size());
   // Pad to expected size so the file-size check would otherwise pass.
   trx_id_bucket empty{};
   for (int i = 0; i < 5; ++i) {
      auto bdata = fc::raw::pack(empty);
      f.write(bdata.data(), bdata.size());
   }
   f.flush();
   f.close();

   trx_id_index_reader r(index_path());
   BOOST_CHECK(!r.valid());
}

BOOST_FIXTURE_TEST_CASE(bucket_count_above_cap_is_invalid, trx_id_index_fixture) {
   // Just write the header claiming an absurd bucket_count.  We intentionally
   // do NOT write the buckets payload (that would be ~4GB); the reader must
   // reject the header before attempting to allocate.
   fc::cfile f;
   f.set_file_path(index_path());
   f.open(fc::cfile::create_or_update_rw_mode);
   trx_id_index_header hdr;
   hdr.bucket_count = (1u << 29); // beyond the cap of 1<<28
   auto data = fc::raw::pack(hdr);
   f.write(data.data(), data.size());
   f.flush();
   f.close();

   trx_id_index_reader r(index_path());
   BOOST_CHECK(!r.valid());
}

BOOST_FIXTURE_TEST_CASE(file_size_mismatch_is_invalid, trx_id_index_fixture) {
   // Header claims 8 buckets but file only contains 2.
   fc::cfile f;
   f.set_file_path(index_path());
   f.open(fc::cfile::create_or_update_rw_mode);
   trx_id_index_header hdr;
   hdr.bucket_count = 8;
   auto data = fc::raw::pack(hdr);
   f.write(data.data(), data.size());
   trx_id_bucket empty{};
   for (int i = 0; i < 2; ++i) {
      auto bdata = fc::raw::pack(empty);
      f.write(bdata.data(), bdata.size());
   }
   f.flush();
   f.close();

   trx_id_index_reader r(index_path());
   BOOST_CHECK(!r.valid());
}

// Hand-craft a fully populated bucket array (load factor 1.0).  A naive probe
// loop would spin forever looking for an empty slot.  The bounded probe must
// terminate and return nullopt for a missing prefix.
BOOST_FIXTURE_TEST_CASE(lookup_terminates_on_full_table, trx_id_index_fixture) {
   constexpr uint32_t bucket_count = 8; // power of two, small for the test
   fc::cfile f;
   f.set_file_path(index_path());
   f.open(fc::cfile::create_or_update_rw_mode);
   trx_id_index_header hdr;
   hdr.bucket_count = bucket_count;
   auto hdr_data = fc::raw::pack(hdr);
   f.write(hdr_data.data(), hdr_data.size());

   // Fill EVERY bucket with a non-zero block_num and a unique non-matching
   // prefix.  Choose prefixes whose initial slot is bucket 0 so the probe
   // walks the whole table looking for prefix 0xCAFEBABE...
   for (uint32_t i = 0; i < bucket_count; ++i) {
      trx_id_bucket b{};
      // (bucket_count is a power of two, so any prefix64 value has its slot
      // determined by the low log2(bucket_count) bits — set those to 0.)
      b.prefix64  = (uint64_t{i} << 8); // all start at slot 0, distinct values
      b.block_num = 1000 + i;           // non-zero
      auto bdata = fc::raw::pack(b);
      f.write(bdata.data(), bdata.size());
   }
   f.flush();
   f.close();

   trx_id_index_reader r(index_path());
   BOOST_REQUIRE(r.valid());

   // Lookup a prefix that isn't stored.  Must terminate (not hang) and return nullopt.
   auto missing = make_trx_id(0xDEADBEEFCAFEBABEULL);
   auto result = r.lookup(missing);
   BOOST_CHECK(!result.has_value());
}

BOOST_AUTO_TEST_SUITE_END()

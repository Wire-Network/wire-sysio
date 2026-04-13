#include <boost/test/unit_test.hpp>
#include <fc/io/cfile.hpp>
#include <fc/io/raw.hpp>

#include <sysio/trace_api/abi_store.hpp>
#include <sysio/trace_api/test_common.hpp>

using namespace sysio;
using namespace sysio::trace_api;
using namespace sysio::trace_api::test_common;

namespace {

struct abi_store_fixture {
   fc::temp_directory tempdir;

   std::filesystem::path store_path() const {
      return tempdir.path() / "test_abi_store.log";
   }

   // Convenience: make a small ABI blob from a string tag
   static std::vector<char> make_abi(const std::string& tag) {
      return std::vector<char>(tag.begin(), tag.end());
   }
};

} // namespace

BOOST_AUTO_TEST_SUITE(abi_store_tests)

// ---------------------------------------------------------------------------
// Writer / Reader round-trip
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(empty_writer_is_valid, abi_store_fixture) {
   abi_store_writer w;
   BOOST_REQUIRE_EQUAL(w.entry_count(), 0u);
   w.write(store_path());

   abi_store_reader r(store_path());
   BOOST_CHECK(r.valid());
   BOOST_CHECK(!r.lookup("sysio.token"_n, 1));
}

BOOST_FIXTURE_TEST_CASE(single_entry_round_trip, abi_store_fixture) {
   auto blob = make_abi("abi-v1");
   abi_store_writer w;
   w.add("sysio.token"_n, 100, blob);
   w.write(store_path());

   abi_store_reader r(store_path());
   BOOST_REQUIRE(r.valid());

   auto result = r.lookup("sysio.token"_n, 100);
   BOOST_REQUIRE(result.has_value());
   BOOST_CHECK_EQUAL_COLLECTIONS(result->begin(), result->end(), blob.begin(), blob.end());
}

BOOST_FIXTURE_TEST_CASE(multiple_accounts_round_trip, abi_store_fixture) {
   auto blob1 = make_abi("token-abi-v1");
   auto blob2 = make_abi("eosio-abi-v1");
   abi_store_writer w;
   w.add("sysio.token"_n, 50,  blob1);
   w.add("sysio"_n,       200, blob2);
   w.write(store_path());

   abi_store_reader r(store_path());
   BOOST_REQUIRE(r.valid());

   auto r1 = r.lookup("sysio.token"_n, 50);
   BOOST_REQUIRE(r1.has_value());
   BOOST_CHECK_EQUAL_COLLECTIONS(r1->begin(), r1->end(), blob1.begin(), blob1.end());

   auto r2 = r.lookup("sysio"_n, 200);
   BOOST_REQUIRE(r2.has_value());
   BOOST_CHECK_EQUAL_COLLECTIONS(r2->begin(), r2->end(), blob2.begin(), blob2.end());
}

// ---------------------------------------------------------------------------
// ABI version selection (return the version in effect at query global_seq)
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(returns_abi_in_effect_at_global_seq, abi_store_fixture) {
   // Three ABI versions for the same account
   auto v1 = make_abi("abi-v1");
   auto v2 = make_abi("abi-v2");
   auto v3 = make_abi("abi-v3");

   abi_store_writer w;
   w.add("sysio.token"_n, 100, v1);
   w.add("sysio.token"_n, 200, v2);
   w.add("sysio.token"_n, 300, v3);
   w.write(store_path());

   abi_store_reader r(store_path());
   BOOST_REQUIRE(r.valid());

   // Before any version: not found
   BOOST_CHECK(!r.lookup("sysio.token"_n, 99));

   // Exactly at v1
   auto at100 = r.lookup("sysio.token"_n, 100);
   BOOST_REQUIRE(at100.has_value());
   BOOST_CHECK_EQUAL_COLLECTIONS(at100->begin(), at100->end(), v1.begin(), v1.end());

   // Between v1 and v2: v1 is in effect
   auto at150 = r.lookup("sysio.token"_n, 150);
   BOOST_REQUIRE(at150.has_value());
   BOOST_CHECK_EQUAL_COLLECTIONS(at150->begin(), at150->end(), v1.begin(), v1.end());

   // Exactly at v2
   auto at200 = r.lookup("sysio.token"_n, 200);
   BOOST_REQUIRE(at200.has_value());
   BOOST_CHECK_EQUAL_COLLECTIONS(at200->begin(), at200->end(), v2.begin(), v2.end());

   // After v3
   auto at500 = r.lookup("sysio.token"_n, 500);
   BOOST_REQUIRE(at500.has_value());
   BOOST_CHECK_EQUAL_COLLECTIONS(at500->begin(), at500->end(), v3.begin(), v3.end());
}

BOOST_FIXTURE_TEST_CASE(lookup_wrong_account_returns_nullopt, abi_store_fixture) {
   abi_store_writer w;
   w.add("sysio.token"_n, 100, make_abi("token-abi"));
   w.write(store_path());

   abi_store_reader r(store_path());
   BOOST_REQUIRE(r.valid());
   BOOST_CHECK(!r.lookup("sysio.msig"_n, 100)); // different account
}

// ---------------------------------------------------------------------------
// Multiple accounts share the sorted index correctly
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(accounts_do_not_bleed_into_each_other, abi_store_fixture) {
   // sysio.token has v1 at seq=100; sysio has v1 at seq=200.
   // Looking up sysio.token at seq=250 must return sysio.token's ABI, not sysio's.
   abi_store_writer w;
   w.add("sysio.token"_n, 100, make_abi("token-100"));
   w.add("sysio"_n,       200, make_abi("sysio-200"));
   w.write(store_path());

   abi_store_reader r(store_path());
   BOOST_REQUIRE(r.valid());

   auto token_result = r.lookup("sysio.token"_n, 250);
   BOOST_REQUIRE(token_result.has_value());
   BOOST_CHECK(*token_result == make_abi("token-100"));

   // sysio.token at seq=50: not found (before first version)
   BOOST_CHECK(!r.lookup("sysio.token"_n, 50));
}

// ---------------------------------------------------------------------------
// Empty ABI blob (account deleted / cleared ABI)
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(empty_blob_round_trip, abi_store_fixture) {
   abi_store_writer w;
   w.add("clearme"_n, 999, {}); // empty ABI
   w.write(store_path());

   abi_store_reader r(store_path());
   BOOST_REQUIRE(r.valid());

   auto result = r.lookup("clearme"_n, 999);
   BOOST_REQUIRE(result.has_value());
   BOOST_CHECK(result->empty());
}

// ---------------------------------------------------------------------------
// Error paths
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(missing_file_is_invalid, abi_store_fixture) {
   abi_store_reader r(tempdir.path() / "nonexistent.log");
   BOOST_CHECK(!r.valid());
}

BOOST_FIXTURE_TEST_CASE(bad_magic_is_invalid, abi_store_fixture) {
   fc::cfile f;
   f.set_file_path(store_path());
   f.open(fc::cfile::create_or_update_rw_mode);
   abi_store_header hdr;
   hdr.magic = 0xDEADBEEF;
   auto data = fc::raw::pack(hdr);
   f.write(data.data(), data.size());
   f.flush();
   f.close();

   abi_store_reader r(store_path());
   BOOST_CHECK(!r.valid());
}

BOOST_FIXTURE_TEST_CASE(bad_version_is_invalid, abi_store_fixture) {
   fc::cfile f;
   f.set_file_path(store_path());
   f.open(fc::cfile::create_or_update_rw_mode);
   abi_store_header hdr;
   hdr.version = 99;
   auto data = fc::raw::pack(hdr);
   f.write(data.data(), data.size());
   f.flush();
   f.close();

   abi_store_reader r(store_path());
   BOOST_CHECK(!r.valid());
}

// ---------------------------------------------------------------------------
// Many entries
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(many_accounts_many_versions, abi_store_fixture) {
   const int NUM_ACCOUNTS = 10;
   const int VERSIONS_PER_ACCOUNT = 5;

   abi_store_writer w;
   // Add in reverse order to verify the writer sorts correctly.
   for (int a = NUM_ACCOUNTS - 1; a >= 0; --a) {
      for (int v = VERSIONS_PER_ACCOUNT - 1; v >= 0; --v) {
         auto acct = chain::name(static_cast<uint64_t>(a + 1) * 0x10000000000000ULL);
         uint64_t seq = static_cast<uint64_t>(a * 100 + v * 10 + 1);
         w.add(acct, seq, make_abi("a" + std::to_string(a) + "v" + std::to_string(v)));
      }
   }
   BOOST_REQUIRE_EQUAL(w.entry_count(), static_cast<size_t>(NUM_ACCOUNTS * VERSIONS_PER_ACCOUNT));
   w.write(store_path());

   abi_store_reader r(store_path());
   BOOST_REQUIRE(r.valid());

   // Spot-check several (account, global_seq) lookups
   for (int a = 0; a < NUM_ACCOUNTS; ++a) {
      auto acct = chain::name(static_cast<uint64_t>(a + 1) * 0x10000000000000ULL);
      // Lookup at the exact seq of the last version for this account
      int v = VERSIONS_PER_ACCOUNT - 1;
      uint64_t seq = static_cast<uint64_t>(a * 100 + v * 10 + 1);
      auto result = r.lookup(acct, seq);
      BOOST_REQUIRE_MESSAGE(result.has_value(), "missing entry for account " << a << " version " << v);
      auto expected = make_abi("a" + std::to_string(a) + "v" + std::to_string(v));
      BOOST_CHECK_EQUAL_COLLECTIONS(result->begin(), result->end(), expected.begin(), expected.end());
   }
}

BOOST_AUTO_TEST_SUITE_END()

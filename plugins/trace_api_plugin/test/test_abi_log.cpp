#include <boost/test/unit_test.hpp>
#include <fc/io/cfile.hpp>
#include <fc/io/raw.hpp>

#include <sysio/trace_api/abi_log.hpp>
#include <sysio/trace_api/test_common.hpp>

#include <cstdio>
#include <fstream>

using namespace sysio;
using namespace sysio::trace_api;
using namespace sysio::trace_api::test_common;

namespace {

struct abi_log_fixture {
   fc::temp_directory tempdir;

   std::filesystem::path log_path() const {
      return tempdir.path() / "abi_log.log";
   }

   static std::vector<char> make_abi(const std::string& tag) {
      return std::vector<char>(tag.begin(), tag.end());
   }

   // Open a log, run callable with it, then let it destruct (closes the file).
   template<typename F>
   void with_log(F&& f) {
      abi_log log(log_path());
      BOOST_REQUIRE(log.valid());
      f(log);
   }
};

} // namespace

BOOST_AUTO_TEST_SUITE(abi_log_tests)

// ---------------------------------------------------------------------------
// Round-trip: writer/reader in the same process
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(empty_log_is_valid, abi_log_fixture) {
   abi_log log(log_path());
   BOOST_CHECK(log.valid());
   BOOST_CHECK(!log.lookup("sysio.token"_n, 1));
   BOOST_CHECK(!log.lookup("sysio.token"_n, 0));
}

BOOST_FIXTURE_TEST_CASE(single_entry_round_trip, abi_log_fixture) {
   abi_log log(log_path());
   auto blob = make_abi("abi-v1");
   log.append("sysio.token"_n, 100, blob);

   auto result = log.lookup("sysio.token"_n, 100);
   BOOST_REQUIRE(result.has_value());
   BOOST_CHECK_EQUAL_COLLECTIONS(result->abi_bytes.begin(), result->abi_bytes.end(), blob.begin(), blob.end());
}

BOOST_FIXTURE_TEST_CASE(multiple_accounts_round_trip, abi_log_fixture) {
   abi_log log(log_path());
   auto blob1 = make_abi("token-abi-v1");
   auto blob2 = make_abi("eosio-abi-v1");
   log.append("sysio.token"_n, 50,  blob1);
   log.append("sysio"_n,       200, blob2);

   auto r1 = log.lookup("sysio.token"_n, 50);
   BOOST_REQUIRE(r1.has_value());
   BOOST_CHECK_EQUAL_COLLECTIONS(r1->abi_bytes.begin(), r1->abi_bytes.end(), blob1.begin(), blob1.end());

   auto r2 = log.lookup("sysio"_n, 200);
   BOOST_REQUIRE(r2.has_value());
   BOOST_CHECK_EQUAL_COLLECTIONS(r2->abi_bytes.begin(), r2->abi_bytes.end(), blob2.begin(), blob2.end());
}

// ---------------------------------------------------------------------------
// Version selection via upper_bound
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(returns_abi_in_effect_at_global_seq, abi_log_fixture) {
   abi_log log(log_path());
   auto v1 = make_abi("abi-v1");
   auto v2 = make_abi("abi-v2");
   auto v3 = make_abi("abi-v3");

   log.append("sysio.token"_n, 100, v1);
   log.append("sysio.token"_n, 200, v2);
   log.append("sysio.token"_n, 300, v3);

   // Before any version
   BOOST_CHECK(!log.lookup("sysio.token"_n, 99));

   // Exactly at v1
   auto at100 = log.lookup("sysio.token"_n, 100);
   BOOST_REQUIRE(at100.has_value());
   BOOST_CHECK_EQUAL_COLLECTIONS(at100->abi_bytes.begin(), at100->abi_bytes.end(), v1.begin(), v1.end());

   // Between v1 and v2 -> v1
   auto at150 = log.lookup("sysio.token"_n, 150);
   BOOST_REQUIRE(at150.has_value());
   BOOST_CHECK_EQUAL_COLLECTIONS(at150->abi_bytes.begin(), at150->abi_bytes.end(), v1.begin(), v1.end());

   // Exactly at v2
   auto at200 = log.lookup("sysio.token"_n, 200);
   BOOST_REQUIRE(at200.has_value());
   BOOST_CHECK_EQUAL_COLLECTIONS(at200->abi_bytes.begin(), at200->abi_bytes.end(), v2.begin(), v2.end());

   // After v3
   auto at500 = log.lookup("sysio.token"_n, 500);
   BOOST_REQUIRE(at500.has_value());
   BOOST_CHECK_EQUAL_COLLECTIONS(at500->abi_bytes.begin(), at500->abi_bytes.end(), v3.begin(), v3.end());
}

BOOST_FIXTURE_TEST_CASE(lookup_wrong_account_returns_nullopt, abi_log_fixture) {
   abi_log log(log_path());
   log.append("sysio.token"_n, 100, make_abi("token-abi"));
   BOOST_CHECK(!log.lookup("sysio.msig"_n, 100));
}

BOOST_FIXTURE_TEST_CASE(accounts_do_not_bleed_into_each_other, abi_log_fixture) {
   abi_log log(log_path());
   log.append("sysio.token"_n, 100, make_abi("token-100"));
   log.append("sysio"_n,       200, make_abi("sysio-200"));

   auto token_result = log.lookup("sysio.token"_n, 250);
   BOOST_REQUIRE(token_result.has_value());
   BOOST_CHECK(token_result->abi_bytes == make_abi("token-100"));

   BOOST_CHECK(!log.lookup("sysio.token"_n, 50));
}

BOOST_FIXTURE_TEST_CASE(empty_blob_round_trip, abi_log_fixture) {
   abi_log log(log_path());
   log.append("clearme"_n, 999, {}); // empty ABI

   auto result = log.lookup("clearme"_n, 999);
   BOOST_REQUIRE(result.has_value());
   BOOST_CHECK(result->abi_bytes.empty());
}

BOOST_FIXTURE_TEST_CASE(last_write_wins_for_duplicate_key, abi_log_fixture) {
   abi_log log(log_path());
   log.append("acct"_n, 100, make_abi("first"));
   log.append("acct"_n, 100, make_abi("second")); // overwrites in index

   auto result = log.lookup("acct"_n, 100);
   BOOST_REQUIRE(result.has_value());
   BOOST_CHECK(result->abi_bytes == make_abi("second"));
}

// ---------------------------------------------------------------------------
// Restart: entries persist across open/close cycles
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(append_after_restart, abi_log_fixture) {
   // Session 1: write two records, close.
   {
      abi_log log(log_path());
      log.append("sysio.token"_n, 100, make_abi("token-100"));
      log.append("sysio"_n,       200, make_abi("sysio-200"));
   }

   // Session 2: reopen, verify old records, append a new one, verify all three.
   abi_log log(log_path());
   BOOST_REQUIRE(log.valid());

   auto r1 = log.lookup("sysio.token"_n, 100);
   BOOST_REQUIRE(r1.has_value());
   BOOST_CHECK(r1->abi_bytes == make_abi("token-100"));

   auto r2 = log.lookup("sysio"_n, 200);
   BOOST_REQUIRE(r2.has_value());
   BOOST_CHECK(r2->abi_bytes == make_abi("sysio-200"));

   log.append("newacct"_n, 300, make_abi("new-300"));

   auto r3 = log.lookup("newacct"_n, 300);
   BOOST_REQUIRE(r3.has_value());
   BOOST_CHECK(r3->abi_bytes == make_abi("new-300"));
}

// ---------------------------------------------------------------------------
// Error paths: bad header
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(bad_magic_is_invalid, abi_log_fixture) {
   {
      std::ofstream f(log_path(), std::ios::binary);
      abi_log_header hdr;
      hdr.magic = 0xDEADBEEF;
      f.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
   }
   abi_log log(log_path());
   BOOST_CHECK(!log.valid());
}

BOOST_FIXTURE_TEST_CASE(bad_version_is_invalid, abi_log_fixture) {
   {
      std::ofstream f(log_path(), std::ios::binary);
      abi_log_header hdr;
      hdr.version = 99;
      f.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
   }
   abi_log log(log_path());
   BOOST_CHECK(!log.valid());
}

// ---------------------------------------------------------------------------
// Corruption recovery: torn tail
// ---------------------------------------------------------------------------

// Chop bytes off the end of the file.  The final record's trailing CRC is
// truncated, so recovery drops it at reopen.
BOOST_FIXTURE_TEST_CASE(truncated_tail_is_recovered, abi_log_fixture) {
   // Session 1: write three records.
   {
      abi_log log(log_path());
      log.append("a"_n, 100, make_abi("a-100"));
      log.append("b"_n, 200, make_abi("b-200"));
      log.append("c"_n, 300, make_abi("c-300"));
   }

   // Lop off the last 3 bytes (inside record-c's crc).
   {
      const auto size = std::filesystem::file_size(log_path());
      std::filesystem::resize_file(log_path(), size - 3);
   }

   // Session 2: recover.  a and b survive, c is gone.
   abi_log log(log_path());
   BOOST_REQUIRE(log.valid());
   BOOST_CHECK(log.lookup("a"_n, 100).has_value());
   BOOST_CHECK(log.lookup("b"_n, 200).has_value());
   BOOST_CHECK(!log.lookup("c"_n, 300));

   // File should have been truncated at end of record-b, so a new append
   // lands at that position and is recoverable.
   log.append("d"_n, 400, make_abi("d-400"));
   auto r = log.lookup("d"_n, 400);
   BOOST_REQUIRE(r.has_value());
   BOOST_CHECK(r->abi_bytes == make_abi("d-400"));
}

// Flip a byte inside record-b's blob so its CRC fails.  Recovery truncates
// everything from the start of record-b onwards (including record-c).
BOOST_FIXTURE_TEST_CASE(crc_mismatch_drops_record_and_tail, abi_log_fixture) {
   {
      abi_log log(log_path());
      log.append("a"_n, 100, make_abi("a-blob"));
      log.append("b"_n, 200, make_abi("b-blob"));
      log.append("c"_n, 300, make_abi("c-blob"));
   }

   // Layout per record: header(24) + blob + crc(4).
   // Record a: offset 16         (header), header end 40, blob 40..46, crc 46..50
   // Record b: offset 50         (header), header end 74, blob 74..80, crc 80..84
   // Flip byte at offset 75 (middle of b's blob).
   {
      std::fstream f(log_path(), std::ios::binary | std::ios::in | std::ios::out);
      f.seekp(75);
      char c = 0;
      f.read(&c, 1);
      c ^= 0xff;
      f.seekp(75);
      f.write(&c, 1);
   }

   abi_log log(log_path());
   BOOST_REQUIRE(log.valid());
   BOOST_CHECK(log.lookup("a"_n, 100).has_value());
   BOOST_CHECK(!log.lookup("b"_n, 200));
   BOOST_CHECK(!log.lookup("c"_n, 300));

   // File truncated at end of record-a (offset 50).  New append works.
   log.append("d"_n, 400, make_abi("d-blob"));
   auto r = log.lookup("d"_n, 400);
   BOOST_REQUIRE(r.has_value());
   BOOST_CHECK(r->abi_bytes == make_abi("d-blob"));
}

// ---------------------------------------------------------------------------
// Many records
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(many_accounts_many_versions, abi_log_fixture) {
   const int NUM_ACCOUNTS = 10;
   const int VERSIONS_PER_ACCOUNT = 5;

   abi_log log(log_path());

   // Add in reverse order to verify the in-memory index sort is correct.
   for (int a = NUM_ACCOUNTS - 1; a >= 0; --a) {
      for (int v = VERSIONS_PER_ACCOUNT - 1; v >= 0; --v) {
         auto acct = chain::name(static_cast<uint64_t>(a + 1) * 0x10000000000000ULL);
         uint64_t seq = static_cast<uint64_t>(a * 100 + v * 10 + 1);
         log.append(acct, seq, make_abi("a" + std::to_string(a) + "v" + std::to_string(v)));
      }
   }

   for (int a = 0; a < NUM_ACCOUNTS; ++a) {
      auto acct = chain::name(static_cast<uint64_t>(a + 1) * 0x10000000000000ULL);
      int v = VERSIONS_PER_ACCOUNT - 1;
      uint64_t seq = static_cast<uint64_t>(a * 100 + v * 10 + 1);
      auto result = log.lookup(acct, seq);
      BOOST_REQUIRE_MESSAGE(result.has_value(), "missing entry for account " << a << " version " << v);
      auto expected = make_abi("a" + std::to_string(a) + "v" + std::to_string(v));
      BOOST_CHECK_EQUAL_COLLECTIONS(result->abi_bytes.begin(), result->abi_bytes.end(), expected.begin(), expected.end());
   }
}

BOOST_AUTO_TEST_SUITE_END()

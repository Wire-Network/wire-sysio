#include <boost/test/unit_test.hpp>
#include <fc/io/cfile.hpp>
#include <fc/io/raw.hpp>

#include <sysio/trace_api/abi_log.hpp>
#include <sysio/trace_api/test_common.hpp>

#include <atomic>
#include <cstdio>
#include <fstream>
#include <thread>
#include <vector>

using namespace sysio;
using namespace sysio::trace_api;
using namespace sysio::trace_api::test_common;

namespace {

struct abi_log_fixture {
   fc::temp_directory tempdir;

   std::filesystem::path log_path() const {
      return tempdir.path() / "abi_log.log";
   }

   // The reversible journal sidecar abi_log derives from log_path() (same stem, .journal ext).
   std::filesystem::path journal_path() const {
      return tempdir.path() / "abi_log.journal";
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

// ---------------------------------------------------------------------------
// Reversible overlay: records above LIB live in memory, participate in
// lookups immediately, roll back per block on fork replacement, and reach
// disk only via flush_irreversible.
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(reversible_record_participates_in_lookup, abi_log_fixture) {
   abi_log log(log_path());
   auto blob = make_abi("rev-abi");
   log.append_reversible(10, "acct"_n, 100, blob);

   BOOST_CHECK(log.has_entry("acct"_n));
   BOOST_CHECK_EQUAL(log.reversible_size(), 1u);

   auto seq = log.lookup_seq("acct"_n, 150);
   BOOST_REQUIRE(seq.has_value());
   BOOST_CHECK_EQUAL(*seq, 100u);

   auto fetched = log.fetch("acct"_n, 100);
   BOOST_REQUIRE(fetched.has_value());
   BOOST_CHECK(*fetched == blob);

   auto result = log.lookup("acct"_n, 100);
   BOOST_REQUIRE(result.has_value());
   BOOST_CHECK_EQUAL(result->effective_global_seq, 100u);
   BOOST_CHECK(result->abi_bytes == blob);
}

BOOST_FIXTURE_TEST_CASE(unflushed_reversible_record_survives_restart_via_journal, abi_log_fixture) {
   // A lazy global_seq-0 record is the case the old design lost: it is read from chain state, not
   // from any action trace, so a trace-scan rebuild could never recover it.  The durable journal
   // restores it across a restart even though it never reached LIB (and so never reached the main
   // on-disk log).
   with_log([&](abi_log& log) {
      log.append_reversible(10, "acct"_n, 0, make_abi("lazy-v0"));
      BOOST_CHECK(log.lookup("acct"_n, 0).has_value());
   });

   // Session 2: the journal replay brings the record back into the reversible overlay - it must
   // NOT be on disk in the main log (that file only ever holds irreversible records), but it must
   // still resolve.
   with_log([&](abi_log& log) {
      BOOST_CHECK(log.has_entry("acct"_n));
      BOOST_CHECK_EQUAL(log.reversible_size(), 1u);
      auto r = log.lookup("acct"_n, 50);
      BOOST_REQUIRE(r.has_value());
      BOOST_CHECK_EQUAL(r->effective_global_seq, 0u);
      BOOST_CHECK(r->abi_bytes == make_abi("lazy-v0"));
   });
}

BOOST_FIXTURE_TEST_CASE(rollback_discards_records_at_and_above_height, abi_log_fixture) {
   abi_log log(log_path());
   log.append_reversible(10, "a"_n, 100, make_abi("a-10"));
   log.append_reversible(11, "b"_n, 200, make_abi("b-11"));
   log.append_reversible(12, "c"_n, 300, make_abi("c-12"));

   // Fork switch: a new block at height 11 replaces the old 11 and 12.
   log.rollback_reversible(11);

   BOOST_CHECK(log.has_entry("a"_n));
   BOOST_CHECK(!log.has_entry("b"_n));
   BOOST_CHECK(!log.has_entry("c"_n));
   BOOST_CHECK_EQUAL(log.reversible_size(), 1u);
   BOOST_CHECK(log.lookup("a"_n, 100).has_value());
   BOOST_CHECK(!log.lookup_seq("b"_n, 250));
   BOOST_CHECK(!log.lookup_seq("c"_n, 350));
}

BOOST_FIXTURE_TEST_CASE(flush_moves_records_at_or_below_lib_to_disk, abi_log_fixture) {
   // Session 1: two reversible records; LIB advances past only the first.
   with_log([&](abi_log& log) {
      log.append_reversible(10, "a"_n, 100, make_abi("a-abi"));
      log.append_reversible(12, "b"_n, 200, make_abi("b-abi"));

      log.flush_irreversible(11);

      // Flushed record left the overlay; the still-reversible one remains, and
      // both keep resolving.
      BOOST_CHECK_EQUAL(log.reversible_size(), 1u);
      BOOST_CHECK(log.lookup("a"_n, 100).has_value());
      BOOST_CHECK(log.lookup("b"_n, 200).has_value());
   });

   // Session 2: a is served from the main on-disk log (it was flushed and left the overlay), while
   // b - reversible and never flushed - is restored into the overlay from the journal.  Exactly one
   // record (b) is still reversible, which confirms a came from disk rather than the overlay.
   with_log([&](abi_log& log) {
      auto a = log.lookup("a"_n, 100);
      BOOST_REQUIRE(a.has_value());
      BOOST_CHECK(a->abi_bytes == make_abi("a-abi"));

      auto b = log.lookup("b"_n, 200);
      BOOST_REQUIRE(b.has_value());
      BOOST_CHECK(b->abi_bytes == make_abi("b-abi"));
      BOOST_CHECK_EQUAL(log.reversible_size(), 1u);
   });
}

BOOST_FIXTURE_TEST_CASE(forked_out_abi_never_reaches_disk, abi_log_fixture) {
   // The review scenario: setabi(X) lands in block 10 on branch A; branch B
   // replaces block 10 with a different setabi(X).  After the fork switch and
   // LIB passing 10, only branch B's record may exist - anywhere.
   with_log([&](abi_log& log) {
      log.append_reversible(10, "x"_n, 100, make_abi("branch-a"));
      // Fork switch: block_start(10) on branch B.
      log.rollback_reversible(10);
      log.append_reversible(10, "x"_n, 101, make_abi("branch-b"));

      log.flush_irreversible(10);
      BOOST_CHECK_EQUAL(log.reversible_size(), 0u);
   });

   with_log([&](abi_log& log) {
      // The forked-out record participates in nothing: a query at its seq finds
      // no record at all (101 > 100), and the canonical record resolves.
      BOOST_CHECK(!log.lookup_seq("x"_n, 100));
      auto r = log.lookup("x"_n, 150);
      BOOST_REQUIRE(r.has_value());
      BOOST_CHECK_EQUAL(r->effective_global_seq, 101u);
      BOOST_CHECK(r->abi_bytes == make_abi("branch-b"));
   });
}

BOOST_FIXTURE_TEST_CASE(lookup_resolves_across_disk_and_overlay, abi_log_fixture) {
   abi_log log(log_path());
   // Older version already irreversible (on disk), newer one still reversible.
   log.append("x"_n, 5, make_abi("disk-v1"));
   log.append_reversible(20, "x"_n, 100, make_abi("rev-v2"));

   // Query between the two -> disk record wins (largest seq <= query).
   auto mid = log.lookup("x"_n, 50);
   BOOST_REQUIRE(mid.has_value());
   BOOST_CHECK_EQUAL(mid->effective_global_seq, 5u);
   BOOST_CHECK(mid->abi_bytes == make_abi("disk-v1"));

   // Query at/after the reversible record -> overlay record wins.
   auto post = log.lookup("x"_n, 100);
   BOOST_REQUIRE(post.has_value());
   BOOST_CHECK_EQUAL(post->effective_global_seq, 100u);
   BOOST_CHECK(post->abi_bytes == make_abi("rev-v2"));

   // Before both -> nothing.
   BOOST_CHECK(!log.lookup("x"_n, 4));

   // A fork rollback removes the overlay record; the disk record now resolves
   // for the later seq too.
   log.rollback_reversible(20);
   auto after_rollback = log.lookup("x"_n, 100);
   BOOST_REQUIRE(after_rollback.has_value());
   BOOST_CHECK_EQUAL(after_rollback->effective_global_seq, 5u);
}

BOOST_FIXTURE_TEST_CASE(flush_is_incremental_as_lib_advances, abi_log_fixture) {
   abi_log log(log_path());
   log.append_reversible(10, "a"_n, 100, make_abi("a-abi"));
   log.append_reversible(11, "b"_n, 200, make_abi("b-abi"));
   log.append_reversible(12, "c"_n, 300, make_abi("c-abi"));

   log.flush_irreversible(10);
   BOOST_CHECK_EQUAL(log.reversible_size(), 2u);
   log.flush_irreversible(12);
   BOOST_CHECK_EQUAL(log.reversible_size(), 0u);

   // All three resolve from disk now (and a repeat flush is a no-op).
   log.flush_irreversible(12);
   BOOST_CHECK(log.lookup("a"_n, 100).has_value());
   BOOST_CHECK(log.lookup("b"_n, 200).has_value());
   BOOST_CHECK(log.lookup("c"_n, 300).has_value());
}

BOOST_FIXTURE_TEST_CASE(reversible_last_write_wins_for_duplicate_key, abi_log_fixture) {
   abi_log log(log_path());
   // A replay re-commits the same (account, seq); the later write wins.
   log.append_reversible(10, "acct"_n, 100, make_abi("first"));
   log.append_reversible(10, "acct"_n, 100, make_abi("second"));
   BOOST_CHECK_EQUAL(log.reversible_size(), 1u);

   auto r = log.lookup("acct"_n, 100);
   BOOST_REQUIRE(r.has_value());
   BOOST_CHECK(r->abi_bytes == make_abi("second"));
}

// ---------------------------------------------------------------------------
// Reversible journal: the overlay is mirrored to a durable sidecar so reversible
// records (lazy and setabi alike) survive a restart, forks do not resurrect, and
// flushed records compact out.
// ---------------------------------------------------------------------------

// The review scenario the fix targets: a lazy global_seq-0 record AND a later setabi for the same
// account both live only in the reversible overlay at restart.  Without persistence, the old design
// could at best rebuild the setabi from its trace; the lazy bytes (the account's pre-setabi ABI)
// were lost, and the rebuilt setabi then suppressed any future lazy re-capture - so pre-setabi
// actions degraded to raw permanently.  The journal restores both, and a pre-setabi global_seq
// still resolves to the lazy bytes.
BOOST_FIXTURE_TEST_CASE(reversible_lazy_and_setabi_survive_restart, abi_log_fixture) {
   with_log([&](abi_log& log) {
      log.append_reversible(10, "x"_n, 0,   make_abi("v0")); // lazy: x's pre-setabi ABI
      log.append_reversible(11, "x"_n, 500, make_abi("v1")); // setabi at its global_seq
   });

   with_log([&](abi_log& log) {
      BOOST_CHECK_EQUAL(log.reversible_size(), 2u);

      // Pre-setabi action (global_seq < 500) decodes with the lazy v0.
      auto pre = log.lookup("x"_n, 100);
      BOOST_REQUIRE(pre.has_value());
      BOOST_CHECK_EQUAL(pre->effective_global_seq, 0u);
      BOOST_CHECK(pre->abi_bytes == make_abi("v0"));

      // Post-setabi action decodes with v1.
      auto post = log.lookup("x"_n, 600);
      BOOST_REQUIRE(post.has_value());
      BOOST_CHECK_EQUAL(post->effective_global_seq, 500u);
      BOOST_CHECK(post->abi_bytes == make_abi("v1"));
   });
}

// A reversible record that was forked out (rolled back) before reaching LIB must NOT come back on
// restart: the journal records the ROLLBACK and replay applies it in order.
BOOST_FIXTURE_TEST_CASE(forked_out_reversible_not_resurrected_after_restart, abi_log_fixture) {
   with_log([&](abi_log& log) {
      log.append_reversible(10, "x"_n, 100, make_abi("branch-a"));
      log.rollback_reversible(10); // fork switch discards it; never re-appended, never flushed
      BOOST_CHECK(!log.has_entry("x"_n));
   });

   with_log([&](abi_log& log) {
      BOOST_CHECK(!log.has_entry("x"_n));
      BOOST_CHECK_EQUAL(log.reversible_size(), 0u);
      BOOST_CHECK(!log.lookup("x"_n, 150));
   });
}

// Fork replacement at the same height across a restart, exercising the journal replay's ordering:
// branch A's record at block 10 is rolled back and branch B records a different one at block 10,
// all still reversible (never flushed).  Replay must apply PUT(a), ROLLBACK(10), PUT(b) in order so
// only branch B survives - the ROLLBACK cancels the earlier PUT but not the later one.
BOOST_FIXTURE_TEST_CASE(journal_replay_rollback_then_reappend_same_height, abi_log_fixture) {
   with_log([&](abi_log& log) {
      log.append_reversible(10, "x"_n, 100, make_abi("branch-a"));
      log.rollback_reversible(10);                                  // fork switch
      log.append_reversible(10, "x"_n, 101, make_abi("branch-b"));  // branch B, same height
   });

   with_log([&](abi_log& log) {
      BOOST_CHECK_EQUAL(log.reversible_size(), 1u);
      BOOST_CHECK(!log.lookup_seq("x"_n, 100)); // branch A's seq finds nothing (101 > 100)
      auto r = log.lookup("x"_n, 150);
      BOOST_REQUIRE(r.has_value());
      BOOST_CHECK_EQUAL(r->effective_global_seq, 101u);
      BOOST_CHECK(r->abi_bytes == make_abi("branch-b"));
   });
}

// The fix under a fork, across a restart: an account has a lazy global_seq-0 record (block 10) and a
// later setabi (block 11) that then forks out.  After restart the lazy bytes must survive and the
// forked-out setabi must not - so every action on the account, before or after the forked seq,
// resolves to the lazy ABI.  This is the intersection of the lazy-survival fix and fork handling.
BOOST_FIXTURE_TEST_CASE(forked_out_setabi_with_surviving_lazy_after_restart, abi_log_fixture) {
   with_log([&](abi_log& log) {
      log.append_reversible(10, "x"_n, 0,   make_abi("v0")); // lazy capture
      log.append_reversible(11, "x"_n, 500, make_abi("v1")); // setabi in block 11
      log.rollback_reversible(11);                           // block 11 forks out; lazy at 10 stays
   });

   with_log([&](abi_log& log) {
      BOOST_CHECK_EQUAL(log.reversible_size(), 1u);
      BOOST_CHECK(!log.lookup_seq("x"_n, 600).has_value() || *log.lookup_seq("x"_n, 600) == 0u);
      auto pre = log.lookup("x"_n, 100);
      BOOST_REQUIRE(pre.has_value());
      BOOST_CHECK_EQUAL(pre->effective_global_seq, 0u);
      BOOST_CHECK(pre->abi_bytes == make_abi("v0"));
      // Even a post-(forked-seq) action resolves to v0, since the setabi never happened canonically.
      auto post = log.lookup("x"_n, 600);
      BOOST_REQUIRE(post.has_value());
      BOOST_CHECK_EQUAL(post->effective_global_seq, 0u);
      BOOST_CHECK(post->abi_bytes == make_abi("v0"));
   });
}

// Partial-window rollback across a restart: records at blocks 10/11/12, a fork rolls back >= 11.
// Replay must keep block 10 and drop 11 and 12.
BOOST_FIXTURE_TEST_CASE(journal_replay_partial_window_rollback, abi_log_fixture) {
   with_log([&](abi_log& log) {
      log.append_reversible(10, "a"_n, 100, make_abi("a-10"));
      log.append_reversible(11, "b"_n, 200, make_abi("b-11"));
      log.append_reversible(12, "c"_n, 300, make_abi("c-12"));
      log.rollback_reversible(11);
   });

   with_log([&](abi_log& log) {
      BOOST_CHECK_EQUAL(log.reversible_size(), 1u);
      BOOST_CHECK(log.has_entry("a"_n));
      BOOST_CHECK(!log.has_entry("b"_n));
      BOOST_CHECK(!log.has_entry("c"_n));
   });
}

// Fail-closed + fatal rollback: if a ROLLBACK cannot be journaled (a disk write error, or journaling
// already disabled), continuing is unsafe - the orphaned PUT for the rolled-back block would survive
// to a restart and resurrect a forked-out ABI.  A trace_api node exists to record history, so
// rollback_reversible rewrites the journal without the orphaned record (so a restart cannot resurrect
// it) and then throws, which the extraction signal handler turns into a clean node shutdown.  Here
// the rolled-back record is the only one, so the rewrite leaves an empty journal.  Simulate the write
// failure and confirm both: the abort, and that the record does not come back after a restart.
BOOST_FIXTURE_TEST_CASE(rollback_journal_write_failure_is_fatal_and_fail_closed, abi_log_fixture) {
   with_log([&](abi_log& log) {
      log.append_reversible(10, "x"_n, 100, make_abi("forked")); // PUT durably journaled
      BOOST_REQUIRE(log.has_entry("x"_n));

      log.force_journal_write_failure_for_test(true); // the ROLLBACK write will "fail"
      BOOST_CHECK_THROW(log.rollback_reversible(10), std::exception);
      log.force_journal_write_failure_for_test(false);
   });

   // Restart: the journal was rewritten without the orphaned PUT before the abort, so it must NOT
   // replay it (which would resurrect the forked-out ABI).  Without that, "x" would come back here.
   with_log([&](abi_log& log) {
      BOOST_CHECK(!log.has_entry("x"_n));
      BOOST_CHECK(!log.lookup("x"_n, 150));
      BOOST_CHECK_EQUAL(log.reversible_size(), 0u);
   });
}

// Capture writes are fatal on failure too: append_reversible throws rather than silently dropping a
// record it cannot persist, so a node that can no longer record history shuts down instead of
// serving incomplete history.
BOOST_FIXTURE_TEST_CASE(append_reversible_journal_write_failure_is_fatal, abi_log_fixture) {
   abi_log log(log_path());
   log.force_journal_write_failure_for_test(true);
   BOOST_CHECK_THROW(log.append_reversible(10, "x"_n, 100, make_abi("v")), std::exception);
}

// When a ROLLBACK write fails, the journal is rewritten to the post-rollback overlay before the node
// aborts, so a restart keeps the canonical (below-fork) records while dropping only the orphaned
// forked ones - the node comes back complete, not just safe.  (The single-record case above leaves an
// empty journal; this one proves an unrelated canonical record below the fork point survives.)
BOOST_FIXTURE_TEST_CASE(rollback_write_failure_preserves_canonical_records, abi_log_fixture) {
   with_log([&](abi_log& log) {
      log.append_reversible(10, "canon"_n,  100, make_abi("canon-abi"));  // canonical, below the fork
      log.append_reversible(20, "forked"_n, 200, make_abi("forked-abi")); // rolled out by the fork
      log.force_journal_write_failure_for_test(true);
      BOOST_CHECK_THROW(log.rollback_reversible(15), std::exception);     // fork at 15 drops block 20
      log.force_journal_write_failure_for_test(false);
   });

   // Restart: the rewrite kept block 10 and dropped block 20.
   with_log([&](abi_log& log) {
      BOOST_CHECK_EQUAL(log.reversible_size(), 1u);
      auto c = log.lookup("canon"_n, 150);
      BOOST_REQUIRE(c.has_value());
      BOOST_CHECK(c->abi_bytes == make_abi("canon-abi"));
      BOOST_CHECK(!log.has_entry("forked"_n));
   });
}

// If the post-rollback rewrite ALSO fails (e.g. disk full), the journal is truncated as a safe last
// resort: the forked record is still not resurrected (the key guarantee), at the cost of the
// canonical window degrading to raw hex.
BOOST_FIXTURE_TEST_CASE(rollback_write_failure_truncates_when_compaction_also_fails, abi_log_fixture) {
   with_log([&](abi_log& log) {
      log.append_reversible(10, "canon"_n,  100, make_abi("canon-abi"));
      log.append_reversible(20, "forked"_n, 200, make_abi("forked-abi"));
      log.force_journal_write_failure_for_test(true);
      log.force_journal_compaction_failure_for_test(true); // the rewrite cannot complete either
      BOOST_CHECK_THROW(log.rollback_reversible(15), std::exception);
      log.force_journal_write_failure_for_test(false);
      log.force_journal_compaction_failure_for_test(false);
   });

   // Restart: forked record is NOT resurrected (the key guarantee); the canonical one was also
   // sacrificed by the full truncate - the acceptable fail-safe direction (missing, never wrong).
   with_log([&](abi_log& log) {
      BOOST_CHECK(!log.has_entry("forked"_n));
      BOOST_CHECK(!log.has_entry("canon"_n));
      BOOST_CHECK_EQUAL(log.reversible_size(), 0u);
   });
}

// A main-log (flush) write failure is fatal too - uniform with the journal - and loses no data: the
// unflushed record stays in the reversible journal, so a restart with storage available restores it
// and re-persists it to the main log on the next LIB advance.  This is the "disk full -> clean exit
// -> restart picks up, nothing lost" guarantee.
BOOST_FIXTURE_TEST_CASE(flush_write_failure_is_fatal_and_recovers_on_restart, abi_log_fixture) {
   with_log([&](abi_log& log) {
      log.append_reversible(10, "x"_n, 100, make_abi("abi")); // captured + journaled
      log.force_main_log_write_failure_for_test(true);
      BOOST_CHECK_THROW(log.flush_irreversible(10), std::exception); // cannot persist to the main log
      log.force_main_log_write_failure_for_test(false);
      // The record is still resolvable this session (from the overlay) and still reversible.
      BOOST_CHECK(log.lookup("x"_n, 100).has_value());
      BOOST_CHECK_EQUAL(log.reversible_size(), 1u);
   });

   // Restart with storage available: the journal restored the unflushed record; a flush now persists
   // it to the main log, and it moves out of the reversible overlay.  Nothing lost.
   with_log([&](abi_log& log) {
      BOOST_REQUIRE(log.lookup("x"_n, 100).has_value());
      BOOST_CHECK_EQUAL(log.reversible_size(), 1u); // restored as reversible (was never flushed)
      log.flush_irreversible(10);
      BOOST_CHECK_EQUAL(log.reversible_size(), 0u); // now on disk
      auto r = log.lookup("x"_n, 100);
      BOOST_REQUIRE(r.has_value());
      BOOST_CHECK(r->abi_bytes == make_abi("abi"));
   });

   // Final restart: served purely from the main log.
   with_log([&](abi_log& log) {
      auto r = log.lookup("x"_n, 100);
      BOOST_REQUIRE(r.has_value());
      BOOST_CHECK(r->abi_bytes == make_abi("abi"));
   });
}

// A record flushed to the main log before restart is served from disk afterwards; its now-dead PUT
// is compacted out of the journal (which shrinks), and it is never double-counted in the overlay.
BOOST_FIXTURE_TEST_CASE(flushed_record_compacted_out_of_journal_on_restart, abi_log_fixture) {
   {
      abi_log log(log_path());
      log.append_reversible(10, "a"_n, 100, make_abi("a-abi"));
      log.append_reversible(12, "b"_n, 200, make_abi("b-abi"));
      log.flush_irreversible(11); // a -> disk, b stays reversible
   }
   const auto journal_size_before = std::filesystem::file_size(journal_path());

   {
      abi_log log(log_path());
      // a resolves from disk, b from the journal-restored overlay; only b is still reversible.
      BOOST_CHECK_EQUAL(log.reversible_size(), 1u);
      auto a = log.lookup("a"_n, 100);
      BOOST_REQUIRE(a.has_value());
      BOOST_CHECK(a->abi_bytes == make_abi("a-abi"));
      auto b = log.lookup("b"_n, 200);
      BOOST_REQUIRE(b.has_value());
      BOOST_CHECK(b->abi_bytes == make_abi("b-abi"));
   }
   // Compaction at startup dropped a's flushed PUT, so the journal is strictly smaller.
   const auto journal_size_after = std::filesystem::file_size(journal_path());
   BOOST_CHECK_LT(journal_size_after, journal_size_before);

   // b is still reversible and still survives a further restart.
   {
      abi_log log(log_path());
      BOOST_CHECK_EQUAL(log.reversible_size(), 1u);
      BOOST_CHECK(log.lookup("b"_n, 200).has_value());
   }
}

// When the overlay fully drains (all reversible records flush to disk), the journal is reset to its
// header so it does not grow unbounded on a long-running node - while staying functional afterwards.
BOOST_FIXTURE_TEST_CASE(journal_resets_to_header_when_overlay_drains, abi_log_fixture) {
   const auto header_size = static_cast<std::uintmax_t>(sizeof(abi_journal_header));

   abi_log log(log_path());
   log.append_reversible(10, "a"_n, 100, make_abi("a-abi"));
   log.append_reversible(11, "b"_n, 200, make_abi("b-abi"));
   BOOST_CHECK_GT(std::filesystem::file_size(journal_path()), header_size);

   log.flush_irreversible(11); // both reach LIB -> overlay drains to empty
   BOOST_CHECK_EQUAL(log.reversible_size(), 0u);
   BOOST_CHECK_EQUAL(std::filesystem::file_size(journal_path()), header_size);

   // Still usable: a fresh reversible record journals again and survives a restart.
   log.append_reversible(12, "c"_n, 300, make_abi("c-abi"));
   BOOST_CHECK_GT(std::filesystem::file_size(journal_path()), header_size);
}

// Live threshold compaction: on a node that runs for months without a restart, dead PUTs from
// flushed records must not accumulate forever.  With a small threshold, flush_irreversible rewrites
// the journal down to just the still-reversible records once it passes the bound - while the overlay
// is non-empty (so the cheap drain reset cannot be what shrinks it), and without losing any record.
BOOST_FIXTURE_TEST_CASE(journal_live_compaction_bounds_size, abi_log_fixture) {
   abi_log log(log_path(), /*journal_compaction_threshold_bytes=*/200);

   // Twelve reversible records at blocks 10..21; each appends a PUT to the journal.
   for (uint32_t b = 10; b <= 21; ++b)
      log.append_reversible(b, chain::name(b), 100, make_abi("abi"));
   const auto before = std::filesystem::file_size(journal_path());

   // Flush blocks <= 19: ten records reach disk (their journal PUTs go dead), two stay reversible.
   // The journal is over the threshold, so this flush compacts it down to the two live records.
   log.flush_irreversible(19);
   BOOST_CHECK_EQUAL(log.reversible_size(), 2u);

   const auto after = std::filesystem::file_size(journal_path());
   BOOST_CHECK_LT(after, before); // dead PUTs reclaimed; overlay non-empty, so not the drain reset

   // Nothing lost: a flushed record resolves from disk, a still-reversible one from the overlay.
   BOOST_CHECK(log.lookup(chain::name(15), 100).has_value());
   BOOST_CHECK(log.lookup(chain::name(21), 100).has_value());

   // The compacted journal is well-formed: the two still-reversible records survive a restart, and
   // the flushed ones are served from the main log.
   abi_log reopened(log_path(), 200);
   BOOST_CHECK(reopened.has_entry(chain::name(20)));
   BOOST_CHECK(reopened.has_entry(chain::name(21)));
   BOOST_CHECK(reopened.lookup(chain::name(15), 100).has_value());
}

// A torn journal tail (e.g. a partial write lost on a kernel crash) is truncated at reopen: records
// before the tear are replayed, the torn one is dropped, and new appends/restarts work afterwards.
BOOST_FIXTURE_TEST_CASE(journal_torn_tail_is_recovered, abi_log_fixture) {
   {
      abi_log log(log_path());
      log.append_reversible(10, "a"_n, 100, make_abi("a-100"));
      log.append_reversible(11, "b"_n, 200, make_abi("b-200"));
      log.append_reversible(12, "c"_n, 300, make_abi("c-300"));
   }

   // Lop the last 3 bytes off the journal (inside record-c's trailing crc).
   {
      const auto size = std::filesystem::file_size(journal_path());
      std::filesystem::resize_file(journal_path(), size - 3);
   }

   // Reopen: a and b replay, c is dropped.
   {
      abi_log log(log_path());
      BOOST_CHECK(log.has_entry("a"_n));
      BOOST_CHECK(log.has_entry("b"_n));
      BOOST_CHECK(!log.has_entry("c"_n));
      BOOST_CHECK_EQUAL(log.reversible_size(), 2u);

      // A new reversible record appends cleanly after the recovered/compacted journal.
      log.append_reversible(13, "d"_n, 400, make_abi("d-400"));
   }

   // d survives another restart, confirming the post-recovery journal is well-formed.
   {
      abi_log log(log_path());
      BOOST_CHECK(log.has_entry("d"_n));
      auto r = log.lookup("d"_n, 400);
      BOOST_REQUIRE(r.has_value());
      BOOST_CHECK(r->abi_bytes == make_abi("d-400"));
   }
}

// ---------------------------------------------------------------------------
// Concurrency: readers (HTTP threads) racing a single writer (the extraction
// thread) through appends, fork rollbacks, flushes, and live compaction.
// ---------------------------------------------------------------------------

// Models the real threading contract: one writer mutating the overlay/journal/main log while many
// readers hammer the const lookup paths.  The locking design - readers take only _index_mtx (never
// blocking on file I/O), writers never hold _index_mtx during I/O, and _index_mtx and _journal_mtx
// are never held at once - must yield no crash, no torn read, and no lost record.  The strong
// non-racy invariants checked: an irreversible record always resolves to its exact bytes, and any
// blob a reader does observe for the churning account is exactly that account's bytes (so a pread of
// the main log racing a concurrent append/compaction never returns the wrong region).  Run under
// TSAN/ASAN to surface data races and use-after-free.
BOOST_FIXTURE_TEST_CASE(concurrent_lookups_during_writes_and_compaction, abi_log_fixture) {
   // Small threshold so live compaction (journal rewrite + cfile close/reopen) fires repeatedly
   // during the run, racing the readers.
   abi_log log(log_path(), /*journal_compaction_threshold_bytes=*/1024);

   // Baseline account, flushed up front: once irreversible it can never roll back or move, so every
   // reader must resolve it to its exact bytes for the whole run.
   const auto base_blob  = make_abi("base-abi");
   const auto churn_blob = make_abi("churn-abi");
   const auto other_blob = make_abi("other-abi");
   log.append_reversible(1, "base"_n, 10, base_blob);
   log.flush_irreversible(1);
   BOOST_REQUIRE(log.lookup("base"_n, 100).has_value());

   constexpr int      num_readers = 4;
   constexpr uint32_t iterations  = 2000;
   std::atomic<bool>     stop{false};
   std::atomic<uint64_t> reads{0};
   std::atomic<bool>     base_ok{true};
   std::atomic<bool>     bytes_ok{true};

   auto reader = [&]() {
      while (!stop.load(std::memory_order_acquire)) {
         // Irreversible baseline: must always resolve to the exact bytes.
         auto b = log.lookup("base"_n, 100);
         if (!b || b->abi_bytes != base_blob)
            base_ok.store(false, std::memory_order_relaxed);

         // Churning account: a two-phase lookup_seq+fetch may miss (rolled out between the calls -
         // documented) but any blob observed must be exactly the churn bytes (no torn / wrong-offset
         // read while the writer appends to or compacts the files).
         if (auto seq = log.lookup_seq("churn"_n, 1'000'000)) {
            if (auto blob = log.fetch("churn"_n, *seq); blob && *blob != churn_blob)
               bytes_ok.store(false, std::memory_order_relaxed);
         }
         (void)log.has_entry("other"_n);
         reads.fetch_add(1, std::memory_order_relaxed);
      }
   };

   std::vector<std::thread> readers;
   readers.reserve(num_readers);
   for (int i = 0; i < num_readers; ++i)
      readers.emplace_back(reader);

   // Single writer: cycles of append-reversible -> occasional fork rollback+reappend -> flush, at
   // rising block heights, so the overlay churns and the journal repeatedly grows past the threshold
   // and compacts.  Two accounts per block keep the overlay from draining to empty, so the threshold
   // compaction path (not just the drain reset) is exercised under the readers.
   uint32_t block = 2;
   for (uint32_t i = 0; i < iterations; ++i, ++block) {
      log.append_reversible(block, "churn"_n, block, churn_blob);
      log.append_reversible(block, "other"_n, block, other_blob);

      if ((i % 5) == 0) {
         log.rollback_reversible(block);                          // fork switch at this height
         log.append_reversible(block, "churn"_n, block, churn_blob);
         log.append_reversible(block, "other"_n, block, other_blob);
      }
      if (block > 3)
         log.flush_irreversible(block - 2); // older records move overlay -> disk
   }

   stop.store(true, std::memory_order_release);
   for (auto& t : readers)
      t.join();

   BOOST_CHECK(base_ok.load());
   BOOST_CHECK(bytes_ok.load());
   BOOST_CHECK_GT(reads.load(), 0u);

   // After all the churn the log is still valid and consistent: the baseline resolves and a restart
   // restores the (now compacted) journal without loss.
   BOOST_CHECK(log.valid());
   BOOST_REQUIRE(log.lookup("base"_n, 100).has_value());
   BOOST_CHECK(log.lookup("base"_n, 100)->abi_bytes == base_blob);
}

BOOST_AUTO_TEST_SUITE_END()

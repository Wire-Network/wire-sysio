#include <sysio/chain/transaction_dedup.hpp>
#include <sysio/chain/exceptions.hpp>

#include <boost/test/unit_test.hpp>

#include <fc/crypto/sha256.hpp>

using namespace sysio::chain;

// Helper: create a deterministic transaction id from an integer
static transaction_id_type make_id(uint64_t n) {
   return fc::sha256::hash(reinterpret_cast<const char*>(&n), sizeof(n));
}

static fc::time_point_sec make_exp(uint32_t seconds_from_epoch) {
   return fc::time_point_sec(seconds_from_epoch);
}

BOOST_AUTO_TEST_SUITE(transaction_dedup_tests)

// ---- Basic operations ----

BOOST_AUTO_TEST_CASE(record_and_lookup) {
   transaction_dedup d;
   auto id1 = make_id(1);
   auto id2 = make_id(2);
   auto exp = make_exp(100);

   BOOST_CHECK(!d.is_known(id1));
   d.record(id1, exp);
   BOOST_CHECK(d.is_known(id1));
   BOOST_CHECK(!d.is_known(id2));
   BOOST_CHECK_EQUAL(d.size(), 1u);

   d.record(id2, exp);
   BOOST_CHECK(d.is_known(id2));
   BOOST_CHECK_EQUAL(d.size(), 2u);
}

BOOST_AUTO_TEST_CASE(duplicate_rejected) {
   transaction_dedup d;
   auto id = make_id(1);
   auto exp = make_exp(100);

   d.record(id, exp);
   BOOST_CHECK_THROW(d.record(id, exp), tx_duplicate);
   BOOST_CHECK_EQUAL(d.size(), 1u);
}

BOOST_AUTO_TEST_CASE(clear_expired_basic) {
   transaction_dedup d;
   d.record(make_id(1), make_exp(10));
   d.record(make_id(2), make_exp(20));
   d.record(make_id(3), make_exp(30));
   BOOST_CHECK_EQUAL(d.size(), 3u);

   // now=15: clears entry with exp=10
   auto [removed, total] = d.clear_expired(fc::time_point(fc::seconds(15)));
   BOOST_CHECK_EQUAL(removed, 1u);
   BOOST_CHECK_EQUAL(total, 3u);
   BOOST_CHECK(!d.is_known(make_id(1)));
   BOOST_CHECK(d.is_known(make_id(2)));
   BOOST_CHECK(d.is_known(make_id(3)));
   BOOST_CHECK_EQUAL(d.size(), 2u);

   // now=25: clears entry with exp=20
   auto [removed2, total2] = d.clear_expired(fc::time_point(fc::seconds(25)));
   BOOST_CHECK_EQUAL(removed2, 1u);
   BOOST_CHECK_EQUAL(d.size(), 1u);
}

// ---- Transaction-level sessions ----

BOOST_AUTO_TEST_CASE(session_squash) {
   transaction_dedup d;
   d.record(make_id(1), make_exp(100));

   d.push_session();
   d.record(make_id(2), make_exp(100));
   BOOST_CHECK_EQUAL(d.size(), 2u);
   d.squash_session();

   // Entry persists after squash
   BOOST_CHECK(d.is_known(make_id(2)));
   BOOST_CHECK_EQUAL(d.size(), 2u);
}

BOOST_AUTO_TEST_CASE(session_undo) {
   transaction_dedup d;
   d.record(make_id(1), make_exp(100));

   d.push_session();
   d.record(make_id(2), make_exp(100));
   d.record(make_id(3), make_exp(100));
   BOOST_CHECK_EQUAL(d.size(), 3u);

   d.undo_session();
   BOOST_CHECK(d.is_known(make_id(1)));
   BOOST_CHECK(!d.is_known(make_id(2)));
   BOOST_CHECK(!d.is_known(make_id(3)));
   BOOST_CHECK_EQUAL(d.size(), 1u);
}

BOOST_AUTO_TEST_CASE(nested_sessions) {
   transaction_dedup d;
   d.record(make_id(1), make_exp(100));

   // Outer session
   d.push_session();
   d.record(make_id(2), make_exp(100));

   // Inner session
   d.push_session();
   d.record(make_id(3), make_exp(100));
   d.undo_session(); // undo inner

   BOOST_CHECK(d.is_known(make_id(2)));
   BOOST_CHECK(!d.is_known(make_id(3)));

   d.squash_session(); // squash outer
   BOOST_CHECK(d.is_known(make_id(2)));
   BOOST_CHECK_EQUAL(d.size(), 2u);
}

// ---- Block-level revisions ----

BOOST_AUTO_TEST_CASE(block_commit) {
   transaction_dedup d;

   d.start_block_revision(1);
   d.record(make_id(1), make_exp(100));
   d.record(make_id(2), make_exp(100));
   d.commit_block_revision();

   BOOST_CHECK(d.is_known(make_id(1)));
   BOOST_CHECK(d.is_known(make_id(2)));
   BOOST_CHECK_EQUAL(d.size(), 2u);
}

BOOST_AUTO_TEST_CASE(block_abort) {
   transaction_dedup d;
   d.record(make_id(0), make_exp(100)); // pre-existing entry

   d.start_block_revision(1);
   d.record(make_id(1), make_exp(100));
   d.record(make_id(2), make_exp(100));
   d.abort_block_revision();

   BOOST_CHECK(d.is_known(make_id(0)));
   BOOST_CHECK(!d.is_known(make_id(1)));
   BOOST_CHECK(!d.is_known(make_id(2)));
   BOOST_CHECK_EQUAL(d.size(), 1u);
}

BOOST_AUTO_TEST_CASE(block_abort_restores_expired) {
   transaction_dedup d;
   d.record(make_id(1), make_exp(10));
   d.record(make_id(2), make_exp(20));
   d.record(make_id(3), make_exp(30));

   d.start_block_revision(1);
   // clear_expired at now=15 removes entry 1
   d.clear_expired(fc::time_point(fc::seconds(15)));
   BOOST_CHECK(!d.is_known(make_id(1)));
   BOOST_CHECK_EQUAL(d.size(), 2u);

   d.record(make_id(4), make_exp(40));
   d.abort_block_revision();

   // Entry 1 is restored, entry 4 is removed
   BOOST_CHECK(d.is_known(make_id(1)));
   BOOST_CHECK(!d.is_known(make_id(4)));
   BOOST_CHECK_EQUAL(d.size(), 3u);
}

// ---- Pop block revision (fork switch) ----

BOOST_AUTO_TEST_CASE(pop_single_block) {
   transaction_dedup d;

   d.start_block_revision(1);
   d.record(make_id(1), make_exp(100));
   d.commit_block_revision();

   d.start_block_revision(2);
   d.record(make_id(2), make_exp(100));
   d.commit_block_revision();

   BOOST_CHECK_EQUAL(d.size(), 2u);

   // Pop block 2
   d.pop_block_revision();
   BOOST_CHECK(d.is_known(make_id(1)));
   BOOST_CHECK(!d.is_known(make_id(2)));
   BOOST_CHECK_EQUAL(d.size(), 1u);
}

BOOST_AUTO_TEST_CASE(pop_multiple_blocks) {
   transaction_dedup d;

   d.start_block_revision(1);
   d.record(make_id(1), make_exp(100));
   d.commit_block_revision();

   d.start_block_revision(2);
   d.record(make_id(2), make_exp(100));
   d.commit_block_revision();

   d.start_block_revision(3);
   d.record(make_id(3), make_exp(100));
   d.commit_block_revision();

   // Pop blocks 3 and 2 (simulating 2-block fork switch)
   d.pop_block_revision();
   d.pop_block_revision();

   BOOST_CHECK(d.is_known(make_id(1)));
   BOOST_CHECK(!d.is_known(make_id(2)));
   BOOST_CHECK(!d.is_known(make_id(3)));
   BOOST_CHECK_EQUAL(d.size(), 1u);
}

BOOST_AUTO_TEST_CASE(pop_restores_expired) {
   transaction_dedup d;
   d.record(make_id(1), make_exp(10));
   d.record(make_id(2), make_exp(20));

   // Block 1: clears entry 1 (exp=10) at now=15, adds entry 3
   d.start_block_revision(1);
   d.clear_expired(fc::time_point(fc::seconds(15)));
   d.record(make_id(3), make_exp(30));
   d.commit_block_revision();

   BOOST_CHECK(!d.is_known(make_id(1)));
   BOOST_CHECK(d.is_known(make_id(2)));
   BOOST_CHECK(d.is_known(make_id(3)));

   // Pop block 1: restores entry 1, removes entry 3
   d.pop_block_revision();

   BOOST_CHECK(d.is_known(make_id(1)));
   BOOST_CHECK(d.is_known(make_id(2)));
   BOOST_CHECK(!d.is_known(make_id(3)));
   BOOST_CHECK_EQUAL(d.size(), 2u);
}

BOOST_AUTO_TEST_CASE(pop_empty_is_noop) {
   transaction_dedup d;
   d.record(make_id(1), make_exp(100));
   d.pop_block_revision(); // no committed revisions, should be a no-op
   BOOST_CHECK(d.is_known(make_id(1)));
   BOOST_CHECK_EQUAL(d.size(), 1u);
}

// ---- commit_to_lib ----

BOOST_AUTO_TEST_CASE(commit_to_lib_trims) {
   transaction_dedup d;

   for (uint32_t i = 1; i <= 5; ++i) {
      d.start_block_revision(i);
      d.record(make_id(i), make_exp(100));
      d.commit_block_revision();
   }

   // Trim revisions at LIB=3
   d.commit_to_lib(3);

   // Blocks 1-3 are trimmed, can't be popped
   // Blocks 4-5 can still be popped
   d.pop_block_revision(); // pop block 5
   BOOST_CHECK(!d.is_known(make_id(5)));
   d.pop_block_revision(); // pop block 4
   BOOST_CHECK(!d.is_known(make_id(4)));

   // No more revisions to pop -- no-op, matches chainbase db.undo() behavior
   d.pop_block_revision();

   // Entries from blocks 1-3 are still present (committed, not undoable)
   BOOST_CHECK(d.is_known(make_id(1)));
   BOOST_CHECK(d.is_known(make_id(2)));
   BOOST_CHECK(d.is_known(make_id(3)));
}

// ---- Simulated block production with transaction sessions ----

BOOST_AUTO_TEST_CASE(block_with_transaction_sessions) {
   transaction_dedup d;

   d.start_block_revision(1);

   // Tx A succeeds
   d.push_session();
   d.record(make_id(1), make_exp(100));
   d.squash_session();

   // Tx B fails
   d.push_session();
   d.record(make_id(2), make_exp(100));
   d.undo_session();

   // Tx C succeeds
   d.push_session();
   d.record(make_id(3), make_exp(100));
   d.squash_session();

   d.commit_block_revision();

   BOOST_CHECK(d.is_known(make_id(1)));
   BOOST_CHECK(!d.is_known(make_id(2))); // was undone
   BOOST_CHECK(d.is_known(make_id(3)));
   BOOST_CHECK_EQUAL(d.size(), 2u);

   // B can be retried (not a duplicate)
   d.start_block_revision(2);
   d.push_session();
   d.record(make_id(2), make_exp(100));
   d.squash_session();
   d.commit_block_revision();

   BOOST_CHECK(d.is_known(make_id(2)));
   BOOST_CHECK_EQUAL(d.size(), 3u);
}

// ---- Fork switch with transactions: pop and re-apply ----

BOOST_AUTO_TEST_CASE(fork_switch_reapply) {
   transaction_dedup d;

   // Block 1 on fork A: records tx1 and tx2
   d.start_block_revision(1);
   d.push_session();
   d.record(make_id(1), make_exp(100));
   d.squash_session();
   d.push_session();
   d.record(make_id(2), make_exp(100));
   d.squash_session();
   d.commit_block_revision();

   // Block 2 on fork A: records tx3
   d.start_block_revision(2);
   d.push_session();
   d.record(make_id(3), make_exp(100));
   d.squash_session();
   d.commit_block_revision();

   BOOST_CHECK(d.is_known(make_id(1)));
   BOOST_CHECK(d.is_known(make_id(2)));
   BOOST_CHECK(d.is_known(make_id(3)));

   // Fork switch: pop blocks 2 and 1
   d.pop_block_revision(); // undo block 2
   d.pop_block_revision(); // undo block 1

   BOOST_CHECK(!d.is_known(make_id(1)));
   BOOST_CHECK(!d.is_known(make_id(2)));
   BOOST_CHECK(!d.is_known(make_id(3)));
   BOOST_CHECK_EQUAL(d.size(), 0u);

   // Re-apply on fork B: tx1 and tx3 are in fork B's block 1 (tx2 is not)
   d.start_block_revision(1);
   d.push_session();
   d.record(make_id(1), make_exp(100)); // no duplicate error
   d.squash_session();
   d.push_session();
   d.record(make_id(3), make_exp(100)); // no duplicate error
   d.squash_session();
   d.commit_block_revision();

   BOOST_CHECK(d.is_known(make_id(1)));
   BOOST_CHECK(!d.is_known(make_id(2)));
   BOOST_CHECK(d.is_known(make_id(3)));
   BOOST_CHECK_EQUAL(d.size(), 2u);
}

// ---- File persistence ----

BOOST_AUTO_TEST_CASE(file_round_trip) {
   auto tmp = std::filesystem::temp_directory_path() / "test_dedup.bin";

   {
      transaction_dedup d;
      d.record(make_id(1), make_exp(100));
      d.record(make_id(2), make_exp(200));
      d.record(make_id(3), make_exp(300));
      d.write_to_file(tmp);
   }

   {
      transaction_dedup d;
      BOOST_CHECK(d.read_from_file(tmp));
      BOOST_CHECK_EQUAL(d.size(), 3u);
      BOOST_CHECK(d.is_known(make_id(1)));
      BOOST_CHECK(d.is_known(make_id(2)));
      BOOST_CHECK(d.is_known(make_id(3)));
      // File is removed after successful read
      BOOST_CHECK(!std::filesystem::exists(tmp));
   }
}

BOOST_AUTO_TEST_CASE(file_missing_returns_false) {
   transaction_dedup d;
   BOOST_CHECK(!d.read_from_file("/tmp/nonexistent_dedup_file.bin"));
}

// ---- Reset ----

BOOST_AUTO_TEST_CASE(reset_clears_all) {
   transaction_dedup d;
   d.start_block_revision(1);
   d.record(make_id(1), make_exp(100));
   d.commit_block_revision();

   d.start_block_revision(2);
   d.record(make_id(2), make_exp(100));
   // pending revision active

   d.reset();

   BOOST_CHECK_EQUAL(d.size(), 0u);
   BOOST_CHECK(!d.is_known(make_id(1)));
   BOOST_CHECK(!d.is_known(make_id(2)));
   // No committed revisions -- no-op, matches chainbase db.undo() behavior
   d.pop_block_revision();
}

BOOST_AUTO_TEST_SUITE_END()

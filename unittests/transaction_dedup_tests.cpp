#include <sysio/chain/transaction_dedup.hpp>
#include <sysio/chain/exceptions.hpp>

#include <boost/test/unit_test.hpp>

#include <fc/crypto/sha256.hpp>
#include <fc/filesystem.hpp>
#include <fc/io/json.hpp>

using namespace sysio::chain;

namespace sysio::chain {
/// White-box accessor (befriended by transaction_dedup): the undo-bookkeeping vector has no
/// public surface, but its growth characteristics are a regression target -- an irreversible
/// replay records every input transaction with no undo context, and the bookkeeping must not
/// accumulate for the duration of the replay.
struct transaction_dedup_test_access {
   static size_t undo_bookkeeping_size(const transaction_dedup& d) { return d.current_added_.size(); }
};
} // namespace sysio::chain

// Helper: create a deterministic transaction id from an integer
static transaction_id_type make_id(uint64_t n) {
   return fc::sha256::hash(reinterpret_cast<const char*>(&n), sizeof(n));
}

static fc::time_point_sec make_exp(uint32_t seconds_from_epoch) {
   return fc::time_point_sec(seconds_from_epoch);
}

// Serialize the dedup set the same way controller snapshots and integrity hashing do
// (add_to_snapshot), rendered to a JSON string so tests can compare content AND order.
static std::string serialize(const transaction_dedup& d) {
   fc::mutable_variant_object storage;
   auto writer = std::make_shared<variant_snapshot_writer>(storage);
   d.add_to_snapshot(writer);
   writer->finalize();
   return fc::json::to_string(fc::variant(storage), fc::time_point::maximum());
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
   // Unique per-invocation directory to avoid races when parallel ctest runs
   // the three sys-vm variants of this test binary concurrently.
   fc::temp_directory tmp_dir;
   auto tmp = tmp_dir.path() / "test_dedup.bin";

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

BOOST_AUTO_TEST_CASE(snapshot_load_accepts_unsorted_rows) {
   // Snapshots written before the sorted index existed carry rows in insertion order.
   // read_from_snapshot hints the tree's end for the sorted current format; a wrong hint on
   // old-format rows must not affect the result: any row order must load and re-serialize in
   // canonical (expiration, id) order.
   fc::mutable_variant_object storage;
   auto writer = std::make_shared<variant_snapshot_writer>(storage);
   writer->write_section("sysio::chain::transaction_dedup", [](auto& section) {
      // Deliberately not sorted by (expiration, id)
      section.add_row(snapshot_transaction_dedup_entry{make_id(3), make_exp(300)});
      section.add_row(snapshot_transaction_dedup_entry{make_id(1), make_exp(100)});
      section.add_row(snapshot_transaction_dedup_entry{make_id(2), make_exp(200)});
   });
   writer->finalize();

   // The reader holds a reference to the variant, so it must outlive the reader.
   fc::variant snapshot_data(storage);
   transaction_dedup unsorted_load;
   unsorted_load.read_from_snapshot(std::make_shared<variant_snapshot_reader>(snapshot_data));

   BOOST_CHECK_EQUAL(unsorted_load.size(), 3u);
   BOOST_CHECK(unsorted_load.is_known(make_id(1)));
   BOOST_CHECK(unsorted_load.is_known(make_id(2)));
   BOOST_CHECK(unsorted_load.is_known(make_id(3)));

   // Canonical serialization: identical to recording the same entries directly.
   transaction_dedup recorded;
   recorded.record(make_id(1), make_exp(100));
   recorded.record(make_id(2), make_exp(200));
   recorded.record(make_id(3), make_exp(300));
   BOOST_CHECK_EQUAL(serialize(unsorted_load), serialize(recorded));
}

// Builds a dedup section carrying make_id(1) twice -- a shape no honest node serializes (ids are
// unique) but a corrupted or hand-crafted snapshot can. Silently ignoring the failed map insert
// would split the map/index invariant: the map keeps only the first row while the index keeps an
// (expiration, id) entry per row, so size() disagrees with the serialized contents and the
// phantom index entry outlives its map entry.
static fc::variant make_duplicate_id_snapshot(fc::time_point_sec second_exp) {
   fc::mutable_variant_object storage;
   auto writer = std::make_shared<variant_snapshot_writer>(storage);
   writer->write_section("sysio::chain::transaction_dedup", [&](auto& section) {
      section.add_row(snapshot_transaction_dedup_entry{make_id(1), make_exp(100)});
      section.add_row(snapshot_transaction_dedup_entry{make_id(2), make_exp(200)});
      section.add_row(snapshot_transaction_dedup_entry{make_id(1), second_exp});
   });
   writer->finalize();
   return fc::variant(storage);
}

BOOST_AUTO_TEST_CASE(snapshot_load_rejects_duplicate_ids) {
   // The dangerous shape: same id under a DIFFERENT expiration, so map and index would disagree
   // about which expiration the id carries.
   fc::variant different_exp = make_duplicate_id_snapshot(make_exp(300));
   transaction_dedup d;
   BOOST_CHECK_THROW(d.read_from_snapshot(std::make_shared<variant_snapshot_reader>(different_exp)),
                     snapshot_exception);

   // Same id under the SAME expiration is equally corrupt: each id is serialized exactly once.
   fc::variant same_exp = make_duplicate_id_snapshot(make_exp(100));
   transaction_dedup d2;
   BOOST_CHECK_THROW(d2.read_from_snapshot(std::make_shared<variant_snapshot_reader>(same_exp)),
                     snapshot_exception);
}

BOOST_AUTO_TEST_CASE(file_load_rejects_duplicate_ids) {
   fc::temp_directory tmp_dir;
   auto tmp = tmp_dir.path() / "dup_dedup.bin";
   {
      auto writer = std::make_shared<threaded_snapshot_writer>(tmp);
      writer->write_section("sysio::chain::transaction_dedup", [](auto& section) {
         section.add_row(snapshot_transaction_dedup_entry{make_id(1), make_exp(100)});
         section.add_row(snapshot_transaction_dedup_entry{make_id(1), make_exp(300)});
      });
      writer->finalize();
   }

   // The file-load path treats a corrupt file as best-effort: it must report failure and leave
   // the dedup set empty, including any state present before the attempted load.
   transaction_dedup d;
   d.record(make_id(7), make_exp(700));
   BOOST_CHECK(!d.read_from_file(tmp));
   BOOST_CHECK_EQUAL(d.size(), 0u);
   BOOST_CHECK(!d.is_known(make_id(1)));
   BOOST_CHECK(!d.is_known(make_id(7)));
}

// ---- Determinism: complete expiry pruning + canonical serialization order ----
//
// The serialized dedup set feeds calculate_integrity_hash and snapshots, so the entry set AND its
// order must be a pure function of logical chain state: independent of record order, undo path,
// and fork switches. These cases pin the two defects that broke that: clear_expired leaving
// expired entries that sat behind a longer-lived one in insertion order, and undo restoring
// entries at a different position than they originally held.

BOOST_AUTO_TEST_CASE(clear_expired_removes_all_expired) {
   // exp=50 recorded BETWEEN two longer-lived entries: insertion order must not shelter it.
   transaction_dedup d;
   d.record(make_id(1), make_exp(200));
   d.record(make_id(2), make_exp(50));
   d.record(make_id(3), make_exp(300));

   auto [removed, total] = d.clear_expired(fc::time_point(fc::seconds(100)));
   BOOST_CHECK_EQUAL(removed, 1u);
   BOOST_CHECK_EQUAL(total, 3u);
   BOOST_CHECK(d.is_known(make_id(1)));
   BOOST_CHECK(!d.is_known(make_id(2)));
   BOOST_CHECK(d.is_known(make_id(3)));
   BOOST_CHECK_EQUAL(d.size(), 2u);
}

BOOST_AUTO_TEST_CASE(clear_expired_removes_scattered_and_respects_boundary) {
   transaction_dedup d;
   d.record(make_id(1), make_exp(500));
   d.record(make_id(2), make_exp(10));
   d.record(make_id(3), make_exp(400));
   d.record(make_id(4), make_exp(20));
   d.record(make_id(5), make_exp(30));
   d.record(make_id(6), make_exp(100)); // exactly at the boundary: NOT expired (strict <)

   auto [removed, total] = d.clear_expired(fc::time_point(fc::seconds(100)));
   BOOST_CHECK_EQUAL(removed, 3u);
   BOOST_CHECK_EQUAL(total, 6u);
   BOOST_CHECK(d.is_known(make_id(1)));
   BOOST_CHECK(!d.is_known(make_id(2)));
   BOOST_CHECK(d.is_known(make_id(3)));
   BOOST_CHECK(!d.is_known(make_id(4)));
   BOOST_CHECK(!d.is_known(make_id(5)));
   BOOST_CHECK(d.is_known(make_id(6)));
   BOOST_CHECK_EQUAL(d.size(), 3u);
}

BOOST_AUTO_TEST_CASE(serialization_is_insertion_order_independent) {
   transaction_dedup a, b;
   a.record(make_id(1), make_exp(100));
   a.record(make_id(2), make_exp(50));
   a.record(make_id(3), make_exp(75));

   b.record(make_id(3), make_exp(75));
   b.record(make_id(1), make_exp(100));
   b.record(make_id(2), make_exp(50));

   BOOST_CHECK_EQUAL(serialize(a), serialize(b));
}

BOOST_AUTO_TEST_CASE(serialization_identical_after_abort) {
   transaction_dedup d;
   d.record(make_id(1), make_exp(10));
   d.record(make_id(2), make_exp(200));
   d.record(make_id(3), make_exp(20));
   const auto before = serialize(d);

   d.start_block_revision(1);
   d.clear_expired(fc::time_point(fc::seconds(100))); // removes ids 1 and 3
   d.push_session();
   d.record(make_id(4), make_exp(300));
   d.squash_session();
   d.push_session();
   d.record(make_id(5), make_exp(250));
   d.undo_session();
   d.abort_block_revision();

   BOOST_CHECK_EQUAL(serialize(d), before);
}

BOOST_AUTO_TEST_CASE(serialization_identical_after_pop) {
   transaction_dedup d;
   d.record(make_id(1), make_exp(10));
   d.record(make_id(2), make_exp(200));
   d.record(make_id(3), make_exp(20));
   const auto before = serialize(d);

   d.start_block_revision(1);
   d.clear_expired(fc::time_point(fc::seconds(100)));
   d.record(make_id(4), make_exp(300));
   d.commit_block_revision();
   BOOST_CHECK(!d.is_known(make_id(1)));
   BOOST_CHECK(d.is_known(make_id(4)));

   d.pop_block_revision();

   BOOST_CHECK_EQUAL(serialize(d), before);
}

BOOST_AUTO_TEST_CASE(serialization_path_independent_across_fork_switch) {
   // Node A applies blocks 1,2 directly. Node B applies 1,2', pops 2', then applies 2.
   // Same final logical chain -> byte-identical serialization.
   transaction_dedup a, b;
   auto apply_block1 = [&](transaction_dedup& d) {
      d.start_block_revision(1);
      d.record(make_id(1), make_exp(100));
      d.commit_block_revision();
   };
   auto apply_block2 = [&](transaction_dedup& d) {
      d.start_block_revision(2);
      d.record(make_id(2), make_exp(60));
      d.record(make_id(3), make_exp(90));
      d.commit_block_revision();
   };
   apply_block1(a);
   apply_block2(a);

   apply_block1(b);
   b.start_block_revision(2);
   b.record(make_id(9), make_exp(70)); // fork block 2'
   b.commit_block_revision();
   b.pop_block_revision();             // switch forks
   apply_block2(b);

   BOOST_CHECK_EQUAL(serialize(a), serialize(b));
}

// ---- Undo bookkeeping lifecycle ----
//
// current_added_ exists solely so sessions and block revisions can undo records. With no undo
// context active nothing can ever revert (or clear) an entry, so record() must not append --
// an irreversible replay records every input transaction that way, and appending would grow
// the vector for the entire replay (~40 bytes per replayed transaction).

BOOST_AUTO_TEST_CASE(no_undo_bookkeeping_without_undo_context) {
   transaction_dedup d;
   // Replay-style records: no block revision, no session.
   for (uint64_t i = 1; i <= 100; ++i)
      d.record(make_id(i), make_exp(100 + static_cast<uint32_t>(i)));
   BOOST_CHECK_EQUAL(d.size(), 100u);
   BOOST_CHECK_EQUAL(transaction_dedup_test_access::undo_bookkeeping_size(d), 0u);

   // The entries are still fully live: expirable and serialized canonically.
   auto [removed, total] = d.clear_expired(fc::time_point(fc::seconds(150)));
   BOOST_CHECK_EQUAL(removed, 49u); // expirations 101..149 (strict <)
   BOOST_CHECK_EQUAL(total, 100u);

   // With an undo context open, bookkeeping tracks records again and drains on commit.
   d.start_block_revision(1);
   d.record(make_id(200), make_exp(300));
   BOOST_CHECK_EQUAL(transaction_dedup_test_access::undo_bookkeeping_size(d), 1u);
   d.commit_block_revision();
   BOOST_CHECK_EQUAL(transaction_dedup_test_access::undo_bookkeeping_size(d), 0u);
   BOOST_CHECK(d.is_known(make_id(200)));
}

BOOST_AUTO_TEST_CASE(undo_session_without_block_revision_reverts_records) {
   // The controller can open a session with no block revision (transactions requiring checks
   // during irreversible replay). Records made under such a session must remain undoable even
   // though replay-style records outside any context bypass the bookkeeping.
   transaction_dedup d;
   d.record(make_id(1), make_exp(100)); // pre-session, not undoable
   d.push_session();
   d.record(make_id(2), make_exp(200));
   BOOST_CHECK_EQUAL(transaction_dedup_test_access::undo_bookkeeping_size(d), 1u);
   BOOST_CHECK(d.is_known(make_id(2)));
   d.undo_session();
   BOOST_CHECK(!d.is_known(make_id(2)));
   BOOST_CHECK(d.is_known(make_id(1)));
   BOOST_CHECK_EQUAL(d.size(), 1u);
   BOOST_CHECK_EQUAL(transaction_dedup_test_access::undo_bookkeeping_size(d), 0u);
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

#include <sysio/chain/transaction_dedup.hpp>
#include <sysio/chain/transaction_dedup_undo_index.hpp>
#include <sysio/chain/exceptions.hpp>

#include <chainbase/chainbase.hpp>

#include <boost/test/unit_test.hpp>

#include <fc/crypto/sha256.hpp>
#include <fc/filesystem.hpp>
#include <fc/io/json.hpp>

#include <algorithm>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

using namespace sysio::chain;

namespace sysio::chain {
/// White-box accessor (befriended by transaction_dedup): the undo-bookkeeping vector has no
/// public surface, but its growth characteristics are a regression target -- an irreversible
/// replay records every input transaction with no undo context, and the bookkeeping must not
/// accumulate for the duration of the replay.
struct transaction_dedup_test_access {
   /// Total entries currently tracked for undo across all open sessions. Records made with no undo
   /// session open (irreversible replay) are not tracked, so this stays 0 for them.
   static size_t undo_bookkeeping_size(const transaction_dedup& d) {
      size_t n = 0;
      for (const auto& lvl : d.levels_) n += lvl.added.size();
      return n;
   }
   /// Depth of the undo session stack. Must survive a write_to_file/read_from_file round trip so a
   /// fork switch after a clean restart can revert pre-restart reversible blocks.
   static size_t committed_revision_count(const transaction_dedup& d) { return d.levels_.size(); }

   /// Core integrity invariant: the membership map and the sorted index hold exactly the same
   /// (id, expiration) pairs. Every operation must preserve it. A future change that mutates one
   /// structure but not the other is a silent consensus hazard -- size()/serialization would
   /// disagree, or clear_expired would erase an id at the wrong expiration. Checked after each
   /// step of the randomized cross-check below.
   static bool invariant_holds(const transaction_dedup& d) {
      if (d.map_.size() != d.index_.size()) return false;
      for (const auto& [exp, id] : d.index_) {
         auto it = d.map_.find(id);
         if (it == d.map_.end() || it->second != exp) return false;
      }
      for (const auto& [id, exp] : d.map_)
         if (d.index_.find(std::make_pair(exp, id)) == d.index_.end()) return false;
      return true;
   }

   /// The sorted index in iteration order -- the exact (expiration, id) sequence add_to_snapshot
   /// serializes and calculate_integrity_hash folds over. Comparing this to an independent model
   /// pins the determinism contract directly, without going through JSON.
   static std::vector<std::pair<fc::time_point_sec, transaction_id_type>> index_order(const transaction_dedup& d) {
      return { d.index_.begin(), d.index_.end() };
   }
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

// Deterministic, platform-independent PRNG (splitmix64) so the randomized cross-check below
// reproduces exactly from its seed.
static uint64_t rng_next(uint64_t& s) {
   uint64_t z = (s += 0x9e3779b97f4a7c15ull);
   z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
   z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
   return z ^ (z >> 31);
}

// Independent reference model of the dedup's documented semantics, built from a plain std::map
// plus a stack of membership snapshots that mirrors transaction_dedup::levels_ one-for-one (the
// unified undo-session model: block revisions and transaction sessions are the same primitive).
// The randomized test drives identical operations through this and the real dedup and asserts
// they never disagree -- so a regression in the undo lifecycle or the canonical serialization
// order is caught even when no named test describes the scenario.
struct dedup_oracle {
   using membership = std::map<transaction_id_type, fc::time_point_sec>;
   struct level { int64_t revision; membership before; };
   membership         m;
   std::deque<level>  levels;       // mirrors transaction_dedup::levels_
   int64_t            revision = 0;

   void record(const transaction_id_type& id, fc::time_point_sec e) { m[id] = e; }
   void clear_expired(fc::time_point now) {
      for (auto it = m.begin(); it != m.end(); ) {
         if (now > it->second.to_time_point()) it = m.erase(it);
         else ++it;
      }
   }
   void add_undo_session() { ++revision; levels.push_back({revision, m}); }     // snapshot pre-session m
   void squash()           { if (levels.empty()) return; levels.pop_back(); --revision; }              // keep m
   void undo()             { if (levels.empty()) return; m = levels.back().before; levels.pop_back(); --revision; }
   void commit(int64_t rev){ while (!levels.empty() && levels.front().revision <= rev) levels.pop_front(); }
   // Same (expiration, id) ordering the dedup's std::set index uses, for a direct comparison.
   std::vector<std::pair<fc::time_point_sec, transaction_id_type>> index_order() const {
      std::set<std::pair<fc::time_point_sec, transaction_id_type>> s;
      for (const auto& [id, e] : m) s.emplace(e, id);
      return { s.begin(), s.end() };
   }
};

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

   d.add_undo_session();
   d.record(make_id(2), make_exp(100));
   BOOST_CHECK_EQUAL(d.size(), 2u);
   d.squash();

   // Entry persists after squash
   BOOST_CHECK(d.is_known(make_id(2)));
   BOOST_CHECK_EQUAL(d.size(), 2u);
}

BOOST_AUTO_TEST_CASE(session_undo) {
   transaction_dedup d;
   d.record(make_id(1), make_exp(100));

   d.add_undo_session();
   d.record(make_id(2), make_exp(100));
   d.record(make_id(3), make_exp(100));
   BOOST_CHECK_EQUAL(d.size(), 3u);

   d.undo();
   BOOST_CHECK(d.is_known(make_id(1)));
   BOOST_CHECK(!d.is_known(make_id(2)));
   BOOST_CHECK(!d.is_known(make_id(3)));
   BOOST_CHECK_EQUAL(d.size(), 1u);
}

BOOST_AUTO_TEST_CASE(nested_sessions) {
   transaction_dedup d;
   d.record(make_id(1), make_exp(100));

   // Outer session
   d.add_undo_session();
   d.record(make_id(2), make_exp(100));

   // Inner session
   d.add_undo_session();
   d.record(make_id(3), make_exp(100));
   d.undo(); // undo inner

   BOOST_CHECK(d.is_known(make_id(2)));
   BOOST_CHECK(!d.is_known(make_id(3)));

   d.squash(); // squash outer
   BOOST_CHECK(d.is_known(make_id(2)));
   BOOST_CHECK_EQUAL(d.size(), 2u);
}

// ---- Block-level revisions ----

BOOST_AUTO_TEST_CASE(block_commit) {
   transaction_dedup d;

   d.add_undo_session();
   d.record(make_id(1), make_exp(100));
   d.record(make_id(2), make_exp(100));
   

   BOOST_CHECK(d.is_known(make_id(1)));
   BOOST_CHECK(d.is_known(make_id(2)));
   BOOST_CHECK_EQUAL(d.size(), 2u);
}

BOOST_AUTO_TEST_CASE(block_abort) {
   transaction_dedup d;
   d.record(make_id(0), make_exp(100)); // pre-existing entry

   d.add_undo_session();
   d.record(make_id(1), make_exp(100));
   d.record(make_id(2), make_exp(100));
   d.undo();

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

   d.add_undo_session();
   // clear_expired at now=15 removes entry 1
   d.clear_expired(fc::time_point(fc::seconds(15)));
   BOOST_CHECK(!d.is_known(make_id(1)));
   BOOST_CHECK_EQUAL(d.size(), 2u);

   d.record(make_id(4), make_exp(40));
   d.undo();

   // Entry 1 is restored, entry 4 is removed
   BOOST_CHECK(d.is_known(make_id(1)));
   BOOST_CHECK(!d.is_known(make_id(4)));
   BOOST_CHECK_EQUAL(d.size(), 3u);
}

// ---- Pop block revision (fork switch) ----

BOOST_AUTO_TEST_CASE(pop_single_block) {
   transaction_dedup d;

   d.add_undo_session();
   d.record(make_id(1), make_exp(100));
   

   d.add_undo_session();
   d.record(make_id(2), make_exp(100));
   

   BOOST_CHECK_EQUAL(d.size(), 2u);

   // Pop block 2
   d.undo();
   BOOST_CHECK(d.is_known(make_id(1)));
   BOOST_CHECK(!d.is_known(make_id(2)));
   BOOST_CHECK_EQUAL(d.size(), 1u);
}

BOOST_AUTO_TEST_CASE(pop_multiple_blocks) {
   transaction_dedup d;

   d.add_undo_session();
   d.record(make_id(1), make_exp(100));
   

   d.add_undo_session();
   d.record(make_id(2), make_exp(100));
   

   d.add_undo_session();
   d.record(make_id(3), make_exp(100));
   

   // Pop blocks 3 and 2 (simulating 2-block fork switch)
   d.undo();
   d.undo();

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
   d.add_undo_session();
   d.clear_expired(fc::time_point(fc::seconds(15)));
   d.record(make_id(3), make_exp(30));
   

   BOOST_CHECK(!d.is_known(make_id(1)));
   BOOST_CHECK(d.is_known(make_id(2)));
   BOOST_CHECK(d.is_known(make_id(3)));

   // Pop block 1: restores entry 1, removes entry 3
   d.undo();

   BOOST_CHECK(d.is_known(make_id(1)));
   BOOST_CHECK(d.is_known(make_id(2)));
   BOOST_CHECK(!d.is_known(make_id(3)));
   BOOST_CHECK_EQUAL(d.size(), 2u);
}

BOOST_AUTO_TEST_CASE(pop_empty_is_noop) {
   transaction_dedup d;
   d.record(make_id(1), make_exp(100));
   d.undo(); // no committed revisions, should be a no-op
   BOOST_CHECK(d.is_known(make_id(1)));
   BOOST_CHECK_EQUAL(d.size(), 1u);
}

// ---- commit_to_lib ----

BOOST_AUTO_TEST_CASE(commit_to_lib_trims) {
   transaction_dedup d;

   for (uint32_t i = 1; i <= 5; ++i) {
      d.add_undo_session();
      d.record(make_id(i), make_exp(100));
      
   }

   // Trim revisions at LIB=3
   d.commit(3);

   // Blocks 1-3 are trimmed, can't be popped
   // Blocks 4-5 can still be popped
   d.undo(); // pop block 5
   BOOST_CHECK(!d.is_known(make_id(5)));
   d.undo(); // pop block 4
   BOOST_CHECK(!d.is_known(make_id(4)));

   // No more revisions to pop -- no-op, matches chainbase db.undo() behavior
   d.undo();

   // Entries from blocks 1-3 are still present (committed, not undoable)
   BOOST_CHECK(d.is_known(make_id(1)));
   BOOST_CHECK(d.is_known(make_id(2)));
   BOOST_CHECK(d.is_known(make_id(3)));
}

// ---- Simulated block production with transaction sessions ----

BOOST_AUTO_TEST_CASE(block_with_transaction_sessions) {
   transaction_dedup d;

   d.add_undo_session();

   // Tx A succeeds
   d.add_undo_session();
   d.record(make_id(1), make_exp(100));
   d.squash();

   // Tx B fails
   d.add_undo_session();
   d.record(make_id(2), make_exp(100));
   d.undo();

   // Tx C succeeds
   d.add_undo_session();
   d.record(make_id(3), make_exp(100));
   d.squash();

   

   BOOST_CHECK(d.is_known(make_id(1)));
   BOOST_CHECK(!d.is_known(make_id(2))); // was undone
   BOOST_CHECK(d.is_known(make_id(3)));
   BOOST_CHECK_EQUAL(d.size(), 2u);

   // B can be retried (not a duplicate)
   d.add_undo_session();
   d.add_undo_session();
   d.record(make_id(2), make_exp(100));
   d.squash();
   

   BOOST_CHECK(d.is_known(make_id(2)));
   BOOST_CHECK_EQUAL(d.size(), 3u);
}

// ---- Fork switch with transactions: pop and re-apply ----

BOOST_AUTO_TEST_CASE(fork_switch_reapply) {
   transaction_dedup d;

   // Block 1 on fork A: records tx1 and tx2
   d.add_undo_session();
   d.add_undo_session();
   d.record(make_id(1), make_exp(100));
   d.squash();
   d.add_undo_session();
   d.record(make_id(2), make_exp(100));
   d.squash();
   

   // Block 2 on fork A: records tx3
   d.add_undo_session();
   d.add_undo_session();
   d.record(make_id(3), make_exp(100));
   d.squash();
   

   BOOST_CHECK(d.is_known(make_id(1)));
   BOOST_CHECK(d.is_known(make_id(2)));
   BOOST_CHECK(d.is_known(make_id(3)));

   // Fork switch: pop blocks 2 and 1
   d.undo(); // undo block 2
   d.undo(); // undo block 1

   BOOST_CHECK(!d.is_known(make_id(1)));
   BOOST_CHECK(!d.is_known(make_id(2)));
   BOOST_CHECK(!d.is_known(make_id(3)));
   BOOST_CHECK_EQUAL(d.size(), 0u);

   // Re-apply on fork B: tx1 and tx3 are in fork B's block 1 (tx2 is not)
   d.add_undo_session();
   d.add_undo_session();
   d.record(make_id(1), make_exp(100)); // no duplicate error
   d.squash();
   d.add_undo_session();
   d.record(make_id(3), make_exp(100)); // no duplicate error
   d.squash();
   

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

// Regression: the committed reversible undo stack must survive a clean restart, exactly like
// chainbase's persisted undo stack. Before the fix, write_to_file/read_from_file restored only
// the membership set, leaving committed_revisions_ empty -- so pop_block_revision (run by a fork
// switch over pre-restart reversible blocks) was a silent no-op while db.undo() reverted chainbase,
// stranding the popped blocks' transaction ids in the dedup set and rejecting the canonical fork
// with tx_duplicate.
BOOST_AUTO_TEST_CASE(file_round_trip_preserves_revision_stack) {
   fc::temp_directory tmp_dir;
   auto tmp = tmp_dir.path() / "test_dedup_revisions.bin";

   {
      transaction_dedup d;
      // Two irreversible/base entries recorded with no revision context.
      d.record(make_id(1), make_exp(10));
      d.record(make_id(2), make_exp(20));

      // Block 1: expires entry 1 (exp=10) at now=15 and adds entry 3 -- populates BOTH the added
      // and expired lists of a committed revision, the exact data that must round-trip.
      d.add_undo_session();
      d.clear_expired(fc::time_point(fc::seconds(15)));
      d.record(make_id(3), make_exp(30));
      

      // Block 2: adds entry 4.
      d.add_undo_session();
      d.record(make_id(4), make_exp(40));
      

      BOOST_CHECK_EQUAL(transaction_dedup_test_access::committed_revision_count(d), 2u);
      d.write_to_file(tmp);
   }

   transaction_dedup d;
   BOOST_CHECK(d.read_from_file(tmp));

   // Membership matches the pre-shutdown head state...
   BOOST_CHECK(!d.is_known(make_id(1))); // expired in block 1
   BOOST_CHECK(d.is_known(make_id(2)));
   BOOST_CHECK(d.is_known(make_id(3)));
   BOOST_CHECK(d.is_known(make_id(4)));
   // ...and so does the reversible undo stack.
   BOOST_CHECK_EQUAL(transaction_dedup_test_access::committed_revision_count(d), 2u);

   // Fork switch pops block 2: removes entry 4. (No-op before the fix.)
   d.undo();
   BOOST_CHECK(!d.is_known(make_id(4)));
   BOOST_CHECK(d.is_known(make_id(3)));

   // Fork switch pops block 1: removes entry 3 AND restores the entry it expired (entry 1) --
   // proves both lists of the committed revision survived the round trip, not just block_num.
   d.undo();
   BOOST_CHECK(!d.is_known(make_id(3)));
   BOOST_CHECK(d.is_known(make_id(1)));
   BOOST_CHECK(d.is_known(make_id(2)));
   BOOST_CHECK_EQUAL(d.size(), 2u);
}

// Regression: a restored instance must be pop-for-pop equivalent to one that never restarted.
// This is the property the restart bug violated -- the restarted node diverged from its peers on
// the very next fork switch. Build two identical instances, serialize/restore one, then drive both
// through the same fork-switch pops and assert byte-identical serialized state at every step.
BOOST_AUTO_TEST_CASE(restart_is_pop_equivalent_to_no_restart) {
   fc::temp_directory tmp_dir;
   auto tmp = tmp_dir.path() / "test_dedup_equiv.bin";

   auto build = [](transaction_dedup& d) {
      d.record(make_id(100), make_exp(50));
      for (uint32_t b = 1; b <= 3; ++b) {
         d.add_undo_session();
         d.clear_expired(fc::time_point(fc::seconds(5 * b))); // nothing expires (exp=50), exercises the path
         d.record(make_id(b), make_exp(50));
         
      }
   };

   transaction_dedup control;
   build(control);

   {
      transaction_dedup d;
      build(d);
      d.write_to_file(tmp);
   }
   transaction_dedup restored;
   BOOST_CHECK(restored.read_from_file(tmp));

   BOOST_CHECK_EQUAL(serialize(restored), serialize(control));

   // Two-block fork switch on both; serialized state must stay identical throughout.
   control.undo();
   restored.undo();
   BOOST_CHECK_EQUAL(serialize(restored), serialize(control));

   control.undo();
   restored.undo();
   BOOST_CHECK_EQUAL(serialize(restored), serialize(control));
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

   d.add_undo_session();
   d.clear_expired(fc::time_point(fc::seconds(100))); // removes ids 1 and 3
   d.add_undo_session();
   d.record(make_id(4), make_exp(300));
   d.squash();
   d.add_undo_session();
   d.record(make_id(5), make_exp(250));
   d.undo();
   d.undo();

   BOOST_CHECK_EQUAL(serialize(d), before);
}

BOOST_AUTO_TEST_CASE(serialization_identical_after_pop) {
   transaction_dedup d;
   d.record(make_id(1), make_exp(10));
   d.record(make_id(2), make_exp(200));
   d.record(make_id(3), make_exp(20));
   const auto before = serialize(d);

   d.add_undo_session();
   d.clear_expired(fc::time_point(fc::seconds(100)));
   d.record(make_id(4), make_exp(300));
   
   BOOST_CHECK(!d.is_known(make_id(1)));
   BOOST_CHECK(d.is_known(make_id(4)));

   d.undo();

   BOOST_CHECK_EQUAL(serialize(d), before);
}

BOOST_AUTO_TEST_CASE(serialization_path_independent_across_fork_switch) {
   // Node A applies blocks 1,2 directly. Node B applies 1,2', pops 2', then applies 2.
   // Same final logical chain -> byte-identical serialization.
   transaction_dedup a, b;
   auto apply_block1 = [&](transaction_dedup& d) {
      d.add_undo_session();
      d.record(make_id(1), make_exp(100));
      
   };
   auto apply_block2 = [&](transaction_dedup& d) {
      d.add_undo_session();
      d.record(make_id(2), make_exp(60));
      d.record(make_id(3), make_exp(90));
      
   };
   apply_block1(a);
   apply_block2(a);

   apply_block1(b);
   b.add_undo_session();
   b.record(make_id(9), make_exp(70)); // fork block 2'
   
   b.undo();             // switch forks
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

   // With an undo session open, records ARE tracked for undo, and stay tracked (reversible) until
   // the session becomes irreversible at LIB.
   d.add_undo_session();
   d.record(make_id(200), make_exp(300));
   BOOST_CHECK_EQUAL(transaction_dedup_test_access::undo_bookkeeping_size(d), 1u);
      // the block's undo session remains on the stack -- still reversible
   BOOST_CHECK_EQUAL(transaction_dedup_test_access::undo_bookkeeping_size(d), 1u);
   d.commit(1);          // now irreversible: tracking is dropped, the entry stays
   BOOST_CHECK_EQUAL(transaction_dedup_test_access::undo_bookkeeping_size(d), 0u);
   BOOST_CHECK(d.is_known(make_id(200)));
}

BOOST_AUTO_TEST_CASE(undo_session_without_block_revision_reverts_records) {
   // The controller can open a session with no block revision (transactions requiring checks
   // during irreversible replay). Records made under such a session must remain undoable even
   // though replay-style records outside any context bypass the bookkeeping.
   transaction_dedup d;
   d.record(make_id(1), make_exp(100)); // pre-session, not undoable
   d.add_undo_session();
   d.record(make_id(2), make_exp(200));
   BOOST_CHECK_EQUAL(transaction_dedup_test_access::undo_bookkeeping_size(d), 1u);
   BOOST_CHECK(d.is_known(make_id(2)));
   d.undo();
   BOOST_CHECK(!d.is_known(make_id(2)));
   BOOST_CHECK(d.is_known(make_id(1)));
   BOOST_CHECK_EQUAL(d.size(), 1u);
   BOOST_CHECK_EQUAL(transaction_dedup_test_access::undo_bookkeeping_size(d), 0u);
}

// ---- Reset ----

BOOST_AUTO_TEST_CASE(reset_clears_all) {
   transaction_dedup d;
   d.add_undo_session();
   d.record(make_id(1), make_exp(100));
   

   d.add_undo_session();
   d.record(make_id(2), make_exp(100));
   // pending revision active

   d.reset();

   BOOST_CHECK_EQUAL(d.size(), 0u);
   BOOST_CHECK(!d.is_known(make_id(1)));
   BOOST_CHECK(!d.is_known(make_id(2)));
   // No committed revisions -- no-op, matches chainbase db.undo() behavior
   d.undo();
}

// ============================================================================
// Exhaustive scenario coverage + regression guards (added 2026-06-11).
// ============================================================================

// ---- Internal map/index invariant ----

BOOST_AUTO_TEST_CASE(invariant_holds_through_basic_ops) {
   using acc = transaction_dedup_test_access;
   transaction_dedup d;
   BOOST_CHECK(acc::invariant_holds(d));
   d.record(make_id(1), make_exp(100));
   d.record(make_id(2), make_exp(50));
   d.record(make_id(3), make_exp(150));
   BOOST_CHECK(acc::invariant_holds(d));
   d.clear_expired(fc::time_point(fc::seconds(120)));   // removes id 2 (exp 50) and id 1 (exp 100)
   BOOST_CHECK(acc::invariant_holds(d));
   auto order = acc::index_order(d);
   BOOST_REQUIRE_EQUAL(order.size(), 1u);
   BOOST_CHECK(d.is_known(make_id(3)));
}

// ---- clear_expired exhaustive boundaries ----

BOOST_AUTO_TEST_CASE(clear_expired_exact_second_boundary) {
   // Removal is strict (block_time > expiration). At equality the entry is still live -- it can be
   // included in a block at exactly its expiration second -- so it must survive.
   transaction_dedup d;
   d.record(make_id(99),  make_exp(99));
   d.record(make_id(100), make_exp(100));
   d.record(make_id(101), make_exp(101));
   auto [removed, total] = d.clear_expired(fc::time_point(fc::seconds(100)));
   BOOST_CHECK_EQUAL(removed, 1u);      // only exp=99
   BOOST_CHECK_EQUAL(total, 3u);
   BOOST_CHECK(!d.is_known(make_id(99)));
   BOOST_CHECK(d.is_known(make_id(100))); // equality survives
   BOOST_CHECK(d.is_known(make_id(101)));
}

BOOST_AUTO_TEST_CASE(clear_expired_empty_and_none_and_all) {
   using acc = transaction_dedup_test_access;
   transaction_dedup d;
   { auto [r, t] = d.clear_expired(fc::time_point(fc::seconds(1000))); BOOST_CHECK_EQUAL(r, 0u); BOOST_CHECK_EQUAL(t, 0u); }
   for (uint64_t i = 1; i <= 40; ++i) d.record(make_id(i), make_exp(100 + static_cast<uint32_t>(i)));
   { auto [r, t] = d.clear_expired(fc::time_point(fc::seconds(50)));   BOOST_CHECK_EQUAL(r, 0u);  BOOST_CHECK_EQUAL(t, 40u); }   // none expired
   { auto [r, t] = d.clear_expired(fc::time_point(fc::seconds(10000))); BOOST_CHECK_EQUAL(r, 40u); BOOST_CHECK_EQUAL(t, 40u); }  // all expired
   BOOST_CHECK_EQUAL(d.size(), 0u);
   BOOST_CHECK(acc::invariant_holds(d));
}

BOOST_AUTO_TEST_CASE(zero_expiration_boundary) {
   transaction_dedup d;
   d.record(make_id(1), make_exp(0));
   d.record(make_id(2), make_exp(1));
   auto [removed, total] = d.clear_expired(fc::time_point(fc::seconds(1)));
   BOOST_CHECK_EQUAL(removed, 1u);       // exp=0 < 1
   BOOST_CHECK(!d.is_known(make_id(1)));
   BOOST_CHECK(d.is_known(make_id(2)));  // exp=1 == 1, survives
}

BOOST_AUTO_TEST_CASE(far_future_expiration_survives_clear) {
   using acc = transaction_dedup_test_access;
   transaction_dedup d;
   d.record(make_id(1), make_exp(0xFFFFFFF0u)); // far future
   d.record(make_id(2), make_exp(100));
   auto [removed, total] = d.clear_expired(fc::time_point(fc::seconds(1000)));
   BOOST_CHECK_EQUAL(removed, 1u);
   BOOST_CHECK(d.is_known(make_id(1)));
   BOOST_CHECK(acc::invariant_holds(d));
}

// ---- id reuse after removal ----

BOOST_AUTO_TEST_CASE(id_recordable_again_after_expiry) {
   transaction_dedup d;
   d.record(make_id(1), make_exp(100));
   d.clear_expired(fc::time_point(fc::seconds(200)));
   BOOST_CHECK(!d.is_known(make_id(1)));
   BOOST_CHECK_NO_THROW(d.record(make_id(1), make_exp(300)));
   BOOST_CHECK(d.is_known(make_id(1)));
   BOOST_CHECK_EQUAL(d.size(), 1u);
}

BOOST_AUTO_TEST_CASE(id_recordable_again_after_undo) {
   transaction_dedup d;
   d.add_undo_session();
   d.record(make_id(1), make_exp(100));
   d.undo();
   BOOST_CHECK(!d.is_known(make_id(1)));
   BOOST_CHECK_NO_THROW(d.record(make_id(1), make_exp(100)));
   BOOST_CHECK(d.is_known(make_id(1)));
}

// ---- duplicate rejection leaves state intact ----

BOOST_AUTO_TEST_CASE(duplicate_record_does_not_corrupt_state) {
   using acc = transaction_dedup_test_access;
   transaction_dedup d;
   d.record(make_id(1), make_exp(100));
   d.record(make_id(2), make_exp(200));
   const auto before = serialize(d);
   // A duplicate, even under a different expiration, throws and changes nothing.
   BOOST_CHECK_THROW(d.record(make_id(1), make_exp(999)), tx_duplicate);
   BOOST_CHECK_EQUAL(d.size(), 2u);
   BOOST_CHECK_EQUAL(serialize(d), before);
   BOOST_CHECK(acc::invariant_holds(d));
   // The original expiration is untouched: id 1 still clears at 100, not 999.
   d.clear_expired(fc::time_point(fc::seconds(150)));
   BOOST_CHECK(!d.is_known(make_id(1)));
   BOOST_CHECK(d.is_known(make_id(2)));
}

// ---- canonical ordering with shared expirations ----

BOOST_AUTO_TEST_CASE(same_expiration_ordered_by_id) {
   using acc = transaction_dedup_test_access;
   transaction_dedup a, b;
   for (uint64_t i = 1; i <= 20; ++i) a.record(make_id(i), make_exp(100));
   for (uint64_t i = 20; i >= 1; --i) b.record(make_id(i), make_exp(100));
   BOOST_CHECK_EQUAL(serialize(a), serialize(b));   // tie-broken by id, order-independent
   BOOST_CHECK(acc::invariant_holds(a));
   auto [removed, total] = a.clear_expired(fc::time_point(fc::seconds(101)));
   BOOST_CHECK_EQUAL(removed, 20u);                 // the whole shared-expiration band
}

BOOST_AUTO_TEST_CASE(large_set_serialization_is_order_independent) {
   using acc = transaction_dedup_test_access;
   constexpr uint64_t N = 2000;
   transaction_dedup fwd, perm;
   for (uint64_t i = 0; i < N; ++i) fwd.record(make_id(i), make_exp(static_cast<uint32_t>(i % 500) + 1));
   for (uint64_t k = 0; k < N; ++k) {                // 1999 is coprime to 2000 -> a bijection
      uint64_t i = (k * 1999ull) % N;
      perm.record(make_id(i), make_exp(static_cast<uint32_t>(i % 500) + 1));
   }
   BOOST_CHECK_EQUAL(serialize(fwd), serialize(perm));
   BOOST_CHECK_EQUAL(fwd.size(), N);
   BOOST_CHECK(acc::invariant_holds(fwd));
}

// ---- block-revision defensive paths ----

BOOST_AUTO_TEST_CASE(nested_sessions_stack_and_unwind) {
   // A block revision is just an undo session; nesting stacks them and undo unwinds one at a time.
   // (The controller commits a block before starting the next, so it never nests blocks -- this
   // pins the underlying primitive the unified model and the chainbase-driven path both rely on.)
   transaction_dedup d;
   d.record(make_id(0), make_exp(100));   // permanent (no session open)
   d.add_undo_session();
   d.record(make_id(1), make_exp(100));
   d.add_undo_session();             // nests on top of session 1
   d.record(make_id(2), make_exp(100));
   d.undo();                // unwinds session 2 only
   BOOST_CHECK(!d.is_known(make_id(2)));
   BOOST_CHECK(d.is_known(make_id(1)));
   d.undo();                // unwinds session 1
   BOOST_CHECK(!d.is_known(make_id(1)));
   BOOST_CHECK(d.is_known(make_id(0)));
   BOOST_CHECK_EQUAL(d.size(), 1u);
}

BOOST_AUTO_TEST_CASE(undo_and_commit_without_open_session_are_noops) {
   transaction_dedup d;
   d.record(make_id(1), make_exp(100));   // permanent (no session open)
   BOOST_CHECK_NO_THROW(d.commit(0));      // nothing to trim
   BOOST_CHECK_NO_THROW(d.undo());         // nothing to undo
   BOOST_CHECK_EQUAL(d.size(), 1u);
   BOOST_CHECK(d.is_known(make_id(1)));
}

BOOST_AUTO_TEST_CASE(empty_session_ops_are_safe) {
   transaction_dedup d;
   d.record(make_id(1), make_exp(100));
   BOOST_CHECK_NO_THROW(d.undo());
   BOOST_CHECK_NO_THROW(d.squash());
   BOOST_CHECK_EQUAL(d.size(), 1u);
}

BOOST_AUTO_TEST_CASE(undo_reverts_one_session_at_a_time) {
   transaction_dedup d;
   d.add_undo_session();
   d.record(make_id(1), make_exp(100));
                 // committed block 1
   d.add_undo_session();
   d.record(make_id(2), make_exp(100));    // open session for block 2
   d.undo();                 // reverts the top session (block 2) only
   BOOST_CHECK(!d.is_known(make_id(2)));
   BOOST_CHECK(d.is_known(make_id(1)));    // block 1 remains
   d.undo();                 // reverts block 1
   BOOST_CHECK(!d.is_known(make_id(1)));
   BOOST_CHECK_EQUAL(d.size(), 0u);
}

// ---- commit_to_lib edges ----

BOOST_AUTO_TEST_CASE(commit_to_lib_below_all_keeps_poppable) {
   transaction_dedup d;
   for (uint32_t b = 1; b <= 3; ++b) { d.add_undo_session(); d.record(make_id(b), make_exp(100));  }
   d.commit(0);                     // below all block nums: nothing trimmed
   d.undo(); d.undo(); d.undo();
   BOOST_CHECK_EQUAL(d.size(), 0u);        // all popped
}

BOOST_AUTO_TEST_CASE(commit_to_lib_above_all_makes_permanent) {
   transaction_dedup d;
   for (uint32_t b = 1; b <= 3; ++b) { d.add_undo_session(); d.record(make_id(b), make_exp(100));  }
   d.commit(3);                     // at/above all: irreversible
   d.undo(); d.undo(); d.undo();  // all no-ops
   BOOST_CHECK_EQUAL(d.size(), 3u);
}

BOOST_AUTO_TEST_CASE(pop_after_partial_commit_to_lib) {
   transaction_dedup d;
   for (uint32_t b = 1; b <= 5; ++b) { d.add_undo_session(); d.record(make_id(b), make_exp(100));  }
   d.commit(3);                     // blocks 1-3 permanent, 4-5 poppable
   d.undo(); BOOST_CHECK(!d.is_known(make_id(5)));
   d.undo(); BOOST_CHECK(!d.is_known(make_id(4)));
   d.undo();                 // nothing left to pop -- no-op
   BOOST_CHECK(d.is_known(make_id(1)) && d.is_known(make_id(2)) && d.is_known(make_id(3)));
   BOOST_CHECK_EQUAL(d.size(), 3u);
}

// ---- snapshot / file edge cases ----

BOOST_AUTO_TEST_CASE(empty_snapshot_round_trip) {
   using acc = transaction_dedup_test_access;
   fc::mutable_variant_object storage;
   auto writer = std::make_shared<variant_snapshot_writer>(storage);
   transaction_dedup{}.add_to_snapshot(writer);
   writer->finalize();
   fc::variant snap(storage);
   transaction_dedup loaded;
   loaded.record(make_id(9), make_exp(1));   // read_from_snapshot resets first, so this is dropped
   loaded.read_from_snapshot(std::make_shared<variant_snapshot_reader>(snap));
   BOOST_CHECK_EQUAL(loaded.size(), 0u);
   BOOST_CHECK(acc::invariant_holds(loaded));
}

BOOST_AUTO_TEST_CASE(snapshot_excludes_revision_stack) {
   // A chain snapshot is loaded as an irreversible root with no reversible blocks beneath it, so
   // its revision stack must be empty -- add_to_snapshot serializes membership only. (The on-restart
   // dedup FILE additionally persists the stack; see file_round_trip_preserves_revision_stack.)
   using acc = transaction_dedup_test_access;
   transaction_dedup d;
   d.add_undo_session(); d.record(make_id(1), make_exp(100)); 
   d.add_undo_session(); d.record(make_id(2), make_exp(100)); 
   BOOST_REQUIRE_EQUAL(acc::committed_revision_count(d), 2u);

   fc::mutable_variant_object storage;
   auto writer = std::make_shared<variant_snapshot_writer>(storage);
   d.add_to_snapshot(writer);
   writer->finalize();
   fc::variant snap(storage);

   transaction_dedup loaded;
   loaded.read_from_snapshot(std::make_shared<variant_snapshot_reader>(snap));
   BOOST_CHECK_EQUAL(loaded.size(), 2u);
   BOOST_CHECK(loaded.is_known(make_id(1)) && loaded.is_known(make_id(2)));
   BOOST_CHECK_EQUAL(acc::committed_revision_count(loaded), 0u);  // stack NOT carried by snapshots
   loaded.undo();
   BOOST_CHECK(loaded.is_known(make_id(2)));                      // nothing to pop
}

BOOST_AUTO_TEST_CASE(empty_file_round_trip) {
   using acc = transaction_dedup_test_access;
   fc::temp_directory tmp_dir;
   auto tmp = tmp_dir.path() / "empty_dedup.bin";
   { transaction_dedup d; d.write_to_file(tmp); }
   transaction_dedup d;
   BOOST_CHECK(d.read_from_file(tmp));
   BOOST_CHECK_EQUAL(d.size(), 0u);
   BOOST_CHECK_EQUAL(acc::committed_revision_count(d), 0u);
}

BOOST_AUTO_TEST_CASE(file_read_consumes_file) {
   fc::temp_directory tmp_dir;
   auto tmp = tmp_dir.path() / "consume_dedup.bin";
   { transaction_dedup d; d.record(make_id(1), make_exp(100)); d.write_to_file(tmp); }
   transaction_dedup d;
   BOOST_CHECK(d.read_from_file(tmp));
   BOOST_CHECK(!std::filesystem::exists(tmp));   // removed on success
   transaction_dedup d2;
   BOOST_CHECK(!d2.read_from_file(tmp));          // second read: gone
}

BOOST_AUTO_TEST_CASE(restored_instance_is_fully_functional) {
   // After a file round trip the instance must remain operational, not merely readable: clear_expired
   // on restored membership and pop of a restored committed revision both behave correctly.
   using acc = transaction_dedup_test_access;
   fc::temp_directory tmp_dir;
   auto tmp = tmp_dir.path() / "func_dedup.bin";
   {
      transaction_dedup d;
      d.record(make_id(1), make_exp(10));
      d.record(make_id(2), make_exp(500));
      d.add_undo_session();
      d.record(make_id(3), make_exp(500));
      
      d.write_to_file(tmp);
   }
   transaction_dedup d;
   BOOST_REQUIRE(d.read_from_file(tmp));
   auto [removed, total] = d.clear_expired(fc::time_point(fc::seconds(100)));
   BOOST_CHECK_EQUAL(removed, 1u);            // id 1 (exp 10)
   BOOST_CHECK_EQUAL(total, 3u);
   d.undo();                    // pops restored block 1 -> removes id 3
   BOOST_CHECK(!d.is_known(make_id(3)));
   BOOST_CHECK(d.is_known(make_id(2)));
   BOOST_CHECK(acc::invariant_holds(d));
}

// ============================================================================
// Randomized oracle cross-check -- the catch-all regression guard.
//
// Drives identical pseudo-random operation sequences (expiry sweeps, transactions with
// commit/undo, block commit/abort, fork pops, LIB advances) through the real dedup and the
// independent dedup_oracle, asserting after every step that they agree on size, membership, the
// canonical serialization order, and the internal map/index invariant -- then that the final
// state survives file and snapshot round trips. Fixed seeds => failures reproduce exactly.
// ============================================================================

BOOST_AUTO_TEST_CASE(fuzz_matches_reference_oracle) {
   using acc = transaction_dedup_test_access;
   for (uint64_t seed : { 1ull, 7ull, 42ull, 1337ull, 9999ull }) {
      transaction_dedup d;
      dedup_oracle oracle;
      uint64_t rng = seed;
      uint64_t next_id = 0;             // monotonic -> records are always fresh (never tx_duplicate)
      uint32_t now_sec = 0;
      std::vector<int64_t> poppable;    // revisions of committed (reversible) block sessions

      auto check = [&](const char* where) {
         BOOST_REQUIRE_MESSAGE(acc::invariant_holds(d),
            "map/index invariant broken (" << where << ") seed=" << seed);
         BOOST_REQUIRE_MESSAGE(d.size() == oracle.m.size(),
            "size mismatch (" << where << ") seed=" << seed << " got=" << d.size() << " exp=" << oracle.m.size());
         BOOST_REQUIRE_MESSAGE(acc::index_order(d) == oracle.index_order(),
            "serialization order diverged (" << where << ") seed=" << seed);
      };

      for (int blk = 0; blk < 250; ++blk) {
         now_sec += 1 + static_cast<uint32_t>(rng_next(rng) % 4);

         d.add_undo_session();                                         oracle.add_undo_session();   // block session
         d.clear_expired(fc::time_point(fc::seconds(now_sec)));        oracle.clear_expired(fc::time_point(fc::seconds(now_sec)));
         check("after clear");

         int ntrx = static_cast<int>(rng_next(rng) % 9);
         for (int t = 0; t < ntrx; ++t) {
            d.add_undo_session();                                      oracle.add_undo_session();    // trx session
            uint32_t e = now_sec + 1 + static_cast<uint32_t>(rng_next(rng) % 25);
            auto id = make_id(++next_id);
            d.record(id, fc::time_point_sec(e));                       oracle.record(id, fc::time_point_sec(e));
            if (rng_next(rng) % 5 == 0) { d.undo();   oracle.undo(); }
            else                        { d.squash(); oracle.squash(); }
         }
         check("after trx");

         if (rng_next(rng) % 6 == 0) {
            d.undo();                                                  oracle.undo();   // abort the block session
            check("after abort");
         } else {
            // Commit: the block session simply remains on the stack (reversible until LIB advances).
            poppable.push_back(d.revision());
            check("after commit");

            while (!poppable.empty() && rng_next(rng) % 4 == 0) {      // occasional fork-switch pops
               d.undo();                                              oracle.undo();
               poppable.pop_back();
               check("after pop");
            }
            if (poppable.size() > 4 && rng_next(rng) % 3 == 0) {       // occasional LIB advance
               int64_t lib = poppable[poppable.size() / 3];
               d.commit(lib);                                         oracle.commit(lib);
               poppable.erase(std::remove_if(poppable.begin(), poppable.end(),
                              [lib](int64_t r){ return r <= lib; }), poppable.end());
               check("after commit_to_lib");
            }
         }
      }

      // The accumulated state must round-trip through the dedup file (membership + revision stack)...
      {
         fc::temp_directory td;
         auto path = td.path() / "fuzz_dedup.bin";
         d.write_to_file(path);
         transaction_dedup reloaded;
         BOOST_REQUIRE(reloaded.read_from_file(path));
         BOOST_REQUIRE_MESSAGE(acc::index_order(reloaded) == oracle.index_order(),
            "file round trip diverged seed=" << seed);
         BOOST_REQUIRE_EQUAL(acc::committed_revision_count(reloaded), poppable.size());
      }
      // ...and through a chain snapshot (membership only).
      {
         fc::mutable_variant_object storage;
         auto writer = std::make_shared<variant_snapshot_writer>(storage);
         d.add_to_snapshot(writer);
         writer->finalize();
         fc::variant snap(storage);
         transaction_dedup reloaded;
         reloaded.read_from_snapshot(std::make_shared<variant_snapshot_reader>(snap));
         BOOST_REQUIRE_MESSAGE(acc::index_order(reloaded) == oracle.index_order(),
            "snapshot round trip diverged seed=" << seed);
      }
   }
}

// ============================================================================
// Database-driven undo: the dedup registered as a chainbase undo participant. Proves that
// db.start_undo_session / undo / squash / commit drive the dedup's undo in lockstep, so the
// controller can stop hand-pairing its own dedup undo calls with the database's.
// ============================================================================

BOOST_AUTO_TEST_CASE(driven_by_chainbase_database) {
   using acc = transaction_dedup_test_access;
   fc::temp_directory tmp_dir;
   chainbase::database db(tmp_dir.path(), chainbase::database::read_write, 8 * 1024 * 1024, false,
                          chainbase::pinnable_mapped_file::map_mode::heap);
   transaction_dedup d;
   db.add_undo_participant(std::make_unique<dedup_undo_index>(d));
   BOOST_CHECK_EQUAL(db.revision(), 0);

   // Block 1: the database opens the undo session (drives add_undo_session); the dedup records.
   {
      auto block = db.start_undo_session(true);
      BOOST_CHECK_EQUAL(d.revision(), 1);
      d.record(make_id(1), make_exp(100));
      d.record(make_id(2), make_exp(100));
      block.push();                              // keep the block
   }
   BOOST_CHECK(d.is_known(make_id(1)) && d.is_known(make_id(2)));
   BOOST_CHECK_EQUAL(acc::committed_revision_count(d), 1u);

   // Block 2, then a database undo (fork switch) must revert exactly block 2.
   {
      auto block = db.start_undo_session(true);
      d.record(make_id(3), make_exp(100));
      block.push();
   }
   BOOST_CHECK(d.is_known(make_id(3)));
   BOOST_CHECK_EQUAL(db.revision(), 2);
   db.undo();                                    // -> dedup.undo()
   BOOST_CHECK(!d.is_known(make_id(3)));
   BOOST_CHECK(d.is_known(make_id(1)) && d.is_known(make_id(2)));
   BOOST_CHECK_EQUAL(acc::committed_revision_count(d), 1u);

   // A failed transaction: a nested session undone by the database leaves no trace.
   {
      auto block = db.start_undo_session(true);
      {
         auto trx = db.start_undo_session(true);
         d.record(make_id(4), make_exp(100));
         BOOST_CHECK(d.is_known(make_id(4)));
         trx.undo();                             // -> dedup.undo(): drops id 4
      }
      BOOST_CHECK(!d.is_known(make_id(4)));
      d.record(make_id(5), make_exp(100));
      block.push();
   }
   BOOST_CHECK(d.is_known(make_id(5)) && !d.is_known(make_id(4)));

   // LIB advance makes everything irreversible; the undo stack drains, entries remain.
   db.commit(db.revision());
   BOOST_CHECK_EQUAL(acc::committed_revision_count(d), 0u);
   BOOST_CHECK(acc::invariant_holds(d));
   BOOST_CHECK(d.is_known(make_id(1)) && d.is_known(make_id(2)) && d.is_known(make_id(5)));
   BOOST_CHECK_EQUAL(d.size(), 3u);
}

BOOST_AUTO_TEST_SUITE_END()

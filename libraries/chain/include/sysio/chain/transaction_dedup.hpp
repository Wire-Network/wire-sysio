#pragma once

#include <sysio/chain/types.hpp>
#include <sysio/chain/snapshot.hpp>

#include <boost/unordered/unordered_flat_map.hpp>

#include <cstdint>
#include <deque>
#include <filesystem>
#include <set>
#include <utility>
#include <vector>

namespace sysio::chain {

/// High-performance transaction deduplication index.
/// Replaces the chainbase transaction_multi_index with O(1) membership tests, while keeping a
/// chainbase-style nested undo stack so that fork switches and aborts restore state exactly.
///
/// Undo model: a single stack of undo sessions (levels_), keyed by a monotonic revision exactly
/// like a chainbase index. add_undo_session opens a level; record/clear_expired mutate the top
/// level's tracking; squash merges the top level into its parent; undo reverts it; commit(rev)
/// makes levels at or below a revision irreversible. Block revisions and transaction sessions are
/// the SAME mechanism at different nesting depths -- this is what lets the database drive the
/// dedup as a registered undo participant (see add_undo_participant / dedup_undo_index), so the
/// controller no longer has to hand-pair every chainbase undo op with a dedup one. There are no
/// block-specific entry points (start_block_revision / commit_block_revision / commit_to_lib / ...):
/// a block revision and a transaction session are the same add_undo_session, the database drives
/// them, and a block's revision is simply its block number.
///
/// Determinism contract: the entry set and its serialized order are a pure function of the logical
/// chain state, independent of the path a node took to reach it (live sync, replay, fork switches,
/// snapshot load). The sorted index makes serialization order canonical, and clear_expired removes
/// ALL expired entries (a complete prefix of the sorted order), so two honest nodes at the same
/// block always serialize byte-identical sections. This matters because the serialized form feeds
/// calculate_integrity_hash and snapshots.
class transaction_dedup {
public:
   using dedup_entry = std::pair<transaction_id_type, fc::time_point_sec>;

   static constexpr size_t   default_map_capacity = 2'000'000;

   transaction_dedup();

   /// Clear all entries and undo state.
   void reset();

   /// Insert a transaction; throws tx_duplicate if already present.
   void record(const transaction_id_type& id, fc::time_point_sec expiration);

   /// O(1) membership test.
   bool is_known(const transaction_id_type& id) const;

   /// Remove ALL entries with expiration earlier than block_time, saving removed entries in the
   /// current undo level so they can be restored on undo. Returns {num_removed, total_before_removal}.
   std::pair<uint32_t, size_t> clear_expired(fc::time_point block_time);

   // --- chainbase-aligned undo lifecycle (drives, or is driven by, a chainbase database) -------

   /// Open a new (nested) undo session on top of the stack. Subsequent record/clear_expired are
   /// tracked here and reverted as a unit by undo().
   void add_undo_session();
   /// Merge the top undo session into its parent (or into permanent state if it is the only one).
   void squash();
   /// Revert the top undo session: remove what it recorded and restore what it expired.
   void undo();
   /// Revert every open undo session (mirrors chainbase database::undo_all).
   void undo_all();
   /// Discard undo sessions at or below the given revision (they become irreversible).
   void commit(int64_t revision);
   /// Set the current revision; only valid with no open undo sessions (used to align with the db).
   void set_revision(uint64_t revision);
   /// Current revision (highest open session, or the committed base if none are open).
   int64_t revision() const { return revision_; }
   /// {oldest undoable revision, current revision} -- matches chainbase so a registered participant
   /// can be validated for consistency against the segment indices.
   std::pair<uint64_t, uint64_t> undo_stack_revision_range() const;

   /// Persistence -- uses snapshot format for integrity (BLAKE3) and code reuse.
   void write_to_file(const std::filesystem::path& filepath) const;
   bool read_from_file(const std::filesystem::path& filepath);

   /// Snapshot support. Rows are written in (expiration, id) order -- canonical for any node at
   /// the same logical state -- and re-sorted on read, so snapshots produced by older versions
   /// (insertion order) load correctly.
   ///
   /// Membership ONLY. The undo stack is deliberately excluded: a chain snapshot is loaded as an
   /// irreversible root with no reversible blocks beneath it, so the loading node's undo stack is
   /// correctly empty (read_from_snapshot calls reset()). The on-restart undo stack is persisted
   /// separately by write_to_file/read_from_file, which mirror chainbase's persisted reversible
   /// undo stack -- see write_revisions_to_file.
   void add_to_snapshot(const snapshot_writer_ptr& snapshot) const;
   void read_from_snapshot(const snapshot_reader_ptr& snapshot);

   size_t size() const { return map_.size(); }

private:
   /// White-box introspection for unit tests (e.g. asserting the undo bookkeeping stays empty
   /// when no undo context is active); defined by the test translation unit only.
   friend struct transaction_dedup_test_access;

   /// Sorted-index key: (expiration, id). Expiration first so clear_expired removes a complete
   /// prefix; id second to break ties canonically.
   using sorted_entry = std::pair<fc::time_point_sec, transaction_id_type>;

   /// One undo session, keyed by chainbase revision. Tracks what it changed so it can be reversed
   /// (undo) or merged into its parent (squash); commit(revision) drops it once irreversible.
   struct undo_level {
      int64_t                  revision = 0;
      std::vector<dedup_entry> added;             // entries recorded during this session (removed on undo)
      std::vector<dedup_entry> expired;           // entries removed by clear_expired (restored on undo)
   };

   /// Serialize/deserialize the undo stack (levels_) and current revision to the dedup FILE only.
   /// Chainbase persists its reversible undo stack across a clean restart; this mirrors that for
   /// the dedup so that a fork switch after restart -- which pops pre-restart reversible blocks --
   /// reverts the dedup entries those blocks added (and restores the ones they expired) instead of
   /// leaving them stranded (which would make the restarted node reject the canonical fork with
   /// tx_duplicate). Intentionally NOT part of add_to_snapshot/read_from_snapshot (see those for
   /// why a chain snapshot's undo stack is empty).
   void write_revisions_to_file(const snapshot_writer_ptr& snapshot) const;
   void read_revisions_from_file(const snapshot_reader_ptr& snapshot);

   boost::unordered_flat_map<transaction_id_type, fc::time_point_sec> map_;
   std::set<sorted_entry> index_;   // same contents as map_, in canonical (expiration, id) order

   /// Nested undo sessions, oldest (lowest revision) at the front. Trimmed at the front by commit/
   /// commit_to_lib (irreversible), pushed/popped at the back by add_undo_session/undo/squash.
   std::deque<undo_level> levels_;
   int64_t                revision_ = 0;
};

/// Snapshot row type for transaction dedup entries.
struct snapshot_transaction_dedup_entry {
   transaction_id_type trx_id;
   fc::time_point_sec  expiration;
};

/// File-persistence row type for one undo session (NOT used by chain snapshots). Carries the
/// session's revision plus the entries it added and the entries its clear_expired removed, so the
/// undo stack can be reconstructed exactly on restart.
struct snapshot_transaction_dedup_revision {
   int64_t                                       revision = 0;
   std::vector<snapshot_transaction_dedup_entry> added;
   std::vector<snapshot_transaction_dedup_entry> expired;
};

/// File-persistence header carrying the current revision, so it survives even when the undo stack
/// is empty (everything irreversible) and the restored dedup can be validated against the database.
struct snapshot_transaction_dedup_meta {
   int64_t revision = 0;
};

} // namespace sysio::chain

FC_REFLECT(sysio::chain::snapshot_transaction_dedup_entry, (trx_id)(expiration))
FC_REFLECT(sysio::chain::snapshot_transaction_dedup_revision, (revision)(added)(expired))
FC_REFLECT(sysio::chain::snapshot_transaction_dedup_meta, (revision))

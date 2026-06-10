#pragma once

#include <sysio/chain/types.hpp>
#include <sysio/chain/snapshot.hpp>

#include <boost/unordered/unordered_flat_map.hpp>

#include <deque>
#include <filesystem>
#include <set>
#include <utility>
#include <vector>

namespace sysio::chain {

/// High-performance transaction deduplication index.
/// Replaces the chainbase transaction_multi_index with O(1) membership tests.
///
/// Supports revision-based undo (like chainbase) so that pop_block during a fork
/// switch correctly restores the dedup state.
///
/// Determinism contract: the entry set and its serialized order are a pure function of the
/// logical chain state, independent of the path a node took to reach it (live sync, replay,
/// fork switches, snapshot load). The sorted index makes serialization order canonical, and
/// clear_expired removes ALL expired entries (a complete prefix of the sorted order), so two
/// honest nodes at the same block always serialize byte-identical sections. This matters
/// because the serialized form feeds calculate_integrity_hash and snapshots.
class transaction_dedup {
public:
   using dedup_entry = std::pair<transaction_id_type, fc::time_point_sec>;

   static constexpr size_t   default_map_capacity = 2'000'000;

   transaction_dedup();

   /// Clear all entries and revision state.
   void reset();

   /// Insert a transaction; throws tx_duplicate if already present.
   void record(const transaction_id_type& id, fc::time_point_sec expiration);

   /// O(1) membership test.
   bool is_known(const transaction_id_type& id) const;

   /// Remove ALL entries with expiration earlier than block_time, saving removed entries in the
   /// current block revision so they can be restored on undo. Returns {num_removed, total_before_removal}.
   std::pair<uint32_t, size_t> clear_expired(fc::time_point block_time);

   /// Transaction-level undo session management (within current block).
   void push_session();
   void squash_session();
   void undo_session();

   /// Block-level revision management (mirrors chainbase session lifecycle).
   /// start: called at start_block, creates a new pending revision.
   /// commit: called at commit_block, finalizes the pending revision onto the committed stack.
   /// abort: called at abort_block, undoes the pending revision (restores cleared + added entries).
   void start_block_revision(uint32_t block_num);
   void commit_block_revision();
   void abort_block_revision();

   /// Undo one committed block revision. Called from pop_block during fork switches.
   void pop_block_revision();

   /// Discard committed revisions at or below the given block number.
   /// Called when LIB advances (mirrors chainbase db.commit(block_num)).
   void commit_to_lib(uint32_t block_num);

   /// Persistence -- uses snapshot format for integrity (BLAKE3) and code reuse.
   void write_to_file(const std::filesystem::path& filepath) const;
   bool read_from_file(const std::filesystem::path& filepath);

   /// Snapshot support. Rows are written in (expiration, id) order -- canonical for any node at
   /// the same logical state -- and re-sorted on read, so snapshots produced by older versions
   /// (insertion order) load correctly.
   void add_to_snapshot(const snapshot_writer_ptr& snapshot) const;
   void read_from_snapshot(const snapshot_reader_ptr& snapshot);

   size_t size() const { return map_.size(); }

private:
   /// Sorted-index key: (expiration, id). Expiration first so clear_expired removes a complete
   /// prefix; id second to break ties canonically.
   using sorted_entry = std::pair<fc::time_point_sec, transaction_id_type>;

   /// Per-block revision for undo support. Tracks what changed so it can be reversed.
   struct block_revision {
      uint32_t                 block_num = 0;
      std::vector<dedup_entry> added;             // entries recorded during this block (removed on undo)
      std::vector<dedup_entry> expired;           // entries removed by clear_expired (restored on undo)
   };

   /// Re-insert entries removed by clear_expired during the given revision. The sorted index
   /// makes reinsertion order irrelevant, so undo reproduces the exact pre-block state (and
   /// serialization order) on every node.
   void restore_expired(const block_revision& rev);

   /// Remove entries recorded during the current (pending) block from map_ and index_.
   void erase_current_added_from(size_t first);

   boost::unordered_flat_map<transaction_id_type, fc::time_point_sec> map_;
   std::set<sorted_entry> index_;   // same contents as map_, in canonical (expiration, id) order

   std::deque<block_revision>    committed_revisions_;  // committed blocks (trimmed at LIB)
   std::optional<block_revision> pending_revision_;     // current in-progress block

   /// Entries recorded since block start, in record order. Pure undo bookkeeping for the current
   /// block (session watermarks index into it); never serialized, so its insertion ordering
   /// cannot leak into snapshots or the integrity hash.
   std::vector<dedup_entry> current_added_;
   std::vector<size_t>      session_stack_;  // watermarks into current_added_ for trx-level undo
};

/// Snapshot row type for transaction dedup entries.
struct snapshot_transaction_dedup_entry {
   transaction_id_type trx_id;
   fc::time_point_sec  expiration;
};

} // namespace sysio::chain

FC_REFLECT(sysio::chain::snapshot_transaction_dedup_entry, (trx_id)(expiration))

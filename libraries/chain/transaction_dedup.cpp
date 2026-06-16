#include <sysio/chain/transaction_dedup.hpp>
#include <sysio/chain/exceptions.hpp>

#include <fc/log/logger.hpp>

#include <filesystem>
#include <iterator>

namespace sysio::chain {

transaction_dedup::transaction_dedup() {
   map_.reserve(default_map_capacity);
}

void transaction_dedup::reset() {
   map_.clear();
   index_.clear();
   map_.reserve(default_map_capacity);
   levels_.clear();
   revision_ = 0;
}

void transaction_dedup::record(const transaction_id_type& id, fc::time_point_sec expiration) {
   auto [it, inserted] = map_.emplace(id, expiration);
   SYS_ASSERT(inserted, tx_duplicate, "duplicate transaction {}", id);
   index_.emplace(expiration, id);
   // Track the insertion in the open undo session so it can be reverted; with no session open
   // (irreversible replay) nothing tracks it -- it is not undoable, and recording the bookkeeping
   // would grow unbounded for the entire replay.
   if (!levels_.empty())
      levels_.back().added.emplace_back(id, expiration);
}

bool transaction_dedup::is_known(const transaction_id_type& id) const {
   return map_.contains(id);
}

std::pair<uint32_t, size_t> transaction_dedup::clear_expired(fc::time_point block_time) {
   const auto total = index_.size();
   // The index is sorted by (expiration, id), so expired entries form a complete prefix: everything
   // expired is removed, not just a run that happens to sit at the front of insertion order. Leaving
   // stragglers behind would make the retained set -- and the integrity hash derived from it --
   // depend on the order entries were recorded.
   uint32_t num_removed = 0;
   auto it = index_.begin();
   while (it != index_.end() && block_time > it->first.to_time_point()) {
      map_.erase(it->second);
      if (!levels_.empty())
         levels_.back().expired.emplace_back(it->second, it->first);
      it = index_.erase(it);
      ++num_removed;
   }
   return {num_removed, total};
}

// --- chainbase-aligned undo lifecycle ---

void transaction_dedup::add_undo_session() {
   ++revision_;
   levels_.push_back(undo_level{revision_, {}, {}});
}

void transaction_dedup::squash() {
   if (levels_.empty())
      return;
   undo_level top = std::move(levels_.back());
   levels_.pop_back();
   --revision_;
   if (levels_.empty())
      return;   // merged into permanent state: map_/index_ already reflect the changes, nothing tracks them
   // Otherwise the parent session now owns this session's changes.
   auto& parent = levels_.back();
   parent.added.insert(parent.added.end(),
                       std::make_move_iterator(top.added.begin()), std::make_move_iterator(top.added.end()));
   parent.expired.insert(parent.expired.end(),
                         std::make_move_iterator(top.expired.begin()), std::make_move_iterator(top.expired.end()));
}

void transaction_dedup::undo() {
   if (levels_.empty())
      return;
   const undo_level top = std::move(levels_.back());
   levels_.pop_back();
   --revision_;
   // Remove what this session added, then restore what it expired. The sorted index makes
   // reinsertion order irrelevant, so undo reproduces the exact pre-session state (and serialization
   // order) on every node.
   for (const auto& [id, exp] : top.added) {
      map_.erase(id);
      index_.erase(sorted_entry{exp, id});
   }
   for (const auto& [id, exp] : top.expired) {
      map_.emplace(id, exp);
      index_.emplace(exp, id);
   }
}

void transaction_dedup::undo_all() {
   while (!levels_.empty())
      undo();
}

void transaction_dedup::commit(int64_t revision) {
   // Sessions at or below this revision are irreversible; drop their tracking (the entries stay in
   // map_/index_). The front of the deque is the oldest, lowest-revision session. The controller's
   // LIB advance (db.commit(block_num), with revision == block_num) drives this via the participant.
   while (!levels_.empty() && levels_.front().revision <= revision)
      levels_.pop_front();
}

void transaction_dedup::set_revision(uint64_t revision) {
   SYS_ASSERT(levels_.empty(), chain_exception,
              "cannot set transaction dedup revision while undo sessions are open");
   revision_ = static_cast<int64_t>(revision);
}

std::pair<uint64_t, uint64_t> transaction_dedup::undo_stack_revision_range() const {
   const int64_t begin = revision_ - static_cast<int64_t>(levels_.size());
   return { static_cast<uint64_t>(begin), static_cast<uint64_t>(revision_) };
}

// --- File persistence (uses snapshot format for integrity and code reuse) ---

void transaction_dedup::write_to_file(const std::filesystem::path& filepath) const {
   const auto tmp = filepath.string() + ".tmp";

   auto writer = std::make_shared<threaded_snapshot_writer>(tmp);
   add_to_snapshot(writer);            // membership set
   write_revisions_to_file(writer);    // reversible undo stack + current revision (file-only; see header)
   writer->finalize();

   std::filesystem::rename(tmp, filepath);
}

bool transaction_dedup::read_from_file(const std::filesystem::path& filepath) {
   if (!std::filesystem::is_regular_file(filepath))
      return false;

   try {
      auto reader = std::make_shared<threaded_snapshot_reader>(filepath);
      reader->validate();
      read_from_snapshot(reader);         // resets, then loads membership (revision_ -> 0)
      read_revisions_from_file(reader);   // loads the undo stack and current revision

      std::filesystem::remove(filepath);
      return true;
   } catch (const fc::exception& e) {
      wlog("Failed to read transaction dedup file {}: {}", filepath.generic_string(), e.to_detail_string());
      reset();
      return false;
   }
}

void transaction_dedup::write_revisions_to_file(const snapshot_writer_ptr& snapshot) const {
   // Current revision first, so it is restored even when the undo stack is empty (everything
   // irreversible) and the restored dedup can be validated against the database's revision.
   snapshot->write_section("sysio::chain::transaction_dedup_meta", [this](auto& section) {
      section.add_row(snapshot_transaction_dedup_meta{revision_});
   });
   snapshot->write_section("sysio::chain::transaction_dedup_revisions", [this](auto& section) {
      for (const auto& lvl : levels_) {
         snapshot_transaction_dedup_revision row;
         row.revision  = lvl.revision;
         row.added.reserve(lvl.added.size());
         for (const auto& [id, exp] : lvl.added)
            row.added.emplace_back(snapshot_transaction_dedup_entry{id, exp});
         row.expired.reserve(lvl.expired.size());
         for (const auto& [id, exp] : lvl.expired)
            row.expired.emplace_back(snapshot_transaction_dedup_entry{id, exp});
         section.add_row(row);
      }
   });
}

void transaction_dedup::read_revisions_from_file(const snapshot_reader_ptr& snapshot) {
   // read_from_snapshot already reset() the stack; restore the revision then the sessions in
   // committed (revision-ascending) order so commit / commit_to_lib / undo see exactly the
   // pre-shutdown stack.
   snapshot->read_section("sysio::chain::transaction_dedup_meta", [this](auto& section) {
      if (!section.empty()) {
         snapshot_transaction_dedup_meta meta;
         section.read_row(meta);
         revision_ = meta.revision;
      }
   });
   snapshot->read_section("sysio::chain::transaction_dedup_revisions", [this](auto& section) {
      bool more = !section.empty();
      while (more) {
         snapshot_transaction_dedup_revision row;
         more = section.read_row(row);
         undo_level lvl;
         lvl.revision  = row.revision;
         lvl.added.reserve(row.added.size());
         for (const auto& e : row.added)
            lvl.added.emplace_back(e.trx_id, e.expiration);
         lvl.expired.reserve(row.expired.size());
         for (const auto& e : row.expired)
            lvl.expired.emplace_back(e.trx_id, e.expiration);
         levels_.push_back(std::move(lvl));
      }
   });
}

// --- Snapshot support ---

void transaction_dedup::add_to_snapshot(const snapshot_writer_ptr& snapshot) const {
   snapshot->write_section("sysio::chain::transaction_dedup", [this](auto& section) {
      // index_ iterates in (expiration, id) order: canonical for a given logical entry set, so the
      // serialized section -- and the integrity hash folded over it -- is identical across nodes
      // regardless of the record/undo path that produced the set.
      for (const auto& [exp, id] : index_) {
         section.add_row(snapshot_transaction_dedup_entry{id, exp});
      }
   });
}

void transaction_dedup::read_from_snapshot(const snapshot_reader_ptr& snapshot) {
   reset();

   snapshot->read_section("sysio::chain::transaction_dedup", [this](auto& section) {
      bool more = !section.empty();
      while (more) {
         snapshot_transaction_dedup_entry entry;
         more = section.read_row(entry);
         // Honest nodes serialize each id exactly once, so a repeated id means the section is
         // corrupt or hand-crafted. Silently ignoring the failed insert would break the map/index
         // invariant: the map keeps only the first row while the index gains an (expiration, id)
         // entry per row, so size() disagrees with the serialized contents and the phantom index
         // entry outlives its map entry -- clear_expired then erases the id at the WRONG expiration
         // (or re-recording the id strands the stale index row for good).
         const bool inserted = map_.emplace(entry.trx_id, entry.expiration).second;
         SYS_ASSERT(inserted, snapshot_exception,
                    "duplicate transaction {} in dedup snapshot section", entry.trx_id);
         // Current-format rows arrive in (expiration, id) order, so hinting the end makes the tree
         // build amortized O(1) per row instead of O(log n) -- ~11x faster on large sets. Old
         // insertion-ordered snapshots ignore a wrong hint and insert correctly at full cost.
         index_.emplace_hint(index_.end(), entry.expiration, entry.trx_id);
      }
      // map_ keys on the id alone, so every accepted row's (expiration, id) pair is necessarily new
      // to index_; equal sizes prove the index insert landed for every row as well.
      SYS_ASSERT(map_.size() == index_.size(), snapshot_exception,
                 "transaction dedup map/index size mismatch after snapshot load ({} vs {})",
                 map_.size(), index_.size());
   });

   ilog("Read {} transaction dedup entries from snapshot", index_.size());
}

} // namespace sysio::chain

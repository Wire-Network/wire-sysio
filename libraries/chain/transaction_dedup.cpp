#include <sysio/chain/transaction_dedup.hpp>
#include <sysio/chain/exceptions.hpp>

#include <fc/log/logger.hpp>

#include <filesystem>

namespace sysio::chain {

transaction_dedup::transaction_dedup() {
   map_.reserve(default_map_capacity);
}

void transaction_dedup::reset() {
   map_.clear();
   index_.clear();
   map_.reserve(default_map_capacity);
   committed_revisions_.clear();
   pending_revision_.reset();
   current_added_.clear();
   session_stack_.clear();
}

void transaction_dedup::record(const transaction_id_type& id, fc::time_point_sec expiration) {
   auto [it, inserted] = map_.emplace(id, expiration);
   SYS_ASSERT(inserted, tx_duplicate, "duplicate transaction {}", id);
   index_.emplace(expiration, id);
   // Keep undo bookkeeping only while an undo context exists -- an open block revision or an
   // active session -- because only those paths can ever revert the entry. Recording without one
   // must not append: nothing would consume or clear the entries until the next
   // start_block_revision, and an irreversible replay (which records every input transaction
   // with no revisions and no sessions) would otherwise accumulate bookkeeping for every
   // replayed transaction for the entire duration of the replay.
   if (pending_revision_ || !session_stack_.empty())
      current_added_.emplace_back(id, expiration);
}

bool transaction_dedup::is_known(const transaction_id_type& id) const {
   return map_.contains(id);
}

std::pair<uint32_t, size_t> transaction_dedup::clear_expired(fc::time_point block_time) {
   // clear_expired must not be called after record() within the same block; record() appends to
   // current_added_, which start_block_revision just cleared.
   assert(!pending_revision_ || current_added_.empty());
   const auto total = index_.size();
   // The index is sorted by (expiration, id), so expired entries form a complete prefix:
   // everything expired is removed, not just a run that happens to sit at the front of
   // insertion order. Leaving stragglers behind would make the retained set -- and the
   // integrity hash derived from it -- depend on the order entries were recorded.
   uint32_t num_removed = 0;
   auto it = index_.begin();
   while (it != index_.end() && block_time > it->first.to_time_point()) {
      map_.erase(it->second);
      if (pending_revision_)
         pending_revision_->expired.emplace_back(it->second, it->first);
      it = index_.erase(it);
      ++num_removed;
   }
   return {num_removed, total};
}

// --- Transaction-level undo session management ---

void transaction_dedup::push_session() {
   session_stack_.push_back(current_added_.size());
}

void transaction_dedup::squash_session() {
   if (!session_stack_.empty())
      session_stack_.pop_back();
}

void transaction_dedup::undo_session() {
   if (session_stack_.empty())
      return;
   const size_t restore_size = session_stack_.back();
   session_stack_.pop_back();
   assert(restore_size <= current_added_.size());
   erase_current_added_from(restore_size);
}

// --- Block-level revision management ---

void transaction_dedup::start_block_revision(uint32_t block_num) {
   // If there's an uncommitted pending revision, abort it first (shouldn't happen normally)
   if (pending_revision_)
      abort_block_revision();
   pending_revision_ = block_revision{block_num, {}, {}};
   // Entries recorded outside any block revision belong to pre-block state and are not undoable.
   current_added_.clear();
   session_stack_.clear();
}

void transaction_dedup::commit_block_revision() {
   if (!pending_revision_)
      return;
   session_stack_.clear();
   pending_revision_->added = std::move(current_added_);
   current_added_.clear();
   committed_revisions_.push_back(std::move(*pending_revision_));
   pending_revision_.reset();
}

void transaction_dedup::abort_block_revision() {
   if (!pending_revision_)
      return;
   session_stack_.clear();
   // Undo entries added during this block
   erase_current_added_from(0);
   // Restore entries that were cleared by clear_expired during this block
   restore_expired(*pending_revision_);
   pending_revision_.reset();
}

void transaction_dedup::pop_block_revision() {
   // pop_block always runs with no block in progress; abort defensively if that ever changes so
   // the pending revision's cleared entries are restored rather than stranded.
   if (pending_revision_)
      abort_block_revision();
   if (committed_revisions_.empty())
      return;

   const auto& rev = committed_revisions_.back();
   // Undo entries added during this block
   for (const auto& [id, exp] : rev.added) {
      map_.erase(id);
      index_.erase(sorted_entry{exp, id});
   }
   // Restore entries that were cleared by clear_expired during this block
   restore_expired(rev);
   committed_revisions_.pop_back();
}

void transaction_dedup::commit_to_lib(uint32_t block_num) {
   auto it = committed_revisions_.begin();
   while (it != committed_revisions_.end() && it->block_num <= block_num) {
      ++it;
   }
   committed_revisions_.erase(committed_revisions_.begin(), it);
}

void transaction_dedup::restore_expired(const block_revision& rev) {
   for (const auto& [id, exp] : rev.expired) {
      map_.emplace(id, exp);
      index_.emplace(exp, id);
   }
}

void transaction_dedup::erase_current_added_from(size_t first) {
   for (size_t i = first; i < current_added_.size(); ++i) {
      const auto& [id, exp] = current_added_[i];
      map_.erase(id);
      index_.erase(sorted_entry{exp, id});
   }
   current_added_.resize(first);
}

// --- File persistence (uses snapshot format for integrity and code reuse) ---

void transaction_dedup::write_to_file(const std::filesystem::path& filepath) const {
   const auto tmp = filepath.string() + ".tmp";

   auto writer = std::make_shared<threaded_snapshot_writer>(tmp);
   add_to_snapshot(writer);
   writer->finalize();

   std::filesystem::rename(tmp, filepath);
}

bool transaction_dedup::read_from_file(const std::filesystem::path& filepath) {
   if (!std::filesystem::is_regular_file(filepath))
      return false;

   try {
      auto reader = std::make_shared<threaded_snapshot_reader>(filepath);
      reader->validate();
      read_from_snapshot(reader);

      std::filesystem::remove(filepath);
      return true;
   } catch (const fc::exception& e) {
      wlog("Failed to read transaction dedup file {}: {}", filepath.generic_string(), e.to_detail_string());
      reset();
      return false;
   }
}

// --- Snapshot support ---

void transaction_dedup::add_to_snapshot(const snapshot_writer_ptr& snapshot) const {
   snapshot->write_section("sysio::chain::transaction_dedup", [this](auto& section) {
      // index_ iterates in (expiration, id) order: canonical for a given logical entry set, so
      // the serialized section -- and the integrity hash folded over it -- is identical across
      // nodes regardless of the record/undo path that produced the set.
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
         // corrupt or hand-crafted. Silently ignoring the failed insert would break the
         // map/index invariant: the map keeps only the first row while the index gains an
         // (expiration, id) entry per row, so size() disagrees with the serialized contents and
         // the phantom index entry outlives its map entry -- clear_expired then erases the id at
         // the WRONG expiration (or re-recording the id strands the stale index row for good).
         const bool inserted = map_.emplace(entry.trx_id, entry.expiration).second;
         SYS_ASSERT(inserted, snapshot_exception,
                    "duplicate transaction {} in dedup snapshot section", entry.trx_id);
         // Current-format rows arrive in (expiration, id) order, so hinting the end makes the
         // tree build amortized O(1) per row instead of O(log n) -- ~11x faster on large sets.
         // Old insertion-ordered snapshots ignore a wrong hint and insert correctly at full cost.
         index_.emplace_hint(index_.end(), entry.expiration, entry.trx_id);
      }
      // map_ keys on the id alone, so every accepted row's (expiration, id) pair is necessarily
      // new to index_; equal sizes prove the index insert landed for every row as well.
      SYS_ASSERT(map_.size() == index_.size(), snapshot_exception,
                 "transaction dedup map/index size mismatch after snapshot load ({} vs {})",
                 map_.size(), index_.size());
   });

   ilog("Read {} transaction dedup entries from snapshot", index_.size());
}

} // namespace sysio::chain

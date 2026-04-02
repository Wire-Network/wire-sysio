#include <sysio/chain/transaction_dedup.hpp>
#include <sysio/chain/exceptions.hpp>

#include <fc/log/logger.hpp>

#include <filesystem>
#include <ranges>

namespace sysio::chain {

transaction_dedup::transaction_dedup() {
   map_.reserve(default_map_capacity);
}

void transaction_dedup::reset() {
   map_.clear();
   deque_.clear();
   map_.reserve(default_map_capacity);
   committed_revisions_.clear();
   pending_revision_.reset();
   session_stack_.clear();
}

void transaction_dedup::record(const transaction_id_type& id, fc::time_point_sec expiration) {
   auto [it, inserted] = map_.emplace(id, expiration);
   SYS_ASSERT(inserted, tx_duplicate, "duplicate transaction {}", id);
   deque_.emplace_back(id, expiration);
}

bool transaction_dedup::is_known(const transaction_id_type& id) const {
   return map_.contains(id);
}

std::pair<uint32_t, size_t> transaction_dedup::clear_expired(fc::time_point block_time) {
   // clear_expired must not be called after record() within the same block.
   // If record() was called, deque_ would be larger than deque_size_at_start.
   assert(!pending_revision_ || deque_.size() == pending_revision_->deque_size_at_start);
   const auto total = deque_.size();
   // Scan for the expiration boundary so they can be removed in one erase call
   auto it = deque_.begin();
   while (it != deque_.end() && block_time > it->second.to_time_point())
      ++it;
   const auto num_removed = static_cast<uint32_t>(it - deque_.begin());
   if (num_removed > 0) {
      for (auto scan = deque_.begin(); scan != it; ++scan) {
         map_.erase(scan->first);
         if (pending_revision_)
            pending_revision_->expired.push_back(*scan);
      }
      deque_.erase(deque_.begin(), it);
      if (pending_revision_)
         pending_revision_->deque_size_at_start = deque_.size();
   }
   return {num_removed, total};
}

// --- Transaction-level undo session management ---

void transaction_dedup::push_session() {
   session_stack_.push_back(deque_.size());
}

void transaction_dedup::squash_session() {
   if (!session_stack_.empty())
      session_stack_.pop_back();
}

void transaction_dedup::undo_session() {
   if (session_stack_.empty())
      return;
   auto restore_size = session_stack_.back();
   session_stack_.pop_back();
   for (auto it = deque_.begin() + restore_size; it != deque_.end(); ++it)
      map_.erase(it->first);
   deque_.erase(deque_.begin() + restore_size, deque_.end());
   // Sessions are always pushed after clear_expired runs, so undo can never
   // shrink the deque below the block revision's start point. If this fires,
   // the call ordering in start_block has been broken.
   assert(!pending_revision_ || deque_.size() >= pending_revision_->deque_size_at_start);
}

// --- Block-level revision management ---

void transaction_dedup::start_block_revision(uint32_t block_num) {
   // If there's an uncommitted pending revision, abort it first (shouldn't happen normally)
   if (pending_revision_)
      abort_block_revision();
   pending_revision_ = block_revision{block_num, deque_.size(), {}};
   session_stack_.clear();
}

void transaction_dedup::commit_block_revision() {
   if (!pending_revision_)
      return;
   session_stack_.clear();
   committed_revisions_.push_back(std::move(*pending_revision_));
   pending_revision_.reset();
}

void transaction_dedup::abort_block_revision() {
   if (!pending_revision_)
      return;
   session_stack_.clear();
   // Undo entries added during this block
   for (auto it = deque_.begin() + pending_revision_->deque_size_at_start; it != deque_.end(); ++it)
      map_.erase(it->first);
   deque_.erase(deque_.begin() + pending_revision_->deque_size_at_start, deque_.end());
   // Restore entries that were cleared by clear_expired during this block (push to front)
   for (const auto& entry : std::views::reverse(pending_revision_->expired)) {
      deque_.push_front(entry);
      map_.emplace(entry.first, entry.second);
   }
   pending_revision_.reset();
}

void transaction_dedup::pop_block_revision() {
   if (committed_revisions_.empty())
      return;

   const auto& rev = committed_revisions_.back();
   // Undo entries added during this block (back to the size at block start)
   for (auto it = deque_.begin() + rev.deque_size_at_start; it != deque_.end(); ++it)
      map_.erase(it->first);
   deque_.erase(deque_.begin() + rev.deque_size_at_start, deque_.end());
   // Restore entries that were cleared by clear_expired during this block
   for (const auto& entry : std::views::reverse(rev.expired)) {
      deque_.push_front(entry);
      map_.emplace(entry.first, entry.second);
   }
   committed_revisions_.pop_back();
}

void transaction_dedup::commit_to_lib(uint32_t block_num) {
   auto it = committed_revisions_.begin();
   while (it != committed_revisions_.end() && it->block_num <= block_num) {
      ++it;
   }
   committed_revisions_.erase(committed_revisions_.begin(), it);
}

// --- File persistence (uses snapshot format for integrity and code reuse) ---

void transaction_dedup::write_to_file(const std::filesystem::path& filepath) const {
   auto tmp = filepath;
   tmp += ".tmp";

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
      for (const auto& [id, exp] : deque_) {
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
         map_.emplace(entry.trx_id, entry.expiration);
         deque_.emplace_back(entry.trx_id, entry.expiration);
      }
   });

   ilog("Read {} transaction dedup entries from snapshot", deque_.size());
}

} // namespace sysio::chain

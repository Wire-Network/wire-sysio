#include <sysio/chain/transaction_dedup.hpp>
#include <sysio/chain/exceptions.hpp>

#include <fc/io/cfile.hpp>
#include <fc/io/raw.hpp>
#include <fc/log/logger.hpp>

#include <filesystem>
#include <ranges>

namespace sysio::chain {

static constexpr uint32_t dedup_file_magic   = 0x44454450; // "DEDP"
static constexpr uint32_t dedup_file_version = 1;

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
   const auto total = deque_.size();
   uint32_t num_removed = 0;
   while (!deque_.empty() && block_time > deque_.front().second.to_time_point()) {
      auto entry = deque_.front();
      map_.erase(entry.first);
      deque_.pop_front();
      if (pending_revision_)
         pending_revision_->expired.push_back(std::move(entry));
      ++num_removed;
   }
   if (pending_revision_ && num_removed > 0)
      pending_revision_->deque_size_at_start = deque_.size();
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
   while (deque_.size() > restore_size) {
      map_.erase(deque_.back().first);
      deque_.pop_back();
   }
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
   // Undo entries added during this block (pop from back)
   while (deque_.size() > pending_revision_->deque_size_at_start) {
      map_.erase(deque_.back().first);
      deque_.pop_back();
   }
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
   // Undo entries added during this block (pop from back to the size at block start)
   while (deque_.size() > rev.deque_size_at_start) {
      map_.erase(deque_.back().first);
      deque_.pop_back();
   }
   // Restore entries that were cleared by clear_expired during this block
   for (const auto& entry : std::views::reverse(rev.expired)) {
      deque_.push_front(entry);
      map_.emplace(entry.first, entry.second);
   }
   committed_revisions_.pop_back();
}

void transaction_dedup::commit_to_lib(uint32_t block_num) {
   while (!committed_revisions_.empty() && committed_revisions_.front().block_num <= block_num) {
      committed_revisions_.pop_front();
   }
}

// --- File persistence ---

void transaction_dedup::write_to_file(const std::filesystem::path& filepath) const {
   auto tmp = filepath;
   tmp += ".tmp";

   fc::cfile f;
   f.set_file_path(tmp);
   f.open("wb");

   fc::raw::pack(f, dedup_file_magic);
   fc::raw::pack(f, dedup_file_version);
   uint64_t count = deque_.size();
   fc::raw::pack(f, count);
   for (const auto& [id, exp] : deque_) {
      fc::raw::pack(f, id);
      fc::raw::pack(f, exp);
   }
   f.close();

   std::filesystem::rename(tmp, filepath);
}

bool transaction_dedup::read_from_file(const std::filesystem::path& filepath) {
   if (!std::filesystem::is_regular_file(filepath))
      return false;

   try {
      fc::cfile f;
      f.set_file_path(filepath);
      f.open("rb");

      uint32_t magic = 0, version = 0;
      fc::raw::unpack(f, magic);
      fc::raw::unpack(f, version);
      SYS_ASSERT(magic == dedup_file_magic && version == dedup_file_version,
                 chain_exception, "Invalid transaction_dedup file: magic={} version={}", magic, version);

      uint64_t count = 0;
      fc::raw::unpack(f, count);

      reset();
      map_.reserve(std::max(static_cast<size_t>(count), default_map_capacity));

      for (uint64_t i = 0; i < count; ++i) {
         transaction_id_type id;
         fc::time_point_sec exp;
         fc::raw::unpack(f, id);
         fc::raw::unpack(f, exp);
         map_.emplace(id, exp);
         deque_.emplace_back(id, exp);
      }
      f.close();

      ilog("Read {} transaction dedup entries from {}", count, filepath.generic_string());
      std::filesystem::remove(filepath);
      return true;
   } catch (const fc::exception& e) {
      wlog("Failed to read transaction dedup file {}: {}", filepath.generic_string(), e.to_detail_string());
      reset();
      return false;
   }
}

// --- Snapshot support ---

void transaction_dedup::add_to_snapshot(const snapshot_writer_ptr& snapshot, const chainbase::database& db) const {
   snapshot->write_section("sysio::chain::transaction_dedup", [this, &db](auto& section) {
      for (const auto& [id, exp] : deque_) {
         section.add_row(snapshot_transaction_dedup_entry{id, exp}, db);
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

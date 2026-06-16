#pragma once

#include <sysio/trace_api/common.hpp>
#include <sysio/chain/types.hpp>
#include <fc/io/cfile.hpp>
#include <fc/reflect/reflect.hpp>
#include <cstddef>
#include <cstring>
#include <deque>
#include <filesystem>
#include <optional>
#include <utility>
#include <vector>

namespace sysio::trace_api {

// ---------------------------------------------------------------------------
// On-disk format: trace_trx_idx_<range>.log
//
// Layout: trx_id_index_header (16 bytes) followed immediately by
// bucket_count trx_id_bucket records (16 bytes each).
//
// Serialized with fc::raw (native-endian, same convention as all other
// trace slice files — x86_64 Linux only).
//
// Open-addressing hash table with linear probing, load factor <= 0.5.
// bucket_count is always a power of two (modulo via bitwise AND).
// Empty slot sentinel: block_num == 0. SYSIO block numbers start at 1.
// ---------------------------------------------------------------------------

struct trx_id_index_header {
   // Stored little-endian on disk so a hex dump of the first 4 bytes reads "TRIX".
   static constexpr uint32_t magic_value     = 0x58495254; // bytes on disk: 'T','R','I','X'
   static constexpr uint32_t current_version = 1;

   // Hard cap on bucket_count in a readable index.  At 16 bytes per bucket, 2^28
   // buckets = 4 GB.  A realistic worst-case slice (default 10K blocks at
   // ~1K trxs/block = 10M trxs at load factor 0.5 = 2^25 buckets) fits 8x
   // inside this cap.  Anything larger is treated as a corrupt/malicious file.
   // The writer derives its entry-count bound from this same constant so it can
   // never produce an index the reader refuses.
   static constexpr uint32_t max_bucket_count = 1u << 28;

   uint32_t magic        = magic_value;
   uint32_t version      = current_version;
   uint32_t bucket_count = 0;
   uint32_t reserved     = 0;
};
static_assert(sizeof(trx_id_index_header) == 16);

struct trx_id_bucket {
   uint64_t prefix64  = 0; // first 8 bytes of trx sha256 interpreted as uint64_t
   uint32_t block_num = 0; // 0 = empty; SYSIO block numbers start at 1
   uint32_t reserved  = 0;
};
static_assert(sizeof(trx_id_bucket) == 16);

/// 64-bit hash key shared by writer and reader: the first 8 bytes of the trx sha256.
/// fc::sha256 is a plain struct with no padding before its data.
inline uint64_t trx_id_prefix(const chain::transaction_id_type& id) {
   uint64_t p;
   static_assert(sizeof(chain::transaction_id_type) >= sizeof(p));
   std::memcpy(&p, &id, sizeof(p));
   return p;
}

// ---------------------------------------------------------------------------
// Writer: accumulate (trx_id, block_num) pairs, write index file when done.
// ---------------------------------------------------------------------------

class trx_id_index_writer {
public:
   void add(const chain::transaction_id_type& trx_id, uint32_t block_num);
   void write(const std::filesystem::path& path) const;
   size_t entry_count() const { return _entries.size(); }

private:
   // (prefix64, block_num) pairs in insertion order.  write() applies last-
   // write-wins per prefix64 when populating the bucket array, so the latest
   // add for a given prefix is what ends up in the on-disk hash table.
   // std::deque avoids the O(N) reallocation+copy of std::vector growth for
   // multi-million-entry slices without needing an up-front reserve hint.
   std::deque<std::pair<uint64_t, uint32_t>> _entries;
};

// ---------------------------------------------------------------------------
// Reader: memory-map the index file and answer prefix lookups.
//
// A lookup touches at most a handful of buckets (load factor <= 0.5), so the
// file is mmap'd rather than bulk-read: only the probed pages fault in, the
// kernel page cache shares them across requests, and a not-found probe across
// many slices stays O(1) I/O per slice instead of O(file size).  Move-only;
// the destructor unmaps.  Files are immutable once written (temp + rename),
// and an unlink by retention pruning leaves an existing mapping valid.
//
// Collisions (two trx_ids sharing a 64-bit prefix) are not explicitly
// confirmed - callers confirm hits against the recorded block data.
// ---------------------------------------------------------------------------

class trx_id_index_reader {
public:
   explicit trx_id_index_reader(const std::filesystem::path& path);
   trx_id_index_reader(trx_id_index_reader&& other) noexcept;
   trx_id_index_reader& operator=(trx_id_index_reader&& other) noexcept;
   trx_id_index_reader(const trx_id_index_reader&)            = delete;
   trx_id_index_reader& operator=(const trx_id_index_reader&) = delete;
   ~trx_id_index_reader();

   bool valid() const { return _valid; }

   // Returns the block_num for trx_id's prefix, or nullopt if not found.
   std::optional<uint32_t> lookup(const chain::transaction_id_type& trx_id) const;

private:
   void unmap() noexcept;

   const trx_id_bucket* _buckets      = nullptr; // into the mapping, just past the header
   uint32_t             _bucket_count = 0;
   void*                _map_base     = nullptr;
   std::size_t          _map_size     = 0;
   bool                 _valid        = false;
};

} // namespace sysio::trace_api

FC_REFLECT(sysio::trace_api::trx_id_index_header, (magic)(version)(bucket_count)(reserved))
FC_REFLECT(sysio::trace_api::trx_id_bucket, (prefix64)(block_num)(reserved))

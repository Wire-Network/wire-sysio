#pragma once

#include <sysio/chain/types.hpp>
#include <fc/io/cfile.hpp>
#include <fc/reflect/reflect.hpp>
#include <boost/iostreams/device/mapped_file.hpp>
#include <filesystem>
#include <optional>
#include <vector>

namespace sysio::trace_api {

// ---------------------------------------------------------------------------
// On-disk format: abi_store.log
//
// Single-file layout (written atomically via .tmp + rename):
//
//   Header         (16 bytes): magic, version, entry_count, reserved
//   Index          (entry_count * 32 bytes, sorted by account ASC, global_seq ASC):
//                    account(8) + global_seq(8) + blob_offset(8) + blob_size(8)
//                    blob_offset is relative to the start of the blob area.
//   Blob area      (variable): raw ABI bytes concatenated in index order
//
// Serialized with fc::raw (native-endian), consistent with other trace slice files.
//
// To find the ABI for account A in effect at global_seq Q:
//   Binary-search the index for the last entry where account==A && global_seq<=Q,
//   then read blob_size bytes from (blob_area_start + blob_offset) in the file.
// ---------------------------------------------------------------------------

struct abi_store_header {
   static constexpr uint32_t magic_value    = 0x41424942; // "ABIB"
   static constexpr uint32_t current_version = 1;

   uint32_t magic       = magic_value;
   uint32_t version     = current_version;
   uint64_t entry_count = 0;
};
static_assert(sizeof(abi_store_header) == 16);

struct abi_store_index_entry {
   uint64_t account;     // chain::name value
   uint64_t global_seq;
   uint64_t blob_offset; // relative to blob area start
   uint64_t blob_size;
};
static_assert(sizeof(abi_store_index_entry) == 32);

// ---------------------------------------------------------------------------
// Writer: accumulate (account, global_seq, abi_bytes) triples; write atomically.
// If the same (account, global_seq) key is added twice, last-write-wins.
// ---------------------------------------------------------------------------

class abi_store_writer {
public:
   void add(chain::name account, uint64_t global_seq, std::vector<char> abi_bytes);
   void write(const std::filesystem::path& path) const;
   // Pre-populate from an existing abi_store.log so previously captured ABIs survive
   // across node restarts and are included in the next write().
   void load(const std::filesystem::path& path);
   size_t entry_count() const { return _entries.size(); }

private:
   struct entry {
      uint64_t          account;
      uint64_t          global_seq;
      std::vector<char> abi_bytes;
   };
   std::vector<entry> _entries;
};

// ---------------------------------------------------------------------------
// Reader: load the index from disk, answer (account, global_seq) lookups.
// The file is mmap'd for the lifetime of the reader so that:
//  - blob reads are zero-copy memcpy from the page cache (no syscall per query),
//  - a concurrent writer's atomic rename swaps the path to a new inode without
//    invalidating this reader's view (we still hold the old inode via mmap).
// The index is copied into a vector for binary_search.
// ---------------------------------------------------------------------------

class abi_store_reader {
public:
   explicit abi_store_reader(const std::filesystem::path& path);

   bool valid() const { return _valid; }

   // Returns the ABI bytes in effect for account at global_seq (i.e., the ABI
   // with the largest global_seq <= the query), or nullopt if none is found.
   std::optional<std::vector<char>> lookup(chain::name account, uint64_t global_seq) const;

private:
   boost::iostreams::mapped_file_source _file;
   std::vector<abi_store_index_entry>   _index; // sorted by (account, global_seq)
   uint64_t                             _blob_area_offset{0};
   bool                                 _valid{false};
};

} // namespace sysio::trace_api

FC_REFLECT(sysio::trace_api::abi_store_header,      (magic)(version)(entry_count))
FC_REFLECT(sysio::trace_api::abi_store_index_entry, (account)(global_seq)(blob_offset)(blob_size))

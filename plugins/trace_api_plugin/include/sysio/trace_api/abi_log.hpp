#pragma once

#include <sysio/chain/types.hpp>
#include <fc/io/cfile.hpp>
#include <fc/reflect/reflect.hpp>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace sysio::trace_api {

// ---------------------------------------------------------------------------
// abi_log: append-only on-disk log of ABI records with an in-memory sorted
// index keyed by (account, global_sequence).
//
// Appends stream records to the end of the file with no rewrite of
// existing records.  The lookup index lives in memory and is rebuilt by
// scanning the file at startup.
//
// On-disk format (fc::raw-packed little-endian, x86_64 Linux):
//
//   Header (16 bytes): magic "ABIL" (u32), version 1 (u32), reserved (u64)
//   Records (repeated until EOF):
//     account    (u64)
//     global_seq (u64)
//     blob_size  (u64)
//     blob_bytes (blob_size bytes)
//     crc32      (u32) over (account, global_seq, blob_size, blob_bytes)
//
// Writes are not fsync'd; the on-disk tail may lose the last few records on
// a kernel crash.  On startup we scan records, validate CRCs, and truncate
// the file at the first invalid record.  Lost records can be rebuilt by
// replaying setabi + lazy-fetch against the chain.
//
// Thread-safety:
//   - append() takes _append_mtx for the cfile write, releases it, then
//     takes _index_mtx to insert into the lookup index.  Two mutexes so
//     lookups never wait for file I/O on a concurrent append.
//   - lookup() takes _index_mtx briefly to copy (blob_offset, blob_size),
//     releases it, then pread()s the blob bytes.  pread is atomic w.r.t.
//     the fd's shared position and safe to issue concurrently.
// ---------------------------------------------------------------------------

struct abi_log_header {
   // Stored little-endian on disk so a hex dump of the first 4 bytes reads "ABIL".
   static constexpr uint32_t magic_value     = 0x4C494241; // bytes on disk: 'A','B','I','L'
   static constexpr uint32_t current_version = 1;

   uint32_t magic    = magic_value;
   uint32_t version  = current_version;
   uint64_t reserved = 0;
};
static_assert(sizeof(abi_log_header) == 16);

class abi_log {
public:
   explicit abi_log(const std::filesystem::path& path);

   abi_log(const abi_log&)            = delete;
   abi_log& operator=(const abi_log&) = delete;

   bool valid() const { return _valid; }

   // Append a new ABI record.  Thread-safe.  Last-write-wins for duplicate
   // (account, global_seq) keys.
   void append(chain::name account, uint64_t global_seq, std::vector<char> abi_bytes);

   struct lookup_result {
      uint64_t          effective_global_seq = 0; // global_seq of the ABI record that matched
      std::vector<char> abi_bytes;
   };

   // Look up the ABI in effect for account at the largest recorded
   // global_seq <= the query.  Returns nullopt if no record matches.
   // The returned effective_global_seq is the global_seq the ABI was
   // recorded at (used as a stable cache key by decoders).
   // Thread-safe; may run concurrently with append().
   std::optional<lookup_result> lookup(chain::name account, uint64_t global_seq) const;

   // Returns true if at least one record exists for the account at any
   // global_sequence.  Used by chain extraction to decide whether to lazy-
   // fetch an ABI on first encounter.  Thread-safe.
   bool has_entry(chain::name account) const;

private:
   // Fixed-size record header on disk: (account, global_seq, blob_size).
   // Trailer is a u32 crc32 over (header + blob_bytes).
   // chain::name is a uint64_t-wrapping struct, so this is 24 bytes with no
   // padding and the on-disk wire format is identical to (u64, u64, u64).
   struct record_header {
      chain::name account;
      uint64_t    global_seq = 0;
      uint64_t    blob_size  = 0;
   };
   static_assert(sizeof(record_header) == 24);

   struct index_entry {
      uint64_t blob_file_offset = 0; // file offset of blob_bytes (not the record_header)
      uint64_t blob_size        = 0;
   };
   using index_key = std::pair<chain::name /*account*/, uint64_t /*global_seq*/>;

   // Walk the log file from the header onwards.  Returns the offset of the
   // end of the last valid record.  The file is truncated at that offset if
   // any trailing bytes were discarded due to CRC failure or truncation.
   uint64_t recover_from_disk(const std::filesystem::path& path);

   static uint32_t compute_record_crc(const record_header& hdr, const char* blob, uint64_t blob_size);

   // _append_mtx serializes the cfile write + _end_offset update.
   // _index_mtx guards _index.  Separate so lookups never block on a
   // concurrent append's file I/O.  Append acquires _append_mtx first and
   // always releases it before acquiring _index_mtx, so no deadlock is
   // possible (lookups only take _index_mtx).
   std::mutex                       _append_mtx;
   mutable std::mutex               _index_mtx;
   std::map<index_key, index_entry> _index;
   fc::cfile                        _cfile;        // held open for appends; reads go through fileno() + pread
   uint64_t                         _end_offset{0};
   bool                             _valid{false};
};

} // namespace sysio::trace_api

FC_REFLECT(sysio::trace_api::abi_log_header, (magic)(version)(reserved))

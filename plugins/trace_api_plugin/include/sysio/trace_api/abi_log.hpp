#pragma once

#include <sysio/chain/types.hpp>
#include <fc/io/cfile.hpp>
#include <fc/reflect/reflect.hpp>
#include <atomic>
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
// index keyed by (account, global_sequence), plus an in-memory overlay of
// reversible records keyed the same way.
//
// Irreversibility invariant: the on-disk file only ever contains records
// whose block is at or below LIB - a fork can therefore never invalidate a
// written record, mirroring how the other trace files treat LIB as the
// finality boundary (e.g. the bloom sidecar is only built for irreversible
// slices).  Records for blocks above LIB live in the in-memory reversible
// overlay, which:
//   - participates in lookups immediately (actions in pending blocks decode),
//   - is discarded per-block by rollback_reversible() when a fork replaces a
//     block ("last write wins per block", like slice re-writes), and
//   - is flushed to the file by flush_irreversible() as LIB advances.
//
// Reversible journal: the overlay is also mirrored to a durable append-only
// sidecar ("<stem>.journal") so a restart restores it exactly.  This cannot
// be re-derived from the recorded block traces: setabi records leave an
// action trace, but lazy-fetch (global_seq 0) records are read from chain
// state at first-encounter and leave no trace - and once a later setabi
// supersedes the account, chain state no longer holds the pre-setabi ABI, so
// the lazy bytes are unrecoverable unless they were persisted before the
// restart.  The journal records every reversible mutation in order (PUT on
// append_reversible, ROLLBACK on rollback_reversible); flush_irreversible
// writes nothing to it (a flushed record already lives in the main file and
// is dropped from the overlay on replay).  On restart the journal is
// replayed into the overlay - skipping any (account, global_seq) already on
// disk - then compacted (rewritten to contain only the live, still-reversible
// records).  A journal write failure is advisory: the in-memory overlay still
// serves the current session; only restart durability of that one record is
// lost.
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
//   - append() takes _append_mtx for the file write, releases it, then
//     takes _index_mtx to insert into the lookup index.  Two mutexes so
//     lookups never wait for file I/O on a concurrent append.
//   - Appends go through raw-fd pwrite (one contiguous record per call),
//     never the buffered FILE*.  A failed write therefore leaves all torn
//     bytes in the kernel file - never stranded in a user-space stdio
//     buffer - so recovery is a single resize_file back to _end_offset.
//   - lookup()/fetch() take _index_mtx briefly to copy (blob_offset,
//     blob_size), release it, then pread() the blob bytes.  pread is atomic
//     w.r.t. the fd's shared position and safe to issue concurrently.
//   - The reversible overlay is guarded by _index_mtx (all overlay
//     operations are brief, pure-memory map manipulations).  During
//     flush_irreversible() a record is inserted into the disk index before
//     it is erased from the overlay, so concurrent lookups never observe a
//     gap.  A lookup_seq()/fetch() pair that races a fork rollback can see
//     the record disappear between the two calls; callers treat that as
//     "no ABI recorded", which is correct - the action being decoded was on
//     the same forked-out block and is itself being re-written.
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

// Header for the reversible journal sidecar.  Same 16-byte shape as the main
// log header but a distinct magic so the two files can never be confused.
struct abi_journal_header {
   // Stored little-endian on disk so a hex dump of the first 4 bytes reads "ABIJ".
   static constexpr uint32_t magic_value     = 0x4A494241; // bytes on disk: 'A','B','I','J'
   static constexpr uint32_t current_version = 1;

   uint32_t magic    = magic_value;
   uint32_t version  = current_version;
   uint64_t reserved = 0;
};
static_assert(sizeof(abi_journal_header) == 16);

// One reversible-overlay mutation as recorded in the journal.  Persisted as a
// single byte; converted to/from the wire value with magic_enum (never a raw
// static_cast) so a rename cannot silently change the on-disk encoding.
enum class abi_journal_op : uint8_t {
   put      = 1, ///< add/overwrite a reversible record: (block_num, account, global_seq, blob)
   rollback = 2, ///< discard reversible records with block_num >= the recorded height (fork replace)
};

class abi_log {
public:
   // Once the reversible journal grows past this many bytes, flush_irreversible rewrites it down to
   // just the live overlay (see compact_journal), so it cannot grow without bound on a node that runs
   // for months without a restart.  The default suits production; tests pass a small value to drive
   // the path cheaply.
   static constexpr size_t default_journal_compaction_threshold = 8u << 20; // 8 MiB

   explicit abi_log(const std::filesystem::path& path,
                    size_t journal_compaction_threshold_bytes = default_journal_compaction_threshold);

   abi_log(const abi_log&)            = delete;
   abi_log& operator=(const abi_log&) = delete;

   bool valid() const { return _valid.load(std::memory_order_relaxed); }

   // Append an irreversible ABI record directly to disk.  Thread-safe.
   // Last-write-wins for duplicate (account, global_seq) keys.  On a write
   // failure the torn tail is truncated back to the last good record and
   // false is returned (the record is lost); if even the truncation fails
   // the log disables itself (valid() turns false) rather than serve
   // mis-aligned blob offsets.  Callers must only pass records whose block
   // is already irreversible - reversible records go through
   // append_reversible() so a fork can roll them back.
   bool append(chain::name account, uint64_t global_seq, std::vector<char> abi_bytes);

   // Record an ABI for a block that is not yet irreversible.  In-memory
   // only; participates in lookups immediately.  The record becomes
   // permanent when flush_irreversible() passes block_num, or is discarded
   // by rollback_reversible() if a fork replaces the block.  Last-write-wins
   // for duplicate (account, global_seq) keys.  No-op when valid() is false
   // (the log is fully disabled, lookups would never serve the record).
   // Thread-safe.
   void append_reversible(uint32_t block_num, chain::name account, uint64_t global_seq,
                          std::vector<char> abi_bytes);

   // Discard reversible records with block_num >= the given height.  Called
   // when a block at that height starts being applied: any previously
   // accepted block at or above it is being replaced by a fork switch, so
   // its ABI records must stop participating in lookups.  Records already
   // flushed to disk are never touched (a fork cannot reach below LIB).
   // Thread-safe.
   void rollback_reversible(uint32_t block_num);

   // Persist reversible records whose block can no longer fork out
   // (block_num <= lib) to the on-disk log and drop them from the overlay.
   // Driven by store_provider::append_lib, i.e. the chain's irreversible
   // block signal - the same point at which the other trace files treat a
   // block as final.  A failed disk write leaves the affected records in the
   // overlay for retry on the next LIB advance.  When the log is disabled
   // (valid() false) flushable records are dropped instead, so the overlay
   // cannot grow without bound.  Thread-safe.
   void flush_irreversible(uint32_t lib);

   // Number of records currently in the reversible overlay.  Test/diagnostic
   // aid.  Thread-safe.
   size_t reversible_size() const;

   struct lookup_result {
      uint64_t          effective_global_seq = 0; // global_seq of the ABI record that matched
      std::vector<char> abi_bytes;
   };

   // Resolve only the effective global_seq for account at the largest
   // recorded global_seq <= the query (no blob I/O - cheap enough to call
   // per action as a cache-key probe).  Resolves over the union of the
   // on-disk index and the reversible overlay.  Returns nullopt if no
   // record matches.  Thread-safe; may run concurrently with append().
   std::optional<uint64_t> lookup_seq(chain::name account, uint64_t global_seq) const;

   // Fetch the blob recorded at an exact (account, effective_global_seq)
   // key as previously resolved by lookup_seq.  Served from the reversible
   // overlay when present there, otherwise performs the blob pread.
   // Returns nullopt when the key is not recorded or the read fails.
   // Thread-safe; may run concurrently with append().
   std::optional<std::vector<char>> fetch(chain::name account, uint64_t effective_global_seq) const;

   // Composition of lookup_seq + fetch: the ABI in effect for account at
   // global_seq with its blob.  The returned effective_global_seq is the
   // global_seq the ABI was recorded at (used as a stable cache key by
   // decoders).  Thread-safe; may run concurrently with append().
   std::optional<lookup_result> lookup(chain::name account, uint64_t global_seq) const;

   // Returns true if at least one record exists for the account at any
   // global_sequence, on disk or in the reversible overlay.  Used by chain
   // extraction to decide whether to lazy-fetch an ABI on first encounter.
   // Thread-safe.
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

   // One reversible (above-LIB) record: the blob is held in memory because the
   // file only ever contains irreversible records.  block_num is the accepted
   // block that committed the record; rollback_reversible and
   // flush_irreversible partition the overlay by it.
   struct reversible_entry {
      uint32_t          block_num = 0;
      std::vector<char> abi_bytes;
   };

   // Walk the log file from the header onwards.  Returns the offset of the
   // end of the last valid record.  The file is truncated at that offset if
   // any trailing bytes were discarded due to CRC failure or truncation.
   uint64_t recover_from_disk(const std::filesystem::path& path);

   static uint32_t compute_record_crc(const record_header& hdr, const char* blob, uint64_t blob_size);

   // --- reversible journal (durable mirror of the in-memory overlay) ---

   // Derive the journal path from the main log path: same directory and stem,
   // extension ".journal".
   static std::filesystem::path derive_journal_path(const std::filesystem::path& log_path);

   // Open (or create) the journal, replay it into the overlay, then compact.
   // Constructor-only, after the main log has been validated.
   void init_journal(const std::filesystem::path& journal_path);

   // Write the journal header at offset 0 and flush.  Returns false on I/O error.
   bool write_journal_header();

   // Replay journal records in order into a working overlay (applying PUT and
   // ROLLBACK), truncating any torn/corrupt tail.  Requires the journal open.
   std::map<index_key, reversible_entry> replay_journal_records();

   // Rewrite the journal so it holds only the current _reversible overlay
   // (drops flushed/dead PUTs and collapses applied ROLLBACKs).  Atomic via a
   // temp file + rename; reopens _journal_cfile for appending on success.
   // Snapshots the overlay under _index_mtx, then does all file I/O under
   // _journal_mtx, so it is safe to call live from flush_irreversible as well
   // as from the constructor.
   void compact_journal();

   // Frame and append one already-encoded journal body.  No-op when journaling
   // is disabled.  Returns false on write failure (the torn tail is truncated;
   // journaling is disabled only if that truncation also fails).  Takes
   // _journal_mtx; never nests with _index_mtx.
   bool journal_append(const std::vector<char>& body);

   // Encode a PUT / ROLLBACK body (op byte + fc::raw fields).  No file I/O.
   static std::vector<char> encode_put_body(uint32_t block_num, chain::name account,
                                            uint64_t global_seq, const std::vector<char>& abi_bytes);
   static std::vector<char> encode_rollback_body(uint32_t block_num);

   // Wrap a body as a journal record: 4-byte little-endian length, body, 4-byte crc32.
   static std::vector<char> frame_journal_record(const std::vector<char>& body);

   // _append_mtx serializes the cfile write + _end_offset update.
   // _index_mtx guards _index and _reversible.  Separate so lookups never
   // block on a concurrent append's file I/O.  Append acquires _append_mtx
   // first and always releases it before acquiring _index_mtx, so no
   // deadlock is possible (lookups only take _index_mtx).
   std::mutex                            _append_mtx;
   mutable std::mutex                    _index_mtx;
   std::map<index_key, index_entry>      _index;
   std::map<index_key, reversible_entry> _reversible;
   fc::cfile                        _cfile;        // holds the fd; appends use pwrite, reads use pread (both via fileno())
   uint64_t                         _end_offset{0};
   // Atomic because append() can flip it to false (failed tail truncation) while lookups on HTTP
   // threads read it concurrently.
   std::atomic<bool>                _valid{false};

   // Journal sidecar state.  _journal_mtx serializes journal file writes and
   // never nests with _append_mtx/_index_mtx.  _journal_enabled gates appends:
   // false means the journal could not be opened (or a write failed
   // irrecoverably), in which case the overlay still works for the current
   // session but its records will not survive a restart.  Only the single
   // extraction thread mutates the journal, so these need no atomicity beyond
   // the mutex that orders the writes.
   std::mutex                       _journal_mtx;
   fc::cfile                        _journal_cfile;
   std::filesystem::path            _journal_path;
   uint64_t                         _journal_end_offset{0};
   bool                             _journal_enabled{false};
   size_t                           _journal_compaction_threshold{default_journal_compaction_threshold};
};

} // namespace sysio::trace_api

FC_REFLECT(sysio::trace_api::abi_log_header, (magic)(version)(reserved))
FC_REFLECT(sysio::trace_api::abi_journal_header, (magic)(version)(reserved))

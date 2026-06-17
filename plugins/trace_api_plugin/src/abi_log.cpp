#include <sysio/trace_api/abi_log.hpp>
#include <sysio/trace_api/logging.hpp>

#include <fc/io/raw.hpp>

#include <boost/crc.hpp>

#include <magic_enum/magic_enum.hpp>

#include <cassert>
#include <cstring>
#include <stdexcept>
#include <type_traits>

namespace sysio::trace_api {

namespace {

// Record layout: header(24) + blob_bytes(blob_size) + crc32(4).
// CRC covers header + blob_bytes (not itself).
constexpr uint64_t record_trailer_size = sizeof(uint32_t);

// Journal record framing: [u32 body length][body bytes][u32 crc32].
// The trailing crc covers the 4 length bytes plus the body, so a corrupt
// length is caught as readily as corrupt body bytes.
constexpr uint64_t journal_record_len_size = sizeof(uint32_t);
constexpr uint64_t journal_record_crc_size = sizeof(uint32_t);

constexpr uint64_t file_header_offset = 0;

/**
 * Read an fc::raw-packed fixed-size POD header through cfile's positional API.
 */
template<typename T>
T pread_packed_header(const fc::cfile& file) {
   static_assert(std::is_trivially_copyable_v<T>);
   std::vector<char> data(sizeof(T));
   file.pread(data.data(), data.size(), file_header_offset);
   fc::datastream<const char*> ds(data.data(), data.size());
   T value;
   fc::raw::unpack(ds, value);
   return value;
}

/**
 * Write an fc::raw-packed fixed-size POD header through cfile's positional API.
 */
template<typename T>
void pwrite_packed_header(fc::cfile& file, const T& value) {
   static_assert(std::is_trivially_copyable_v<T>);
   auto data = fc::raw::pack(value);
   assert(data.size() == sizeof(T));
   file.pwrite(data.data(), data.size(), file_header_offset);
}

} // namespace

uint32_t abi_log::compute_record_crc(const record_header& hdr, const char* blob, uint64_t blob_size) {
   boost::crc_32_type crc;
   crc.process_bytes(&hdr, sizeof(hdr));
   if (blob_size > 0)
      crc.process_bytes(blob, blob_size);
   return crc.checksum();
}

abi_log::abi_log(const std::filesystem::path& path, size_t journal_compaction_threshold_bytes)
   : _journal_compaction_threshold(journal_compaction_threshold_bytes) {
   _cfile.set_file_path(path);
   bool existed = false;
   try {
      existed = _cfile.open_existing_or_create_new();
   } catch (...) {
      fc_wlog(_log, "trace_api: abi_log failed to open {}", path.generic_string());
      return;
   }

   if (!existed) {
      // Fresh file: write the header positionally, with no stdio buffering mixed into later pread()
      // and pwrite() calls.
      try {
         pwrite_packed_header(_cfile, abi_log_header{});
      } catch (...) {
         fc_wlog(_log, "trace_api: abi_log failed to write header to {}", path.generic_string());
         return;
      }
      _end_offset = sizeof(abi_log_header);
      _valid = true;
      init_journal(derive_journal_path(path));
      return;
   }

   // Existing file: validate header, scan records, populate index, truncate any bad tail.
   try {
      abi_log_header hdr = pread_packed_header<abi_log_header>(_cfile);
      if (hdr.magic != abi_log_header::magic_value) {
         fc_wlog(_log, "trace_api: abi_log {} has wrong magic {:#x}, ignoring",
              path.generic_string(), hdr.magic);
         return;
      }
      if (hdr.version != abi_log_header::current_version) {
         fc_wlog(_log, "trace_api: abi_log {} has unsupported version {}, ignoring",
              path.generic_string(), hdr.version);
         return;
      }
   } catch (...) {
      fc_wlog(_log, "trace_api: abi_log {} header read failed", path.generic_string());
      return;
   }

   _end_offset = recover_from_disk(path);
   _valid = true;
   init_journal(derive_journal_path(path));
}

uint64_t abi_log::recover_from_disk(const std::filesystem::path& path) {
   const uint64_t header_size = sizeof(abi_log_header);
   const uint64_t file_size = std::filesystem::file_size(path);

   // pread correctness: all writes on this cfile go through raw-fd pwrite.  Never introduce
   // unflushed buffered writes on this cfile or pread may see stale data.
   uint64_t offset = header_size;
   while (offset < file_size) {
      const uint64_t record_start = offset;

      // Need at least record_header + crc trailer.
      if (file_size - record_start < sizeof(record_header) + record_trailer_size) {
         fc_wlog(_log, "trace_api: abi_log {} torn tail at offset {} (less than minimal record), truncating",
              path.generic_string(), record_start);
         break;
      }

      record_header rh{};
      try {
         _cfile.pread(reinterpret_cast<char*>(&rh), sizeof(rh), record_start);
      } catch (const std::exception& e) {
         fc_wlog(_log, "trace_api: abi_log {} failed to read record header at offset {}: {}, truncating",
              path.generic_string(), record_start, e.what());
         break;
      }

      const uint64_t blob_file_offset = record_start + sizeof(record_header);
      const uint64_t crc_offset       = blob_file_offset + rh.blob_size;
      const uint64_t record_end       = crc_offset + record_trailer_size;

      if (record_end > file_size) {
         fc_wlog(_log, "trace_api: abi_log {} record at {} claims blob_size {} but file has only {} bytes remaining, truncating",
              path.generic_string(), record_start, rh.blob_size, file_size - blob_file_offset);
         break;
      }

      std::vector<char> blob;
      uint32_t stored_crc = 0;
      try {
         if (rh.blob_size > 0) {
            blob.resize(rh.blob_size);
            _cfile.pread(blob.data(), rh.blob_size, blob_file_offset);
         }
         _cfile.pread(reinterpret_cast<char*>(&stored_crc), sizeof(stored_crc), crc_offset);
      } catch (const std::exception& e) {
         fc_wlog(_log, "trace_api: abi_log {} failed to read record body at offset {}: {}, truncating",
              path.generic_string(), record_start, e.what());
         break;
      }

      const uint32_t computed_crc = compute_record_crc(rh, blob.data(), rh.blob_size);
      if (computed_crc != stored_crc) {
         fc_wlog(_log, "trace_api: abi_log {} crc mismatch at record offset {} (stored {:#x} vs computed {:#x}), truncating",
              path.generic_string(), record_start, stored_crc, computed_crc);
         break;
      }

      // std::map::operator[] — duplicate (account, global_seq) silently overwrites (last-write-wins).
      _index[{rh.account, rh.global_seq}] = index_entry{blob_file_offset, rh.blob_size};
      offset = record_end;
   }

   if (offset < file_size) {
      // Drop the bad tail.  resize_file works regardless of whether the
      // file is currently open; subsequent writes via _cfile will append
      // from _end_offset.
      std::error_code ec;
      std::filesystem::resize_file(path, offset, ec);
      if (ec) {
         fc_wlog(_log, "trace_api: abi_log {} failed to truncate to {}: {}",
              path.generic_string(), offset, ec.message());
      }
   }

   return offset;
}

// NOTE: callers of append()/has_entry() are expected to be single-threaded
// (the chain extraction thread).  The append+index-insert sequence is not
// strictly atomic across the two mutexes; a concurrent caller could slip an
// insert for the same key between our write and our index update, producing
// a duplicate record.  This is harmless given last-write-wins but not obvious.
bool abi_log::append(chain::name account, uint64_t global_seq, std::vector<char> abi_bytes) {
   if (!valid()) return false;
   // Test aid: simulate a main-log disk write error (no bytes written) so the fatal flush path can
   // be exercised deterministically; always false in production.
   if (_force_main_log_write_failure) return false;

   record_header rh{ account, global_seq, abi_bytes.size() };
   const uint32_t crc = compute_record_crc(rh, abi_bytes.data(), abi_bytes.size());

   // One contiguous buffer so the whole record lands in a single pwrite.  Raw-fd writes (no stdio
   // buffering) keep failure recovery simple: torn bytes from a partial write are fully in the
   // kernel file - never half-stranded in a user-space FILE* buffer - so truncating the file back
   // to _end_offset restores the invariant "file ends exactly at the last good record".
   std::vector<char> record(sizeof(rh) + abi_bytes.size() + record_trailer_size);
   std::memcpy(record.data(), &rh, sizeof(rh));
   if (!abi_bytes.empty())
      std::memcpy(record.data() + sizeof(rh), abi_bytes.data(), abi_bytes.size());
   std::memcpy(record.data() + sizeof(rh) + abi_bytes.size(), &crc, sizeof(crc));

   uint64_t blob_file_offset = 0;
   {
      std::lock_guard<std::mutex> lock(_append_mtx);
      try {
         // "_end_offset is EOF" is a maintained invariant (restored below on failure), so this
         // positional write appends without relying on O_APPEND.
         _cfile.pwrite(record.data(), record.size(), _end_offset);
      } catch (const std::exception& e) {
         fc_wlog(_log, "trace_api: abi_log append of {} bytes at offset {} failed: {}; truncating torn tail",
                 record.size(), _end_offset, e.what());
         // Without this truncation, every subsequent append would land at the real (torn) EOF while
         // its index entry is computed from _end_offset - permanently mis-aligned blob offsets served
         // to decoders.  resize_file works on the path regardless of the open fd, and concurrent
         // lookups only pread committed regions below _end_offset, which the truncation never touches.
         std::error_code ec;
         std::filesystem::resize_file(_cfile.get_file_path(), _end_offset, ec);
         if (ec) {
            fc_wlog(_log, "trace_api: abi_log could not truncate torn tail to offset {}: {}; "
                          "disabling further ABI capture", _end_offset, ec.message());
            _valid.store(false, std::memory_order_relaxed);
         }
         return false;
      }

      blob_file_offset = _end_offset + sizeof(rh);
      _end_offset += record.size();
   }

   {
      std::lock_guard<std::mutex> lock(_index_mtx);
      // std::map::operator[] — duplicate (account, global_seq) silently overwrites (last-write-wins).
      _index[{rh.account, rh.global_seq}] = index_entry{blob_file_offset, rh.blob_size};
   }
   return true;
}

void abi_log::append_reversible(uint32_t block_num, chain::name account, uint64_t global_seq,
                                std::vector<char> abi_bytes) {
   if (!valid()) return;
   // Journal before the in-memory insert: the overlay is volatile, so persisting first means a crash
   // in between still restores the record on the next start (the encode reads abi_bytes before it is
   // moved into the overlay).  Recording history is the whole point of a trace_api node, so a failure
   // to persist is fatal, not a silent degradation: throw, which the extraction signal handler turns
   // into a clean node shutdown.  Treating every persistence failure uniformly as fatal keeps the
   // operator contract simple - the node either records completely or stops.
   if (!journal_append(encode_put_body(block_num, account, global_seq, abi_bytes))) {
      fc_elog(_log, "trace_api: abi_log could not persist reversible ABI record for {} at global_seq {}",
              account, global_seq);
      throw std::runtime_error("trace_api: failed to persist an ABI journal record - a trace_api node "
                               "must record complete history. Resolve the storage error (disk full or "
                               "I/O failure) and relaunch.");
   }
   std::lock_guard<std::mutex> lock(_index_mtx);
   // std::map::operator[] — duplicate (account, global_seq) silently overwrites (last-write-wins).
   // A replay that re-commits a key already flushed to disk leaves both copies; fetch prefers the
   // overlay and the bytes are identical, so the duplication is harmless and resolves at flush.
   _reversible[{account, global_seq}] = reversible_entry{block_num, std::move(abi_bytes)};
}

void abi_log::rollback_reversible(uint32_t block_num) {
   // Cheap peek: is there anything to discard?  In normal forward operation no reversible record
   // sits at or above block_num, so this is a no-op and the journal is never touched (block_start
   // fires every block; a per-block journal write would be unbounded).  Single writer (the
   // extraction thread) guarantees nothing adds a qualifying record between the peek and the erase.
   bool any = false;
   {
      std::lock_guard<std::mutex> lock(_index_mtx);
      for (const auto& [key, entry] : _reversible) {
         if (entry.block_num >= block_num) { any = true; break; }
      }
   }
   if (!any)
      return;

   // Journal the rollback before applying it: persisting the intent first means a crash in between
   // cannot resurrect the forked-out records on the next start (replay applies the ROLLBACK in order).
   const bool recorded = journal_append(encode_rollback_body(block_num));

   {
      std::lock_guard<std::mutex> lock(_index_mtx);
      for (auto it = _reversible.begin(); it != _reversible.end();) {
         if (it->second.block_num >= block_num)
            it = _reversible.erase(it);
         else
            ++it;
      }
   }

   // Fail-closed, then fatal.  A PUT for the rolled-back block may already be durable; if the ROLLBACK
   // could not be recorded (write error, or journaling disabled), that PUT is now orphaned and a
   // restart would replay it and resurrect a forked-out ABI - the exact wrong-decode this journal
   // prevents.  Leave the on-disk journal both safe AND complete before aborting: the overlay has
   // already had the rolled-back records erased, so rewrite the journal to it (compact_journal),
   // which drops the orphaned forked PUTs (block >= block_num) while keeping the canonical records
   // below the fork - so a restart neither resurrects forked data nor loses the still-reversible
   // window.  If even that rewrite fails (e.g. the disk is full), fall back to truncating the journal
   // to its header: safe, though the current window then degrades to raw hex.  Then abort - a
   // trace_api node that cannot record must stop rather than serve incomplete/incorrect history.
   if (!recorded) {
      compact_journal();
      if (!_journal_enabled)
         reset_journal_fail_closed();
      fc_elog(_log, "trace_api: abi_log could not persist ABI rollback at block {}", block_num);
      throw std::runtime_error("trace_api: failed to persist an ABI journal rollback - a trace_api node "
                               "must record complete history. Resolve the storage error (disk full or "
                               "I/O failure) and relaunch.");
   }
}

void abi_log::flush_irreversible(uint32_t lib) {
   if (!valid()) {
      // The disk log is disabled, so these records can never be persisted and lookups never serve
      // them (valid() gates every read path).  Drop the flushable range so the overlay stays
      // bounded on a node running in this degraded state.
      std::lock_guard<std::mutex> lock(_index_mtx);
      for (auto it = _reversible.begin(); it != _reversible.end();) {
         if (it->second.block_num <= lib)
            it = _reversible.erase(it);
         else
            ++it;
      }
      return;
   }

   // Snapshot the flushable set under the lock (blob copies - flushes are rare and ABIs are
   // small), then write outside it so lookups never wait on file I/O.
   std::vector<std::pair<index_key, std::vector<char>>> flushable;
   {
      std::lock_guard<std::mutex> lock(_index_mtx);
      for (const auto& [key, entry] : _reversible) {
         if (entry.block_num <= lib)
            flushable.emplace_back(key, entry.abi_bytes);
      }
   }

   for (auto& [key, bytes] : flushable) {
      // append() inserts into _index before we erase from the overlay, so concurrent lookups
      // always find the record in at least one of the two maps.
      if (!append(key.first, key.second, std::move(bytes))) {
         // Cannot persist this irreversible record to the main log.  Recording history is the point
         // of a trace_api node, so this is fatal - uniform with the journal write paths - rather than
         // serving incomplete history.  No data is lost: the record is still in the reversible overlay
         // AND its journal PUT, so a restart with storage available restores it from the journal and
         // re-flushes it on the next LIB advance.  The throw routes through the extraction store_lib
         // handler to a clean controller shutdown.
         fc_elog(_log, "trace_api: abi_log could not persist irreversible ABI record for {} at global_seq {}",
                 key.first, key.second);
         throw std::runtime_error("trace_api: failed to persist an ABI record to the main log - a "
                                  "trace_api node must record complete history. Resolve the storage "
                                  "error (disk full or I/O failure) and relaunch.");
      }
      std::lock_guard<std::mutex> lock(_index_mtx);
      _reversible.erase(key);
   }

   // Bound the journal so it cannot grow without limit on a node that runs for months without a
   // restart.  Two mechanisms, both keyed off the post-flush state (single writer - the extraction
   // thread - so nothing adds a record between the check and the action):
   //
   //  - Drain reset (cheap truncate): once the overlay is empty, every journaled PUT is dead (its
   //    key is now on disk and a replay would drop it), so truncate the file back to its header.
   //    This is the common case on a healthy node, where the overlay drains between capture bursts;
   //    the _journal_end_offset guard skips the no-capture block so its rate tracks captures.
   //
   //  - Threshold compaction (rewrite to live overlay): the backstop for a busy node whose overlay
   //    rarely hits exactly empty.  Once the journal passes _journal_compaction_threshold it is
   //    rewritten down to just the still-reversible records, so its size is bounded by the threshold
   //    regardless of uptime, and the rewrite cost amortizes to O(1) per byte written.
   //
   // Both are crash-safe: a crash mid-truncate/mid-rewrite leaves either the old journal or a torn
   // tail the next replay discards, and replay's drop-if-on-disk keeps the reconstructed overlay
   // correct either way.
   bool overlay_empty = false;
   {
      std::lock_guard<std::mutex> lock(_index_mtx);
      overlay_empty = _reversible.empty();
   }
   if (overlay_empty) {
      std::lock_guard<std::mutex> lock(_journal_mtx);
      const uint64_t header_size = sizeof(abi_journal_header);
      if (_journal_enabled && _journal_end_offset > header_size) {
         std::error_code ec;
         std::filesystem::resize_file(_journal_path, header_size, ec);
         if (ec)
            fc_wlog(_log, "trace_api: abi_log journal {} failed to reset to header after drain: {}",
                    _journal_path.generic_string(), ec.message());
         else
            _journal_end_offset = header_size;
      }
   } else if (_journal_enabled && _journal_end_offset >= _journal_compaction_threshold) {
      compact_journal(); // takes _journal_mtx itself; never called with it held
   }
}

size_t abi_log::reversible_size() const {
   std::lock_guard<std::mutex> lock(_index_mtx);
   return _reversible.size();
}

bool abi_log::has_entry(chain::name account) const {
   if (!valid()) return false;
   std::lock_guard<std::mutex> lock(_index_mtx);
   auto it = _index.lower_bound({account, 0});
   if (it != _index.end() && it->first.first == account)
      return true;
   auto rit = _reversible.lower_bound({account, 0});
   return rit != _reversible.end() && rit->first.first == account;
}

std::optional<uint64_t> abi_log::lookup_seq(chain::name account, uint64_t global_seq) const {
   std::optional<uint64_t> result;
   if (!valid()) return result;

   std::lock_guard<std::mutex> lock(_index_mtx);
   // Largest recorded seq <= the query across the union of the on-disk index and the reversible
   // overlay.  Both maps share the key type, so one generic probe serves both.
   auto probe = [&](const auto& map) {
      auto it = map.upper_bound(index_key{account, global_seq});
      if (it == map.begin())
         return;
      --it;
      if (it->first.first != account)
         return;
      if (!result || it->first.second > *result)
         result = it->first.second;
   };
   probe(_index);
   probe(_reversible);
   return result;
}

std::optional<std::vector<char>> abi_log::fetch(chain::name account, uint64_t effective_global_seq) const {
   std::optional<std::vector<char>> result;
   if (!valid()) return result;

   uint64_t blob_file_offset = 0;
   uint64_t blob_size        = 0;
   {
      std::lock_guard<std::mutex> lock(_index_mtx);
      // Overlay first: reversible records have no disk presence, and during a flush a record
      // briefly exists in both maps with identical bytes, so overlay-first is always consistent.
      auto rit = _reversible.find({account, effective_global_seq});
      if (rit != _reversible.end()) {
         result.emplace(rit->second.abi_bytes);
         return result;
      }
      auto it = _index.find({account, effective_global_seq});
      if (it == _index.end())
         return result;
      blob_file_offset = it->second.blob_file_offset;
      blob_size        = it->second.blob_size;
   }

   if (blob_size == 0) {
      result.emplace();
      return result;
   }

   // pread correctness: append() pwrite()s the whole record (no user-space buffering) and advances
   // _end_offset only afterwards, so every indexed blob region is fully visible to the fd by the
   // time its index entry exists.
   try {
      std::vector<char> out(blob_size);
      _cfile.pread(out.data(), blob_size, blob_file_offset);
      result.emplace(std::move(out));
   } catch (const std::exception& e) {
      fc_wlog(_log, "trace_api: abi_log pread of {} bytes at {} failed: {}", blob_size, blob_file_offset, e.what());
   }
   return result;
}

std::optional<abi_log::lookup_result> abi_log::lookup(chain::name account, uint64_t global_seq) const {
   // Composition of the two-phase API; kept for callers (and tests) that want the version and the
   // blob in one call.
   std::optional<lookup_result> result;
   const std::optional<uint64_t> effective_seq = lookup_seq(account, global_seq);
   if (!effective_seq)
      return result;
   std::optional<std::vector<char>> blob = fetch(account, *effective_seq);
   if (!blob)
      return result;
   result.emplace(lookup_result{*effective_seq, std::move(*blob)});
   return result;
}

// ---------------------------------------------------------------------------
// Reversible journal
// ---------------------------------------------------------------------------

std::filesystem::path abi_log::derive_journal_path(const std::filesystem::path& log_path) {
   std::filesystem::path journal = log_path;
   journal.replace_extension(".journal");
   return journal;
}

std::vector<char> abi_log::encode_put_body(uint32_t block_num, chain::name account,
                                           uint64_t global_seq, const std::vector<char>& abi_bytes) {
   // magic_enum (never static_cast) for the on-disk op byte: a rename of the enumerator cannot
   // silently change the encoding.
   const uint8_t  op            = magic_enum::enum_integer(abi_journal_op::put);
   const uint64_t account_value = account.to_uint64_t();

   // Two-pass: size with a counting datastream, then pack into the exact buffer.
   fc::datastream<size_t> sz;
   fc::raw::pack(sz, op);
   fc::raw::pack(sz, block_num);
   fc::raw::pack(sz, account_value);
   fc::raw::pack(sz, global_seq);
   fc::raw::pack(sz, abi_bytes);

   std::vector<char> body(sz.tellp());
   fc::datastream<char*> ds(body.data(), body.size());
   fc::raw::pack(ds, op);
   fc::raw::pack(ds, block_num);
   fc::raw::pack(ds, account_value);
   fc::raw::pack(ds, global_seq);
   fc::raw::pack(ds, abi_bytes);
   return body;
}

std::vector<char> abi_log::encode_rollback_body(uint32_t block_num) {
   const uint8_t op = magic_enum::enum_integer(abi_journal_op::rollback);

   fc::datastream<size_t> sz;
   fc::raw::pack(sz, op);
   fc::raw::pack(sz, block_num);

   std::vector<char> body(sz.tellp());
   fc::datastream<char*> ds(body.data(), body.size());
   fc::raw::pack(ds, op);
   fc::raw::pack(ds, block_num);
   return body;
}

std::vector<char> abi_log::frame_journal_record(const std::vector<char>& body) {
   const uint32_t len = static_cast<uint32_t>(body.size());
   std::vector<char> rec(journal_record_len_size + body.size() + journal_record_crc_size);
   std::memcpy(rec.data(), &len, sizeof(len));
   if (!body.empty())
      std::memcpy(rec.data() + journal_record_len_size, body.data(), body.size());
   boost::crc_32_type crc;
   crc.process_bytes(rec.data(), journal_record_len_size + body.size());
   const uint32_t crcv = crc.checksum();
   std::memcpy(rec.data() + journal_record_len_size + body.size(), &crcv, sizeof(crcv));
   return rec;
}

bool abi_log::write_journal_header() {
   try {
      // Positional header write keeps the journal on the same raw-fd I/O path as journal records.
      pwrite_packed_header(_journal_cfile, abi_journal_header{});
   } catch (...) {
      fc_wlog(_log, "trace_api: abi_log journal failed to write header to {}",
              _journal_path.generic_string());
      return false;
   }
   return true;
}

bool abi_log::journal_append(const std::vector<char>& body) {
   // _force_journal_write_failure is a test aid that simulates a disk write error (false, no bytes
   // written) so the fail-closed rollback path can be exercised deterministically; always false in
   // production.
   if (!_journal_enabled || _force_journal_write_failure)
      return false;

   auto rec = frame_journal_record(body);
   std::lock_guard<std::mutex> lock(_journal_mtx);
   try {
      // Like the main log, _journal_end_offset is maintained as EOF and used as the pwrite offset.
      _journal_cfile.pwrite(rec.data(), rec.size(), _journal_end_offset);
   } catch (const std::exception& e) {
      fc_wlog(_log, "trace_api: abi_log journal append of {} bytes at offset {} failed: {}; truncating torn tail",
              rec.size(), _journal_end_offset, e.what());
      // Restore "file ends at _journal_end_offset" so the next append (or a restart's replay) is
      // never misled by a half-written record.
      std::error_code ec;
      std::filesystem::resize_file(_journal_path, _journal_end_offset, ec);
      if (ec) {
         fc_wlog(_log, "trace_api: abi_log journal could not truncate torn tail to offset {}: {}; "
                       "disabling journaling (reversible records will not survive a restart)",
                 _journal_end_offset, ec.message());
         _journal_enabled = false;
      }
      return false;
   }
   _journal_end_offset += rec.size();
   return true;
}

void abi_log::reset_journal_fail_closed() {
   // A ROLLBACK could not be journaled, so the file may hold orphaned PUT(s) for the rolled-back
   // block.  Best-effort truncate back to the header: that drops every record (the orphaned PUT and
   // the still-live ones alike), so a restart starts from an empty overlay and cannot resurrect the
   // forked-out ABI.  The caller aborts the node immediately after, so this only needs to leave the
   // on-disk journal safe for the restart (re-applied reversible blocks repopulate it); sacrificing
   // restart-durability of the current window degrades to a missing ABI (raw hex), never a wrong one.
   std::lock_guard<std::mutex> lock(_journal_mtx);
   std::error_code ec;
   std::filesystem::resize_file(_journal_path, sizeof(abi_journal_header), ec);
   if (!ec)
      _journal_end_offset = sizeof(abi_journal_header);
   else
      fc_wlog(_log, "trace_api: abi_log journal {} could not be reset before shutdown: {}",
              _journal_path.generic_string(), ec.message());
}

std::map<abi_log::index_key, abi_log::reversible_entry> abi_log::replay_journal_records() {
   std::map<index_key, reversible_entry> working;
   const uint64_t header_size = sizeof(abi_journal_header);
   const uint64_t file_size   = std::filesystem::file_size(_journal_path);

   uint64_t offset = header_size;
   while (offset < file_size) {
      const uint64_t rec_start = offset;

      if (file_size - rec_start < journal_record_len_size + journal_record_crc_size) {
         fc_wlog(_log, "trace_api: abi_log journal {} torn tail at offset {} (less than minimal record), truncating",
                 _journal_path.generic_string(), rec_start);
         break;
      }

      uint32_t body_len = 0;
      try {
         _journal_cfile.pread(reinterpret_cast<char*>(&body_len), sizeof(body_len), rec_start);
      } catch (const std::exception& e) {
         fc_wlog(_log, "trace_api: abi_log journal {} failed to read record length at offset {}: {}, truncating",
                 _journal_path.generic_string(), rec_start, e.what());
         break;
      }

      const uint64_t body_off = rec_start + journal_record_len_size;
      const uint64_t crc_off  = body_off + body_len;
      const uint64_t rec_end  = crc_off + journal_record_crc_size;
      if (rec_end > file_size) {
         fc_wlog(_log, "trace_api: abi_log journal {} record at {} claims body {} but only {} bytes remain, truncating",
                 _journal_path.generic_string(), rec_start, body_len, file_size - body_off);
         break;
      }

      std::vector<char> body(body_len);
      uint32_t stored_crc = 0;
      try {
         if (body_len > 0)
            _journal_cfile.pread(body.data(), body_len, body_off);
         _journal_cfile.pread(reinterpret_cast<char*>(&stored_crc), sizeof(stored_crc), crc_off);
      } catch (const std::exception& e) {
         fc_wlog(_log, "trace_api: abi_log journal {} failed to read record body at offset {}: {}, truncating",
                 _journal_path.generic_string(), rec_start, e.what());
         break;
      }

      // CRC covers the 4 length bytes (as written) plus the body.
      boost::crc_32_type crc;
      char len_bytes[journal_record_len_size];
      std::memcpy(len_bytes, &body_len, sizeof(body_len));
      crc.process_bytes(len_bytes, sizeof(len_bytes));
      if (body_len > 0)
         crc.process_bytes(body.data(), body_len);
      if (crc.checksum() != stored_crc) {
         fc_wlog(_log, "trace_api: abi_log journal {} crc mismatch at record offset {} (stored {:#x} vs computed {:#x}), truncating",
                 _journal_path.generic_string(), rec_start, stored_crc, crc.checksum());
         break;
      }

      try {
         fc::datastream<const char*> ds(body.data(), body.size());
         uint8_t op_raw = 0;
         fc::raw::unpack(ds, op_raw);
         const auto op = magic_enum::enum_cast<abi_journal_op>(op_raw);
         if (!op) {
            // CRC was valid, so this is a format the bytes were validly written for but this build
            // does not understand (version skew).  Skip the record rather than misinterpret it.
            fc_wlog(_log, "trace_api: abi_log journal {} unknown op {} at offset {}, skipping record",
                    _journal_path.generic_string(), op_raw, rec_start);
         } else if (*op == abi_journal_op::put) {
            uint32_t          block_num     = 0;
            uint64_t          account_value = 0;
            uint64_t          global_seq    = 0;
            std::vector<char> blob;
            fc::raw::unpack(ds, block_num);
            fc::raw::unpack(ds, account_value);
            fc::raw::unpack(ds, global_seq);
            fc::raw::unpack(ds, blob);
            // Last-write-wins, exactly as append_reversible's std::map::operator[].
            working[{chain::name(account_value), global_seq}] = reversible_entry{block_num, std::move(blob)};
         } else { // abi_journal_op::rollback
            uint32_t block_num = 0;
            fc::raw::unpack(ds, block_num);
            for (auto it = working.begin(); it != working.end();) {
               if (it->second.block_num >= block_num)
                  it = working.erase(it);
               else
                  ++it;
            }
         }
      } catch (const std::exception& e) {
         fc_wlog(_log, "trace_api: abi_log journal {} failed to decode record at offset {}: {}, truncating",
                 _journal_path.generic_string(), rec_start, e.what());
         break;
      }

      offset = rec_end;
   }

   if (offset < file_size) {
      std::error_code ec;
      std::filesystem::resize_file(_journal_path, offset, ec);
      if (ec)
         fc_wlog(_log, "trace_api: abi_log journal {} failed to truncate to {}: {}",
                 _journal_path.generic_string(), offset, ec.message());
   }
   return working;
}

void abi_log::compact_journal() {
   // Snapshot the live overlay under the index lock (brief - copies a handful of small blobs), then
   // do all file I/O outside it so concurrent lookups never wait.  _index_mtx and _journal_mtx are
   // never held at the same time anywhere, so this two-step lock cannot deadlock.
   std::vector<std::tuple<uint32_t, chain::name, uint64_t, std::vector<char>>> live;
   {
      std::lock_guard<std::mutex> lock(_index_mtx);
      live.reserve(_reversible.size());
      for (const auto& [key, entry] : _reversible)
         live.emplace_back(entry.block_num, key.first, key.second, entry.abi_bytes);
   }

   std::filesystem::path tmp = _journal_path;
   tmp += ".tmp";
   std::error_code ec;

   std::lock_guard<std::mutex> lock(_journal_mtx);
   try {
      // Test aid: simulate a rewrite that cannot complete (e.g. disk full), to exercise the
      // truncate fallback in the fail-closed rollback path.  Always false in production.
      if (_force_journal_compaction_failure)
         throw std::runtime_error("forced journal compaction failure (test)");
      {
         fc::cfile t;
         t.set_file_path(tmp);
         // Truncating open ("wb+") so any stale temp left by a previously interrupted compaction is
         // overwritten rather than appended to.
         t.open(fc::cfile::truncate_rw_mode);
         auto hdr_data = fc::raw::pack(abi_journal_header{});
         t.write(hdr_data.data(), hdr_data.size());
         for (const auto& [block_num, account, global_seq, blob] : live) {
            auto rec = frame_journal_record(encode_put_body(block_num, account, global_seq, blob));
            t.write(rec.data(), rec.size());
         }
         t.flush();
         t.close();
      }
      _journal_cfile.close();
      std::filesystem::rename(tmp, _journal_path); // atomic on POSIX
      _journal_cfile.set_file_path(_journal_path);
      _journal_cfile.open(fc::cfile::update_rw_mode);
      _journal_end_offset = std::filesystem::file_size(_journal_path);
      _journal_enabled = true;
   } catch (const std::exception& e) {
      fc_wlog(_log, "trace_api: abi_log journal compaction failed for {}: {}; "
                    "reversible ABI records will not survive a restart",
              _journal_path.generic_string(), e.what());
      std::filesystem::remove(tmp, ec);
      _journal_enabled = false;
   }
}

void abi_log::init_journal(const std::filesystem::path& journal_path) {
   _journal_path = journal_path;
   _journal_cfile.set_file_path(journal_path);
   bool existed = false;
   try {
      existed = _journal_cfile.open_existing_or_create_new();
   } catch (...) {
      fc_wlog(_log, "trace_api: abi_log journal failed to open {}; reversible ABI records will not "
                    "survive a restart", journal_path.generic_string());
      _journal_enabled = false;
      return;
   }

   if (!existed) {
      if (!write_journal_header()) {
         _journal_enabled = false;
         return;
      }
      _journal_end_offset = sizeof(abi_journal_header);
      _journal_enabled = true;
      return;
   }

   // Existing journal: validate the header.
   bool header_ok = false;
   try {
      abi_journal_header hdr = pread_packed_header<abi_journal_header>(_journal_cfile);
      header_ok = (hdr.magic == abi_journal_header::magic_value &&
                   hdr.version == abi_journal_header::current_version);
      if (!header_ok)
         fc_wlog(_log, "trace_api: abi_log journal {} has wrong magic/version (magic {:#x} version {}); "
                       "resetting it (reversible overlay starts empty)",
                 journal_path.generic_string(), hdr.magic, hdr.version);
   } catch (...) {
      fc_wlog(_log, "trace_api: abi_log journal {} header read failed; resetting it",
              journal_path.generic_string());
   }

   if (!header_ok) {
      // Untrustworthy journal: reset to a fresh header.  The main log is unaffected; only reversible
      // records that lived solely in this journal are lost.
      std::error_code ec;
      std::filesystem::resize_file(journal_path, 0, ec);
      if (ec || !write_journal_header()) {
         fc_wlog(_log, "trace_api: abi_log journal {} could not be reset: {}",
                 journal_path.generic_string(), ec ? ec.message() : std::string("header write failed"));
         _journal_enabled = false;
         return;
      }
      _journal_end_offset = sizeof(abi_journal_header);
      _journal_enabled = true;
      return;
   }

   // Replay into a working overlay, drop any (account, global_seq) already on disk (a flushed
   // record whose PUT is still in the journal), load the rest into the live overlay, then compact
   // the journal down to just those still-reversible records.
   auto working = replay_journal_records();
   for (auto it = working.begin(); it != working.end();) {
      if (_index.find(it->first) != _index.end())
         it = working.erase(it);
      else
         ++it;
   }
   _reversible = std::move(working);
   compact_journal(); // sets _journal_end_offset and _journal_enabled (and reopens the file)
}

} // namespace sysio::trace_api

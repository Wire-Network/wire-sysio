#include <sysio/trace_api/abi_log.hpp>
#include <sysio/trace_api/logging.hpp>

#include <fc/io/raw.hpp>

#include <boost/crc.hpp>

#include <cstring>

namespace sysio::trace_api {

namespace {

// Record layout: header(24) + blob_bytes(blob_size) + crc32(4).
// CRC covers header + blob_bytes (not itself).
constexpr uint64_t record_trailer_size = sizeof(uint32_t);

} // namespace

uint32_t abi_log::compute_record_crc(const record_header& hdr, const char* blob, uint64_t blob_size) {
   boost::crc_32_type crc;
   crc.process_bytes(&hdr, sizeof(hdr));
   if (blob_size > 0)
      crc.process_bytes(blob, blob_size);
   return crc.checksum();
}

abi_log::abi_log(const std::filesystem::path& path) {
   const bool existed = std::filesystem::exists(path);
   _cfile.set_file_path(path);
   try {
      _cfile.open(fc::cfile::create_or_update_rw_mode);
   } catch (...) {
      fc_wlog(_log, "trace_api: abi_log failed to open {}", path.generic_string());
      // Best-effort clean-up: a failed open on some libc versions leaves a zero-byte
      // file behind, which would look like a valid-but-header-less log on the next
      // start and fail an unpack inside the existing-file branch.
      std::error_code ec;
      if (!existed)
         std::filesystem::remove(path, ec);
      return;
   }

   if (!existed) {
      // Fresh file: write header, no records yet.  "ab+" mode always writes at
      // EOF, so the seek below is advisory for the position indicator; the
      // write lands at offset 0 by construction (file is freshly opened, EOF = 0).
      abi_log_header hdr;
      auto data = fc::raw::pack(hdr);
      try {
         _cfile.write(data.data(), data.size());
         _cfile.flush();
      } catch (...) {
         fc_wlog(_log, "trace_api: abi_log failed to write header to {}", path.generic_string());
         return;
      }
      _end_offset = data.size();
      _valid = true;
      return;
   }

   // Existing file: validate header, scan records, populate index, truncate any bad tail.
   try {
      _cfile.seek(0);
      auto ds = _cfile.create_datastream();
      abi_log_header hdr;
      fc::raw::unpack(ds, hdr);
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
}

uint64_t abi_log::recover_from_disk(const std::filesystem::path& path) {
   const uint64_t header_size = sizeof(abi_log_header);
   const uint64_t file_size = std::filesystem::file_size(path);

   // pread correctness: the only buffered (FILE*) write on this cfile is the fresh-file header in
   // the constructor, which flushes immediately; appends use raw-fd pwrite.  Never introduce
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
         // NOTE: the fd comes from fopen("ab+") and carries O_APPEND, so on Linux this pwrite appends
         // at EOF regardless of the offset argument.  That is exactly what we want: "file ends at
         // _end_offset" is a maintained invariant (restored below on failure), so EOF == _end_offset.
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
   std::lock_guard<std::mutex> lock(_index_mtx);
   // std::map::operator[] — duplicate (account, global_seq) silently overwrites (last-write-wins).
   // A replay that re-commits a key already flushed to disk leaves both copies; fetch prefers the
   // overlay and the bytes are identical, so the duplication is harmless and resolves at flush.
   _reversible[{account, global_seq}] = reversible_entry{block_num, std::move(abi_bytes)};
}

void abi_log::rollback_reversible(uint32_t block_num) {
   std::lock_guard<std::mutex> lock(_index_mtx);
   for (auto it = _reversible.begin(); it != _reversible.end();) {
      if (it->second.block_num >= block_num)
         it = _reversible.erase(it);
      else
         ++it;
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
         // Disk write failed; the record is still in the overlay - leave it (and the rest of the
         // flushable range) for retry on the next LIB advance rather than lose it.
         fc_wlog(_log, "trace_api: abi_log flush at lib {} failed for account {} seq {}; will retry",
                 lib, key.first, key.second);
         return;
      }
      std::lock_guard<std::mutex> lock(_index_mtx);
      _reversible.erase(key);
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

} // namespace sysio::trace_api

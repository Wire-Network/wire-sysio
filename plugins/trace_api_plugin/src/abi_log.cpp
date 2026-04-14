#include <sysio/trace_api/abi_log.hpp>
#include <sysio/trace_api/logging.hpp>

#include <fc/io/raw.hpp>

#include <boost/crc.hpp>

#include <cerrno>
#include <cstring>
#include <unistd.h>

namespace sysio::trace_api {

namespace {

// Record layout: header(24) + blob_bytes(blob_size) + crc32(4).
// CRC covers header + blob_bytes (not itself).
constexpr uint64_t record_trailer_size = sizeof(uint32_t);

// pread loop that tolerates short reads.  Returns true iff `n` bytes were
// read into `buf` from `off`.
bool pread_all(int fd, void* buf, size_t n, uint64_t off) {
   auto* p = static_cast<char*>(buf);
   while (n > 0) {
      const ssize_t r = ::pread(fd, p, n, static_cast<off_t>(off));
      if (r > 0) {
         p   += r;
         off += static_cast<uint64_t>(r);
         n   -= static_cast<size_t>(r);
      } else if (r == 0) {
         return false; // EOF before all bytes read
      } else if (errno == EINTR) {
         continue;
      } else {
         return false;
      }
   }
   return true;
}

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
      return;
   }

   if (!existed) {
      // Fresh file: write header, no records yet.
      abi_log_header hdr;
      auto data = fc::raw::pack(hdr);
      try {
         _cfile.seek(0);
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
      if (!pread_all(_cfile.fileno(), &rh, sizeof(rh), record_start)) {
         fc_wlog(_log, "trace_api: abi_log {} failed to read record header at offset {}, truncating",
              path.generic_string(), record_start);
         break;
      }

      const uint64_t blob_offset = record_start + sizeof(record_header);
      const uint64_t crc_offset  = blob_offset + rh.blob_size;
      const uint64_t record_end  = crc_offset + record_trailer_size;

      if (record_end > file_size) {
         fc_wlog(_log, "trace_api: abi_log {} record at {} claims blob_size {} but file has only {} bytes remaining, truncating",
              path.generic_string(), record_start, rh.blob_size, file_size - blob_offset);
         break;
      }

      std::vector<char> blob;
      if (rh.blob_size > 0) {
         blob.resize(rh.blob_size);
         if (!pread_all(_cfile.fileno(), blob.data(), rh.blob_size, blob_offset)) {
            fc_wlog(_log, "trace_api: abi_log {} failed to read blob at offset {}, truncating",
                 path.generic_string(), blob_offset);
            break;
         }
      }

      uint32_t stored_crc = 0;
      if (!pread_all(_cfile.fileno(), &stored_crc, sizeof(stored_crc), crc_offset)) {
         fc_wlog(_log, "trace_api: abi_log {} failed to read crc at offset {}, truncating",
              path.generic_string(), crc_offset);
         break;
      }

      const uint32_t computed_crc = compute_record_crc(rh, blob.data(), rh.blob_size);
      if (computed_crc != stored_crc) {
         fc_wlog(_log, "trace_api: abi_log {} crc mismatch at record offset {} (stored {:#x} vs computed {:#x}), truncating",
              path.generic_string(), record_start, stored_crc, computed_crc);
         break;
      }

      _index[{rh.account, rh.global_seq}] = index_entry{blob_offset, rh.blob_size};
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

void abi_log::append(chain::name account, uint64_t global_seq, std::vector<char> abi_bytes) {
   if (!_valid) return;

   record_header rh{ account, global_seq, abi_bytes.size() };
   const uint32_t crc = compute_record_crc(rh, abi_bytes.data(), abi_bytes.size());

   uint64_t blob_offset = 0;
   {
      std::lock_guard<std::mutex> lock(_append_mtx);
      try {
         _cfile.seek(_end_offset);
         _cfile.write(reinterpret_cast<const char*>(&rh), sizeof(rh));
         if (rh.blob_size > 0)
            _cfile.write(abi_bytes.data(), abi_bytes.size());
         _cfile.write(reinterpret_cast<const char*>(&crc), sizeof(crc));
         _cfile.flush();
      } catch (...) {
         fc_wlog(_log, "trace_api: abi_log append failed at offset {}", _end_offset);
         return;
      }

      blob_offset = _end_offset + sizeof(rh);
      _end_offset += sizeof(rh) + rh.blob_size + record_trailer_size;
   }

   {
      std::lock_guard<std::mutex> lock(_index_mtx);
      _index[{rh.account, rh.global_seq}] = index_entry{blob_offset, rh.blob_size};
   }
}

bool abi_log::has_entry(chain::name account) const {
   if (!_valid) return false;
   std::lock_guard<std::mutex> lock(_index_mtx);
   auto it = _index.lower_bound({account, 0});
   return it != _index.end() && it->first.first == account;
}

std::optional<std::vector<char>> abi_log::lookup(chain::name account, uint64_t global_seq) const {
   if (!_valid) return std::nullopt;

   uint64_t blob_offset = 0;
   uint64_t blob_size   = 0;

   {
      std::lock_guard<std::mutex> lock(_index_mtx);
      auto it = _index.upper_bound({account, global_seq});
      if (it == _index.begin())
         return std::nullopt;
      --it;
      if (it->first.first != account)
         return std::nullopt;
      blob_offset = it->second.blob_offset;
      blob_size   = it->second.blob_size;
   }

   if (blob_size == 0)
      return std::vector<char>{};

   std::vector<char> out(blob_size);
   if (!pread_all(_cfile.fileno(), out.data(), blob_size, blob_offset)) {
      fc_wlog(_log, "trace_api: abi_log pread of {} bytes at {} failed", blob_size, blob_offset);
      return std::nullopt;
   }
   return out;
}

} // namespace sysio::trace_api

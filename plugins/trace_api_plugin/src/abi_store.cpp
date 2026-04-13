#include <sysio/trace_api/abi_store.hpp>

#include <fc/io/raw.hpp>
#include <fc/log/logger.hpp>

#include <algorithm>
#include <numeric>

namespace sysio::trace_api {

// ---------------------------------------------------------------------------
// abi_store_writer
// ---------------------------------------------------------------------------

void abi_store_writer::add(chain::name account, uint64_t global_seq, std::vector<char> abi_bytes) {
   _entries.push_back({account.to_uint64_t(), global_seq, std::move(abi_bytes)});
}

void abi_store_writer::write(const std::filesystem::path& path) const {
   // Sort by (account, global_seq); stable so last-add-wins for duplicate keys.
   std::vector<size_t> order(_entries.size());
   std::iota(order.begin(), order.end(), 0);
   std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
      if (_entries[a].account != _entries[b].account)
         return _entries[a].account < _entries[b].account;
      return _entries[a].global_seq < _entries[b].global_seq;
   });

   // Compute blob offsets (relative to blob area start).
   uint32_t running_offset = 0;
   std::vector<uint32_t> blob_offsets(order.size());
   for (size_t i = 0; i < order.size(); ++i) {
      blob_offsets[i] = running_offset;
      running_offset += static_cast<uint32_t>(_entries[order[i]].abi_bytes.size());
   }

   const auto tmp_path = std::filesystem::path(path).replace_extension(".tmp");

   fc::cfile f;
   f.set_file_path(tmp_path);
   f.open(fc::cfile::create_or_update_rw_mode);

   // Header
   abi_store_header hdr;
   hdr.entry_count = static_cast<uint32_t>(order.size());
   auto hdr_data = fc::raw::pack(hdr);
   f.write(hdr_data.data(), hdr_data.size());

   // Index (sorted)
   for (size_t i = 0; i < order.size(); ++i) {
      const auto& e = _entries[order[i]];
      abi_store_index_entry ie;
      ie.account     = e.account;
      ie.global_seq  = e.global_seq;
      ie.blob_offset = blob_offsets[i];
      ie.blob_size   = static_cast<uint32_t>(e.abi_bytes.size());
      auto ie_data = fc::raw::pack(ie);
      f.write(ie_data.data(), ie_data.size());
   }

   // Blob area (in same sorted order)
   for (size_t i : order) {
      const auto& blob = _entries[i].abi_bytes;
      if (!blob.empty())
         f.write(blob.data(), blob.size());
   }

   f.flush();
   f.close();

   std::filesystem::rename(tmp_path, path);
}

// ---------------------------------------------------------------------------
// abi_store_reader
// ---------------------------------------------------------------------------

abi_store_reader::abi_store_reader(const std::filesystem::path& path)
   : _path(path) {
   try {
      fc::cfile f;
      f.set_file_path(path);
      f.open("rb");
      f.seek(0);
      auto ds = f.create_datastream();

      abi_store_header hdr;
      fc::raw::unpack(ds, hdr);

      if (hdr.magic != abi_store_header::magic_value) {
         wlog("trace_api: abi_store {} has wrong magic, ignoring", path.generic_string());
         return;
      }
      if (hdr.version != abi_store_header::current_version) {
         wlog("trace_api: abi_store {} has unsupported version {}, ignoring",
              path.generic_string(), hdr.version);
         return;
      }

      _index.resize(hdr.entry_count);
      for (auto& e : _index)
         fc::raw::unpack(ds, e);

      // Blob area starts immediately after header + index.
      _blob_area_offset = sizeof(abi_store_header) +
                          static_cast<uint64_t>(hdr.entry_count) * sizeof(abi_store_index_entry);
      _valid = true;
   } catch (...) {
      wlog("trace_api: failed to load abi_store from {}", path.generic_string());
   }
}

std::optional<std::vector<char>> abi_store_reader::lookup(chain::name account, uint64_t global_seq) const {
   if (!_valid || _index.empty())
      return std::nullopt;

   const uint64_t acct = account.to_uint64_t();

   // upper_bound gives first entry strictly greater than (acct, global_seq).
   // Decrement to get the last entry <= the query, then check the account matches.
   const abi_store_index_entry key{acct, global_seq, 0, 0};
   auto it = std::upper_bound(_index.begin(), _index.end(), key,
      [](const abi_store_index_entry& a, const abi_store_index_entry& b) {
         if (a.account != b.account) return a.account < b.account;
         return a.global_seq < b.global_seq;
      });

   if (it == _index.begin())
      return std::nullopt;
   --it;
   if (it->account != acct)
      return std::nullopt;

   std::optional<std::vector<char>> result;
   result.emplace(it->blob_size);
   try {
      if (it->blob_size > 0) {
         fc::cfile f;
         f.set_file_path(_path);
         f.open("rb");
         f.seek(static_cast<long>(_blob_area_offset + it->blob_offset));
         f.read(result->data(), it->blob_size);
      }
   } catch (...) {
      wlog("trace_api: failed to read ABI blob from {}", _path.generic_string());
      return std::nullopt;
   }
   return result;
}

// ---------------------------------------------------------------------------
// abi_store_writer::load
// ---------------------------------------------------------------------------

void abi_store_writer::load(const std::filesystem::path& path) {
   try {
      fc::cfile f;
      f.set_file_path(path);
      f.open("rb");
      f.seek(0);
      auto ds = f.create_datastream();

      abi_store_header hdr;
      fc::raw::unpack(ds, hdr);
      if (hdr.magic != abi_store_header::magic_value) return;
      if (hdr.version != abi_store_header::current_version) return;

      std::vector<abi_store_index_entry> index(hdr.entry_count);
      for (auto& e : index)
         fc::raw::unpack(ds, e);

      const uint64_t blob_area_start = sizeof(abi_store_header) +
                                       static_cast<uint64_t>(hdr.entry_count) * sizeof(abi_store_index_entry);
      for (const auto& ie : index) {
         std::vector<char> blob(ie.blob_size);
         if (ie.blob_size > 0) {
            f.seek(static_cast<long>(blob_area_start + ie.blob_offset));
            f.read(blob.data(), ie.blob_size);
         }
         _entries.push_back({ie.account, ie.global_seq, std::move(blob)});
      }
   } catch (...) {
      wlog("trace_api: failed to load abi_store into writer from {}", path.generic_string());
      _entries.clear();
   }
}

} // namespace sysio::trace_api

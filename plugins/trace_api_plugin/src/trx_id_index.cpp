#include <sysio/trace_api/trx_id_index.hpp>
#include <sysio/trace_api/store_provider.hpp>

#include <fc/io/raw.hpp>
#include <fc/log/logger.hpp>

#include <bit>
#include <cstring>

namespace sysio::trace_api {

uint64_t trx_id_index_writer::prefix_of(const chain::transaction_id_type& id) {
   // transaction_id_type is fc::sha256 — use the first 8 bytes as the hash key.
   // fc::sha256 is a plain struct with no padding before its data.
   uint64_t p;
   static_assert(sizeof(chain::transaction_id_type) >= sizeof(p));
   std::memcpy(&p, &id, sizeof(p));
   return p;
}

void trx_id_index_writer::add(const chain::transaction_id_type& trx_id, uint32_t block_num) {
   _entries.emplace_back(prefix_of(trx_id), block_num);
}

void trx_id_index_writer::write(const std::filesystem::path& path) const {
   // Target load factor <= 0.5; bucket_count is a power of two for fast modulo.
   const uint32_t n = static_cast<uint32_t>(_entries.size());
   const uint32_t bucket_count = std::max<uint32_t>(4u, std::bit_ceil(n * 2 + 1));
   const uint32_t mask = bucket_count - 1;

   std::vector<trx_id_bucket> buckets(bucket_count); // zero-initialized = all empty

   for (const auto& [prefix, block_num] : _entries) {
      uint32_t idx = static_cast<uint32_t>(prefix) & mask;
      while (buckets[idx].block_num != 0) {
         idx = (idx + 1) & mask;
      }
      buckets[idx].prefix64  = prefix;
      buckets[idx].block_num = block_num;
   }

   fc::cfile f;
   f.set_file_path(path);
   f.open(fc::cfile::create_or_update_rw_mode);

   trx_id_index_header header;
   header.bucket_count = bucket_count;
   auto hdr_data = fc::raw::pack(header);
   f.write(hdr_data.data(), hdr_data.size());

   for (const auto& b : buckets) {
      auto bkt_data = fc::raw::pack(b);
      f.write(bkt_data.data(), bkt_data.size());
   }
   f.flush();
}

// ---------------------------------------------------------------------------

uint64_t trx_id_index_reader::prefix_of(const chain::transaction_id_type& id) {
   uint64_t p;
   std::memcpy(&p, &id, sizeof(p));
   return p;
}

trx_id_index_reader::trx_id_index_reader(const std::filesystem::path& path) {
   try {
      fc::cfile f;
      f.set_file_path(path);
      f.open("rb");
      f.seek(0);

      const auto header = extract_store<trx_id_index_header>(f);
      if (header.magic != trx_id_index_header::magic_value) {
         wlog("trace_api: trx_id index {} has wrong magic, ignoring", path.generic_string());
         return;
      }
      if (header.version != trx_id_index_header::current_version) {
         wlog("trace_api: trx_id index {} has unsupported version {}, ignoring",
              path.generic_string(), header.version);
         return;
      }
      if (header.bucket_count == 0) {
         _valid = true;
         return;
      }

      _buckets.resize(header.bucket_count);
      for (auto& b : _buckets) {
         auto ds = f.create_datastream();
         fc::raw::unpack(ds, b);
      }
      _valid = true;
   } catch (...) {
      wlog("trace_api: failed to load trx_id index from {}", path.generic_string());
   }
}

std::optional<uint32_t> trx_id_index_reader::lookup(const chain::transaction_id_type& trx_id) const {
   if (!_valid || _buckets.empty())
      return std::nullopt;

   const uint64_t prefix = prefix_of(trx_id);
   const uint32_t mask   = static_cast<uint32_t>(_buckets.size()) - 1;
   uint32_t idx          = static_cast<uint32_t>(prefix) & mask;

   while (_buckets[idx].block_num != 0) {
      if (_buckets[idx].prefix64 == prefix)
         return _buckets[idx].block_num;
      idx = (idx + 1) & mask;
   }
   return std::nullopt;
}

} // namespace sysio::trace_api

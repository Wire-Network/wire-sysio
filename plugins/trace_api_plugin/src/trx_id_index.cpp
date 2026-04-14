#include <sysio/trace_api/trx_id_index.hpp>
#include <sysio/trace_api/logging.hpp>
#include <sysio/trace_api/store_provider.hpp>

#include <fc/io/raw.hpp>

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

   // Last-write-wins per prefix: probe forward until either an empty bucket
   // (fresh insert) OR a bucket already holding this prefix (overwrite).
   // Combined with the index-builder's per-block_num dedup pass, this means a
   // trx that's been re-recorded under a different block_num after a fork
   // resolves to the latest entry, matching the linear-scan get_trx_block_number
   // path which returns *(--trx_block_nums.end()) (highest/most recent).
   for (const auto& [prefix, block_num] : _entries) {
      uint32_t idx = static_cast<uint32_t>(prefix) & mask;
      while (buckets[idx].block_num != 0 && buckets[idx].prefix64 != prefix) {
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

// Hard cap on bucket_count read from disk.  At 16 bytes per bucket, 2^28
// buckets = 4 GB.  A realistic worst-case slice (default 10K blocks at
// ~1K trxs/block = 10M trxs at load factor 0.5 = 2^25 buckets) fits 8x
// inside this cap.  Anything larger is treated as a corrupt/malicious file.
static constexpr uint32_t max_bucket_count = 1u << 28;

trx_id_index_reader::trx_id_index_reader(const std::filesystem::path& path) {
   try {
      fc::cfile f;
      f.set_file_path(path);
      f.open("rb");
      f.seek(0);

      const auto header = extract_store<trx_id_index_header>(f);
      if (header.magic != trx_id_index_header::magic_value) {
         fc_wlog(_log,"trace_api: trx_id index {} has wrong magic, ignoring", path.generic_string());
         return;
      }
      if (header.version != trx_id_index_header::current_version) {
         fc_wlog(_log,"trace_api: trx_id index {} has unsupported version {}, ignoring",
              path.generic_string(), header.version);
         return;
      }
      if (header.bucket_count == 0) {
         _valid = true;
         return;
      }

      // Open-addressing math (mask = bucket_count - 1) requires a power of two.
      if (!std::has_single_bit(header.bucket_count)) {
         fc_wlog(_log,"trace_api: trx_id index {} bucket_count {} is not a power of two, ignoring",
              path.generic_string(), header.bucket_count);
         return;
      }
      // Cap allocation against malicious / corrupt headers.
      if (header.bucket_count > max_bucket_count) {
         fc_wlog(_log,"trace_api: trx_id index {} bucket_count {} exceeds cap {}, ignoring",
              path.generic_string(), header.bucket_count, max_bucket_count);
         return;
      }
      // File length must equal header + bucket_count * sizeof(bucket).
      const uint64_t expected_size = sizeof(trx_id_index_header) +
                                     uint64_t{header.bucket_count} * sizeof(trx_id_bucket);
      const uint64_t actual_size = std::filesystem::file_size(path);
      if (actual_size != expected_size) {
         fc_wlog(_log,"trace_api: trx_id index {} size {} != expected {}, ignoring",
              path.generic_string(), actual_size, expected_size);
         return;
      }

      _buckets.resize(header.bucket_count);
      for (auto& b : _buckets) {
         auto ds = f.create_datastream();
         fc::raw::unpack(ds, b);
      }
      _valid = true;
   } catch (...) {
      fc_wlog(_log,"trace_api: failed to load trx_id index from {}", path.generic_string());
   }
}

std::optional<uint32_t> trx_id_index_reader::lookup(const chain::transaction_id_type& trx_id) const {
   if (!_valid || _buckets.empty())
      return std::nullopt;

   const uint64_t prefix       = prefix_of(trx_id);
   const uint32_t bucket_count = static_cast<uint32_t>(_buckets.size());
   const uint32_t mask         = bucket_count - 1;
   uint32_t idx                = static_cast<uint32_t>(prefix) & mask;

   // Bounded probe loop: a well-formed index has load factor <= 0.5 and an
   // empty bucket terminates the chain.  The bound guards against a corrupt
   // file with no empty buckets at all (would otherwise loop forever).
   for (uint32_t probes = 0; probes < bucket_count; ++probes) {
      if (_buckets[idx].block_num == 0)
         return std::nullopt;
      if (_buckets[idx].prefix64 == prefix)
         return _buckets[idx].block_num;
      idx = (idx + 1) & mask;
   }
   return std::nullopt;
}

} // namespace sysio::trace_api

#include <sysio/trace_api/trx_id_index.hpp>
#include <sysio/trace_api/logging.hpp>

#include <sysio/chain/exceptions.hpp>

#include <fc/io/raw.hpp>

#include <bit>
#include <cstring>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace sysio::trace_api {

namespace {

// Closes the wrapped fd on scope exit; the mmap'd region taken from it stays valid.
struct fd_closer {
   int fd;
   ~fd_closer() {
      if (fd >= 0)
         ::close(fd);
   }
};

} // namespace

void trx_id_index_writer::add(const chain::transaction_id_type& trx_id, uint32_t block_num) {
   _entries.emplace_back(trx_id_prefix(trx_id), block_num);
}

void trx_id_index_writer::write(const std::filesystem::path& path) const {
   // Target load factor <= 0.5; bucket_count is a power of two for fast modulo.
   // The bound is derived from the reader's max_bucket_count so the writer can
   // never produce a file the reader refuses: entries <= max/4 gives
   // bit_ceil(2n + 1) <= max, and also keeps bit_ceil's argument far below the
   // uint32 range where its result would be unrepresentable (UB).  In practice
   // a slice holds at most ~10M trxs, 6x inside this bound.
   SYS_ASSERT(_entries.size() <= trx_id_index_header::max_bucket_count / 4,
              chain::plugin_exception,
              "trx_id_index entry count {} exceeds the writable bound {}",
              _entries.size(), trx_id_index_header::max_bucket_count / 4);
   const uint32_t n = static_cast<uint32_t>(_entries.size());
   const uint32_t bucket_count = std::max<uint32_t>(4u, std::bit_ceil(n * 2 + 1));
   const uint32_t mask = bucket_count - 1;

   // Value-initialize all buckets to zero.  block_num == 0 is the empty-slot
   // sentinel that terminates the probe loop below; do NOT replace this with
   // reserve()+emplace_back() or any path that leaves block_num uninitialized.
   std::vector<trx_id_bucket> buckets(bucket_count);

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

   // Bulk write: trx_id_bucket is {u64, u32, u32} with no padding and
   // static_assert(sizeof(trx_id_bucket) == 16) in the header.  fc::raw::pack
   // writes those fields little-endian, identical to the in-memory layout on
   // x86_64 LE -- so a single write of the contiguous vector is equivalent to
   // the field-by-field pack and avoids the per-bucket call overhead.
   f.write(reinterpret_cast<const char*>(buckets.data()),
           buckets.size() * sizeof(trx_id_bucket));
   f.flush();
}

// ---------------------------------------------------------------------------

trx_id_index_reader::trx_id_index_reader(const std::filesystem::path& path) {
   const fd_closer fd{::open(path.c_str(), O_RDONLY | O_CLOEXEC)};
   if (fd.fd < 0) {
      fc_wlog(_log, "trace_api: failed to open trx_id index {}", path.generic_string());
      return;
   }

   struct stat st{};
   if (::fstat(fd.fd, &st) != 0) {
      fc_wlog(_log, "trace_api: failed to stat trx_id index {}", path.generic_string());
      return;
   }
   const uint64_t actual_size = static_cast<uint64_t>(st.st_size);
   // Reject before mapping: too small for a header, or larger than the biggest
   // index the cap permits (guards the address-space reservation against a
   // corrupt/malicious sparse file).
   const uint64_t max_size = sizeof(trx_id_index_header) +
                             uint64_t{trx_id_index_header::max_bucket_count} * sizeof(trx_id_bucket);
   if (actual_size < sizeof(trx_id_index_header) || actual_size > max_size) {
      fc_wlog(_log, "trace_api: trx_id index {} size {} outside [{}, {}], ignoring",
              path.generic_string(), actual_size, sizeof(trx_id_index_header), max_size);
      return;
   }

   void* base = ::mmap(nullptr, actual_size, PROT_READ, MAP_PRIVATE, fd.fd, 0);
   if (base == MAP_FAILED) {
      fc_wlog(_log, "trace_api: failed to mmap trx_id index {}", path.generic_string());
      return;
   }
   _map_base = base;
   _map_size = actual_size;

   // Validate the header out of the mapping.  Any failure below unmaps and
   // leaves the reader invalid; callers fall back to the linear scan.
   trx_id_index_header header{};
   std::memcpy(&header, _map_base, sizeof(header));

   if (header.magic != trx_id_index_header::magic_value) {
      fc_wlog(_log, "trace_api: trx_id index {} has wrong magic, ignoring", path.generic_string());
      unmap();
      return;
   }
   if (header.version != trx_id_index_header::current_version) {
      fc_wlog(_log, "trace_api: trx_id index {} has unsupported version {}, ignoring",
              path.generic_string(), header.version);
      unmap();
      return;
   }
   // File length must equal header + bucket_count * sizeof(bucket).  Checked
   // before the bucket_count == 0 early-out so a header-only claim with
   // trailing junk is rejected rather than treated as a valid empty index
   // (which would authoritatively skip the slice).
   const uint64_t expected_size = sizeof(trx_id_index_header) +
                                  uint64_t{header.bucket_count} * sizeof(trx_id_bucket);
   if (actual_size != expected_size) {
      fc_wlog(_log, "trace_api: trx_id index {} size {} != expected {}, ignoring",
              path.generic_string(), actual_size, expected_size);
      unmap();
      return;
   }
   if (header.bucket_count == 0) {
      // Valid empty index: every lookup misses.  No bucket array to point at.
      unmap();
      _valid = true;
      return;
   }
   // Open-addressing math (mask = bucket_count - 1) requires a power of two.
   if (!std::has_single_bit(header.bucket_count)) {
      fc_wlog(_log, "trace_api: trx_id index {} bucket_count {} is not a power of two, ignoring",
              path.generic_string(), header.bucket_count);
      unmap();
      return;
   }
   if (header.bucket_count > trx_id_index_header::max_bucket_count) {
      fc_wlog(_log, "trace_api: trx_id index {} bucket_count {} exceeds cap {}, ignoring",
              path.generic_string(), header.bucket_count, trx_id_index_header::max_bucket_count);
      unmap();
      return;
   }

   // The bucket array begins right after the 16-byte header: page-aligned base
   // + 16 keeps the 8-byte alignment trx_id_bucket requires.  fc::raw::pack
   // wrote the buckets little-endian with no padding (see the bulk-write note
   // in trx_id_index_writer::write), so the on-disk bytes are the in-memory
   // representation on x86_64 LE.
   _buckets      = reinterpret_cast<const trx_id_bucket*>(static_cast<const char*>(_map_base) + sizeof(header));
   _bucket_count = header.bucket_count;
   _valid        = true;
}

trx_id_index_reader::trx_id_index_reader(trx_id_index_reader&& other) noexcept
   : _buckets(other._buckets)
   , _bucket_count(other._bucket_count)
   , _map_base(other._map_base)
   , _map_size(other._map_size)
   , _valid(other._valid) {
   other._buckets      = nullptr;
   other._bucket_count = 0;
   other._map_base     = nullptr;
   other._map_size     = 0;
   other._valid        = false;
}

trx_id_index_reader& trx_id_index_reader::operator=(trx_id_index_reader&& other) noexcept {
   if (this != &other) {
      unmap();
      _buckets      = other._buckets;
      _bucket_count = other._bucket_count;
      _map_base     = other._map_base;
      _map_size     = other._map_size;
      _valid        = other._valid;
      other._buckets      = nullptr;
      other._bucket_count = 0;
      other._map_base     = nullptr;
      other._map_size     = 0;
      other._valid        = false;
   }
   return *this;
}

trx_id_index_reader::~trx_id_index_reader() {
   unmap();
}

void trx_id_index_reader::unmap() noexcept {
   if (_map_base) {
      ::munmap(_map_base, _map_size);
      _map_base = nullptr;
      _map_size = 0;
   }
   _buckets      = nullptr;
   _bucket_count = 0;
}

std::optional<uint32_t> trx_id_index_reader::lookup(const chain::transaction_id_type& trx_id) const {
   if (!_valid || _bucket_count == 0)
      return std::nullopt;

   const uint64_t prefix = trx_id_prefix(trx_id);
   const uint32_t mask   = _bucket_count - 1;
   uint32_t idx          = static_cast<uint32_t>(prefix) & mask;

   // Bounded probe loop: a well-formed index has load factor <= 0.5 and an
   // empty bucket terminates the chain.  The bound guards against a corrupt
   // file with no empty buckets at all (would otherwise loop forever).
   for (uint32_t probes = 0; probes < _bucket_count; ++probes) {
      if (_buckets[idx].block_num == 0)
         return std::nullopt;
      if (_buckets[idx].prefix64 == prefix)
         return _buckets[idx].block_num;
      idx = (idx + 1) & mask;
   }
   return std::nullopt;
}

} // namespace sysio::trace_api

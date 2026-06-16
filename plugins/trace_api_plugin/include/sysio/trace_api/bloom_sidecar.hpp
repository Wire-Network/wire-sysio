#pragma once

#include <boost/bloom.hpp>
#include <boost/crc.hpp>
#include <boost/unordered/unordered_flat_set.hpp>
#include <fc/io/cfile.hpp>
#include <sysio/chain/name.hpp>
#include <sysio/trace_api/trace.hpp>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>

namespace sysio::trace_api {

/// Per-slice bloom sidecar: lets get_actions skip a whole slice when the requested receiver (or receiver+action) is
/// not present anywhere in the slice's action traces.  Contains two filters in one file:
///   - receivers:    boost::bloom::filter<uint64_t, K> over action_trace_v0::receiver
///   - recv_action:  boost::bloom::filter<uint64_t, K> over pack_recv_action(receiver, action)
/// A negative bloom probe is authoritative (skip the slice).  A positive probe falls through to the normal scan.
/// Missing or corrupt sidecar -> reader is invalid -> caller falls back to full scan.
namespace bloom {

/// File format constants.  Stored little-endian on disk so a hex dump of the first 4 bytes reads "WIRB"; matches the
/// convention in blk_offset_index_header and the rest of the trace_api sidecars.  Native-endian, x86_64 Linux only.
inline constexpr uint32_t magic_value    = 0x42524957;  // bytes on disk: 'W','I','R','B'
inline constexpr uint32_t file_version   = 1;
inline constexpr uint32_t k_hashes       = 7;           ///< Fixed at compile time; reader rejects mismatched files.
inline constexpr double   target_fpr     = 0.01;        ///< 1% false-positive rate.  Irrelevant for negatives.
inline constexpr uint32_t min_capacity   = 32;          ///< Floor on filter sizing to avoid degenerate tiny filters.
/// Defensive upper bound on per-filter bit count in a loaded sidecar - a corrupted or maliciously-crafted file with
/// an absurd capacity value would otherwise trigger a huge std::vector allocation.  128 MiB per filter is ~500x the
/// size of a realistic busy-mainnet slice bloom, so no legitimate file hits it.
inline constexpr uint64_t max_capacity_bits = 128ull * 1024 * 1024 * 8;

/// Raw on-disk header.  Body layout: recv bits (recv_capacity_bits/8 bytes) then recv_action bits, trailing uint32
/// CRC32 over [header | body].  Fields are ordered so natural alignment produces no padding and the layout is
/// stable across compilers; a `reserved` pad word keeps the uint64 fields 8-byte aligned without #pragma pack.
struct header {
   uint32_t magic                     = magic_value;
   uint32_t version                   = file_version;
   uint32_t k_hash_count              = k_hashes;
   uint32_t n_recv                    = 0;      ///< Distinct receivers inserted (pre-rounding).
   uint32_t n_recv_action             = 0;      ///< Distinct (receiver, action) pairs inserted.
   uint32_t reserved                  = 0;
   uint64_t recv_capacity_bits        = 0;      ///< Filter capacity in bits; reader constructs filter with this.
   uint64_t recv_action_capacity_bits = 0;
};
static_assert(sizeof(header) == 4 * 6 + 8 * 2, "bloom::header layout drift");

/// Deterministic packing of (receiver, action) into one 64-bit key for the composite filter.  Rotate receiver to
/// separate it from the action in the bit distribution so distinct (r, a) pairs don't collide on the common
/// receiver==action case.  Must match between write and read paths.
inline uint64_t pack_recv_action(chain::name r, chain::name a) noexcept {
   const uint64_t rv = r.to_uint64_t();
   const uint64_t av = a.to_uint64_t();
   return ((rv << 13) | (rv >> (64 - 13))) ^ av;
}

using filter_t = boost::bloom::filter<uint64_t, k_hashes>;

} // namespace bloom

/// Accumulates distinct receivers and (receiver, action) pairs from a slice's trace data log.  Driven by the
/// maintenance thread once the slice is fully irreversible (build_recv_bloom streams the data log through
/// add_block); finalize sizes and materializes two blooms and writes the sidecar file atomically (temp + rename).
/// Memory cost is two hash sets keyed on uint64_t; at Wire-mainnet scale these stay a few tens of KB per slice.
class bloom_builder {
public:
   void add_action(const action_trace_v0& a) {
      _receivers.insert(a.receiver.to_uint64_t());
      _recv_actions.insert(bloom::pack_recv_action(a.receiver, a.action));
   }

   void add_block(const block_trace_v0& bt) {
      for (const auto& trx : bt.transactions) {
         for (const auto& act : trx.actions) {
            add_action(act);
         }
      }
   }

   /// true when no actions have been fed; finalize_and_write still produces a valid file whose probes always miss.
   bool empty() const noexcept { return _receivers.empty() && _recv_actions.empty(); }

   std::size_t receiver_count() const noexcept { return _receivers.size(); }
   std::size_t recv_action_count() const noexcept { return _recv_actions.size(); }

   /// Writes to `path + ".tmp"` then renames over `path` to keep the sidecar crash-consistent: either the old file
   /// remains intact or the new one is fully installed, never a partial file under the canonical name.
   void finalize_and_write(const std::filesystem::path& path) const {
      const std::size_t n_recv = std::max<std::size_t>(_receivers.size(), bloom::min_capacity);
      const std::size_t n_recv_action = std::max<std::size_t>(_recv_actions.size(), bloom::min_capacity);

      bloom::filter_t recv_f{n_recv, bloom::target_fpr};
      for (uint64_t v : _receivers) recv_f.insert(v);

      bloom::filter_t ra_f{n_recv_action, bloom::target_fpr};
      for (uint64_t v : _recv_actions) ra_f.insert(v);

      bloom::header hdr{};
      hdr.n_recv                      = static_cast<uint32_t>(_receivers.size());
      hdr.n_recv_action               = static_cast<uint32_t>(_recv_actions.size());
      hdr.recv_capacity_bits          = recv_f.capacity();
      hdr.recv_action_capacity_bits   = ra_f.capacity();

      const auto recv_bits            = recv_f.array();
      const auto ra_bits              = ra_f.array();

      boost::crc_32_type crc;
      crc.process_bytes(&hdr, sizeof(hdr));
      crc.process_bytes(recv_bits.data(), recv_bits.size());
      crc.process_bytes(ra_bits.data(), ra_bits.size());
      const uint32_t crc_v = crc.checksum();

      const auto tmp = std::filesystem::path(path).concat(".tmp");
      {
         // fc::cfile throws std::ios_base::failure on open/write error; the store_provider append path swallows that
         // (bloom is advisory) and bare file-system errors surface at the rename.  Scope the cfile so it closes (and
         // flushes) before we rename, keeping the temp-then-rename atomicity guarantee intact.
         fc::cfile out(tmp, fc::cfile::truncate_rw_mode);
         out.write(reinterpret_cast<const char*>(&hdr),             sizeof(hdr));
         out.write(reinterpret_cast<const char*>(recv_bits.data()), recv_bits.size());
         out.write(reinterpret_cast<const char*>(ra_bits.data()),   ra_bits.size());
         out.write(reinterpret_cast<const char*>(&crc_v),           sizeof(crc_v));
      }
      std::filesystem::rename(tmp, path);
   }

private:
   boost::unordered_flat_set<uint64_t> _receivers;
   boost::unordered_flat_set<uint64_t> _recv_actions;
};

/// Load-time view of a sidecar.  Constructor is strict: any failure (missing file, bad magic, version mismatch,
/// truncated body, CRC mismatch) leaves the reader in an invalid state and may_contain_* always returns true.
/// Returning true on invalid means "don't skip" - the correct fail-safe since a false negative would silently drop
/// matching actions from the response.
class bloom_reader {
public:
   bloom_reader() = default;

   explicit bloom_reader(const std::filesystem::path& path) {
      load(path);
   }

   bool valid() const noexcept { return _valid; }

   /// Invariant: on invalid reader, returns true so the caller treats the slice as "may contain, scan".
   bool may_contain_receiver(chain::name r) const noexcept {
      if (!_valid) return true;
      return _recv.may_contain(r.to_uint64_t());
   }

   bool may_contain_recv_action(chain::name r, chain::name a) const noexcept {
      if (!_valid) return true;
      return _recv_action.may_contain(bloom::pack_recv_action(r, a));
   }

private:
   void load(const std::filesystem::path& path) {
      // Any failure here leaves _valid == false, which fails safe: the probe methods return true and the caller
      // scans the slice.  fc::cfile throws std::ios_base::failure on open/read errors; we catch broadly to keep
      // that guarantee without propagating to the query path.
      try {
         std::error_code ec;
         const auto file_size = std::filesystem::file_size(path, ec);
         if (ec || file_size < sizeof(bloom::header) + sizeof(uint32_t)) return;

         fc::cfile in(path, fc::cfile::read_only_mode);

         bloom::header hdr;
         in.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
         if (hdr.magic                      != bloom::magic_value)   return;
         if (hdr.version                    != bloom::file_version)  return;
         if (hdr.k_hash_count               != bloom::k_hashes)      return;
         if (hdr.recv_capacity_bits        %  CHAR_BIT != 0)         return;
         if (hdr.recv_action_capacity_bits %  CHAR_BIT != 0)         return;
         // Defensive cap: a corrupted or crafted header with an absurd capacity would otherwise trigger a huge
         // allocation below.  Real bloom sizes are <1 MB even for busy-mainnet slices; 128 MB per filter is a wide
         // safety margin.
         if (hdr.recv_capacity_bits        > bloom::max_capacity_bits) return;
         if (hdr.recv_action_capacity_bits > bloom::max_capacity_bits) return;

         const std::size_t recv_bytes    = hdr.recv_capacity_bits / CHAR_BIT;
         const std::size_t ra_bytes      = hdr.recv_action_capacity_bits / CHAR_BIT;
         const std::size_t expected_size = sizeof(bloom::header) + recv_bytes + ra_bytes + sizeof(uint32_t);
         if (file_size != expected_size) return;

         std::vector<unsigned char> recv_buf(recv_bytes);
         std::vector<unsigned char> ra_buf(ra_bytes);
         in.read(reinterpret_cast<char*>(recv_buf.data()), recv_bytes);
         in.read(reinterpret_cast<char*>(ra_buf.data()),   ra_bytes);
         uint32_t file_crc = 0;
         in.read(reinterpret_cast<char*>(&file_crc), sizeof(file_crc));

         boost::crc_32_type crc;
         crc.process_bytes(&hdr,             sizeof(hdr));
         crc.process_bytes(recv_buf.data(),  recv_buf.size());
         crc.process_bytes(ra_buf.data(),    ra_buf.size());
         if (crc.checksum() != file_crc) return;

         // boost::bloom guarantees filter{f.capacity()}.capacity() == f.capacity() (see boost/bloom/detail/core.hpp
         // :480), so reconstruction with the saved capacity produces an array of the saved byte size.  The
         // size-equality check below is a belt-and-suspenders guard against a future boost::bloom change.
         bloom::filter_t recv_f{hdr.recv_capacity_bits};
         bloom::filter_t ra_f{hdr.recv_action_capacity_bits};
         if (recv_f.array().size() != recv_buf.size())  return;
         if (ra_f.array().size()   != ra_buf.size())    return;
         std::memcpy(recv_f.array().data(), recv_buf.data(), recv_buf.size());
         std::memcpy(ra_f.array().data(),   ra_buf.data(),   ra_buf.size());

         _recv        = std::move(recv_f);
         _recv_action = std::move(ra_f);
         _valid       = true;
      } catch (const std::exception&) {
         // File-system or read error: leave _valid == false so may_contain_* returns true -> scan fallback.
         _valid = false;
      }
   }

   bool            _valid = false;
   bloom::filter_t _recv;
   bloom::filter_t _recv_action;
};

} // namespace sysio::trace_api

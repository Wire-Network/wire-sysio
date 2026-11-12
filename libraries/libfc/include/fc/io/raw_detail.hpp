#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

namespace fc::raw::detail {

/// Allocation granularity for byte reads, not a limit on accepted encoded lengths.
inline constexpr size_t unpack_chunk_bytes = 4096;

/// Grow sequences as elements are decoded, retaining existing elements exactly as resize-before-unpack did.
template <typename Sequence, typename Unpack>
void unpack_sequence(Sequence& value, uint32_t count, Unpack unpack) {
   for (uint32_t i = 0; i < count; ++i) {
      if (i == value.size())
         value.emplace_back();
      unpack(value[i]);
   }
   value.resize(count);
}

/** Read each byte chunk before growing its destination, including streams without a byte-count remaining().
 * Prefilling scratch from retained bytes and zeros preserves the existing values of nonthrowing short reads.
 * Boost fundamental vectors also use this helper: their existing encoding stores count bytes, not count*sizeof(T).
 */
template <typename Stream, typename Vector>
void unpack_bytes(Stream& stream, Vector& value, uint32_t count) {
   std::array<char, unpack_chunk_bytes> scratch;
   for (size_t offset = 0; offset < count;) {
      const auto size = std::min(unpack_chunk_bytes, size_t(count) - offset);
      std::memset(scratch.data(), 0, size);
      if (offset < value.size())
         std::memcpy(scratch.data(), reinterpret_cast<const char*>(value.data()) + offset,
                     std::min(size, value.size() - offset));
      stream.read(scratch.data(), size);
      if (value.size() < offset + size)
         value.resize(offset + size);
      std::memcpy(reinterpret_cast<char*>(value.data()) + offset, scratch.data(), size);
      offset += size;
   }
   value.resize(count);
}

} // namespace fc::raw::detail

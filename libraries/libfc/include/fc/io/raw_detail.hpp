#pragma once

#include <fc/io/datastream.hpp>

#include <algorithm>
#include <cstdint>
#include <type_traits>

namespace fc::raw::detail {

/// Allocation granularity, not a limit on accepted encoded lengths.
inline constexpr size_t unpack_chunk_bytes = 4096;

/// Whether the elements fit in one allocation chunk, without overflowing a count/size multiplication.
template <typename Container>
constexpr bool is_small_allocation(uint32_t count) {
   return count <= unpack_chunk_bytes / sizeof(typename Container::value_type);
}

/// Unknown streams, logical bounds and wrappers do not establish how many bytes are physically available.
template <typename Stream>
bool has_available_bytes(Stream&, uint32_t) {
   return false;
}

/// Only memory datastreams provide an exact count of bytes available for a successful read.
template <typename Storage, typename Enable>
bool has_available_bytes(datastream<Storage, Enable>& stream, uint32_t count) {
   if constexpr (std::is_pointer_v<Storage> || std::is_same_v<Storage, std::vector<char>> ||
                 std::is_same_v<Storage, std::deque<char>>)
      return stream.remaining() >= count;
   return false;
}

/// Retain existing elements; allocate up front only for small counts or counts covered by available bytes.
template <typename Stream, typename Sequence, typename Unpack>
void unpack_sequence(Stream& stream, Sequence& value, uint32_t count, Unpack unpack) {
   if (is_small_allocation<Sequence>(count) || has_available_bytes(stream, count)) {
      value.resize(count);
      for (uint32_t i = 0; i < count; ++i)
         unpack(value[i]);
      return;
   }
   for (uint32_t i = 0; i < count; ++i) {
      if (i == value.size())
         value.emplace_back();
      unpack(value[i]);
   }
   value.resize(count);
}

/// Read directly into char storage, growing by at most one chunk ahead of reads unless all bytes are available.
template <typename Stream, typename Vector>
void unpack_bytes(Stream& stream, Vector& value, uint32_t count) {
   static_assert(std::is_same_v<typename Vector::value_type, char>);
   if (count <= unpack_chunk_bytes || has_available_bytes(stream, count)) {
      value.resize(count);
      if (count)
         stream.read(value.data(), count);
      return;
   }
   for (size_t offset = 0; offset < count;) {
      const auto size = std::min(unpack_chunk_bytes, size_t(count) - offset);
      if (value.size() < offset + size)
         value.resize(offset + size);
      stream.read(value.data() + offset, size);
      offset += size;
   }
   value.resize(count);
}

} // namespace fc::raw::detail

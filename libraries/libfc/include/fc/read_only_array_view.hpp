#pragma once

#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

namespace fc {

/** Elements whose native representations can be copied into values. */
template <typename T>
concept read_only_array_element = std::is_trivially_copyable_v<T> && std::is_trivially_copy_constructible_v<T> &&
                                  !std::is_const_v<T> && !std::is_volatile_v<T>;

/**
 * Non-owning view of an array stored as native object representations, including read-only mappings.
 *
 * Retains byte storage and copies only the requested element into a value using memcpy and bit_cast.
 * Construction does not access the storage. Access returns a value, never a reference into storage that
 * might not contain live T objects, and never writes to the source storage.
 *
 * The caller must provide suitably aligned storage for count complete T representations, with count * sizeof(T)
 * representable as size_t. The storage must outlive the view. Every accessed representation must be readable
 * and describe a valid T value, with no concurrent modification. This view does not decode endianness or
 * validate records.
 */
template <read_only_array_element T>
class read_only_array_view {
public:
   /// Construct an empty view without accessing storage.
   constexpr read_only_array_view() noexcept = default;

   /// Bind a view to validated native representations without reading or writing the storage.
   read_only_array_view(const void* storage, std::size_t count) noexcept
      : _size(count) {
      assert(count <= std::numeric_limits<std::size_t>::max() / sizeof(T));
      assert(storage != nullptr || count == 0);
      assert(reinterpret_cast<std::uintptr_t>(storage) % alignof(T) == 0);
      _data = static_cast<const std::byte*>(storage);
   }

   /// Return an element by value; index must be less than size().
   [[nodiscard]] T operator[](std::size_t index) const noexcept {
      assert(index < _size);
      std::array<std::byte, sizeof(T)> representation;
      std::memcpy(representation.data(), _data + index * sizeof(T), sizeof(T));
      return std::bit_cast<T>(representation);
   }

   /// Return the number of represented elements.
   [[nodiscard]] constexpr std::size_t size() const noexcept { return _size; }

   /// Return whether the view contains no elements.
   [[nodiscard]] constexpr bool empty() const noexcept { return _size == 0; }

private:
   const std::byte* _data = nullptr;
   std::size_t _size = 0;
};

} // namespace fc

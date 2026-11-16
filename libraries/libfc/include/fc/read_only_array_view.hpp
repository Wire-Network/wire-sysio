#pragma once

#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <type_traits>

namespace fc {

/** Elements that can be copied by value and whose lifetimes can be started implicitly. */
template <typename T>
concept read_only_array_element = std::is_trivially_copyable_v<T> && std::is_trivially_copy_constructible_v<T> &&
                                  !std::is_const_v<T> && !std::is_volatile_v<T>;

/**
 * Non-owning view of an array stored as native object representations, including read-only mappings.
 *
 * Uses std::start_lifetime_as_array when available. The C++20 fallback retains byte storage and copies only
 * the requested element into a value; it never starts object lifetimes in, or writes to, the source storage.
 * Neither implementation accesses the storage during construction. Access returns a value, never a reference
 * into storage that might not contain live T objects.
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
#if defined(__cpp_lib_start_lifetime_as) && __cpp_lib_start_lifetime_as >= 202207L
      _data = std::start_lifetime_as_array<T>(storage, count);
#else
      _data = static_cast<const std::byte*>(storage);
#endif
   }

   /// Return an element by value; index must be less than size().
   [[nodiscard]] T operator[](std::size_t index) const noexcept {
      assert(index < _size);
#if defined(__cpp_lib_start_lifetime_as) && __cpp_lib_start_lifetime_as >= 202207L
      return _data[index];
#else
      std::array<std::byte, sizeof(T)> representation;
      std::memcpy(representation.data(), _data + index * sizeof(T), sizeof(T));
      return std::bit_cast<T>(representation);
#endif
   }

   /// Return the number of represented elements.
   [[nodiscard]] constexpr std::size_t size() const noexcept { return _size; }

   /// Return whether the view contains no elements.
   [[nodiscard]] constexpr bool empty() const noexcept { return _size == 0; }

private:
#if defined(__cpp_lib_start_lifetime_as) && __cpp_lib_start_lifetime_as >= 202207L
   const T* _data = nullptr;
#else
   const std::byte* _data = nullptr;
#endif
   std::size_t _size = 0;
};

} // namespace fc

#pragma once

#include <fc/exception/exception.hpp>
#include <fc/variant.hpp>
#include <fc/variant_object.hpp>

#include <limits>
#include <optional>
#include <type_traits>

namespace sysio::underwriter_detail {

/// Decode an FC-reflected enum at a table/ABI trust boundary. Reflected enum
/// decoding accepts both the ABI spelling and a numeric representation while
/// validating that the value names a declared enumerator. Malformed values are
/// reported as `nullopt` so callers can skip the untrusted row safely.
template<typename Enum>
requires std::is_enum_v<Enum>
std::optional<Enum> decode_enum_variant(const fc::variant& value) {
   // FC's generic reflected-enum path coerces bool/null/double through
   // `as_int64()`. Admit only the two exact integer storage types produced by
   // the ABI serializer plus symbolic strings, and range-check unsigned input
   // before the reflected decoder converts through int64_t.
   if (!value.is_string() && !value.is_int64() && !value.is_uint64()) {
      return std::nullopt;
   }
   if (value.is_uint64() &&
       value.as_uint64() > static_cast<uint64_t>(
          std::numeric_limits<int64_t>::max())) {
      return std::nullopt;
   }
   try {
      return value.as<Enum>();
   } catch (const fc::exception&) {
      return std::nullopt;
   }
}

/// Look up and decode one required FC-reflected enum field without throwing on
/// a missing key. Both absence and malformed values return `nullopt` so a
/// caller can skip an untrusted table row atomically.
template<typename Enum>
requires std::is_enum_v<Enum>
std::optional<Enum> decode_enum_field(const fc::variant_object& object,
                                      const char* key) {
   const auto it = object.find(key);
   if (it == object.end()) return std::nullopt;
   return decode_enum_variant<Enum>(it->value());
}

} // namespace sysio::underwriter_detail

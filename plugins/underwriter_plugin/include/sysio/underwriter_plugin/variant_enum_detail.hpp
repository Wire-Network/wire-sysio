#pragma once

#include <fc/exception/exception.hpp>
#include <fc/variant.hpp>

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
   try {
      return value.as<Enum>();
   } catch (const fc::exception&) {
      return std::nullopt;
   }
}

} // namespace sysio::underwriter_detail

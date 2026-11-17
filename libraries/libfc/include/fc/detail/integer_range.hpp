#pragma once

/* Range-checked integer parsing and range errors shared by the libfc translation units. */

#include <fc/exception/exception.hpp>
#include <fc/int128.hpp>
#include <fc/int256.hpp>
#include <fc/reflect/typename.hpp>

#include <concepts>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

namespace fc::detail {

/// Throws the parse_error_exception for `value`, a number outside [`min`, `max`], the range of `type`.
[[noreturn]] inline void throw_out_of_range(std::string_view value, std::string_view type, std::string_view min,
                                            std::string_view max) {
   FC_THROW_EXCEPTION(parse_error_exception, "{} is out of range for {} [{}, {}]", value, type, min, max);
}

/// Throws the parse_error_exception for `value`, a number outside the range of the 128-bit integer type `T`.
template <typename T>
[[noreturn]] void throw_out_of_range(std::string_view value) {
   throw_out_of_range(value, get_typename<T>::name(), fc::to_string(std::numeric_limits<T>::min()),
                      fc::to_string(std::numeric_limits<T>::max()));
}

/// A fc 256-bit integer type.
template <typename T>
concept int256_or_uint256 = std::same_as<T, fc::int256> || std::same_as<T, fc::uint256>;

/// The name errors give the 256-bit integer type `T`.
template <int256_or_uint256 T>
inline constexpr std::string_view int256_type_name = std::same_as<T, fc::int256> ? "int256_t" : "uint256_t";

/// Throws the parse_error_exception for `value`, a number outside the range of the 256-bit integer type `T`.
template <int256_or_uint256 T>
[[noreturn]] void throw_out_of_256bit_range(std::string_view value) {
   // A fc 256-bit type has the limits of its boost base.
   using limits = std::numeric_limits<std::conditional_t<std::same_as<T, fc::int256>, boost::multiprecision::int256_t,
                                                         boost::multiprecision::uint256_t>>;
   throw_out_of_range(value, int256_type_name<T>, limits::min().str(), limits::max().str());
}

/// The number `number` as the 256-bit integer type `T`, read as boost reads it: decimal, hex after "0x" or octal
/// after "0", after an optional '-'.
/// @param text the text a range error names
/// @throw parse_error_exception if the number is outside the range of `T`
template <int256_or_uint256 T>
T parse_256bit(const std::string& number, std::string_view text) {
   // Holds every fc::int256 and fc::uint256, and throws where those wrap.
   boost::multiprecision::checked_int256_t value;
   try {
      value = boost::multiprecision::checked_int256_t{number};
   } catch (const std::overflow_error&) {
      throw_out_of_256bit_range<T>(text);
   }
   if (std::same_as<T, fc::uint256> && value < 0)
      throw_out_of_256bit_range<T>(text);
   return T{value};
}

/// The number `text` as the 256-bit integer type `T`, as `parse_256bit(text, text)`.
template <int256_or_uint256 T>
T parse_256bit(const std::string& text) {
   return parse_256bit<T>(text, text);
}

} // namespace fc::detail

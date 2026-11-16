#pragma once

/* Range-checked integer parsing and range errors shared by the libfc translation units. */

#include <fc/exception/exception.hpp>
#include <fc/int128.hpp>
#include <fc/int256.hpp>
#include <fc/reflect/typename.hpp>
#include <fc/string.hpp>

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

/// Throws the parse_error_exception for `text`, which is not a number the 256-bit integer type `T` reads.
template <int256_or_uint256 T>
[[noreturn]] void throw_not_a_256bit_number(std::string_view text) {
   FC_THROW_EXCEPTION(parse_error_exception, "Couldn't parse {} from \"{}\"", int256_type_name<T>, text);
}

/// The characters of 256-bit integer text.
namespace int256_text {
inline constexpr char minus_sign = '-';
inline constexpr char zero_digit = '0';
inline constexpr std::string_view hex_prefix = "0x";
inline constexpr std::string_view upper_hex_prefix = "0X";
} // namespace int256_text

/// Whether 256-bit integer text needs "0x" before hex digits.
enum class hex_prefix_rule {
   required, ///< digits without the prefix are decimal
   optional  ///< digits without the prefix are decimal when they all are, and hex otherwise
};

/**
 * The number `text` as the 256-bit integer type `T`: an optional '-', then decimal digits, or hex digits after "0x"
 * or "0X" (or, with `hex_prefix_rule::optional`, without it). Digits after a leading zero are decimal. Empty text and
 * a bare "0x" are zero; "-" and "-0x" are not numbers.
 * @throw parse_error_exception if `text` is not such a number, or the number is outside the range of `T`
 */
template <int256_or_uint256 T>
T parse_256bit(std::string_view text, hex_prefix_rule prefix_rule) {
   using namespace int256_text;
   std::string_view digits = text;
   const bool negative = digits.starts_with(minus_sign);
   if (negative)
      digits.remove_prefix(1);
   const bool prefixed = digits.starts_with(hex_prefix) || digits.starts_with(upper_hex_prefix);
   if (prefixed)
      digits.remove_prefix(hex_prefix.size());
   const bool hex = prefixed || (prefix_rule == hex_prefix_rule::optional && !all_digits(digits));
   if (!(hex ? all_hex_digits(digits) : all_digits(digits)) || (negative && digits.empty()))
      throw_not_a_256bit_number<T>(text);
   if (digits.empty())
      return T{0};
   if (!hex) {
      // boost reads digits after a leading zero as octal.
      while (digits.size() > 1 && digits.front() == zero_digit)
         digits.remove_prefix(1);
   }

   std::string number; // the text in the grammar boost reads
   if (negative)
      number += minus_sign;
   if (hex)
      number += hex_prefix;
   number += digits;
   // Holds every fc::int256 and fc::uint256, and throws where those wrap.
   boost::multiprecision::checked_int256_t value;
   try {
      value = boost::multiprecision::checked_int256_t{number};
   } catch (const std::overflow_error&) {
      throw_out_of_256bit_range<T>(text);
   } catch (const std::runtime_error&) {
      // boost refuses no text the checks above accept; this keeps a refusal from escaping as std::runtime_error.
      // Caught after std::overflow_error, which derives from it.
      throw_not_a_256bit_number<T>(text);
   }
   if (std::same_as<T, fc::uint256> && value < 0)
      throw_out_of_256bit_range<T>(text);
   return T{value};
}

} // namespace fc::detail

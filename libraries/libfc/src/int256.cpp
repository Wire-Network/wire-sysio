#include <fc/detail/integer_range.hpp>
#include <fc/exception/exception.hpp>
#include <fc/int256.hpp>
#include <fc/string.hpp>
#include <fc/variant.hpp>

#include <string>
#include <string_view>

namespace fc {
namespace {

constexpr char minus_sign = '-';
constexpr char zero_digit = '0';
constexpr std::string_view hex_prefix = "0x";
constexpr std::string_view upper_hex_prefix = "0X";

/// Throws the parse_error_exception for `text`, which is not a number `T` can be read from.
template <detail::int256_or_uint256 T>
[[noreturn]] void throw_not_a_number(std::string_view text) {
   FC_THROW_EXCEPTION(parse_error_exception, "Couldn't parse {} from \"{}\"", detail::int256_type_name<T>, text);
}

/// The text of `v` as the 256-bit integer type `T`, read as to_uint256() documents.
template <detail::int256_or_uint256 T>
T text_to_256bit(const fc::variant& v) {
   const std::string text = v.as_string();
   std::string_view digits = text;
   const bool negative = digits.starts_with(minus_sign);
   if (negative)
      digits.remove_prefix(1);

   std::string number; // the text in the grammar boost reads
   if (negative)
      number += minus_sign;
   if (all_digits(digits)) {
      // boost reads digits after a leading zero as octal.
      while (digits.size() > 1 && digits.front() == zero_digit)
         digits.remove_prefix(1);
   } else {
      if (digits.starts_with(hex_prefix) || digits.starts_with(upper_hex_prefix))
         digits.remove_prefix(hex_prefix.size());
      if (!all_hex_digits(digits))
         throw_not_a_number<T>(text);
      number += hex_prefix;
   }
   if (digits.empty()) {
      if (negative)
         throw_not_a_number<T>(text);
      return T{0};
   }
   number += digits;
   return detail::parse_256bit<T>(number, text);
}

} // anonymous namespace

fc::uint256 to_uint256(const fc::variant& v) {
   return text_to_256bit<fc::uint256>(v);
}

fc::int256 to_int256(const fc::variant& v) {
   return text_to_256bit<fc::int256>(v);
}

} // namespace fc

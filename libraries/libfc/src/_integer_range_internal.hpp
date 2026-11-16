#pragma once

/* Range errors shared by the integer conversions of the libfc translation units. */

#include <fc/exception/exception.hpp>
#include <fc/int128.hpp>
#include <fc/reflect/typename.hpp>

#include <limits>
#include <string_view>

namespace fc::detail {

/// Throws the parse_error_exception for `value`, a number outside [`min`, `max`], the range of `type`.
[[noreturn]] inline void throw_out_of_range(std::string_view value, std::string_view type, std::string_view min,
                                            std::string_view max) {
   FC_THROW_EXCEPTION(parse_error_exception, "{} is out of range for {} [{}, {}]", value, type, min, max);
}

/// Throws the parse_error_exception for `value`, a number outside the range of the 128-bit integer type `T`.
template<typename T>
[[noreturn]] void throw_out_of_range(std::string_view value) {
   throw_out_of_range(value, get_typename<T>::name(), fc::to_string(std::numeric_limits<T>::min()),
                      fc::to_string(std::numeric_limits<T>::max()));
}

} // namespace fc::detail

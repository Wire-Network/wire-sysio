#pragma once

#include <array>
#include <concepts>
#include <type_traits>

#include <boost/multiprecision/cpp_int.hpp>

namespace fc {
class variant;
using int256   = boost::multiprecision::int256_t;
using int256_t = int256;

using uint256   = boost::multiprecision::uint256_t;
using uint256_t = uint256;

fc::uint256 to_uint256(const fc::variant& v);
fc::int256 to_int256(const fc::variant& v);

#if __cplusplus >= 202002L

// Concept: requires T to be a specialization of boost::multiprecision::number
template <typename T>
struct is_boost_number : std::false_type {};

template <typename Backend, boost::multiprecision::expression_template_option ET>
struct is_boost_number<boost::multiprecision::number<Backend, ET>> : std::true_type {};

template <typename T>
concept BoostMultiprecisionNumber = is_boost_number<std::remove_cv_t<std::remove_reference_t<T>>>::value;

template <BoostMultiprecisionNumber T>
std::array<std::uint8_t, sizeof(T)> to_bytes_be(const T& v) {
   std::array<std::uint8_t, sizeof(T)> out{};
   boost::multiprecision::export_bits(v, out.begin(), 8, false); // false = big-endian
   return out;
}

template <BoostMultiprecisionNumber T>
T from_bytes_be(const std::uint8_t* data) {
   T v;
   boost::multiprecision::import_bits(v, data, data + sizeof(T), 8, false);
   return v;
}

#endif

}
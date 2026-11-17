#include <fc/detail/integer_range.hpp>
#include <fc/int256.hpp>
#include <fc/variant.hpp>

namespace fc {

fc::uint256 to_uint256(const fc::variant& v) {
   return detail::parse_256bit<fc::uint256>(v.as_string(), detail::hex_prefix_rule::optional);
}

fc::int256 to_int256(const fc::variant& v) {
   return detail::parse_256bit<fc::int256>(v.as_string(), detail::hex_prefix_rule::optional);
}

} // namespace fc

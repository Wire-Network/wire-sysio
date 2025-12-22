#include <fc/int256.hpp>
#include <fc/crypto/hex.hpp>

namespace fc {
fc::uint256 to_uint256(const fc::variant& v) {
   auto s = v.as_string();
   if (all_digits(s))
      return fc::uint256(s);

   return fc::uint256(fc::from_hex(s));
}

fc::int256 to_int256(const fc::variant& v) {
   auto s = v.as_string();
   if (all_digits(s))
      return fc::int256(s);

   return fc::int256(fc::from_hex(s));
}
}
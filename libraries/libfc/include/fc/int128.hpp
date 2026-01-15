#pragma once
#include <limits>
#include <stdint.h>
#include <string>

#include <boost/multiprecision/cpp_int.hpp>

namespace fc
{

   using int128   = boost::multiprecision::int128_t;
   using int128_t = int128;
   using uint128   = boost::multiprecision::uint128_t;
   using uint128_t = uint128;

  class variant;

  fc::uint128 to_uint128(std::uint64_t hi, uint64_t lo);
  fc::uint128 to_uint128(const fc::variant& v);
  fc::int128 to_int128(const fc::variant& v);


  void to_variant( const uint128& var,  variant& vo );
  void from_variant( const variant& var,  uint128& vo );
  void to_variant( const int128& var,  variant& vo );
  void from_variant( const variant& var,  int128& vo );

  namespace raw
  {
    template<typename Stream>
    void pack( Stream& s, const uint128& u ) { s.write( (char*)&u, sizeof(u) ); }
    template<typename Stream>
    void unpack( Stream& s, uint128& u ) { s.read( (char*)&u, sizeof(u) ); }
  }

} // namespace fc

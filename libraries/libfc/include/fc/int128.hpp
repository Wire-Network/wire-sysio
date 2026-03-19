#pragma once
#include <cstdint>
#include <stdint.h>
#include <string>

namespace fc
{

   using int128   = __int128;
   using int128_t = int128;
   using uint128   = unsigned __int128;
   using uint128_t = uint128;

  // String conversion helpers (replacement for boost::multiprecision .str())
  std::string to_string( unsigned __int128 v );
  std::string to_string( __int128 v );
  unsigned __int128 uint128_from_string( const std::string& s );
  __int128 int128_from_string( const std::string& s );

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

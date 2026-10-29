#pragma once

/**
 *  fc serialization support for the Berkeley SoftFloat types.
 *
 *  `softfloat64_t` / `softfloat128_t` are typedefs of anonymous structs declared in a vendored
 *  C header, so they live in the global namespace and carry no namespace of their own for
 *  argument-dependent lookup to find.  Their `fc::to_variant` / `fc::from_variant` /
 *  `fc::to_json_stream` overloads consequently have to be reachable by other means: either
 *  declared before the code that needs them, or pulled in by ADL on the *other* argument
 *  (`fc::variant`, `fc::json_writer`), which is what carries namespace `fc` into the lookup set.
 *
 *  Collecting them here rather than at the bottom of `database_utils.hpp` -- a chain database
 *  utility header that merely happened to be their first consumer -- means any translation unit
 *  that needs them can include exactly this, and that a header needing the declarations early
 *  (`database_utils.hpp` itself did, for its BE key codec) gets them by include order instead of
 *  by a hand-maintained block of forward declarations.
 *
 *  Deliberately depends only on libfc plus the softfloat headers, so it can be included from
 *  anywhere in `libraries/chain` without dragging in chain types.
 */

#include <softfloat/softfloat.hpp>

#include <fc/crypto/hex.hpp>
#include <fc/exception/exception.hpp>
#include <fc/int128.hpp>
#include <fc/io/json_stream.hpp>
#include <fc/io/raw.hpp>
#include <fc/variant.hpp>

#include <cstring>
#include <string>

namespace fc {

   /// Reinterpret a softfloat64_t's storage as a native double.  Bit-for-bit; no rounding.
   inline
   void float64_to_double (const softfloat64_t& f, double& d) {
      memcpy(&d, &f, sizeof(d));
   }

   /// Inverse of float64_to_double.
   inline
   void double_to_float64 (const double& d, softfloat64_t& f) {
      memcpy(&f, &d, sizeof(f));
   }

   /// Reinterpret a softfloat128_t's storage as a 128-bit unsigned integer.
   inline
   void float128_to_uint128 (const softfloat128_t& f, uint128_t& u) {
      memcpy(&u, &f, sizeof(u));
   }

   /// Inverse of float128_to_uint128.
   inline
   void uint128_to_float128 (const uint128_t& u,  softfloat128_t& f) {
      memcpy(&f, &u, sizeof(f));
   }

   /// JSON shape for softfloat64_t is the native double spelling.
   inline
   void to_variant( const softfloat64_t& f, variant& v ) {
      double double_f;
      float64_to_double(f, double_f);
      v = variant(double_f);
   }

   inline
   void from_variant( const variant& v, softfloat64_t& f ) {
      double double_f;
      from_variant(v, double_f);
      double_to_float64(double_f, f);
   }

   namespace detail {
      /// fc's canonical softfloat128 spelling: "0x" + 16 little-endian hex bytes.
      /// Assumes platform is little endian and hex representation of the 128-bit
      /// integer is in little endian order.  Shared by to_variant / to_json_stream
      /// so the two emission paths cannot drift.
      inline std::string softfloat128_to_hex_string( const softfloat128_t& f ) {
         char as_bytes[sizeof(uint128_t)];
         memcpy(as_bytes, &f, sizeof(as_bytes));
         std::string s = "0x";
         s.append( to_hex( as_bytes, sizeof(as_bytes) ) );
         return s;
      }
   }

   inline
   void to_variant( const softfloat128_t& f, variant& v ) {
      v = detail::softfloat128_to_hex_string( f );
   }

   /// JSON shape mirrors to_variant: "0x<hex>" with little-endian bytes.
   inline
   void to_json_stream( const softfloat128_t& f, json_writer& w ) {
      w.value_string( detail::softfloat128_to_hex_string( f ) );
   }

   inline
   void from_variant( const variant& v, softfloat128_t& f ) {
      // Temporarily hold the binary in uint128_t before casting it to softfloat128_t
      char temp[sizeof(uint128_t)];
      memset(temp, 0, sizeof(temp));
      auto s = v.as_string();
      FC_ASSERT( s.size() == 2 + 2 * sizeof(temp) && s.find("0x") == 0,
                 "Failure in converting hex data into a softfloat128_t" );
      auto sz = from_hex( s.substr(2), temp, sizeof(temp) );
      // Assumes platform is little endian and hex representation of 128-bit integer is in little endian order.
      FC_ASSERT( sz == sizeof(temp), "Failure in converting hex data into a softfloat128_t" );
      memcpy(&f, temp, sizeof(f));
   }

} // namespace fc

// Binary packing for the softfloat types.  These are intentionally at global scope: the types
// are global, so ADL on a softfloat operand finds these operators wherever a datastream is
// packed, without every caller having to name a namespace.
template<typename DataStream>
DataStream& operator << ( DataStream& ds, const softfloat64_t& v ) {
   double double_v;
   fc::float64_to_double(v, double_v);
   fc::raw::pack(ds, double_v);
   return ds;
}

template<typename DataStream>
DataStream& operator >> ( DataStream& ds, softfloat64_t& v ) {
   double double_v;
   fc::raw::unpack(ds, double_v);
   fc::double_to_float64(double_v, v);
   return ds;
}

template<typename DataStream>
DataStream& operator << ( DataStream& ds, const softfloat128_t& v ) {
   fc::uint128_t uint128_v;
   fc::float128_to_uint128(v, uint128_v);
   fc::raw::pack(ds, uint128_v);
   return ds;
}

template<typename DataStream>
DataStream& operator >> ( DataStream& ds, softfloat128_t& v ) {
   fc::uint128_t uint128_v;
   fc::raw::unpack(ds, uint128_v);
   fc::uint128_to_float128(uint128_v, v);
   return ds;
}

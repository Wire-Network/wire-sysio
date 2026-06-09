#pragma once

// Boost multiprecision variant support.
// Include this header only in files that need UInt<N>/Int<N> aliases
// or generic boost::multiprecision::number serialization.
// Most code should NOT need this — prefer fc/variant.hpp alone.

#include <boost/multiprecision/cpp_int.hpp>
#include <fc/variant.hpp>

namespace fc
{
   template<size_t Size>
   using UInt = boost::multiprecision::number<
         boost::multiprecision::cpp_int_backend<Size, Size, boost::multiprecision::unsigned_magnitude, boost::multiprecision::unchecked, void> >;
   template<size_t Size>
   using Int = boost::multiprecision::number<
         boost::multiprecision::cpp_int_backend<Size, Size, boost::multiprecision::signed_magnitude, boost::multiprecision::unchecked, void> >;

   void to_variant( const UInt<8>& n, fc::variant& v );
   void from_variant( const fc::variant& v, UInt<8>& n );

   void to_variant( const UInt<16>& n, fc::variant& v );
   void from_variant( const fc::variant& v, UInt<16>& n );

   void to_variant( const UInt<32>& n, fc::variant& v );
   void from_variant( const fc::variant& v, UInt<32>& n );

   void to_variant( const UInt<64>& n, fc::variant& v );
   void from_variant( const fc::variant& v, UInt<64>& n );

   template<typename T> void to_variant( const boost::multiprecision::number<T>& n, fc::variant& v ) {
      v = n.str();
   }
   template<typename T> void from_variant( const fc::variant& v, boost::multiprecision::number<T>& n ) {
      n = boost::multiprecision::number<T>(v.get_string());
   }

} // namespace fc

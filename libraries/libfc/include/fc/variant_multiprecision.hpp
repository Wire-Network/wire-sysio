#pragma once

// Boost multiprecision variant support.
// Include this header only in files that need the UInt<N>/Int<N> aliases
// or their variant conversions, declared for UInt<8/16/32/64/256> and Int<256>.
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

   /// Stores the decimal text of `n`.
   void to_variant( const UInt<256>& n, fc::variant& v );
   /// Reads `v` as variant::as_uint256() does.
   /// @throw parse_error_exception if `v` holds a number outside the range of `n`
   /// @throw bad_cast_exception if `v` holds a type variant::as_uint256() does not read
   void from_variant( const fc::variant& v, UInt<256>& n );

   /// Stores the decimal text of `n`.
   void to_variant( const Int<256>& n, fc::variant& v );
   /// Reads `v` as variant::as_int256() does.
   /// @throw parse_error_exception if `v` holds a number outside the range of `n`
   /// @throw bad_cast_exception if `v` holds a type variant::as_int256() does not read
   void from_variant( const fc::variant& v, Int<256>& n );

} // namespace fc

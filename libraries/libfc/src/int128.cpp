#include <fc/crypto/city.hpp>
#include <fc/int128.hpp>
#include <fc/variant.hpp>
#include <fc/log/logger.hpp>
#include <fc/exception/exception.hpp>

#include <limits>
#include <string_view>

#include "_integer_range_internal.hpp"

namespace fc {

std::string to_string( unsigned __int128 v ) {
   if( v == 0 ) return "0";
   char buf[40]; // uint128 max is 39 digits
   char* p = buf + sizeof(buf);
   *--p = '\0';
   while( v > 0 ) {
      *--p = '0' + static_cast<char>(v % 10);
      v /= 10;
   }
   return p;
}

std::string to_string( __int128 v ) {
   if( v == 0 ) return "0";
   if( v < 0 ) return "-" + to_string(-static_cast<unsigned __int128>(v));
   return to_string(static_cast<unsigned __int128>(v));
}

namespace {

constexpr unsigned    decimal_radix   = 10;
constexpr unsigned    hex_radix       = 16;
constexpr std::size_t hex_prefix_size = 2; // "0x"

/// `uint128_from_string` and `int128_from_string`, for the result type `T`.
//
// Accepts both decimal ("123") and hex ("0x7B") input for backward compatibility
// with boost::multiprecision::uint128_t whose constructor accepted both formats.
// Note: to_string() always outputs decimal — the asymmetry is intentional to avoid
// changing the JSON wire format for serialized uint128 values.
template<typename T>
T parse_integer( const std::string& s ) {
   using limits = std::numeric_limits<T>;
   constexpr unsigned __int128 bit_pattern_max = std::numeric_limits<unsigned __int128>::max();

   const bool             negative = limits::is_signed && s.starts_with('-');
   const std::string_view number   = std::string_view{s}.substr(negative);
   const bool             hex      = number.size() >= hex_prefix_size && number[0] == '0' &&
                                     (number[1] == 'x' || number[1] == 'X');
   const unsigned         radix    = hex ? hex_radix : decimal_radix;
   // Hex without a sign is the bit pattern of the result. Any other number is a magnitude, which for a signed
   // type reaches one further below zero than above it.
   const unsigned __int128 max_magnitude =
      (hex && !negative) ? bit_pattern_max : static_cast<unsigned __int128>(limits::max()) + negative;

   unsigned __int128 magnitude = 0;
   for( const char c : number.substr(hex ? hex_prefix_size : 0) ) {
      uint8_t digit = 0;
      if( hex ) {
         if( c >= '0' && c <= '9' )      digit = c - '0';
         else if( c >= 'a' && c <= 'f' ) digit = c - 'a' + 10;
         else if( c >= 'A' && c <= 'F' ) digit = c - 'A' + 10;
         else FC_ASSERT( false, "Invalid hex character in uint128 string: {}", std::string(1,c) );
      } else {
         FC_ASSERT( c >= '0' && c <= '9', "Invalid character in uint128 string: {}", std::string(1,c) );
         digit = c - '0';
      }
      if( magnitude > (max_magnitude - digit) / radix ) // magnitude * radix + digit would pass max_magnitude
         detail::throw_out_of_range<T>( s );
      magnitude = magnitude * radix + digit;
   }
   return static_cast<T>( negative ? -magnitude : magnitude );
}

} // anonymous namespace

unsigned __int128 uint128_from_string( const std::string& s ) {
   return parse_integer<unsigned __int128>( s );
}

__int128 int128_from_string( const std::string& s ) {
   return parse_integer<__int128>( s );
}

fc::uint128 to_uint128(std::uint64_t hi, uint64_t lo) {
   return (fc::uint128(hi) << 64) | fc::uint128(lo);
}

void to_variant(const fc::uint128& var, fc::variant& vo) {
   vo = fc::to_string(var);
}
void from_variant(const variant& var, fc::uint128& vo) {
   if( var.is_uint128() || var.is_uint64() || var.is_int64() || var.is_string() ) {
      vo = var.as_uint128();
   } else {
      FC_THROW_EXCEPTION( bad_cast_exception, "Cannot convert variant of type '{}' into a uint128_t", var.get_type() );
   }
}

void to_variant(const fc::int128& var, fc::variant& vo) {
   vo = fc::to_string(var);
}
void from_variant(const variant& var, fc::int128& vo) {
   if( var.is_int128() || var.is_uint128() || var.is_int64() || var.is_uint64() || var.is_string() ) {
      vo = var.as_int128();
   } else {
      FC_THROW_EXCEPTION( bad_cast_exception, "Cannot convert variant of type '{}' into a int128_t", var.get_type() );
   }
}

} // namespace fc

/*
 * Portions of the above code were adapted from the work of Evan Teran.
 *
 * Copyright (c) 2008
 * Evan Teran
 *
 * Permission to use, copy, modify, and distribute this software and its
 * documentation for any purpose and without fee is hereby granted, provided
 * that the above copyright notice appears in all copies and that both the
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the same name not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission. We make no representations about the
 * suitability this software for any purpose. It is provided "as is"
 * without express or implied warranty.
 */

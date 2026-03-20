#include <fc/crypto/city.hpp>
#include <fc/int128.hpp>
#include <fc/variant.hpp>
#include <fc/log/logger.hpp>
#include <fc/exception/exception.hpp>

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

// Accepts both decimal ("123") and hex ("0x7B") input for backward compatibility
// with boost::multiprecision::uint128_t whose constructor accepted both formats.
// Note: to_string() always outputs decimal — the asymmetry is intentional to avoid
// changing the JSON wire format for serialized uint128 values.
unsigned __int128 uint128_from_string( const std::string& s ) {
   if( s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X') ) {
      unsigned __int128 result = 0;
      for( size_t i = 2; i < s.size(); ++i ) {
         char c = s[i];
         uint8_t digit;
         if( c >= '0' && c <= '9' )      digit = c - '0';
         else if( c >= 'a' && c <= 'f' ) digit = c - 'a' + 10;
         else if( c >= 'A' && c <= 'F' ) digit = c - 'A' + 10;
         else FC_ASSERT( false, "Invalid hex character in uint128 string: {}", std::string(1,c) );
         result = (result << 4) | digit;
      }
      return result;
   }
   unsigned __int128 result = 0;
   for( char c : s ) {
      FC_ASSERT( c >= '0' && c <= '9', "Invalid character in uint128 string: {}", std::string(1,c) );
      result = result * 10 + (c - '0');
   }
   return result;
}

__int128 int128_from_string( const std::string& s ) {
   if( s.empty() ) return 0;
   if( s[0] == '-' ) {
      return static_cast<__int128>(-uint128_from_string(s.substr(1)));
   }
   // hex strings handled by uint128_from_string
   return static_cast<__int128>(uint128_from_string(s));
}

fc::uint128 to_uint128(std::uint64_t hi, uint64_t lo) {
   return (fc::uint128(hi) << 64) | fc::uint128(lo);
}

void to_variant(const fc::uint128& var, fc::variant& vo) {
   vo = fc::to_string(var);
}
void from_variant(const variant& var, fc::uint128& vo) {
   if( var.is_uint128() ) {
      vo = var.as_uint128();
   } else if( var.is_uint64() ) {
      vo = var.as_uint64();
   } else if( var.is_int64() ) {
      vo = var.as_int64();
   } else if( var.is_string() ) {
      vo = fc::uint128_from_string(var.as_string());
   } else {
      FC_THROW_EXCEPTION( bad_cast_exception, "Cannot convert variant of type '{}' into a uint128_t", var.get_type() );
   }
}

void to_variant(const fc::int128& var, fc::variant& vo) {
   vo = fc::to_string(var);
}
void from_variant(const variant& var, fc::int128& vo) {
   if( var.is_int128() ) {
      vo = var.as_int128();
   } else if( var.is_uint128() ) {
      vo = static_cast<fc::int128>(var.as_uint128());
   } else if( var.is_int64() ) {
      vo = var.as_int64();
   } else if( var.is_uint64() ) {
      vo = var.as_uint64();
   } else if( var.is_string() ) {
      vo = fc::int128_from_string(var.as_string());
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

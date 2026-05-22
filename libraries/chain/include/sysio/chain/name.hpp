#pragma once
#include <fc/basic_name.hpp>
#include <fc/reflect/reflect.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

namespace sysio::chain {
  struct name;
}
namespace fc {
  class variant;
  void to_variant(const sysio::chain::name& c, fc::variant& v);
  void from_variant(const fc::variant& v, sysio::chain::name& check);
} // fc

namespace sysio::chain {

   /// Alphabet + length traits for the SYSIO account-name encoding: up to 13
   /// base-32 symbols over ".12345a-z". Drives fc::basic_name; see that header.
   struct sysio_name_traits {
      static constexpr int              max_len  = 13;
      static constexpr std::string_view alphabet = ".12345abcdefghijklmnopqrstuvwxyz";

      // Symbol 0 ('.') is an ordinary interior character, not a terminator:
      // to_string() keeps interior dots and only trims trailing padding.
      static constexpr bool             zero_terminates = false;

      // Declared here, defined in name.cpp — keeps <sysio/chain/exceptions.hpp>
      // and the SYS_ASSERT machinery out of this very widely-included header.
      [[noreturn]] static void throw_invalid( std::string_view in, const char* why );
   };

   /// A 64-bit packed identifier — account, table, action, permission, ...
   ///
   /// The packed encoding and all value semantics live in fc::basic_name; name
   /// adds only the dotted-namespace helpers prefix()/suffix(). Immutable except
   /// via assignment (e.g. fc::from_variant).
   struct name : fc::basic_name<sysio_name_traits> {
      using base = fc::basic_name<sysio_name_traits>;
      using base::base;                  // inherits name(uint64_t), name(std::string_view)
      constexpr name() = default;

      /**
       *  Returns the prefix.
       *  for example:
       *    "sysio.any" -> "sysio"
       *    "sysio" -> "sysio"
       */
      constexpr name prefix() const {
         uint64_t result                 = value;
         bool     not_dot_character_seen = false;
         uint64_t mask                   = 0xFull;

         // Get characters one-by-one in name in order from right to left
         for (int32_t offset = 0; offset <= 59;) {
            auto c = (value >> offset) & mask;

            if (!c) {                        // if this character is a dot
               if (not_dot_character_seen) { // we found the rightmost dot character
                  result = (value >> offset) << offset;
                  break;
               }
            } else {
               not_dot_character_seen = true;
            }

            if (offset == 0) {
               offset += 4;
               mask = 0x1Full;
            } else {
               offset += 5;
            }
         }

         return name{ result };
      }

      /**
       *  Returns the suffix.
       *  for example:
       *    "sysio.any" -> "any"
       *    "sysio" -> "sysio"
       */
      constexpr name suffix() const {
         uint32_t remaining_bits_after_last_actual_dot = 0;
         uint32_t tmp                                  = 0;
         for (int32_t remaining_bits = 59; remaining_bits >= 4; remaining_bits -= 5) { // Note: remaining_bits must remain signed integer
            // Get characters one-by-one in name in order from left to right (not including the 13th character)
            auto c = (value >> remaining_bits) & 0x1Full;
            if (!c) { // if this character is a dot
               tmp = static_cast<uint32_t>(remaining_bits);
            } else { // if this character is not a dot
               remaining_bits_after_last_actual_dot = tmp;
            }
         }

         uint64_t thirteenth_character = value & 0x0Full;
         if (thirteenth_character) { // if 13th character is not a dot
            remaining_bits_after_last_actual_dot = tmp;
         }

         if (remaining_bits_after_last_actual_dot == 0) // there is no actual dot in the %name other than potentially leading dots
            return name{ value };

         // At this point remaining_bits_after_last_actual_dot has to be within the range of 4 to 59 (and restricted to
         // increments of 5).

         // Mask for remaining bits corresponding to characters after last actual dot, except for 4 least significant bits
         // (corresponds to 13th character).
         uint64_t mask  = (1ull << remaining_bits_after_last_actual_dot) - 16;
         uint32_t shift = 64 - remaining_bits_after_last_actual_dot;

         return name{ ((value & mask) << shift) + (thirteenth_character << (shift - 1)) };
      }
   };

   // Each char of the string is encoded into a 5-bit chunk and left-shifted to
   // its slot starting with the highest slot for the first char. The 13th char,
   // if the string is long enough, is encoded into a 4-bit chunk in the lowest
   // 4 bits. 64 = 12 * 5 + 4. Non-validating — see fc::basic_name::pack.
   inline constexpr uint64_t string_to_uint64_t( std::string_view str ) {
      return name::pack( str );
   }

   inline constexpr name string_to_name( std::string_view str ) {
      return name( string_to_uint64_t( str ) );
   }

   inline namespace literals {
#if defined(__clang__)
# pragma clang diagnostic push
# pragma clang diagnostic ignored "-Wgnu-string-literal-operator-template"
#endif
      template <typename T, T... Str>
      inline constexpr name operator""_n() {
         constexpr const char buf[] = {Str...};
         static_assert(name::is_valid_literal(std::string_view{buf, sizeof(buf)}),
                       "invalid _n literal: character not in .12345a-z, or longer than 13");
         return name{std::integral_constant<uint64_t, string_to_uint64_t(std::string_view{buf, sizeof(buf)})>::value};
      }
#if defined(__clang__)
# pragma clang diagnostic pop
#endif
   } // namespace literals

} // sysio::chain

namespace std {
   template<> struct hash<sysio::chain::name> : private hash<uint64_t> {
      typedef sysio::chain::name argument_type;

      size_t operator()(const argument_type& name) const noexcept {
         static_assert(sizeof(size_t) == sizeof(uint64_t));
         return __builtin_bswap64(name.to_uint64_t());
      }
   };
};

FC_REFLECT_DERIVED_EMPTY( sysio::chain::name, (fc::basic_name<sysio::chain::sysio_name_traits>) )

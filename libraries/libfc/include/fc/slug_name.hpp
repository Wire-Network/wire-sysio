#pragma once
/**
 * @file fc/slug_name.hpp
 * @brief Host-side mirror of `sysio::slug_name` — 8-byte packed identifier for
 *        Chain/Token/Reserve `code` fields.
 *
 * Mirrors the contract-side `sysio::slug_name`
 * (wire-sysio/contracts/sysio.opp.common/include/sysio.opp.common/slug_name.hpp).
 * Both implementations use **the same packing algorithm** so a value packed
 * on either side is byte-identical when serialized as `uint64`. Wire format
 * (protobuf): plain `uint64` field.
 *
 * ## Alphabet + packing
 *
 * Alphabet: `[A-Z0-9_]+`, max 8 chars. 38-value alphabet (A-Z = 1..26,
 * 0-9 = 27..36, `_` = 37; value 0 = terminator/padding) packed in 6-bit slots:
 * bits [0..5] = char[0], …, bits [42..47] = char[7]. Bits [48..63] unused.
 *
 * Encoded values live in [0, 2^48) — safely under JS Number's 2^53 limit.
 *
 * ## Usage
 *
 * - Compile-time: `"ETH"_s`, `"USDC"_s` (via the `_c` literal suffix in
 *   `fc::slug_name_literals`). Compile error on invalid input.
 * - Runtime: `fc::slug_name{"ETH"}` (validates + throws fc::exception on
 *   invalid input).
 *
 * @see sysio::slug_name (contract-side mirror)
 */

#include <fc/exception/exception.hpp>
#include <fc/reflect/reflect.hpp>
#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>

namespace fc {

/// 8-byte packed identifier, alphabet `[A-Z0-9_]+`, max 8 chars.
/// See file-level docs for the packing format. Byte-identical with
/// `sysio::slug_name` for the same input string.
struct slug_name {
   uint64_t value = 0;

   constexpr slug_name() = default;
   constexpr explicit slug_name(uint64_t raw) : value(raw) {}

   /// Construct from a string. Throws `fc::assert_exception` on >8 chars
   /// or invalid alphabet.
   explicit slug_name(std::string_view s) {
      FC_ASSERT(s.size() <= 8, "slug_name: max 8 characters (got {})", s.size());
      uint64_t out = 0;
      for (std::size_t i = 0; i < s.size(); ++i) {
         const auto v = char_to_slot(s[i]);
         FC_ASSERT(v != INVALID_SLOT,
                   "slug_name: invalid character (alphabet is [A-Z0-9_]); got '{}'",
                   std::string(1, s[i]));
         out |= (v << (i * 6));
      }
      value = out;
   }

   /// Unpack to a string. Stops at the first null (zero) slot.
   std::string to_string() const {
      std::string out;
      out.reserve(8);
      for (std::size_t i = 0; i < 8; ++i) {
         const auto slot = (value >> (i * 6)) & 0x3F;
         if (slot == 0) break;
         out.push_back(slot_to_char(slot));
      }
      return out;
   }

   operator std::string() const { return to_string(); }

   friend bool operator==(slug_name a, slug_name b) { return a.value == b.value; }
   friend bool operator!=(slug_name a, slug_name b) { return a.value != b.value; }
   friend bool operator<(slug_name a, slug_name b)  { return a.value <  b.value; }
   friend bool operator<=(slug_name a, slug_name b) { return a.value <= b.value; }
   friend bool operator>(slug_name a, slug_name b)  { return a.value >  b.value; }
   friend bool operator>=(slug_name a, slug_name b) { return a.value >= b.value; }

   /// Sentinel for invalid characters.
   static constexpr uint64_t INVALID_SLOT = static_cast<uint64_t>(-1);

   /// Map alphabet character to its 6-bit slot value.
   /// Returns INVALID_SLOT for out-of-alphabet input.
   static constexpr uint64_t char_to_slot(char c) {
      if (c >= 'A' && c <= 'Z') return static_cast<uint64_t>(1 + (c - 'A'));
      if (c >= '0' && c <= '9') return static_cast<uint64_t>(27 + (c - '0'));
      if (c == '_')              return 37;
      return INVALID_SLOT;
   }

   /// Inverse of char_to_slot. Returns '\0' for slot==0 (terminator).
   static constexpr char slot_to_char(uint64_t slot) {
      if (slot == 0)                  return '\0';
      if (slot >= 1 && slot <= 26)    return static_cast<char>('A' + (slot - 1));
      if (slot >= 27 && slot <= 36)   return static_cast<char>('0' + (slot - 27));
      if (slot == 37)                 return '_';
      return '\0';
   }
};

namespace slug_name_literals {

/// Internal: invalid-input sentinel for the compile-time literal. Mirrors
/// the contract-side helper. Non-constexpr so any constant-evaluation
/// reaching it fails to compile.
[[noreturn]] inline void codename_literal_failed(const char* msg) {
   FC_ASSERT(false, "{}", msg);
   __builtin_unreachable();
}

/// Compile-time slug_name literal: `"ETH"_s`, `"USDC"_s`, `"PRIMARY"_s`.
/// Bad characters or >8 chars cause a compile error.
constexpr slug_name operator""_s(const char* s, std::size_t n) {
   if (n > 8) {
      codename_literal_failed("slug_name literal: max 8 characters");
   }
   uint64_t out = 0;
   for (std::size_t i = 0; i < n; ++i) {
      const auto slot = slug_name::char_to_slot(s[i]);
      if (slot == slug_name::INVALID_SLOT) {
         codename_literal_failed("slug_name literal: invalid character (alphabet is [A-Z0-9_])");
      }
      out |= (slot << (i * 6));
   }
   return slug_name{out};
}

} // namespace slug_name_literals

using slug_name_literals::operator""_s;

} // namespace fc

FC_REFLECT(fc::slug_name, (value))

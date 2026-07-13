#pragma once
/**
 * @file slug_name.hpp
 * @brief 8-byte packed identifier for Chain/Token/Reserve `code` fields.
 *
 * `sysio::slug_name` is the contract-side type for slug_name-keyed entities
 * (Chain.code, Token.code, ChainToken.{chain,token}_code, Reserve.code,
 * ReserveAmount.{chain,reserve}_code, TokenAmount.token_code).
 *
 * Wire format: `uint64`. Protobuf fields are plain uint64; this type is the
 * C++ wrapper. Mirrored host-side as `fc::slug_name` (libfc/include/fc/slug_name.hpp)
 * with identical packing semantics — byte identity guaranteed by the same
 * encoding algorithm in both implementations.
 *
 * ## Alphabet + packing
 *
 * Alphabet: `[A-Z0-9_]+`, max 8 chars. 38-value alphabet (A-Z = 1..26,
 * 0-9 = 27..36, `_` = 37; value 0 reserved for terminator/padding) packed
 * most-significant-symbol-first in 6-bit slots: bits [42..47] = char[0], bits [36..41] = char[1], …,
 * bits [0..5] = char[7]. Bits [48..63] are unused (always 0).
 *
 * Encoded values therefore live in [0, 2^48) — comfortably under JS Number's
 * 2^53 safe-integer limit, so TS code can use plain `number` (not `bigint`).
 *
 * ## Usage
 *
 * - Compile-time: `"ETH"_s`, `"USDC"_s`, `"PRIMARY"_s` (literal suffix).
 *   Invalid characters or >8 chars trigger a compile error (constexpr-throw).
 * - Runtime: `sysio::slug_name{"ETH"}` (validates + throws via sysio::check on
 *   invalid input; never silently produces a wrong value).
 *
 * @see fc::slug_name in libfc/include/fc/slug_name.hpp (host-side mirror)
 * @see project_codename_type.md memory + the data-model-refactor plan §3.1
 */

#include <sysio/check.hpp>
#include <sysio/serialize.hpp>
#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>

namespace sysio {

/// 8-byte packed identifier, alphabet `[A-Z0-9_]+`, max 8 chars.
/// See file-level docs for the packing format.
struct slug_name {
   uint64_t value = 0;

   constexpr slug_name() = default;
   constexpr explicit slug_name(uint64_t raw) : value(raw) {}

   /// Construct from a string (compile-time-or-runtime validated).
   /// Throws via `sysio::check` on >8 chars or invalid alphabet.
   explicit slug_name(std::string_view s) {
      sysio::check(s.size() <= 8, "slug_name: max 8 characters");
      uint64_t out = 0;
      for (std::size_t i = 0; i < s.size(); ++i) {
         const auto v = char_to_slot(s[i]);
         sysio::check(v != INVALID_SLOT, "slug_name: invalid character (alphabet is [A-Z0-9_])");
         out |= (v << (42 - i * 6));   // most-significant-symbol-first
      }
      value = out;
   }

   /// Unpack to a string. Stops at the first null (zero) slot.
   std::string to_string() const {
      std::string out;
      out.reserve(8);
      for (std::size_t i = 0; i < 8; ++i) {
         const auto slot = (value >> (42 - i * 6)) & 0x3F;
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

   SYSLIB_SERIALIZE(slug_name, (value))

   /// Sentinel for invalid characters. Public so tests / parsers can detect.
   static constexpr uint64_t INVALID_SLOT = static_cast<uint64_t>(-1);

   /// Map alphabet character to its 6-bit slot value.
   /// Returns INVALID_SLOT for out-of-alphabet input.
   static constexpr uint64_t char_to_slot(char c) {
      if (c >= 'A' && c <= 'Z') return static_cast<uint64_t>(1 + (c - 'A'));   // 1..26
      if (c >= '0' && c <= '9') return static_cast<uint64_t>(27 + (c - '0'));  // 27..36
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

/// Internal: invalid-input sentinel for the compile-time literal. CDT compiles
/// without exceptions, so we can't `throw` in a constexpr context. Instead the
/// literal calls this non-constexpr function which causes any compile-time
/// evaluation to fail (constexpr cannot invoke a non-constexpr function). At
/// runtime — should never be reached — the function calls sysio::check.
[[noreturn]] inline void codename_literal_failed(const char* msg) {
   sysio::check(false, msg);
   __builtin_unreachable();
}

/// Compile-time slug_name literal: `"ETH"_s`, `"USDC"_s`, `"PRIMARY"_s`.
/// Bad characters or >8 chars cause a compile error (the call to
/// `codename_literal_failed` is not constant-evaluable).
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
      out |= (slot << (42 - i * 6));
   }
   return slug_name{out};
}

} // namespace slug_name_literals

using slug_name_literals::operator""_s;

} // namespace sysio

#pragma once
/**
 * @file fc/slug_name.hpp
 * @brief 8-byte packed identifier for Chain/Token/Reserve `code` fields.
 *
 * fc::slug_name is fc::basic_name specialised to the slug alphabet — up to 8
 * characters over [A-Z0-9_], packed most-significant-symbol-first into a
 * uint64. It mirrors the contract-side sysio::slug_name; both sides must use
 * the identical encoding so a packed value is byte-identical on the wire
 * (protobuf `uint64`).
 *
 * Encoded values live in [0, 2^48) — under JS Number's 2^53 safe-integer
 * limit, so TS/JS code can use a plain `number` rather than bigint.
 *
 * @see fc::basic_name, sysio::slug_name (contract-side mirror)
 */

#include <fc/basic_name.hpp>
#include <fc/exception/exception.hpp>
#include <fc/variant.hpp>
#include <fc/variant_object.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>

namespace fc {

struct slug_name_traits {
   // max_len is 8, not the 11 (= ceil(64/6)) that would fill all 64 bits:
   // 8 symbols x 6 bits = 48, keeping every encoded value in [0, 2^48) —
   // under JS Number's 2^53 safe-integer limit, so the TS/JS side can use a
   // plain `number` instead of bigint. 11 symbols would span the full 64 bits.
   static constexpr int max_len = 8;

   // Symbol 0 is the '\0' pad/terminator; 1-26 = A-Z; 27-36 = 0-9; 37 = '_'.
   // Stored as a named array so the length comes from the literal via sizeof —
   // a leading NUL otherwise defeats std::string_view's auto-length.
   static constexpr char alphabet_storage[] =
      "\0ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_";
   static constexpr std::string_view alphabet{ alphabet_storage,
                                               sizeof(alphabet_storage) - 1 };

   // A symbol-0 ('\0') slot terminates the string — to_string() stops there, so
   // a raw value with an interior zero decodes identically to the contract-side
   // sysio::slug_name, which also stops at the first zero.
   static constexpr bool zero_terminates = true;

   // MSB-first: the first symbol occupies bits [42..47], the last symbol bits
   // [0..5]. Byte-identical with the contract-side sysio::slug_name.
   static constexpr basic_name_endianness packing = basic_name_endianness::MSB;

   [[noreturn]] static void throw_invalid( std::string_view in, const char* why ) {
      FC_ASSERT( false, "invalid slug_name '{}': {}", std::string(in), why );
      __builtin_unreachable();
   }
};

/// 8-byte packed identifier — alphabet [A-Z0-9_], <= 8 chars, MSB-first.
/// Byte-identical with the contract-side sysio::slug_name.
using slug_name = basic_name<slug_name_traits>;

namespace slug_name_literals {

/// Compile-time slug_name literal: `"ETH"_s`, `"USDC"_s`. A character outside
/// the alphabet, or more than 8 characters, is a compile error.
#if defined(__clang__)
# pragma clang diagnostic push
# pragma clang diagnostic ignored "-Wgnu-string-literal-operator-template"
#endif
template <typename T, T... Str>
inline constexpr slug_name operator""_s() {
   constexpr char buf[] = {Str...};
   static_assert(slug_name::is_valid_literal(std::string_view{buf, sizeof(buf)}),
                 "invalid _s literal: character not in [A-Z0-9_], or longer than 8");
   return slug_name{ std::integral_constant<uint64_t, slug_name::pack(std::string_view{buf, sizeof(buf)})>::value };
}
#if defined(__clang__)
# pragma clang diagnostic pop
#endif

} // namespace slug_name_literals

using slug_name_literals::operator""_s;

/// JSON carrier for a slug_name — a DUAL carrier, and deliberately so.
///
/// A canonical slug renders as its string spelling (`"LIQSOL"`), and the zero
/// sentinel as the empty string. A value below 2^42 renders as the **raw
/// integer**, because `to_string()` cannot represent it: `zero_terminates` is
/// true, so decoding stops at the first zero symbol slot, and every such value
/// collapses to `""`. Emitting the integer instead keeps this conversion TOTAL
/// and INJECTIVE over all 2^64 — `""` means exactly zero and nothing else.
///
/// The integer arm must not be replaced by a throw. Only `chain_code` is bound
/// to the proven source outpost, so a non-canonical `token_code` is plantable
/// from a forgeable attestation payload; a throwing conversion would let one
/// such row make an entire table unreadable over `get_table_rows`.
///
/// The two carriers are distinguished by JSON *type* here, and by string
/// LENGTH after a round trip through JSON text — `fc::json` quotes a uint64
/// above 0xffffffff, so the integer arm comes back as a decimal string that
/// `from_variant` re-routes on length (see there). What is never ambiguous is
/// the spelling: a canonical slug is at most max_len symbols and a stringified
/// uint64 past 0xffffffff is at least 10 digits. The carrier could NOT have
/// been a numeric string chosen freely — the slug alphabet contains digits, so
/// `"7"` is itself a valid canonical slug.
inline void to_variant(const slug_name& s, fc::variant& v) {
   const std::string text = s.to_string();
   // `pack` is the non-validating encoder, so this is a pure round-trip test:
   // the string spelling is used only when it recovers the value exactly.
   if (slug_name::pack(text) == s.value) {
      v = text;
      return;
   }
   v = s.value;
}

/// Accepts every carrier `to_variant` can emit, plus — TRANSITIONALLY — the
/// `{"value": <uint64>}` object that abigen's reflected struct emitted before
/// `slug_name` became an ABI builtin.
///
/// The object arm is what makes the cross-repo landing window survivable: with
/// no variant conversions, a slug converts through
/// `FC_REFLECT_TEMPLATE(basic_name<Traits>, (value))` and is therefore
/// object-only, while a string/integer-only reader rejects that object. There
/// is no value both spellings accept, so a JSON *writer* cannot straddle the
/// window the way a reader can. Delete this arm once no writer emits the
/// object form.
inline void from_variant(const fc::variant& v, slug_name& s) {
   if (v.is_string()) {
      const std::string_view text = v.get_string();
      // The integer carrier arrives here as a STRING whenever it crossed JSON
      // TEXT: fc::json quotes a uint64 above 0xffffffff (fc/io/json.cpp), and
      // `next_key` is json text that a paginating caller feeds back as a bound.
      // Length disambiguates exactly — a canonical slug is at most max_len
      // symbols, while a stringified uint64 past 0xffffffff is at least 10
      // digits — so no valid spelling is diverted. In particular the 8-digit
      // "12345678" stays a slug, keeping the rule that `"7"` is the slug 7 and
      // not the integer 7.
      if (text.size() > static_cast<std::size_t>(slug_name_traits::max_len)) {
         s = slug_name{ v.as_uint64() };
         return;
      }
      // Validating: the ctor round-trip-checks and rejects a non-canonical or
      // out-of-alphabet spelling. `""` is the zero sentinel.
      s = slug_name{ text };
      return;
   }
   if (v.is_object()) {
      s = slug_name{ v.get_object()["value"].as_uint64() };
      return;
   }
   s = slug_name{ v.as_uint64() };
}

} // namespace fc

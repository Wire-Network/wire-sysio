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

#include <algorithm>
#include <string>
#include <string_view>

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

   // A code must START with a letter. This is what makes the string carrier
   // unambiguous: no legal code can be spelled like a number, so a bare JSON
   // string is always a code and never a decimal. Without it the alphabet's
   // digits make "7" both a valid code (packed 149533581377536) and a valid
   // decimal, and "1E3" / "0X10" additionally collide with JS numeric syntax —
   // an ambiguity no reader can resolve from the value alone. Digits and '_'
   // remain legal in every position after the first ("V1", "USDC", "TRAIL_").
   // The empty string is unaffected: it is the zero sentinel, not a spelling.
   static constexpr std::string_view leading_alphabet{ "ABCDEFGHIJKLMNOPQRSTUVWXYZ" };

   // A symbol-0 ('\0') slot terminates the string — to_string() stops there, so
   // a raw value with an interior zero decodes identically to the contract-side
   // sysio::slug_name, which also stops at the first zero.
   static constexpr bool zero_terminates = true;

   // MSB-first: the first symbol occupies bits [42..47], the last symbol bits
   // [0..5]. Byte-identical with the contract-side sysio::slug_name.
   static constexpr basic_name_endianness packing = basic_name_endianness::MSB;

   // The rejection messages. Identical to the contract-side
   // sysio::slug_name_traits so a failure reads the same on both sides of the
   // wire; the host additionally names the offending input via throw_invalid.
   static constexpr const char* bad_char_message =
      "character is not in allowed character set for slug_names ([A-Z0-9_])";
   static constexpr const char* too_long_message = "string is too long to be a valid slug_name";
   static constexpr const char* bad_leading_char_message =
      "slug_name must start with a letter ([A-Z])";
   static constexpr const char* bad_final_symbol_message =
      "final character in slug_name does not fit its packed slot";
   static constexpr const char* not_normalized_message =
      "slug_name is not properly normalized";

   [[noreturn]] static void throw_invalid( std::string_view in, const char* why ) {
      FC_ASSERT( false, "invalid slug_name '{}': {}", std::string(in), why );
      __builtin_unreachable();
   }
};

/// 8-byte packed identifier — alphabet [A-Z0-9_], <= 8 chars, MSB-first.
///
/// DERIVED, not an alias, and byte-identical with the contract-side
/// sysio::slug_name, which is derived for the same reason. Two things need it:
/// abigen only matches a builtin on a real type (CDT side), and is_canonical()
/// below belongs to this encoding rather than to every basic_name.
struct slug_name : basic_name<slug_name_traits> {
   using base = basic_name<slug_name_traits>;
   using base::base;
   constexpr slug_name() = default;

   /// Does this value have a canonical spelling? A slug_name built from a RAW
   /// uint64 bypasses the validating constructor — and nothing validates on
   /// deserialization either, since the reflected member is written directly —
   /// so it can hold a value no spelling produces: anything whose leading symbol
   /// slot is empty, or that uses one of the 26 unused symbol values, or that
   /// sets any of bits 48-63. Such a value cannot round-trip.
   ///
   /// This lives on slug_name and NOT on basic_name because it is meaningless
   /// for `name`: that alphabet is exactly 2^5 with no gaps and its 13 symbols
   /// consume all 64 bits, so every uint64 IS a canonical name and the predicate
   /// could never be false.
   bool is_canonical() const {
      const std::string text = to_string();
      return is_valid_literal(text) && pack(text) == value;
   }
};

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

/// JSON carrier for a slug_name: the canonical string spelling, and nothing
/// else. A slug renders as its text (`"LIQSOL"`), the zero sentinel as `""`.
///
/// The string is unambiguous because `leading_alphabet` forbids a code from
/// starting with a digit: no legal spelling can be read as a number, so a bare
/// JSON string is always a code. That is what removes the need for a second,
/// type-disjoint carrier — and why the carrier could not have been a numeric
/// string before the rule existed.
///
/// Render a slug as its spelling. TOTAL, like sysio::chain::name — a renderer is
/// a READ path, and a throwing one turns one bad row into a failure of everything
/// that scans it (an unspellable code would stall every underwriter_plugin commit,
/// not just that cell). Validation lives on the WRITE path: from_variant's string
/// arm goes through the validating constructor, so a non-canonical spelling is
/// refused at construction.
///
/// TOTAL is where the resemblance to `name` ends, and the difference is the whole
/// reason is_canonical() exists here and would be meaningless there. `name`'s
/// alphabet is exactly 2^5 with no gaps and its 13 symbols consume all 64 bits, so
/// every raw uint64 IS a canonical name and its render is total AND injective —
/// nothing is ever lost. slug_name's decode is injective only over its canonical
/// range. Outside it the render either REJECTS on the way back or silently
/// NORMALIZES, and the leading symbol does not tell you which:
///
///   - a digit-leading symbol spells something from_variant refuses (loud);
///   - a value below 1<<42 renders "" — indistinguishable from the zero sentinel;
///   - to_string() reads only bits 0-47 and stops at the first zero symbol, so
///     `pack("ETH") | 1<<63` renders "ETH" and re-parses to a DIFFERENT value.
///
/// The last one means two distinct raw values can share a spelling. is_canonical()
/// is what separates them; a successful render is not.
inline void to_variant(const slug_name& s, fc::variant& v) {
   v = s.to_string();
}

namespace detail {

/// Checked unsigned decode for the transitional object arm's `value`.
///
/// `fc::variant::as_uint64` COERCES where this needs to validate: it wraps a
/// negative `int64` to `UINT64_MAX`, truncates a `double`, turns null/bool into
/// 0/1, and its string path goes through a `lexical_cast` that does not reject a
/// sign (`variant.cpp`, `as_uint64`). `from_variant` feeds `encode_field`, so a
/// bound of `{"value": -1}` would otherwise encode `be64(UINT64_MAX)` and page
/// from the far end of the table. Every shape that is not an exact non-negative
/// integer is rejected here instead.
///
/// Canonicality is deliberately NOT required: a packed value with no spelling is
/// the one thing the string carrier cannot express, so this arm is the only way
/// to name such a key as a bound.
inline uint64_t checked_packed_value(const fc::variant& v) {
   if (v.is_uint64())
      return v.as_uint64();
   if (v.is_int64()) {
      const int64_t i = v.as_int64();
      FC_ASSERT(i >= 0, "slug_name value must not be negative, got {}", i);
      return static_cast<uint64_t>(i);
   }
   if (v.is_string()) {
      const std::string_view text = v.get_string();
      // All-digits only: no sign, no decimal point, no exponent. `as_uint64`
      // then throws on overflow rather than wrapping.
      //
      // The predicate is bound to a local on purpose: FC_ASSERT stringizes its
      // condition INTO the fmt format string (`#TEST ": " FORMAT`), so a lambda
      // inline here would feed fmt's compile-time checker the lambda's own
      // braces as malformed replacement fields.
      const bool all_digits =
         !text.empty() && std::all_of(text.begin(), text.end(),
                                      [](char c) { return c >= '0' && c <= '9'; });
      FC_ASSERT(all_digits, "slug_name value must be an unsigned decimal, got '{}'",
                std::string(text));
      return v.as_uint64();
   }
   FC_ASSERT(false, "slug_name value must be an unsigned integer, got {}",
             fc::reflector<fc::variant::type_id>::to_string(v.get_type()));
   __builtin_unreachable();
}

} // namespace detail

/// Accepts the string carrier `to_variant` emits, plus — TRANSITIONALLY — the
/// `{"value": <uint64>}` object that abigen's reflected struct emitted before
/// `slug_name` became an ABI builtin.
///
/// The object arm is what makes the cross-repo landing window survivable: with
/// no variant conversions, a slug converts through
/// `FC_REFLECT_TEMPLATE(basic_name<Traits>, (value))` and is therefore
/// object-only, while a string-only reader rejects that object. There is no
/// value both spellings accept, so a JSON *writer* cannot straddle the window
/// the way a reader can. Delete this arm once no writer emits the object form.
inline void from_variant(const fc::variant& v, slug_name& s) {
   if (v.is_object()) {
      s = slug_name{ detail::checked_packed_value(v.get_object()["value"]) };
      return;
   }
   // A number is REJECTED, never coerced. The slug alphabet contains digits, so
   // `"123"` is itself a canonical slug whose packed value is nothing like 123
   // — reading the JSON number 123 as either one would be a silent mis-decode.
   // Same for null/bool, which `as_uint64` would quietly turn into 0/1.
   FC_ASSERT(v.is_string(), "slug_name must be a string, got {}",
             fc::reflector<fc::variant::type_id>::to_string(v.get_type()));
   // Validating: the ctor round-trip-checks and rejects a non-canonical or
   // out-of-alphabet spelling. `""` is the zero sentinel.
   s = slug_name{ std::string_view{ v.get_string() } };
}

} // namespace fc

namespace std {
   /// slug_name is DERIVED, so `std::hash<fc::basic_name<Traits>>` no longer
   /// covers it: a partial specialization is matched by deducing the exact
   /// type, which never considers a derived-to-base conversion. Without this,
   /// `std::hash<fc::slug_name>` resolves to the disabled primary template and
   /// every unordered container over a slug fails to compile. sysio::chain::name
   /// carries its own for the same reason, and hashes the same way.
   template <>
   struct hash<fc::slug_name> {
      size_t operator()(const fc::slug_name& s) const noexcept {
         static_assert(sizeof(size_t) == sizeof(uint64_t));
         return __builtin_bswap64(s.value);
      }
   };
} // namespace std

/// slug_name is DERIVED, so it needs its own reflection — the base's
/// FC_REFLECT_TEMPLATE does not cover it, and a plain FC_REFLECT cannot take an
/// INHERITED member (the pointer is to the base). Same form sysio::chain::name
/// uses, for the same reason; the layout is unchanged.
FC_REFLECT_DERIVED_EMPTY( fc::slug_name, (fc::basic_name<fc::slug_name_traits>) )

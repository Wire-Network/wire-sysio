#pragma once
/**
 * @file fc/basic_name.hpp
 * @brief Generic packed 64-bit identifier, MSB- or LSB-first per traits.
 *
 * basic_name<Traits> is the shared implementation behind sysio::chain::name and
 * fc::slug_name. Both pack a short string into a uint64_t. They differ only in
 * alphabet, length, and packing direction, which Traits supplies.
 *
 * Traits is the policy that specialises the template; it must satisfy the
 * basic_name_traits concept (declared below). alphabet[0] is the pad symbol and
 * any character outside the alphabet maps to symbol 0. zero_terminates selects
 * how to_string() treats a symbol-0 slot — a hard terminator (slug_name) or an
 * ordinary interior character (name's '.'). packing selects MSB- or LSB-first
 * layout; MSB-first makes integer ordering match string ordering, LSB-first
 * places the first symbol in the low bits (legacy wire formats, locality of
 * least-significant prefix).
 *
 * The symbol width is derived — the minimal bits to index the alphabet,
 * ceil(log2(alphabet.size())). When max_len * width exceeds 64 the final
 * symbol is narrowed to whatever fits — this is exactly why sysio name's 13th
 * symbol is 4 bits (13 * 5 = 65 > 64). In MSB layout the narrow symbol sits in
 * the low bits; in LSB layout it sits in the high bits. In both cases at the
 * "far end" of the packed value relative to the first symbol.
 *
 * Mapping out-of-alphabet characters to symbol 0 lets one round-trip check —
 * "the input must be the canonical spelling of its own encoding" — serve as the
 * validating constructor for every alphabet.
 *
 * @see sysio::chain::name (sysio_name_traits), fc::slug_name (slug_name_traits)
 */

#include <fc/reflect/reflect.hpp>

#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <string_view>

namespace fc {

/// Packing direction for basic_name. MSB places the first symbol in the
/// highest-order bits so integer order matches string lex order; LSB places
/// the first symbol in the lowest-order bits and is used by formats that
/// predate the MSB convention.
enum class basic_name_endianness { MSB, LSB };

/// Compile-time contract for a basic_name Traits policy: an alphabet, a
/// length, a zero_terminates flag steering to_string(), a packing direction,
/// and an invalid-input hook. Enforced in place of a prose list of trait
/// requirements.
template <typename Traits>
concept basic_name_traits =
   requires( std::string_view in, const char* why ) {
      { Traits::max_len }                -> std::convertible_to<int>;
      { Traits::alphabet }               -> std::convertible_to<std::string_view>;
      { Traits::zero_terminates }        -> std::convertible_to<bool>;
      { Traits::packing }                -> std::convertible_to<basic_name_endianness>;
      { Traits::throw_invalid(in, why) } -> std::same_as<void>;
   }
   && Traits::max_len > 0
   && std::string_view{ Traits::alphabet }.size() > 0;

template <basic_name_traits Traits>
struct basic_name {
   uint64_t value = 0;

   constexpr basic_name() = default;
   constexpr explicit basic_name(uint64_t v) : value(v) {}

   /// Construct from a string: checks length, then requires the input to be the
   /// canonical spelling of its own encoding (round-trip check). Throws via
   /// Traits::throw_invalid on bad or non-canonical input.
   explicit basic_name(std::string_view str) : value(encode(str)) {}

   constexpr uint64_t to_uint64_t() const { return value; }
   constexpr bool     empty()      const { return value == 0; }
   constexpr bool     good()       const { return value != 0; }
   constexpr explicit operator bool() const { return value != 0; }

   /// Non-validating encode — the constexpr path used by literals and by
   /// string_to_name. Characters outside the alphabet pack as symbol 0.
   static constexpr uint64_t pack(std::string_view str) {
      uint64_t v = 0;
      const int n = static_cast<int>(str.size());
      for (int i = 0; i < Traits::max_len && i < n; ++i)
         v |= (sym_of(str[i]) & width_mask(i)) << shift(i);
      return v;
   }

   /// Compile-time literal check: length within bounds and every character in
   /// the alphabet. For zero_terminates traits the pad symbol (alphabet[0]) is
   /// additionally rejected; accepting it would let a literal like "A\0B"_s
   /// compile to the same packed value as "A"_s, while the runtime constructor
   /// fed the same bytes would throw on the canonical round-trip check. The
   /// literal path bypasses that constructor, so the check has to live here.
   /// Canonicality (trailing pads, an over-wide final symbol) is still left to
   /// the validating constructor's round-trip check.
   static constexpr bool is_valid_literal(std::string_view str) {
      if (str.size() > static_cast<std::size_t>(Traits::max_len))
         return false;
      for (char c : str) {
         if (Traits::alphabet.find(c) == std::string_view::npos)
            return false;
         if constexpr (Traits::zero_terminates) {
            if (c == Traits::alphabet[0]) return false;
         }
      }
      return true;
   }

   std::string to_string() const {
      std::string s;
      for (int i = 0; i < Traits::max_len; ++i) {
         const uint64_t sym = (value >> shift(i)) & width_mask(i);
         // A zero-terminated alphabet (slug_name) ends at the first symbol-0
         // slot; for name, symbol 0 ('.') is an ordinary interior character.
         if (Traits::zero_terminates && sym == 0)
            break;
         s.push_back(char_of(sym));
      }
      if (!Traits::zero_terminates) {
         // trailing symbol-0 slots are padding, not content
         const char pad = char_of(0);
         while (!s.empty() && s.back() == pad)
            s.pop_back();
      }
      return s;
   }

   // Total order on the packed value. With MSB packing this matches the
   // decoded string's lexicographic order; with LSB packing it does not (the
   // first symbol sits in the low bits, so high-order symbols dominate the
   // integer comparison). Defaulted <=> / == synthesize the four relational
   // operators and !=.
   friend constexpr std::strong_ordering operator<=>(basic_name a, basic_name b) = default;
   friend constexpr bool                 operator==(basic_name a, basic_name b) = default;

   // Equality against a raw packed value (!= and the reversed form synthesized).
   friend constexpr bool operator==(basic_name a, uint64_t b) { return a.value == b; }

   friend std::ostream& operator<<(std::ostream& os, const basic_name& n) {
      return os << n.to_string();
   }

private:
   // --- symbol width: minimal bits to index the alphabet ---
   static constexpr int symbol_bits(std::size_t alphabet_size) {
      int b = 0;
      while ((std::size_t{1} << b) < alphabet_size) ++b;
      return b;
   }
   static constexpr int bits       = symbol_bits(Traits::alphabet.size());
   static constexpr int total_bits = Traits::max_len * bits < 64
                                   ? Traits::max_len * bits : 64;
   static_assert((Traits::max_len - 1) * bits < 64,
                 "basic_name: symbol layout does not fit in 64 bits");

   // --- alphabet (single source of truth; both directions derived) ---
   /// symbol value -> character. Out-of-range symbols decode as the pad
   /// character (alphabet[0]).
   static constexpr char char_of(uint64_t s) {
      const std::string_view a = Traits::alphabet;
      return s < a.size() ? a[s] : a[0];
   }
   /// character -> symbol value. Any character not in the alphabet maps to 0.
   static constexpr uint64_t sym_of(char c) {
      const std::string_view a = Traits::alphabet;
      for (std::size_t s = 0; s < a.size(); ++s)
         if (a[s] == c) return static_cast<uint64_t>(s);
      return 0;
   }

   // --- Bit layout. Direction is set by Traits::packing. The final symbol
   //     absorbs any shortfall when max_len * bits > 64. ---
   /// Bit offset of symbol i. MSB: symbol 0 occupies the highest bits and the
   /// final (possibly narrow) symbol sits at offset 0. LSB: symbol 0 occupies
   /// the lowest bits and the final symbol sits at the high end.
   static constexpr uint32_t shift(int i) {
      if constexpr (Traits::packing == basic_name_endianness::MSB) {
         const int s = total_bits - bits * (i + 1);
         return s > 0 ? static_cast<uint32_t>(s) : 0u;
      } else {
         return static_cast<uint32_t>(bits * i);
      }
   }
   /// Value mask of symbol i (the final symbol may be narrower than `bits`).
   /// Position depends on packing direction, but width depends only on i.
   static constexpr uint64_t width_mask(int i) {
      const int w = (i == Traits::max_len - 1) ? total_bits - bits * i : bits;
      return (static_cast<uint64_t>(1) << w) - 1;
   }

   /// Validating encode — mirrors sysio::chain::name::set(): length check, then
   /// require the string to round-trip (rejects non-canonical input).
   static uint64_t encode(std::string_view str) {
      if (static_cast<int>(str.size()) > Traits::max_len)
         Traits::throw_invalid(str, "too long");
      const basic_name packed{ pack(str) };
      if (packed.to_string() != str)
         Traits::throw_invalid(str, "not properly normalized");
      return packed.value;
   }
};

/// fmtlib hook — `format_as` is found by ADL for basic_name and its derivations.
template <typename Traits>
auto format_as(const basic_name<Traits>& n) { return n.to_string(); }

} // namespace fc

namespace std {
   template <typename Traits>
   struct hash<fc::basic_name<Traits>> {
      size_t operator()(const fc::basic_name<Traits>& n) const noexcept {
         static_assert(sizeof(size_t) == sizeof(uint64_t));
         return __builtin_bswap64(n.value);
      }
   };
} // namespace std

FC_REFLECT_TEMPLATE( (typename Traits), fc::basic_name<Traits>, (value) )

#pragma once
/**
 * @file fc/basic_name.hpp
 * @brief Generic MSB-first packed 64-bit identifier.
 *
 * basic_name<Traits> is the shared implementation behind sysio::chain::name and
 * fc::slug_name. Both pack a short string into a uint64_t, most-significant
 * symbol first, so that integer ordering matches string ordering. They differ
 * only in alphabet and length, which Traits supplies.
 *
 * A Traits type must provide:
 *   static constexpr int              max_len;   // number of symbols
 *   static constexpr std::string_view alphabet;  // alphabet[s] is symbol s's
 *                                                // character; alphabet[0] is the
 *                                                // pad symbol. char -> symbol is
 *                                                // derived (any char not in the
 *                                                // alphabet maps to symbol 0).
 *   [[noreturn]] static void throw_invalid(std::string_view in, const char* why);
 *
 * The symbol width is derived — the minimal bits to index the alphabet,
 * ceil(log2(alphabet.size())). Symbols are packed most-significant-first; when
 * max_len * width exceeds 64 the final symbol is narrowed to whatever fits —
 * this is exactly why sysio name's 13th symbol is 4 bits (13 * 5 = 65 > 64).
 *
 * Mapping out-of-alphabet characters to symbol 0 lets one round-trip check —
 * "the input must be the canonical spelling of its own encoding" — serve as the
 * validating constructor for every alphabet.
 *
 * @see sysio::chain::name (sysio_name_traits), fc::slug_name (slug_name_traits)
 */

#include <fc/reflect/reflect.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <string_view>

namespace fc {

template <typename Traits>
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
   /// the alphabet. Canonicality (trailing pads, an over-wide final symbol) is
   /// left to the validating constructor's round-trip check.
   static constexpr bool is_valid_literal(std::string_view str) {
      if (str.size() > static_cast<std::size_t>(Traits::max_len))
         return false;
      for (char c : str)
         if (Traits::alphabet.find(c) == std::string_view::npos)
            return false;
      return true;
   }

   std::string to_string() const {
      std::string s;
      for (int i = 0; i < Traits::max_len; ++i) {
         const uint64_t sym = (value >> shift(i)) & width_mask(i);
         s.push_back(char_of(sym));
      }
      // trailing symbol-0 slots are padding, not content
      const char pad = char_of(0);
      while (!s.empty() && s.back() == pad)
         s.pop_back();
      return s;
   }

   // Total order on the packed value; MSB-first packing makes it match the
   // decoded string's lexicographic order. Defaulted <=> / == synthesize the
   // four relational operators and !=.
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

   // --- MSB-first bit layout. The final symbol absorbs any shortfall when
   //     max_len * bits > 64. ---
   /// Bit offset of symbol i. The final symbol sits in the low bits (offset 0).
   static constexpr uint32_t shift(int i) {
      const int s = total_bits - bits * (i + 1);
      return s > 0 ? static_cast<uint32_t>(s) : 0u;
   }
   /// Value mask of symbol i (the final symbol may be narrower than `bits`).
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

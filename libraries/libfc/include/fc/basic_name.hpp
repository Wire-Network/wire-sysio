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
      { Traits::bad_char_message }         -> std::convertible_to<const char*>;
      { Traits::too_long_message }         -> std::convertible_to<const char*>;
      { Traits::bad_final_symbol_message } -> std::convertible_to<const char*>;
      { Traits::not_normalized_message }   -> std::convertible_to<const char*>;
   }
   && Traits::max_len > 0
   && std::string_view{ Traits::alphabet }.size() > 0;

/// OPTIONAL traits member: the symbols a spelling may START with. Traits that
/// omit it accept any alphabet character in the leading position, which is what
/// `sysio::name` wants. `slug_name` supplies it so that no legal code can be
/// confused with a decimal number — see `slug_name_traits::leading_alphabet`.
template <typename Traits>
concept basic_name_has_leading_alphabet = requires {
   { Traits::leading_alphabet }         -> std::convertible_to<std::string_view>;
   { Traits::bad_leading_char_message } -> std::convertible_to<const char*>;
};

template <basic_name_traits Traits>
struct basic_name {
   uint64_t value = 0;

   constexpr basic_name() = default;
   constexpr explicit basic_name(uint64_t v) : value(v) {}

   /// Construct from a string. Rejects via Traits::throw_invalid unless the
   /// input is the canonical spelling of its own encoding — see
   /// validity_error(), which is the SINGLE validation algorithm this type
   /// has, shared with is_valid_literal() and byte-for-byte the same rules as
   /// the contract-side sysio::basic_name.
   ///
   /// constexpr: Traits::throw_invalid is not constexpr, so it is reached only
   /// on the failure path. A valid literal therefore constant-evaluates, and an
   /// invalid one is a compile error rather than a silent mis-encoding.
   constexpr explicit basic_name(std::string_view str) : value(0) {
      if (const char* why = validity_error(str))
         Traits::throw_invalid(str, why);
      value = pack(str);
   }

   /// Non-validating encode — the constexpr path used by literals and by
   /// string_to_name. Characters outside the alphabet pack as symbol 0.
   static constexpr uint64_t pack(std::string_view str) {
      uint64_t v = 0;
      const int n = static_cast<int>(str.size());
      for (int i = 0; i < Traits::max_len && i < n; ++i)
         v |= (sym_of(str[i]) & width_mask(i)) << shift(i);
      return v;
   }

   /// Is `str` a valid, canonical spelling? The literal path's gate — and the
   /// predicate a caller uses to ask whether a raw packed value has a spelling
   /// at all. Delegates to validity_error so the literal path and the throwing
   /// constructor can never disagree.
   static constexpr bool is_valid_literal(std::string_view str) {
      return validity_error(str) == nullptr;
   }

   /// THE validation algorithm. Returns nullptr when `str` is a valid, canonical
   /// spelling; otherwise the traits' message for the FIRST rule it breaks.
   /// Identical, rule for rule and in the same order, to the contract-side
   /// sysio::basic_name — the two are meant to be diffable.
   ///
   /// Rules 4-6 make pack() lossless, which is what lets the constructor drop
   /// the old `to_string() != str` round-trip: given 1-6, to_string(pack(str))
   /// IS str, so the round trip can be a test assertion instead of a runtime
   /// one. Rules 5 and 6 were previously enforced ONLY by that round-trip and
   /// were absent from is_valid_literal, so the literal path accepted spellings
   /// the constructor rejected — `"abcdefghijklm"_n` compiled and packed as
   /// `abcdefghijkl2` while `name{"abcdefghijklm"}` threw.
   static constexpr const char* validity_error(std::string_view str) {
      // 1. length
      if (str.size() > static_cast<std::size_t>(Traits::max_len))
         return Traits::too_long_message;

      // 2. leading symbol, for traits that restrict it
      if constexpr (basic_name_has_leading_alphabet<Traits>) {
         if (!str.empty()
             && std::string_view{ Traits::leading_alphabet }.find(str[0])
                   == std::string_view::npos)
            return Traits::bad_leading_char_message;
      }

      for (std::size_t i = 0; i < str.size(); ++i) {
         const std::size_t sym = alphabet.find(str[i]);

         // 3. in the alphabet
         if (sym == std::string_view::npos)
            return Traits::bad_char_message;

         // 4. a zero-terminated alphabet has no INTERIOR pad: to_string() stops
         //    at the first symbol-0 slot, so such a spelling cannot round-trip.
         if constexpr (Traits::zero_terminates) {
            if (sym == 0)
               return Traits::bad_char_message;
         }

         // 5. the final slot may be narrower than `bits` (13 x 5 > 64 for name,
         //    leaving 4 bits), and pack() would silently truncate a symbol too
         //    wide for it.
         if (static_cast<uint64_t>(sym) > width_mask(static_cast<int>(i)))
            return Traits::bad_final_symbol_message;
      }

      // 6. a non-zero-terminated alphabet strips TRAILING pads in to_string(),
      //    so a trailing pad cannot round-trip either.
      if constexpr (!Traits::zero_terminates) {
         if (!str.empty() && str.back() == alphabet[0])
            return Traits::not_normalized_message;
      }

      return nullptr;
   }

   constexpr uint64_t to_uint64_t() const { return value; }
   constexpr bool     empty()      const { return value == 0; }
   constexpr bool     good()       const { return value != 0; }
   constexpr explicit operator bool() const { return value != 0; }


   /// Does this value have a canonical spelling? A basic_name built from a RAW
   /// uint64 bypasses the validating constructor, so it can hold a value no
   /// spelling produces — for zero_terminates traits, anything whose leading
   /// symbol slot is empty. Such a value cannot round-trip: to_string() yields a
   /// text that packs to something else. Persisting one makes every later render
   /// of that row throw, so writers that accept a raw uint64 off the wire gate on
   /// this before storing it.
   bool is_canonical() const {
      const std::string text = to_string();
      return is_valid_literal(text) && pack(text) == value;
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
   /// `Traits::alphabet` as a view. The traits concept requires only that the
   /// member be CONVERTIBLE to string_view, so every use binds here rather than
   /// calling find/size/operator[] on the traits member and silently demanding
   /// more of a policy than the concept declares. Mirrors sysio::basic_name.
   static constexpr std::string_view alphabet = Traits::alphabet;

   // --- symbol width: minimal bits to index the alphabet ---
   static constexpr int symbol_bits(std::size_t alphabet_size) {
      int b = 0;
      while ((std::size_t{1} << b) < alphabet_size) ++b;
      return b;
   }
   static constexpr int bits       = symbol_bits(alphabet.size());
   static constexpr int total_bits = Traits::max_len * bits < 64
                                   ? Traits::max_len * bits : 64;
   static_assert((Traits::max_len - 1) * bits < 64,
                 "basic_name: symbol layout does not fit in 64 bits");

   // --- alphabet (single source of truth; both directions derived) ---
   /// symbol value -> character. Out-of-range symbols decode as the pad
   /// character (alphabet[0]).
   static constexpr char char_of(uint64_t s) {
      return s < alphabet.size() ? alphabet[s] : alphabet[0];
   }
   /// character -> symbol value. Any character not in the alphabet maps to 0.
   static constexpr uint64_t sym_of(char c) {
      for (std::size_t s = 0; s < alphabet.size(); ++s)
         if (alphabet[s] == c) return static_cast<uint64_t>(s);
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

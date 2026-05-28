#define BOOST_TEST_DYN_LINK
#include <boost/test/unit_test.hpp>

#include <fc/basic_name.hpp>
#include <fc/slug_name.hpp>
#include <fc/io/raw.hpp>

#include <string>
#include <string_view>
#include <unordered_set>

using fc::slug_name;
using fc::slug_name_literals::operator""_s;

namespace {

/// LSB-packed sibling of slug_name_traits: same alphabet, same length, same
/// zero-terminator semantics, but symbols are packed first-symbol-in-low-bits
/// instead of MSB-first. Exists only here, to exercise the LSB branch of
/// basic_name's shift logic.
struct slug_name_lsb_traits {
   static constexpr int max_len = 8;
   static constexpr char alphabet_storage[] =
      "\0ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_";
   static constexpr std::string_view alphabet{ alphabet_storage,
                                               sizeof(alphabet_storage) - 1 };
   static constexpr bool zero_terminates = true;
   static constexpr fc::basic_name_endianness packing = fc::basic_name_endianness::LSB;

   [[noreturn]] static void throw_invalid( std::string_view in, const char* why ) {
      FC_ASSERT( false, "invalid slug_name_lsb '{}': {}", std::string(in), why );
      __builtin_unreachable();
   }
};

using slug_name_lsb = fc::basic_name<slug_name_lsb_traits>;

} // anonymous namespace

BOOST_AUTO_TEST_SUITE(codename_tests)

// ---------------------------------------------------------------------------
//  Round-trip basics
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(roundtrip_simple_strings) {
   BOOST_CHECK_EQUAL(slug_name{"ETH"}.to_string(),     "ETH");
   BOOST_CHECK_EQUAL(slug_name{"USDC"}.to_string(),    "USDC");
   BOOST_CHECK_EQUAL(slug_name{"POLY"}.to_string(),    "POLY");
   BOOST_CHECK_EQUAL(slug_name{"WIRE"}.to_string(),    "WIRE");
   BOOST_CHECK_EQUAL(slug_name{"SOL"}.to_string(),     "SOL");
   BOOST_CHECK_EQUAL(slug_name{"PRIMARY"}.to_string(), "PRIMARY");
   BOOST_CHECK_EQUAL(slug_name{"ETHEREUM"}.to_string(), "ETHEREUM");  // exactly 8 chars
   BOOST_CHECK_EQUAL(slug_name{"SOLANA"}.to_string(),  "SOLANA");
   BOOST_CHECK_EQUAL(slug_name{"LIQETH"}.to_string(),  "LIQETH");
   BOOST_CHECK_EQUAL(slug_name{"LIQSOL"}.to_string(),  "LIQSOL");
}

BOOST_AUTO_TEST_CASE(roundtrip_with_underscores) {
   BOOST_CHECK_EQUAL(slug_name{"A_B"}.to_string(),     "A_B");
   BOOST_CHECK_EQUAL(slug_name{"X_Y_Z"}.to_string(),   "X_Y_Z");
   BOOST_CHECK_EQUAL(slug_name{"_LEAD"}.to_string(),   "_LEAD");
   BOOST_CHECK_EQUAL(slug_name{"TRAIL_"}.to_string(),  "TRAIL_");
}

BOOST_AUTO_TEST_CASE(roundtrip_with_digits) {
   BOOST_CHECK_EQUAL(slug_name{"V1"}.to_string(),       "V1");
   BOOST_CHECK_EQUAL(slug_name{"0"}.to_string(),        "0");
   BOOST_CHECK_EQUAL(slug_name{"X12345"}.to_string(),   "X12345");
   BOOST_CHECK_EQUAL(slug_name{"01234567"}.to_string(), "01234567");  // 8 digits
}

BOOST_AUTO_TEST_CASE(empty_codename) {
   const slug_name empty;
   BOOST_CHECK_EQUAL(empty.value, 0u);
   BOOST_CHECK_EQUAL(empty.to_string(), "");
}

// ---------------------------------------------------------------------------
//  Literal suffix
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(literal_suffix_matches_runtime_construction) {
   BOOST_CHECK_EQUAL("ETH"_s.value,      slug_name{"ETH"}.value);
   BOOST_CHECK_EQUAL("USDC"_s.value,     slug_name{"USDC"}.value);
   BOOST_CHECK_EQUAL("PRIMARY"_s.value,  slug_name{"PRIMARY"}.value);
   BOOST_CHECK_EQUAL("WIRE"_s.value,     slug_name{"WIRE"}.value);
   BOOST_CHECK_EQUAL("ETHEREUM"_s.value, slug_name{"ETHEREUM"}.value);
}

BOOST_AUTO_TEST_CASE(distinct_codenames_distinct_values) {
   BOOST_CHECK_NE("ETH"_s.value,    "SOL"_s.value);
   BOOST_CHECK_NE("ETH"_s.value,    "ETHEREUM"_s.value);
   BOOST_CHECK_NE("PRIMARY"_s.value, "BACKUP"_s.value);
}

BOOST_AUTO_TEST_CASE(equality_operators) {
   BOOST_CHECK("ETH"_s == "ETH"_s);
   BOOST_CHECK("ETH"_s != "SOL"_s);
   BOOST_CHECK("ABC"_s < "ABD"_s);
}

// ---------------------------------------------------------------------------
//  Encoded values fit JS Number safe-integer space (2^53)
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(values_under_js_safe_integer_limit) {
   // 6 bits × 8 chars = 48 bits → max encoded value 2^48 - 1.
   // JS Number safe limit is 2^53 - 1. Confirm representative codenames
   // are well under that.
   constexpr uint64_t JS_SAFE_LIMIT = (1ULL << 53) - 1;

   BOOST_CHECK_LT("ETHEREUM"_s.value, JS_SAFE_LIMIT);
   BOOST_CHECK_LT("ZZZZZZZZ"_s.value, JS_SAFE_LIMIT);
   BOOST_CHECK_LT("12345678"_s.value, JS_SAFE_LIMIT);
   BOOST_CHECK_LT("________"_s.value, JS_SAFE_LIMIT);

   // The theoretical maximum: all slots = 37 (the `_` char).
   // Value = 37 * (1 + 2^6 + 2^12 + ... + 2^42) = 37 * ((2^48 - 1) / 63).
   const uint64_t max_codename = "________"_s.value;
   BOOST_CHECK_LT(max_codename, JS_SAFE_LIMIT);
   BOOST_CHECK_LT(max_codename, (1ULL << 48));
}

// ---------------------------------------------------------------------------
//  Validation — runtime constructor rejects bad input
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(rejects_too_long) {
   BOOST_CHECK_THROW(slug_name{"TOOLONG12"}, fc::exception);
   BOOST_CHECK_THROW(slug_name{"AAAAAAAAA"}, fc::exception);  // 9 chars
}

BOOST_AUTO_TEST_CASE(rejects_lowercase) {
   BOOST_CHECK_THROW(slug_name{"eth"}, fc::exception);
   BOOST_CHECK_THROW(slug_name{"usdc"}, fc::exception);
}

BOOST_AUTO_TEST_CASE(rejects_special_chars) {
   BOOST_CHECK_THROW(slug_name{"ETH-MAIN"}, fc::exception);
   BOOST_CHECK_THROW(slug_name{"ETH.MAIN"}, fc::exception);
   BOOST_CHECK_THROW(slug_name{"ETH MAIN"}, fc::exception);
   BOOST_CHECK_THROW(slug_name{"$WIRE"},    fc::exception);
   BOOST_CHECK_THROW(slug_name{"!"},        fc::exception);
}

BOOST_AUTO_TEST_CASE(accepts_full_alphabet) {
   // Each char in the alphabet at every position must round-trip
   const std::string all_letters_digits_underscore =
      "AZ09_";  // A, Z, 0, 9, _ — endpoints of each alphabet sub-range
   const slug_name cn{all_letters_digits_underscore};
   BOOST_CHECK_EQUAL(cn.to_string(), all_letters_digits_underscore);
}

// ---------------------------------------------------------------------------
//  Alphabet
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(every_alphabet_char_roundtrips) {
   // every non-pad symbol of the alphabet round-trips as a single-char slug
   const std::string_view alphabet = fc::slug_name_traits::alphabet;
   for (std::size_t s = 1; s < alphabet.size(); ++s) {
      const std::string one(1, alphabet[s]);
      BOOST_CHECK_EQUAL(slug_name{one}.to_string(), one);
   }
}

BOOST_AUTO_TEST_CASE(symbol_zero_is_the_nul_pad) {
   BOOST_CHECK_EQUAL(fc::slug_name_traits::alphabet[0], '\0');
}

// ---------------------------------------------------------------------------
//  FC_REFLECT — serialization round-trip
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(fc_serialization_roundtrip) {
   const slug_name original{"USDC"};
   const std::vector<char> packed = fc::raw::pack(original);
   const slug_name decoded = fc::raw::unpack<slug_name>(packed);
   BOOST_CHECK_EQUAL(decoded.value,        original.value);
   BOOST_CHECK_EQUAL(decoded.to_string(),  original.to_string());
}

BOOST_AUTO_TEST_CASE(fc_serialization_size_is_8_bytes) {
   const slug_name cn{"ETH"};
   const auto packed = fc::raw::pack(cn);
   // The wire format is a single uint64 (no varint tagging from FC_REFLECT
   // for a POD struct with one fixed-width field).
   BOOST_CHECK_EQUAL(packed.size(), 8u);
}

// ---------------------------------------------------------------------------
//  _s literal must reject the pad symbol embedded in the buffer
//  (zero_terminates trait): runtime construction throws on the same bytes;
//  the literal path bypassed the constructor before is_valid_literal was
//  hardened, so it would silently truncate "A\0B"_s -> "A"_s.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(literal_rejects_embedded_pad_for_zero_terminator_trait) {
   // Compile-time: is_valid_literal must say no to an embedded NUL.
   static_assert(slug_name::is_valid_literal(std::string_view{"ETH", 3}),
                 "well-formed slug literal must validate");
   static_assert(!slug_name::is_valid_literal(std::string_view{"A\0B", 3}),
                 "embedded NUL in a zero-terminator alphabet must not validate");
   static_assert(!slug_name::is_valid_literal(std::string_view{"\0A", 2}),
                 "leading NUL must not validate");
   static_assert(!slug_name::is_valid_literal(std::string_view{"A\0", 2}),
                 "trailing NUL is also a non-canonical literal; runtime "
                 "constructor would re-encode \"A\\0\" to just \"A\" and fail "
                 "the round-trip check, so the literal must too");

   // Runtime: the validating constructor already throws on the same input.
   BOOST_CHECK_THROW(slug_name(std::string_view{"A\0B", 3}), fc::exception);
   BOOST_CHECK_THROW(slug_name(std::string_view{"\0A", 2}),  fc::exception);
   BOOST_CHECK_THROW(slug_name(std::string_view{"A\0", 2}),  fc::exception);
}

// ---------------------------------------------------------------------------
//  basic_name LSB packing path - uses a local LSB twin of slug_name_traits.
//  MSB-side behavior is consensus-pinned (slug_name and sysio::chain::name);
//  these tests guard the LSB branch of shift() against regression and
//  document the observable differences from MSB.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(lsb_first_symbol_lives_in_low_bits) {
   // For slug_name_lsb (6 bits per symbol, 8 symbols, max 48 bits used) the
   // first symbol of "A" sits at bits [0..5]. In the alphabet 'A' = 1, so the
   // packed value is exactly 1.
   BOOST_CHECK_EQUAL(slug_name_lsb{"A"}.value, 1ull);
   // 'Z' = 26 by the same logic.
   BOOST_CHECK_EQUAL(slug_name_lsb{"Z"}.value, 26ull);
   // '_' = 37 by the same logic.
   BOOST_CHECK_EQUAL(slug_name_lsb{"_"}.value, 37ull);
   // For comparison, MSB places 'A' at bits [42..47] -> 1 << 42.
   BOOST_CHECK_EQUAL(slug_name{"A"}.value, 1ull << 42);
}

BOOST_AUTO_TEST_CASE(lsb_two_symbol_layout) {
   // "AB" in LSB: 'A' at [0..5] = 1, 'B' at [6..11] = 2 << 6 = 128.
   BOOST_CHECK_EQUAL(slug_name_lsb{"AB"}.value, 1ull | (2ull << 6));
   // "BA" in LSB: 'B' at [0..5] = 2, 'A' at [6..11] = 1 << 6 = 64.
   BOOST_CHECK_EQUAL(slug_name_lsb{"BA"}.value, 2ull | (1ull << 6));
}

BOOST_AUTO_TEST_CASE(lsb_roundtrip_matches_input) {
   for (std::string_view s : { "ETH", "USDC", "POLY", "WIRE", "SOL",
                               "PRIMARY", "ETHEREUM", "A_B", "X12345" }) {
      BOOST_CHECK_EQUAL(slug_name_lsb{s}.to_string(), s);
   }
   BOOST_CHECK_EQUAL(slug_name_lsb{""}.to_string(), "");
   BOOST_CHECK(slug_name_lsb{""}.empty());
}

BOOST_AUTO_TEST_CASE(lsb_and_msb_differ_for_multisymbol_input) {
   // Same string, two packing directions: must encode to distinct values
   // whenever there is more than one symbol. (A one-symbol "A" happens to
   // be the same logical slot but at opposite ends, hence different values
   // also; the multisymbol case is the interesting one.)
   for (std::string_view s : { "AB", "ETH", "USDC", "ETHEREUM" }) {
      BOOST_CHECK_NE(slug_name{s}.value, slug_name_lsb{s}.value);
   }
}

BOOST_AUTO_TEST_CASE(lsb_integer_order_does_not_match_string_lex) {
   // MSB: "AB" < "AC" < "BA" because first-symbol comparison dominates the
   // high bits. LSB: the first symbol sits in the LOW bits, so the second
   // symbol dominates the comparison and "AB" > "BA" by integer value while
   // "AB" < "BA" by string lex. Pin the direction explicitly.
   BOOST_CHECK_LT(slug_name{"AB"}.value, slug_name{"BA"}.value);  // MSB matches lex
   BOOST_CHECK_GT(slug_name_lsb{"AB"}.value, slug_name_lsb{"BA"}.value);  // LSB inverts
}

BOOST_AUTO_TEST_CASE(lsb_round_trip_full_alphabet) {
   // Every non-pad alphabet character round-trips through LSB just like MSB.
   const std::string_view alphabet = slug_name_lsb_traits::alphabet;
   for (std::size_t s = 1; s < alphabet.size(); ++s) {
      const std::string one(1, alphabet[s]);
      BOOST_CHECK_EQUAL(slug_name_lsb{one}.to_string(), one);
   }
}

BOOST_AUTO_TEST_CASE(lsb_rejects_non_canonical_input) {
   // Length / alphabet checks come from basic_name; direction doesn't matter.
   BOOST_CHECK_THROW(slug_name_lsb{"TOOLONG12"}, fc::exception);  // 9 chars
   BOOST_CHECK_THROW(slug_name_lsb{"eth"},        fc::exception);
   BOOST_CHECK_THROW(slug_name_lsb{"ETH-MAIN"},   fc::exception);
}

BOOST_AUTO_TEST_CASE(lsb_serialization_size_is_8_bytes) {
   const slug_name_lsb cn{"ETH"};
   const auto packed = fc::raw::pack(cn);
   BOOST_CHECK_EQUAL(packed.size(), 8u);
   const auto decoded = fc::raw::unpack<slug_name_lsb>(packed);
   BOOST_CHECK_EQUAL(decoded.value,       cn.value);
   BOOST_CHECK_EQUAL(decoded.to_string(), cn.to_string());
}

BOOST_AUTO_TEST_CASE(lsb_hash_distinguishes_distinct_values) {
   // std::hash<basic_name<...>> hashes the packed value via __builtin_bswap64.
   // Verify the LSB instantiation works and yields distinct hashes for
   // distinct inputs - the bswap means equal-value collision is impossible,
   // but the point is that the template plumbing compiles and runs.
   std::hash<slug_name_lsb> h;
   std::unordered_set<std::size_t> seen;
   for (std::string_view s : { "ETH", "USDC", "POLY", "WIRE", "SOL" }) {
      const auto inserted = seen.insert(h(slug_name_lsb{s})).second;
      BOOST_CHECK(inserted);
   }
}

// ---------------------------------------------------------------------------
//  zero_terminates=false (name's alphabet) must NOT reject alphabet[0]:
//  '.' is an ordinary interior character for sysio::chain::name. We can't
//  pull in <sysio/chain/name.hpp> here without a circular libfc dependency,
//  so check the property at the basic_name level with a local trait.
// ---------------------------------------------------------------------------

namespace {

struct name_like_traits {
   static constexpr int              max_len  = 13;
   static constexpr std::string_view alphabet = ".12345abcdefghijklmnopqrstuvwxyz";
   static constexpr bool             zero_terminates = false;
   static constexpr fc::basic_name_endianness packing = fc::basic_name_endianness::MSB;
   [[noreturn]] static void throw_invalid( std::string_view in, const char* why ) {
      FC_ASSERT( false, "invalid name '{}': {}", std::string(in), why );
      __builtin_unreachable();
   }
};

using name_like = fc::basic_name<name_like_traits>;

} // anonymous namespace

BOOST_AUTO_TEST_CASE(non_zero_terminator_trait_accepts_alphabet_zero) {
   // For name-style traits the pad symbol is '.', and '.' is ALSO an ordinary
   // interior character: literals like "sysio.token" must validate. Make sure
   // the NUL-rejection hardening on basic_name::is_valid_literal stayed scoped
   // to zero_terminates=true and didn't accidentally tighten name.
   static_assert(name_like::is_valid_literal(std::string_view{"sysio.token", 11}),
                 "interior '.' must validate for name-style traits");
   static_assert(name_like::is_valid_literal(std::string_view{".alpha", 6}),
                 "leading '.' (the pad symbol) must validate when "
                 "zero_terminates is false");
}

BOOST_AUTO_TEST_SUITE_END()

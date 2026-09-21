#define BOOST_TEST_DYN_LINK
#include <boost/test/unit_test.hpp>

#include <fc/basic_name.hpp>
#include <fc/slug_name.hpp>
#include <fc/io/json.hpp>
#include <fc/io/raw.hpp>
#include <fc/variant.hpp>
#include <fc/variant_object.hpp>

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

   static constexpr const char* bad_char_message =
      "character is not in allowed character set for slug_names ([A-Z0-9_])";
   static constexpr const char* too_long_message = "string is too long to be a valid slug_name";
   static constexpr const char* bad_final_symbol_message =
      "final character in slug_name does not fit its packed slot";
   static constexpr const char* not_normalized_message = "slug_name is not properly normalized";

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
   BOOST_CHECK_EQUAL(slug_name{"TRAIL_"}.to_string(),  "TRAIL_");
}

BOOST_AUTO_TEST_CASE(roundtrip_with_digits) {
   BOOST_CHECK_EQUAL(slug_name{"V1"}.to_string(),       "V1");
   BOOST_CHECK_EQUAL(slug_name{"X12345"}.to_string(),   "X12345");
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
   BOOST_CHECK_LT("Z1234567"_s.value, JS_SAFE_LIMIT);
   BOOST_CHECK_LT("Z_______"_s.value, JS_SAFE_LIMIT);

   // The largest legal code: 'Z' (26) leading, then '_' (37) in every slot the
   // leading rule leaves free.
   const uint64_t max_codename = "Z_______"_s.value;
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

BOOST_AUTO_TEST_CASE(every_alphabet_char_roundtrips_after_the_first_slot) {
   // Every non-pad symbol round-trips — but only a letter may LEAD, so the
   // sweep puts each symbol in the SECOND slot behind a fixed letter.
   const std::string_view alphabet = fc::slug_name_traits::alphabet;
   for (std::size_t s = 1; s < alphabet.size(); ++s) {
      const std::string two = std::string("A") + alphabet[s];
      BOOST_CHECK_EQUAL(slug_name{two}.to_string(), two);
   }
}

BOOST_AUTO_TEST_CASE(a_code_must_start_with_a_letter) {
   // The rule that makes the string carrier unambiguous: no legal code can be
   // spelled like a number, so a bare JSON string is never a decimal.
   for (const char* bad : {"0", "7", "101", "1E3", "0X10", "12345678",
                           "_LEAD", "________", "01234567"}) {
      BOOST_CHECK_THROW(slug_name{bad}, fc::exception);
      BOOST_CHECK_MESSAGE(!slug_name::is_valid_literal(bad),
                          std::string{"must be rejected as a literal: "} + bad);
   }
   // Digits and '_' stay legal everywhere after the first symbol.
   for (const char* good : {"V1", "X12345", "AZ09_", "TRAIL_", "A_B", "Z_______"}) {
      BOOST_CHECK_EQUAL(slug_name{good}.to_string(), good);
      BOOST_CHECK(slug_name::is_valid_literal(good));
   }
   // The zero sentinel is not a spelling and is unaffected.
   BOOST_CHECK_EQUAL(slug_name{""}.value, 0u);
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
   static constexpr const char* bad_char_message =
      "character is not in allowed character set for names ([.1-5a-z])";
   static constexpr const char* too_long_message = "string is too long to be a valid name";
   static constexpr const char* bad_final_symbol_message =
      "thirteenth character in name cannot be a letter that comes after j";
   static constexpr const char* not_normalized_message = "name is not properly normalized";
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

// ── variant carrier ────────────────────────────────────────────────────────
// ONE carrier: the canonical string spelling. A slug renders as its text, zero
// as "", and a value with no spelling throws. The cases below pin that single
// shape from both directions — every writable spelling lands on a string, and
// every non-string is refused rather than coerced.

BOOST_AUTO_TEST_CASE(variant_canonical_slug_is_a_string) {
   fc::variant v;
   fc::to_variant(slug_name{"LIQSOL"}, v);
   BOOST_REQUIRE(v.is_string());
   BOOST_CHECK_EQUAL(v.as_string(), "LIQSOL");

   slug_name back;
   fc::from_variant(v, back);
   BOOST_CHECK(back == slug_name{"LIQSOL"});
}

BOOST_AUTO_TEST_CASE(variant_zero_is_the_empty_string_both_ways) {
   fc::variant v;
   fc::to_variant(slug_name{uint64_t{0}}, v);
   BOOST_REQUIRE(v.is_string());
   BOOST_CHECK_EQUAL(v.as_string(), "");

   slug_name back{uint64_t{12345}};
   fc::from_variant(fc::variant(std::string{}), back);
   BOOST_CHECK_EQUAL(back.value, 0u);
}

BOOST_AUTO_TEST_CASE(variant_non_canonical_has_no_spelling_and_throws) {
   // Every value below 1<<42 has a zero in the char[0] slot, so to_string()
   // truncates it to "" and no string recovers it. Rather than grow a second
   // carrier for those, the conversion refuses them — which is what keeps ""
   // meaning exactly zero. The throw is contained: get_table_rows catches per
   // row and renders that cell as hex.
   for (uint64_t raw : {uint64_t{1}, uint64_t{7}, uint64_t{42},
                        uint64_t{(uint64_t{1} << 42) - 1},
                        (uint64_t{1} << 48) - 1,  // symbols past the alphabet
                        ~uint64_t{0}}) {
      fc::variant v;
      BOOST_CHECK_THROW(fc::to_variant(slug_name{raw}, v), fc::exception);
   }
}

BOOST_AUTO_TEST_CASE(variant_every_canonical_value_round_trips_exactly) {
   // Injectivity across the boundary for the whole canonical range, including
   // its floor (1<<42 is "A") and the zero sentinel.
   for (uint64_t raw : {uint64_t{0}, uint64_t{1} << 42, fc::slug_name{"A"}.value,
                        fc::slug_name{"LIQSOL"}.value, fc::slug_name{"Z1234567"}.value,
                        fc::slug_name{"Z_______"}.value}) {
      fc::variant v;
      BOOST_REQUIRE_NO_THROW(fc::to_variant(slug_name{raw}, v));
      BOOST_REQUIRE(v.is_string());
      slug_name back;
      fc::from_variant(v, back);
      BOOST_CHECK_EQUAL(back.value, raw);
   }
}

BOOST_AUTO_TEST_CASE(variant_accepts_the_transitional_object_carrier) {
   // TRANSITIONAL: the shape abigen's reflected struct emitted before slug_name
   // became an ABI builtin. Deleted once no writer emits it.
   slug_name back;
   fc::from_variant(fc::variant(fc::mutable_variant_object("value", uint64_t{7})), back);
   BOOST_CHECK_EQUAL(back.value, 7u);

   fc::from_variant(
      fc::variant(fc::mutable_variant_object("value", fc::slug_name{"LIQSOL"}.value)), back);
   BOOST_CHECK(back == slug_name{"LIQSOL"});
}

BOOST_AUTO_TEST_CASE(variant_rejects_a_non_canonical_string_spelling) {
   // The string arm validates: an out-of-alphabet or non-canonical spelling is
   // a hard error, not a silent zero.
   slug_name back;
   BOOST_CHECK_THROW(fc::from_variant(fc::variant(std::string{"liqsol"}), back), fc::exception);
   BOOST_CHECK_THROW(fc::from_variant(fc::variant(std::string{"TOOOLONGXX"}), back), fc::exception);
}

BOOST_AUTO_TEST_CASE(variant_carrier_round_trips_through_json_TEXT) {
   // The variant-layer round trip above is NOT sufficient on its own: `next_key`
   // is json TEXT (chain_plugin renders it with fc::json::to_string) and a
   // paginating caller feeds it straight back as a `lower_bound`, re-parsed with
   // fc::json::from_string. A single string carrier is what makes that crossing
   // uneventful — fc::json quotes a uint64 above 0xffffffff, so a numeric
   // carrier would change JSON TYPE mid-flight and land on the string arm as a
   // decimal nobody asked for.
   const uint64_t values[] = {
      0u,                        // the zero sentinel -> ""
      uint64_t{1} << 42,         // the canonical floor ("A")
      slug_name{"ETH"}.value,
      slug_name{"Z1234567"}.value,  // digits after the leading letter
      slug_name{"Z_______"}.value,
   };
   for (const uint64_t raw : values) {
      fc::variant v;
      fc::to_variant(slug_name{raw}, v);
      const std::string text =
         fc::json::to_string(fc::variant(fc::mutable_variant_object("code", v)),
                             fc::time_point::maximum());
      slug_name back;
      BOOST_REQUIRE_NO_THROW(
         fc::from_variant(fc::json::from_string(text).get_object()["code"], back));
      BOOST_CHECK_MESSAGE(back.value == raw,
                          "json text round trip lost " << raw << " via " << text);
   }
}

BOOST_AUTO_TEST_CASE(mvo_accepts_every_spelling_a_caller_can_write) {
   // Pins the whole INPUT surface a test or caller may write for a slug_name
   // field. mutable_variant_object's templated operator() forwards to
   // fc::variant's constructor set (variant_object.hpp:206-211), so every
   // spelling below resolves through a different ctor and must still land on
   // the same value:
   //
   //   const char*      -> variant(const char*)          -> string
   //   std::string      -> variant(std::string)          -> string
   //   std::string_view -> variant(std::string_view)     -> string
   //   fc::slug_name    -> explicit variant(const T&)    -> to_variant -> string
   //
   // Every one lands on a string, because the string IS the carrier. This is
   // why no `codename()`-style wrapper is needed at a call site: the raw
   // literal and the `_s` literal both already work, and a wrapper returning
   // std::string is just identity.
   const slug_name expected{"LIQSOL"};

   const char* const      as_c_str  = "LIQSOL";
   const std::string      as_string = "LIQSOL";
   const std::string_view as_view   = "LIQSOL";

   const fc::variant obj{ fc::mutable_variant_object()
      ("c_str",   as_c_str)
      ("string",  as_string)
      ("view",    as_view)
      ("literal", "LIQSOL"_s)      // the _s literal — validated at compile time
      ("slug",    expected) };     // an fc::slug_name value

   for (const char* key : {"c_str", "string", "view", "literal", "slug"}) {
      const fc::variant& cell = obj.get_object()[key];
      BOOST_REQUIRE_MESSAGE(cell.is_string(), std::string{"not a string: "} + key);
      BOOST_CHECK_EQUAL(cell.as_string(), "LIQSOL");
      slug_name back;
      fc::from_variant(cell, back);
      BOOST_CHECK_MESSAGE(back == expected, std::string{"round trip failed: "} + key);
   }
}

BOOST_AUTO_TEST_CASE(variant_rejects_every_non_string_carrier) {
   // Nothing but a string (and the transitional object) is read. Each value
   // below would otherwise be COERCED by fc::variant::as_uint64 — which is the
   // failure mode a single carrier removes, because none of these coercions is
   // the value the writer meant:
   //
   //   7          -> not a code at all now; a code must start with a letter
   //   -1         -> lexical_cast does not reject a sign for an unsigned
   //                 target, it WRAPS; a bound of -1 would page from the far
   //                 end of the table
   //   null/false -> 0, the absent sentinel, silently
   //   true       -> 1, a value with no spelling at all
   slug_name back;
   BOOST_CHECK_THROW(fc::from_variant(fc::variant(uint64_t{7}), back), fc::exception);
   BOOST_CHECK_THROW(fc::from_variant(fc::variant(uint64_t{1} << 42), back), fc::exception);
   BOOST_CHECK_THROW(fc::from_variant(fc::variant(int64_t{-1}), back), fc::exception);
   BOOST_CHECK_THROW(fc::from_variant(fc::variant(int64_t{-12345678}), back), fc::exception);
   BOOST_CHECK_THROW(fc::from_variant(fc::variant(), back), fc::exception);
   BOOST_CHECK_THROW(fc::from_variant(fc::variant(false), back), fc::exception);
   BOOST_CHECK_THROW(fc::from_variant(fc::variant(true), back), fc::exception);
   BOOST_CHECK_THROW(fc::from_variant(fc::variant(1.5), back), fc::exception);

   // A numeric STRING is not an escape either: it is parsed as a slug like any
   // other spelling, so a signed or over-long decimal is simply invalid text.
   for (const char* spelling : {"-1", "-12345678", "+123456789", "4294967296",
                                "000000000000000042"}) {
      BOOST_CHECK_THROW(fc::from_variant(fc::variant(std::string{spelling}), back),
                        fc::exception);
   }
}

BOOST_AUTO_TEST_CASE(variant_object_arm_rejects_every_coercible_value_shape) {
   // The transitional object arm reaches a RAW uint64, so it is the one place a
   // malformed number can still land on a key. `fc::variant::as_uint64` coerces
   // rather than validates — it wraps a negative int64 to UINT64_MAX, truncates
   // a double, and turns null/bool into 0/1 — and since from_variant feeds
   // encode_field, `{"value": -1}` would encode be64(UINT64_MAX) and page from
   // the far end of the table. Each shape below must be refused, not coerced.
   slug_name back;
   const auto obj = [](const fc::variant& value) {
      return fc::variant(fc::mutable_variant_object("value", value));
   };

   BOOST_CHECK_THROW(fc::from_variant(obj(fc::variant(int64_t{-1})), back), fc::exception);
   BOOST_CHECK_THROW(fc::from_variant(obj(fc::variant(int64_t{-12345678})), back), fc::exception);
   BOOST_CHECK_THROW(fc::from_variant(obj(fc::variant(std::string{"-1"})), back), fc::exception);
   BOOST_CHECK_THROW(fc::from_variant(obj(fc::variant(std::string{"+1"})), back), fc::exception);
   BOOST_CHECK_THROW(fc::from_variant(obj(fc::variant(1.5)), back), fc::exception);
   BOOST_CHECK_THROW(fc::from_variant(obj(fc::variant(-1.0)), back), fc::exception);
   BOOST_CHECK_THROW(fc::from_variant(obj(fc::variant(std::string{"1.5"})), back), fc::exception);
   BOOST_CHECK_THROW(fc::from_variant(obj(fc::variant(std::string{"1E3"})), back), fc::exception);
   BOOST_CHECK_THROW(fc::from_variant(obj(fc::variant()), back), fc::exception);
   BOOST_CHECK_THROW(fc::from_variant(obj(fc::variant(true)), back), fc::exception);
   BOOST_CHECK_THROW(fc::from_variant(obj(fc::variant(std::string{})), back), fc::exception);
   // Out of range: all digits, but past 2^64. boost::lexical_cast throws rather
   // than saturating, so the parse error surfaces instead of a wrong value.
   BOOST_CHECK_THROW(
      fc::from_variant(obj(fc::variant(std::string{"1234567890123456789012345"})), back),
      fc::exception);

   // And the shapes a real pre-builtin writer emits still work. Canonicality is
   // NOT required here: a value with no spelling is exactly what the string
   // carrier cannot express, so this arm is the only way to name such a bound.
   fc::from_variant(obj(fc::variant(uint64_t{7})), back);
   BOOST_CHECK_EQUAL(back.value, 7u);
   fc::from_variant(obj(fc::variant(std::string{"7"})), back);
   BOOST_CHECK_EQUAL(back.value, 7u);
   fc::from_variant(obj(fc::variant(slug_name{"LIQSOL"}.value)), back);
   BOOST_CHECK(back == slug_name{"LIQSOL"});
}

BOOST_AUTO_TEST_SUITE_END()

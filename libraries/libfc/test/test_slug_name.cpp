#define BOOST_TEST_DYN_LINK
#include <boost/test/unit_test.hpp>

#include <fc/slug_name.hpp>
#include <fc/io/raw.hpp>

#include <string>

using fc::slug_name;
using fc::slug_name_literals::operator""_s;

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
//  Slot-to-char round-trip for every valid slot
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(every_valid_slot_roundtrips) {
   for (uint64_t slot = 1; slot <= 37; ++slot) {
      const char c = slug_name::slot_to_char(slot);
      BOOST_CHECK_NE(c, '\0');
      const uint64_t back = slug_name::char_to_slot(c);
      BOOST_CHECK_EQUAL(back, slot);
   }
}

BOOST_AUTO_TEST_CASE(slot_zero_is_terminator) {
   BOOST_CHECK_EQUAL(slug_name::slot_to_char(0), '\0');
}

BOOST_AUTO_TEST_CASE(out_of_range_slot_returns_null) {
   BOOST_CHECK_EQUAL(slug_name::slot_to_char(38), '\0');
   BOOST_CHECK_EQUAL(slug_name::slot_to_char(63), '\0');
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
//  String conversion operator
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(implicit_string_conversion) {
   const std::string s = std::string{slug_name{"PRIMARY"}};
   BOOST_CHECK_EQUAL(s, "PRIMARY");
}

BOOST_AUTO_TEST_SUITE_END()

/**
 * `fc::to_uint256` and `fc::to_int256`: the text of a variant read as a 256-bit integer, the way the
 * Ethereum and Solana clients read RPC quantities and IDL `u256` / `i256` arguments.
 */
#include <fc/exception/exception.hpp>
#include <fc/int256.hpp>
#include <fc/variant.hpp>

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace {

/// 2^256 - 1, the largest fc::uint256 and the largest magnitude of a fc::int256.
const std::string max_decimal = "115792089237316195423570985008687907853269984665640564039457584007913129639935";
/// 2^256.
const std::string past_max_decimal = "115792089237316195423570985008687907853269984665640564039457584007913129639936";
/// 2^256 - 1 in hex.
const std::string max_hex = "0x" + std::string(64, 'f');
/// 2^256 in hex: 33 bytes, whose low 32 are zero.
const std::string past_max_hex = "0x1" + std::string(64, '0');

const fc::uint256 max_uint256{max_decimal};
const fc::int256 max_int256{max_decimal};
const fc::int256 min_int256{-max_int256};

/// A predicate matching the range error for `text` read as `type`.
auto out_of_range(const std::string& text, const std::string& type) {
   return [message = text + " is out of range for " + type + " ["](const fc::parse_error_exception& e) {
      return e.to_detail_string().find(message) != std::string::npos;
   };
}

} // namespace

BOOST_AUTO_TEST_SUITE(to_int256_tests)

/// Decimal text past 2^256 - 1 is refused instead of wrapping modulo 2^256.
BOOST_AUTO_TEST_CASE(decimal_text_past_the_range_throws) {
   BOOST_CHECK_EQUAL(fc::to_uint256(fc::variant(max_decimal)), max_uint256);
   BOOST_CHECK_EQUAL(fc::to_int256(fc::variant(max_decimal)), max_int256);

   BOOST_CHECK_EXCEPTION(fc::to_uint256(fc::variant(past_max_decimal)), fc::parse_error_exception,
                         out_of_range(past_max_decimal, "uint256_t"));
   BOOST_CHECK_EXCEPTION(fc::to_int256(fc::variant(past_max_decimal)), fc::parse_error_exception,
                         out_of_range(past_max_decimal, "int256_t"));
   const std::string far_past_max(200, '9');
   BOOST_CHECK_EXCEPTION(fc::to_uint256(fc::variant(far_past_max)), fc::parse_error_exception,
                         out_of_range(far_past_max, "uint256_t"));

   BOOST_CHECK_EQUAL(fc::to_int256(fc::variant("-" + max_decimal)), min_int256);
   BOOST_CHECK_EXCEPTION(fc::to_int256(fc::variant("-" + past_max_decimal)), fc::parse_error_exception,
                         out_of_range("-" + past_max_decimal, "int256_t"));
}

/// Digits after a leading zero are decimal, not octal.
BOOST_AUTO_TEST_CASE(leading_zero_reads_as_decimal) {
   BOOST_CHECK_EQUAL(fc::to_uint256(fc::variant("010")), fc::uint256{10});
   BOOST_CHECK_EQUAL(fc::to_uint256(fc::variant("09")), fc::uint256{9});
   BOOST_CHECK_EQUAL(fc::to_uint256(fc::variant("000")), fc::uint256{0});
   BOOST_CHECK_EQUAL(fc::to_uint256(fc::variant("00" + max_decimal)), max_uint256);
   BOOST_CHECK_EQUAL(fc::to_int256(fc::variant("010")), fc::int256{10});
   BOOST_CHECK_EQUAL(fc::to_int256(fc::variant("-010")), fc::int256{-10});
   BOOST_CHECK_EXCEPTION(fc::to_uint256(fc::variant("0" + past_max_decimal)), fc::parse_error_exception,
                         out_of_range("0" + past_max_decimal, "uint256_t"));
}

/// Zero-led digit text past 2^256 - 1 throws at every length.
BOOST_AUTO_TEST_CASE(zero_led_digit_text_past_the_range_throws) {
   const std::string in_range = "0" + std::string(77, '7');
   BOOST_CHECK_EQUAL(fc::to_uint256(fc::variant(in_range)), fc::uint256{in_range.substr(1)});
   for (std::size_t sevens = 78; sevens <= 100; ++sevens) {
      const std::string text = "0" + std::string(sevens, '7');
      BOOST_TEST_CONTEXT("sevens " << sevens) {
         BOOST_CHECK_EXCEPTION(fc::to_uint256(fc::variant(text)), fc::parse_error_exception,
                               out_of_range(text, "uint256_t"));
         BOOST_CHECK_EXCEPTION(fc::to_int256(fc::variant(text)), fc::parse_error_exception,
                               out_of_range(text, "int256_t"));
      }
   }
}

/// Hex past 2^256 - 1 is refused instead of keeping its low 256 bits. Leading zero digits do not count.
BOOST_AUTO_TEST_CASE(hex_text_past_the_range_throws) {
   BOOST_CHECK_EQUAL(fc::to_uint256(fc::variant(max_hex)), max_uint256);
   BOOST_CHECK_EQUAL(fc::to_int256(fc::variant(max_hex)), max_int256);
   BOOST_CHECK_EQUAL(fc::to_uint256(fc::variant("0x" + std::string(40, '0') + std::string(64, 'f'))), max_uint256);
   BOOST_CHECK_EQUAL(fc::to_uint256(fc::variant(std::string(40, '0') + std::string(64, 'f'))), max_uint256);

   BOOST_CHECK_EXCEPTION(fc::to_uint256(fc::variant(past_max_hex)), fc::parse_error_exception,
                         out_of_range(past_max_hex, "uint256_t"));
   BOOST_CHECK_EXCEPTION(fc::to_int256(fc::variant(past_max_hex)), fc::parse_error_exception,
                         out_of_range(past_max_hex, "int256_t"));
   const std::string unprefixed_past_max(65, 'f');
   BOOST_CHECK_EXCEPTION(fc::to_uint256(fc::variant(unprefixed_past_max)), fc::parse_error_exception,
                         out_of_range(unprefixed_past_max, "uint256_t"));
}

/// A '-' makes the number negative: a fc::int256 takes it, a fc::uint256 refuses it unless it is zero.
BOOST_AUTO_TEST_CASE(negative_text) {
   BOOST_CHECK_EQUAL(fc::to_int256(fc::variant("-5")), fc::int256{-5});
   BOOST_CHECK_EQUAL(fc::to_int256(fc::variant(int64_t{-5})), fc::int256{-5});
   BOOST_CHECK_EQUAL(fc::to_int256(fc::variant("-0x10")), fc::int256{-16});
   BOOST_CHECK_EQUAL(fc::to_int256(fc::variant("-0")), fc::int256{0});
   BOOST_CHECK_EQUAL(fc::to_int256(fc::variant(INT64_MIN)), fc::int256{INT64_MIN});
   BOOST_CHECK_EQUAL(fc::to_int256(fc::variant(min_int256)), min_int256);

   BOOST_CHECK_EXCEPTION(fc::to_uint256(fc::variant("-5")), fc::parse_error_exception, out_of_range("-5", "uint256_t"));
   BOOST_CHECK_EXCEPTION(fc::to_uint256(fc::variant(int64_t{-1})), fc::parse_error_exception,
                         out_of_range("-1", "uint256_t"));
   BOOST_CHECK_EQUAL(fc::to_uint256(fc::variant("-0")), fc::uint256{0});
   BOOST_CHECK_EQUAL(fc::to_uint256(fc::variant("-0x0")), fc::uint256{0});
}

/// Text that is not a number is a parse_error_exception, whatever part of it is wrong.
BOOST_AUTO_TEST_CASE(text_that_is_not_a_number_throws_parse_error) {
   const std::string embedded_nul{"1\0"
                                  "2",
                                  3};
   const std::string arabic_indic_one = "\xd9\xa1";
   for (const std::string& text : std::vector<std::string>{"-", "-0x", "--5", "+5", " 5", "5 ", "1.5", "0xg1", "0x-5",
                                                           "true", embedded_nul, arabic_indic_one}) {
      BOOST_TEST_CONTEXT("text '" << text << "'") {
         BOOST_CHECK_THROW(fc::to_uint256(fc::variant(text)), fc::parse_error_exception);
         BOOST_CHECK_THROW(fc::to_int256(fc::variant(text)), fc::parse_error_exception);
      }
   }
}

/// What both functions read before range checking, they still read the same way.
BOOST_AUTO_TEST_CASE(numbers_in_range_keep_their_values) {
   BOOST_CHECK_EQUAL(fc::to_uint256(fc::variant("")), fc::uint256{0});
   BOOST_CHECK_EQUAL(fc::to_uint256(fc::variant()), fc::uint256{0});
   BOOST_CHECK_EQUAL(fc::to_uint256(fc::variant("0x")), fc::uint256{0});
   BOOST_CHECK_EQUAL(fc::to_uint256(fc::variant("0x0")), fc::uint256{0});
   BOOST_CHECK_EQUAL(fc::to_uint256(fc::variant("0x7b")), fc::uint256{123});
   BOOST_CHECK_EQUAL(fc::to_uint256(fc::variant("0X1F")), fc::uint256{31});
   BOOST_CHECK_EQUAL(fc::to_uint256(fc::variant("0xabc")), fc::uint256{0xabc});
   BOOST_CHECK_EQUAL(fc::to_uint256(fc::variant("ff")), fc::uint256{255});
   BOOST_CHECK_EQUAL(fc::to_uint256(fc::variant("123")), fc::uint256{123});
   BOOST_CHECK_EQUAL(fc::to_uint256(fc::variant(UINT64_MAX)), fc::uint256{UINT64_MAX});
   BOOST_CHECK_EQUAL(fc::to_uint256(fc::variant(max_uint256)), max_uint256);

   BOOST_CHECK_EQUAL(fc::to_int256(fc::variant("")), fc::int256{0});
   BOOST_CHECK_EQUAL(fc::to_int256(fc::variant("0x7b")), fc::int256{123});
   BOOST_CHECK_EQUAL(fc::to_int256(fc::variant(INT64_MAX)), fc::int256{INT64_MAX});
}

BOOST_AUTO_TEST_SUITE_END()

#include <fc/exception/exception.hpp>
#include <fc/string.hpp>

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <limits>
#include <string_view>

BOOST_AUTO_TEST_SUITE(string_to_integer_suite)

BOOST_AUTO_TEST_CASE(to_uint64_parses_its_whole_range) {
   BOOST_CHECK_EQUAL(fc::to_uint64("0"), 0u);
   BOOST_CHECK_EQUAL(fc::to_uint64("007"), 7u);
   BOOST_CHECK_EQUAL(fc::to_uint64("9223372036854775807"), uint64_t{std::numeric_limits<int64_t>::max()});
   BOOST_CHECK_EQUAL(fc::to_uint64("9223372036854775808"), uint64_t{1} << 63);
   BOOST_CHECK_EQUAL(fc::to_uint64("18446744073709551615"), std::numeric_limits<uint64_t>::max());
   BOOST_CHECK_THROW(fc::to_uint64("18446744073709551616"), fc::parse_error_exception);
}

BOOST_AUTO_TEST_CASE(to_uint64_accepts_a_leading_plus) {
   BOOST_CHECK_EQUAL(fc::to_uint64("+0"), 0u);
   BOOST_CHECK_EQUAL(fc::to_uint64("+1"), 1u);
   BOOST_CHECK_EQUAL(fc::to_uint64("+18446744073709551615"), std::numeric_limits<uint64_t>::max());
   BOOST_CHECK_THROW(fc::to_uint64("+18446744073709551616"), fc::parse_error_exception);
}

// A negative number is outside the range of uint64_t. It used to parse as its two's complement.
BOOST_AUTO_TEST_CASE(to_uint64_rejects_a_negative_number) {
   for (const std::string_view text : {"-1",                      // 2^64 - 1
                                       "-9223372036854775808",    // 2^63
                                       "-18446744073709551361",   // 255
                                       "-18446744073709551615",   // 1
                                       "-18446744073709551616"})
      BOOST_CHECK_THROW(fc::to_uint64(text), fc::parse_error_exception);
}

BOOST_AUTO_TEST_CASE(to_uint64_reads_negative_zero_as_zero) {
   BOOST_CHECK_EQUAL(fc::to_uint64("-0"), 0u);
   BOOST_CHECK_EQUAL(fc::to_uint64("-000"), 0u);
}

BOOST_AUTO_TEST_CASE(to_uint64_rejects_text_that_is_not_a_decimal_integer) {
   for (const std::string_view text : {"", "-", "+", "--1", "+-1", "-+1", " 1", "1 ", "0x10", "1.0", "1e3", "abc"})
      BOOST_CHECK_THROW(fc::to_uint64(text), fc::parse_error_exception);
}

BOOST_AUTO_TEST_CASE(to_int64_parses_its_whole_range) {
   BOOST_CHECK_EQUAL(fc::to_int64("0"), 0);
   BOOST_CHECK_EQUAL(fc::to_int64("-0"), 0);
   BOOST_CHECK_EQUAL(fc::to_int64("+0"), 0);
   BOOST_CHECK_EQUAL(fc::to_int64("-1"), -1);
   BOOST_CHECK_EQUAL(fc::to_int64("+1"), 1);
   BOOST_CHECK_EQUAL(fc::to_int64("9223372036854775807"), std::numeric_limits<int64_t>::max());
   BOOST_CHECK_EQUAL(fc::to_int64("+9223372036854775807"), std::numeric_limits<int64_t>::max());
   BOOST_CHECK_EQUAL(fc::to_int64("-9223372036854775808"), std::numeric_limits<int64_t>::min());
   BOOST_CHECK_THROW(fc::to_int64("9223372036854775808"), fc::parse_error_exception);
   BOOST_CHECK_THROW(fc::to_int64("18446744073709551615"), fc::parse_error_exception);
   BOOST_CHECK_THROW(fc::to_int64("-9223372036854775809"), fc::parse_error_exception);
}

BOOST_AUTO_TEST_CASE(to_int64_rejects_text_that_is_not_a_decimal_integer) {
   for (const std::string_view text : {"", "-", "+", "--1", " 1", "1 ", "0x10", "1.0", "1e3", "abc"})
      BOOST_CHECK_THROW(fc::to_int64(text), fc::parse_error_exception);
}

BOOST_AUTO_TEST_SUITE_END()

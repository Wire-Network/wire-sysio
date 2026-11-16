#include <fc/variant.hpp>
#include <fc/variant_object.hpp>
#include <fc/exception/exception.hpp>
#include <fc/int128.hpp>
#include <fc/int256.hpp>

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <limits>
#include <string>

using fc::variant;
using fc::variant_object;
using fc::mutable_variant_object;
using fc::variants;
using fc::blob;

BOOST_AUTO_TEST_SUITE(variant_as_suite)

BOOST_AUTO_TEST_CASE(as_int64_from_each_compatible_source) {
   BOOST_CHECK_EQUAL(variant{int64_t{-7}}.as_int64(), -7);
   BOOST_CHECK_EQUAL(variant{uint64_t{99}}.as_int64(), 99);
   BOOST_CHECK_EQUAL(variant{double{42.9}}.as_int64(), 42);
   BOOST_CHECK_EQUAL(variant{true}.as_int64(), 1);
   BOOST_CHECK_EQUAL(variant{false}.as_int64(), 0);
   BOOST_CHECK_EQUAL(variant{}.as_int64(), 0);
   BOOST_CHECK_EQUAL(variant{std::string{"-123"}}.as_int64(), -123);
}

BOOST_AUTO_TEST_CASE(as_int64_throws_on_array_or_object) {
   variant arr{variants{}};
   variant obj{variant_object{mutable_variant_object()}};

   BOOST_CHECK_THROW(arr.as_int64(), fc::bad_cast_exception);
   BOOST_CHECK_THROW(obj.as_int64(), fc::bad_cast_exception);
}

BOOST_AUTO_TEST_CASE(as_uint64_from_each_compatible_source) {
   BOOST_CHECK_EQUAL(variant{uint64_t{12}}.as_uint64(), 12u);
   BOOST_CHECK_EQUAL(variant{int64_t{34}}.as_uint64(), 34u);
   BOOST_CHECK_EQUAL(variant{double{56.7}}.as_uint64(), 56u);
   BOOST_CHECK_EQUAL(variant{true}.as_uint64(), 1u);
   BOOST_CHECK_EQUAL(variant{}.as_uint64(), 0u);
   BOOST_CHECK_EQUAL(variant{std::string{"77"}}.as_uint64(), 77u);
}

BOOST_AUTO_TEST_CASE(as_int128_from_each_compatible_source) {
   variant vi128{fc::int128{-1}};
   variant vu128{fc::uint128{42}};
   variant vstr{std::string{"-42"}};

   BOOST_CHECK_EQUAL(static_cast<int64_t>(vi128.as_int128()), -1);
   BOOST_CHECK_EQUAL(static_cast<uint64_t>(vu128.as_int128()), 42u);
   BOOST_CHECK_EQUAL(static_cast<int64_t>(vstr.as_int128()), -42);
   BOOST_CHECK_EQUAL(static_cast<int64_t>(variant{int64_t{99}}.as_int128()), 99);
   BOOST_CHECK_EQUAL(static_cast<int64_t>(variant{}.as_int128()), 0);
}

BOOST_AUTO_TEST_CASE(as_uint128_from_each_compatible_source) {
   variant vu128{fc::uint128{123}};
   BOOST_CHECK_EQUAL(static_cast<uint64_t>(vu128.as_uint128()), 123u);
   BOOST_CHECK_EQUAL(static_cast<uint64_t>(variant{uint64_t{456}}.as_uint128()), 456u);
   BOOST_CHECK_EQUAL(static_cast<uint64_t>(variant{std::string{"789"}}.as_uint128()), 789u);
   BOOST_CHECK_EQUAL(static_cast<uint64_t>(variant{}.as_uint128()), 0u);
}

BOOST_AUTO_TEST_CASE(as_int256_and_as_uint256) {
   variant vi{fc::int256(-7)};
   variant vu{fc::uint256(11)};
   BOOST_CHECK_EQUAL(vi.as_int256().str(), "-7");
   BOOST_CHECK_EQUAL(vu.as_uint256().str(), "11");
   BOOST_CHECK_EQUAL(variant{}.as_int256().str(), "0");
}

namespace {

constexpr fc::uint128 uint128_max = std::numeric_limits<fc::uint128>::max();
constexpr fc::int128  int128_max  = std::numeric_limits<fc::int128>::max();
constexpr fc::int128  int128_min  = std::numeric_limits<fc::int128>::min();
constexpr int64_t     int64_max   = std::numeric_limits<int64_t>::max();
constexpr int64_t     int64_min   = std::numeric_limits<int64_t>::min();
constexpr uint64_t    uint64_max  = std::numeric_limits<uint64_t>::max();

const std::string uint128_max_text       = "340282366920938463463374607431768211455";
const std::string uint128_past_max_text  = "340282366920938463463374607431768211456";
const std::string int128_max_text        = "170141183460469231731687303715884105727";
const std::string int128_past_max_text   = "170141183460469231731687303715884105728";
const std::string int128_min_text        = "-170141183460469231731687303715884105728";
const std::string int128_past_min_text   = "-170141183460469231731687303715884105729";
// 2^256 - 1, the maximum of fc::uint256 and the largest magnitude of the sign-magnitude fc::int256.
const std::string uint256_max_text =
   "115792089237316195423570985008687907853269984665640564039457584007913129639935";
const std::string uint256_past_max_text =
   "115792089237316195423570985008687907853269984665640564039457584007913129639936";
const std::string hex_prefix = "0x";

/// Matches the parse_error_exception a value outside the range of `type` raises.
auto out_of_range_for(const std::string& type) {
   return [type](const fc::parse_error_exception& e) {
      return e.to_detail_string().find(" is out of range for " + type + " [") != std::string::npos;
   };
}

} // namespace

BOOST_AUTO_TEST_CASE(as_uint128_holds_its_whole_range) {
   BOOST_CHECK_EQUAL(fc::to_string(variant{int64_t{0}}.as_uint128()), "0");
   BOOST_CHECK_EQUAL(fc::to_string(variant{std::string{"0"}}.as_uint128()), "0");
   BOOST_CHECK_EQUAL(fc::to_string(variant{int64_max}.as_uint128()), std::to_string(int64_max));
   BOOST_CHECK_EQUAL(fc::to_string(variant{uint64_max}.as_uint128()), std::to_string(uint64_max));
   BOOST_CHECK(variant{uint128_max}.as_uint128() == uint128_max);
   BOOST_CHECK(variant{uint128_max_text}.as_uint128() == uint128_max);
   BOOST_CHECK(variant{fc::int128{7}}.as_uint128() == 7);
   BOOST_CHECK(variant{int128_max}.as_uint128() == static_cast<fc::uint128>(int128_max));
   BOOST_CHECK(variant{hex_prefix + std::string(32, 'f')}.as_uint128() == uint128_max);
   // Leading zeros carry no value, however many digits they make.
   BOOST_CHECK(variant{std::string(50, '0') + uint128_max_text}.as_uint128() == uint128_max);
   BOOST_CHECK(variant{hex_prefix + std::string(50, '0') + std::string(32, 'F')}.as_uint128() == uint128_max);
}

BOOST_AUTO_TEST_CASE(as_uint128_rejects_a_negative_value) {
   BOOST_CHECK_EXCEPTION(variant{int64_t{-1}}.as_uint128(), fc::parse_error_exception, out_of_range_for("uint128_t"));
   BOOST_CHECK_EXCEPTION(variant{int64_min}.as_uint128(), fc::parse_error_exception, out_of_range_for("uint128_t"));
   BOOST_CHECK_EXCEPTION(variant{fc::int128{-1}}.as_uint128(), fc::parse_error_exception,
                         out_of_range_for("uint128_t"));
   BOOST_CHECK_EXCEPTION(variant{int128_min}.as_uint128(), fc::parse_error_exception, out_of_range_for("uint128_t"));
   // A sign is not part of an unsigned number's text.
   BOOST_CHECK_THROW(variant{std::string{"-1"}}.as_uint128(), fc::exception);
   BOOST_CHECK_THROW(variant{std::string{"-0"}}.as_uint128(), fc::exception);
   BOOST_CHECK_THROW(variant{std::string{"+1"}}.as_uint128(), fc::exception);
}

BOOST_AUTO_TEST_CASE(as_uint128_rejects_text_past_the_maximum) {
   BOOST_CHECK_EXCEPTION(variant{uint128_past_max_text}.as_uint128(), fc::parse_error_exception,
                         out_of_range_for("uint128_t"));
   BOOST_CHECK_EXCEPTION(variant{uint128_max_text + "0"}.as_uint128(), fc::parse_error_exception,
                         out_of_range_for("uint128_t"));
   BOOST_CHECK_EXCEPTION(variant{hex_prefix + "1" + std::string(32, '0')}.as_uint128(), fc::parse_error_exception,
                         out_of_range_for("uint128_t"));
   BOOST_CHECK_EXCEPTION(variant{hex_prefix + std::string(33, 'f')}.as_uint128(), fc::parse_error_exception,
                         out_of_range_for("uint128_t"));
   // Out of range and then a bad character: refused for either.
   BOOST_CHECK_THROW(variant{uint128_max_text + "0x"}.as_uint128(), fc::exception);
}

BOOST_AUTO_TEST_CASE(as_uint128_range_error_names_the_value_and_the_range) {
   BOOST_CHECK_EXCEPTION(variant{int64_t{-1}}.as_uint128(), fc::parse_error_exception,
                         [](const fc::parse_error_exception& e) {
                            return e.to_detail_string().find("-1 is out of range for uint128_t [0, " +
                                                             uint128_max_text + "]") != std::string::npos;
                         });
}

BOOST_AUTO_TEST_CASE(as_int128_holds_its_whole_range) {
   BOOST_CHECK_EQUAL(fc::to_string(variant{int64_t{0}}.as_int128()), "0");
   BOOST_CHECK_EQUAL(fc::to_string(variant{std::string{"0"}}.as_int128()), "0");
   BOOST_CHECK_EQUAL(fc::to_string(variant{std::string{"-0"}}.as_int128()), "0");
   BOOST_CHECK_EQUAL(fc::to_string(variant{int64_min}.as_int128()), std::to_string(int64_min));
   BOOST_CHECK_EQUAL(fc::to_string(variant{int64_max}.as_int128()), std::to_string(int64_max));
   BOOST_CHECK_EQUAL(fc::to_string(variant{uint64_max}.as_int128()), std::to_string(uint64_max));
   BOOST_CHECK(variant{int128_max}.as_int128() == int128_max);
   BOOST_CHECK(variant{int128_min}.as_int128() == int128_min);
   BOOST_CHECK(variant{int128_max_text}.as_int128() == int128_max);
   BOOST_CHECK(variant{int128_min_text}.as_int128() == int128_min);
   BOOST_CHECK(variant{static_cast<fc::uint128>(int128_max)}.as_int128() == int128_max);
   BOOST_CHECK(variant{std::string(50, '0') + int128_max_text}.as_int128() == int128_max);
   BOOST_CHECK(variant{"-" + std::string(50, '0') + int128_min_text.substr(1)}.as_int128() == int128_min);
}

BOOST_AUTO_TEST_CASE(as_int128_reads_hex_as_a_bit_pattern) {
   // Unsigned hex is the bit pattern of the result; signed hex is a magnitude.
   BOOST_CHECK(variant{hex_prefix + "7" + std::string(31, 'f')}.as_int128() == int128_max);
   BOOST_CHECK(variant{hex_prefix + "8" + std::string(31, '0')}.as_int128() == int128_min);
   BOOST_CHECK(variant{hex_prefix + std::string(32, 'F')}.as_int128() == -1);
   BOOST_CHECK(variant{std::string{"-0xff"}}.as_int128() == -255);
   BOOST_CHECK(variant{"-" + hex_prefix + "8" + std::string(31, '0')}.as_int128() == int128_min);
   BOOST_CHECK_EXCEPTION(variant{"-" + hex_prefix + "8" + std::string(30, '0') + "1"}.as_int128(),
                         fc::parse_error_exception, out_of_range_for("int128_t"));
   BOOST_CHECK_EXCEPTION(variant{hex_prefix + "1" + std::string(32, '0')}.as_int128(), fc::parse_error_exception,
                         out_of_range_for("int128_t"));
   BOOST_CHECK_EXCEPTION(variant{hex_prefix + std::string(33, 'f')}.as_int128(), fc::parse_error_exception,
                         out_of_range_for("int128_t"));
}

BOOST_AUTO_TEST_CASE(as_int128_rejects_a_value_outside_its_range) {
   BOOST_CHECK_EXCEPTION(variant{uint128_max}.as_int128(), fc::parse_error_exception, out_of_range_for("int128_t"));
   BOOST_CHECK_EXCEPTION(variant{static_cast<fc::uint128>(int128_max) + 1}.as_int128(), fc::parse_error_exception,
                         out_of_range_for("int128_t"));
   BOOST_CHECK_EXCEPTION(variant{int128_past_max_text}.as_int128(), fc::parse_error_exception,
                         out_of_range_for("int128_t"));
   BOOST_CHECK_EXCEPTION(variant{int128_past_min_text}.as_int128(), fc::parse_error_exception,
                         out_of_range_for("int128_t"));
   BOOST_CHECK_EXCEPTION(variant{uint128_max_text}.as_int128(), fc::parse_error_exception,
                         out_of_range_for("int128_t"));
   BOOST_CHECK_EXCEPTION(variant{"-" + uint128_max_text}.as_int128(), fc::parse_error_exception,
                         out_of_range_for("int128_t"));
   BOOST_CHECK_EXCEPTION(variant{int128_max_text + "0"}.as_int128(), fc::parse_error_exception,
                         out_of_range_for("int128_t"));
   BOOST_CHECK_EXCEPTION(variant{int128_min_text + "0"}.as_int128(), fc::parse_error_exception,
                         out_of_range_for("int128_t"));
   BOOST_CHECK_THROW(variant{std::string{"+1"}}.as_int128(), fc::exception);
   BOOST_CHECK_THROW(variant{std::string{"--1"}}.as_int128(), fc::exception);
}

BOOST_AUTO_TEST_CASE(as_int128_range_error_names_the_value_and_the_range) {
   BOOST_CHECK_EXCEPTION(variant{uint128_max}.as_int128(), fc::parse_error_exception,
                         [](const fc::parse_error_exception& e) {
                            return e.to_detail_string().find(uint128_max_text + " is out of range for int128_t [" +
                                                             int128_min_text + ", " + int128_max_text + "]") !=
                                   std::string::npos;
                         });
}

BOOST_AUTO_TEST_CASE(from_variant_uint128_rejects_a_value_outside_its_range) {
   BOOST_CHECK(variant{int64_t{0}}.as<fc::uint128>() == 0);
   BOOST_CHECK(variant{int64_max}.as<fc::uint128>() == static_cast<fc::uint128>(int64_max));
   BOOST_CHECK(variant{uint64_max}.as<fc::uint128>() == uint64_max);
   BOOST_CHECK(variant{uint128_max}.as<fc::uint128>() == uint128_max);
   BOOST_CHECK(variant{uint128_max_text}.as<fc::uint128>() == uint128_max);

   BOOST_CHECK_EXCEPTION(variant{int64_t{-1}}.as<fc::uint128>(), fc::parse_error_exception,
                         out_of_range_for("uint128_t"));
   BOOST_CHECK_EXCEPTION(variant{int64_min}.as<fc::uint128>(), fc::parse_error_exception,
                         out_of_range_for("uint128_t"));
   BOOST_CHECK_EXCEPTION(variant{uint128_past_max_text}.as<fc::uint128>(), fc::parse_error_exception,
                         out_of_range_for("uint128_t"));
   BOOST_CHECK_THROW(variant{std::string{"-1"}}.as<fc::uint128>(), fc::exception);
   BOOST_CHECK_THROW(variant{std::string{"+1"}}.as<fc::uint128>(), fc::exception);

   // Only the integer and string variants convert.
   BOOST_CHECK_THROW(variant{fc::int128{1}}.as<fc::uint128>(), fc::bad_cast_exception);
   BOOST_CHECK_THROW(variant{true}.as<fc::uint128>(), fc::bad_cast_exception);
   BOOST_CHECK_THROW(variant{1.0}.as<fc::uint128>(), fc::bad_cast_exception);
   BOOST_CHECK_THROW(variant{}.as<fc::uint128>(), fc::bad_cast_exception);
}

BOOST_AUTO_TEST_CASE(from_variant_int128_rejects_a_value_outside_its_range) {
   BOOST_CHECK(variant{int64_t{0}}.as<fc::int128>() == 0);
   BOOST_CHECK(variant{int64_min}.as<fc::int128>() == int64_min);
   BOOST_CHECK(variant{uint64_max}.as<fc::int128>() == uint64_max);
   BOOST_CHECK(variant{int128_max}.as<fc::int128>() == int128_max);
   BOOST_CHECK(variant{int128_min}.as<fc::int128>() == int128_min);
   BOOST_CHECK(variant{static_cast<fc::uint128>(int128_max)}.as<fc::int128>() == int128_max);
   BOOST_CHECK(variant{int128_min_text}.as<fc::int128>() == int128_min);

   BOOST_CHECK_EXCEPTION(variant{uint128_max}.as<fc::int128>(), fc::parse_error_exception,
                         out_of_range_for("int128_t"));
   BOOST_CHECK_EXCEPTION(variant{static_cast<fc::uint128>(int128_max) + 1}.as<fc::int128>(),
                         fc::parse_error_exception, out_of_range_for("int128_t"));
   BOOST_CHECK_EXCEPTION(variant{int128_past_max_text}.as<fc::int128>(), fc::parse_error_exception,
                         out_of_range_for("int128_t"));
   BOOST_CHECK_EXCEPTION(variant{int128_past_min_text}.as<fc::int128>(), fc::parse_error_exception,
                         out_of_range_for("int128_t"));

   BOOST_CHECK_THROW(variant{true}.as<fc::int128>(), fc::bad_cast_exception);
   BOOST_CHECK_THROW(variant{1.0}.as<fc::int128>(), fc::bad_cast_exception);
   BOOST_CHECK_THROW(variant{}.as<fc::int128>(), fc::bad_cast_exception);
}

BOOST_AUTO_TEST_CASE(as_uint256_holds_its_whole_range) {
   BOOST_CHECK_EQUAL(variant{int64_t{0}}.as_uint256().str(), "0");
   BOOST_CHECK_EQUAL(variant{std::string{"0"}}.as_uint256().str(), "0");
   BOOST_CHECK_EQUAL(variant{std::string{"-0"}}.as_uint256().str(), "0");
   BOOST_CHECK_EQUAL(variant{int64_max}.as_uint256().str(), std::to_string(int64_max));
   BOOST_CHECK_EQUAL(variant{uint64_max}.as_uint256().str(), std::to_string(uint64_max));
   BOOST_CHECK_EQUAL(variant{fc::uint256{uint256_max_text}}.as_uint256().str(), uint256_max_text);
   BOOST_CHECK_EQUAL(variant{uint256_max_text}.as_uint256().str(), uint256_max_text);
   BOOST_CHECK_EQUAL(variant{fc::int256{uint256_max_text}}.as_uint256().str(), uint256_max_text);
   BOOST_CHECK_EQUAL(variant{hex_prefix + std::string(64, 'f')}.as_uint256().str(), uint256_max_text);
   BOOST_CHECK_EQUAL(variant{hex_prefix + std::string(50, '0') + std::string(64, 'f')}.as_uint256().str(),
                     uint256_max_text);
   // Text after a leading zero is octal.
   BOOST_CHECK_EQUAL(variant{std::string{"010"}}.as_uint256().str(), "8");
}

BOOST_AUTO_TEST_CASE(as_uint256_rejects_a_value_outside_its_range) {
   BOOST_CHECK_EXCEPTION(variant{int64_t{-1}}.as_uint256(), fc::parse_error_exception, out_of_range_for("uint256_t"));
   BOOST_CHECK_EXCEPTION(variant{int64_min}.as_uint256(), fc::parse_error_exception, out_of_range_for("uint256_t"));
   BOOST_CHECK_EXCEPTION(variant{fc::int256{-1}}.as_uint256(), fc::parse_error_exception,
                         out_of_range_for("uint256_t"));
   BOOST_CHECK_EXCEPTION(variant{std::string{"-1"}}.as_uint256(), fc::parse_error_exception,
                         out_of_range_for("uint256_t"));
   BOOST_CHECK_EXCEPTION(variant{"-" + uint256_max_text}.as_uint256(), fc::parse_error_exception,
                         out_of_range_for("uint256_t"));
   BOOST_CHECK_EXCEPTION(variant{uint256_past_max_text}.as_uint256(), fc::parse_error_exception,
                         out_of_range_for("uint256_t"));
   BOOST_CHECK_EXCEPTION(variant{uint256_max_text + "0"}.as_uint256(), fc::parse_error_exception,
                         out_of_range_for("uint256_t"));
   BOOST_CHECK_EXCEPTION(variant{hex_prefix + "1" + std::string(64, '0')}.as_uint256(), fc::parse_error_exception,
                         out_of_range_for("uint256_t"));
   // 258 bits of octal.
   BOOST_CHECK_EXCEPTION(variant{"0" + std::string(86, '7')}.as_uint256(), fc::parse_error_exception,
                         out_of_range_for("uint256_t"));
   BOOST_CHECK_THROW(variant{std::string{"+1"}}.as_uint256(), fc::exception);
   // Out of range and then a bad character: refused for either.
   BOOST_CHECK_THROW(variant{hex_prefix + std::string(64, 'f') + "eg"}.as_uint256(), fc::exception);

   BOOST_CHECK_EXCEPTION(variant{int64_t{-1}}.as<fc::uint256>(), fc::parse_error_exception,
                         out_of_range_for("uint256_t"));
   BOOST_CHECK_EXCEPTION(variant{uint256_past_max_text}.as<fc::uint256>(), fc::parse_error_exception,
                         out_of_range_for("uint256_t"));
}

BOOST_AUTO_TEST_CASE(as_uint256_range_error_names_the_value_and_the_range) {
   BOOST_CHECK_EXCEPTION(variant{int64_t{-1}}.as_uint256(), fc::parse_error_exception,
                         [](const fc::parse_error_exception& e) {
                            return e.to_detail_string().find("-1 is out of range for uint256_t [0, " +
                                                             uint256_max_text + "]") != std::string::npos;
                         });
}

BOOST_AUTO_TEST_CASE(as_int256_holds_its_whole_range) {
   // fc::int256 is sign-magnitude: it holds every uint256 and its negation.
   BOOST_CHECK_EQUAL(variant{int64_t{0}}.as_int256().str(), "0");
   BOOST_CHECK_EQUAL(variant{std::string{"0"}}.as_int256().str(), "0");
   BOOST_CHECK_EQUAL(variant{std::string{"-0"}}.as_int256().str(), "0");
   BOOST_CHECK_EQUAL(variant{int64_min}.as_int256().str(), std::to_string(int64_min));
   BOOST_CHECK_EQUAL(variant{int64_max}.as_int256().str(), std::to_string(int64_max));
   BOOST_CHECK_EQUAL(variant{uint64_max}.as_int256().str(), std::to_string(uint64_max));
   BOOST_CHECK_EQUAL(variant{fc::int256{uint256_max_text}}.as_int256().str(), uint256_max_text);
   BOOST_CHECK_EQUAL(variant{fc::int256{"-" + uint256_max_text}}.as_int256().str(), "-" + uint256_max_text);
   BOOST_CHECK_EQUAL(variant{fc::uint256{uint256_max_text}}.as_int256().str(), uint256_max_text);
   BOOST_CHECK_EQUAL(variant{uint256_max_text}.as_int256().str(), uint256_max_text);
   BOOST_CHECK_EQUAL(variant{"-" + uint256_max_text}.as_int256().str(), "-" + uint256_max_text);
   BOOST_CHECK_EQUAL(variant{hex_prefix + std::string(64, 'f')}.as_int256().str(), uint256_max_text);
   BOOST_CHECK_EQUAL(variant{std::string{"-0xff"}}.as_int256().str(), "-255");
}

BOOST_AUTO_TEST_CASE(as_int256_rejects_a_value_outside_its_range) {
   BOOST_CHECK_EXCEPTION(variant{uint256_past_max_text}.as_int256(), fc::parse_error_exception,
                         out_of_range_for("int256_t"));
   BOOST_CHECK_EXCEPTION(variant{"-" + uint256_past_max_text}.as_int256(), fc::parse_error_exception,
                         out_of_range_for("int256_t"));
   BOOST_CHECK_EXCEPTION(variant{uint256_max_text + "0"}.as_int256(), fc::parse_error_exception,
                         out_of_range_for("int256_t"));
   BOOST_CHECK_EXCEPTION(variant{"-" + uint256_max_text + "0"}.as_int256(), fc::parse_error_exception,
                         out_of_range_for("int256_t"));
   BOOST_CHECK_EXCEPTION(variant{hex_prefix + "1" + std::string(64, '0')}.as_int256(), fc::parse_error_exception,
                         out_of_range_for("int256_t"));
   BOOST_CHECK_EXCEPTION(variant{"0" + std::string(86, '7')}.as_int256(), fc::parse_error_exception,
                         out_of_range_for("int256_t"));
   BOOST_CHECK_THROW(variant{std::string{"+1"}}.as_int256(), std::exception);

   BOOST_CHECK_EXCEPTION(variant{uint256_past_max_text}.as<fc::int256>(), fc::parse_error_exception,
                         out_of_range_for("int256_t"));
}

BOOST_AUTO_TEST_CASE(as_int256_range_error_names_the_value_and_the_range) {
   BOOST_CHECK_EXCEPTION(variant{uint256_past_max_text}.as_int256(), fc::parse_error_exception,
                         [](const fc::parse_error_exception& e) {
                            const std::string message = uint256_past_max_text + " is out of range for int256_t [-" +
                                                        uint256_max_text + ", " + uint256_max_text + "]";
                            return e.to_detail_string().find(message) != std::string::npos;
                         });
}

BOOST_AUTO_TEST_CASE(as_double_from_each_compatible_source) {
   BOOST_CHECK_EQUAL(variant{3.5}.as_double(), 3.5);
   BOOST_CHECK_EQUAL(variant{int64_t{2}}.as_double(), 2.0);
   BOOST_CHECK_EQUAL(variant{uint64_t{3}}.as_double(), 3.0);
   BOOST_CHECK_EQUAL(variant{true}.as_double(), 1.0);
   BOOST_CHECK_EQUAL(variant{}.as_double(), 0.0);
   BOOST_CHECK_CLOSE(variant{std::string{"1.5"}}.as_double(), 1.5, 1e-9);
}

BOOST_AUTO_TEST_CASE(as_double_throws_on_object) {
   variant obj{variant_object{mutable_variant_object()}};
   BOOST_CHECK_THROW(obj.as_double(), fc::bad_cast_exception);
}

BOOST_AUTO_TEST_CASE(as_bool_string_only_accepts_true_and_false) {
   BOOST_CHECK_EQUAL(variant{std::string{"true"}}.as_bool(), true);
   BOOST_CHECK_EQUAL(variant{std::string{"false"}}.as_bool(), false);
   BOOST_CHECK_THROW(variant{std::string{"yes"}}.as_bool(), fc::bad_cast_exception);
   BOOST_CHECK_THROW(variant{std::string{"1"}}.as_bool(), fc::bad_cast_exception);
}

BOOST_AUTO_TEST_CASE(as_bool_numeric_truthiness) {
   BOOST_CHECK_EQUAL(variant{int64_t{0}}.as_bool(), false);
   BOOST_CHECK_EQUAL(variant{int64_t{1}}.as_bool(), true);
   BOOST_CHECK_EQUAL(variant{uint64_t{0}}.as_bool(), false);
   BOOST_CHECK_EQUAL(variant{uint64_t{42}}.as_bool(), true);
   BOOST_CHECK_EQUAL(variant{double{0.0}}.as_bool(), false);
   BOOST_CHECK_EQUAL(variant{double{0.5}}.as_bool(), true);
   BOOST_CHECK_EQUAL(variant{}.as_bool(), false);
}

BOOST_AUTO_TEST_CASE(as_bool_throws_on_object_or_array) {
   variant obj{variant_object{mutable_variant_object()}};
   variant arr{variants{}};
   BOOST_CHECK_THROW(obj.as_bool(), fc::bad_cast_exception);
   BOOST_CHECK_THROW(arr.as_bool(), fc::bad_cast_exception);
}

BOOST_AUTO_TEST_CASE(as_string_round_trips_double_full_precision) {
   const double pi = 3.141592653589793;
   variant v{pi};
   const std::string s = v.as_string();

   double round_tripped = std::stod(s);
   BOOST_CHECK_EQUAL(round_tripped, pi);
}

BOOST_AUTO_TEST_CASE(as_string_for_each_primitive) {
   BOOST_CHECK_EQUAL(variant{int64_t{-1}}.as_string(), "-1");
   BOOST_CHECK_EQUAL(variant{uint64_t{2}}.as_string(), "2");
   BOOST_CHECK_EQUAL(variant{true}.as_string(), "true");
   BOOST_CHECK_EQUAL(variant{false}.as_string(), "false");
   BOOST_CHECK_EQUAL(variant{}.as_string(), "");
}

BOOST_AUTO_TEST_CASE(as_string_throws_on_object_or_array) {
   variant obj{variant_object{mutable_variant_object()}};
   variant arr{variants{}};
   BOOST_CHECK_THROW(obj.as_string(), fc::bad_cast_exception);
   BOOST_CHECK_THROW(arr.as_string(), fc::bad_cast_exception);
}

BOOST_AUTO_TEST_CASE(as_blob_from_string_uses_base64_then_raw_fallback) {
   variant v_b64{std::string{"YWJj"}}; // base64("abc")
   blob b = v_b64.as_blob();
   BOOST_CHECK_EQUAL(b.data.size(), 3u);
   BOOST_CHECK_EQUAL(b.data[0], 'a');

   // A non-base64 string round-trips into the raw chars.
   variant v_raw{std::string{"hi!?"}};
   blob b_raw = v_raw.as_blob();
   BOOST_CHECK_EQUAL(b_raw.data.size(), 4u);
}

BOOST_AUTO_TEST_CASE(as_blob_from_blob_returns_same_data) {
   variant v{blob{{'a', 'b'}}};
   blob b = v.as_blob();
   BOOST_CHECK_EQUAL(b.data.size(), 2u);
   BOOST_CHECK_EQUAL(b.data[0], 'a');
}

BOOST_AUTO_TEST_CASE(as_blob_from_null_is_empty) {
   variant v;
   BOOST_CHECK_EQUAL(v.as_blob().data.size(), 0u);
}

BOOST_AUTO_TEST_CASE(as_blob_throws_on_object_or_array) {
   variant obj{variant_object{mutable_variant_object()}};
   variant arr{variants{}};
   BOOST_CHECK_THROW(obj.as_blob(), fc::bad_cast_exception);
   BOOST_CHECK_THROW(arr.as_blob(), fc::bad_cast_exception);
}

BOOST_AUTO_TEST_CASE(get_string_throws_on_non_string) {
   variant v{int64_t{1}};
   BOOST_CHECK_THROW(v.get_string(), fc::bad_cast_exception);
}

BOOST_AUTO_TEST_CASE(get_array_and_get_object_throw_on_wrong_type) {
   variant s{std::string{"x"}};
   BOOST_CHECK_THROW(s.get_array(), fc::bad_cast_exception);
   BOOST_CHECK_THROW(s.get_object(), fc::bad_cast_exception);
}

BOOST_AUTO_TEST_SUITE_END()

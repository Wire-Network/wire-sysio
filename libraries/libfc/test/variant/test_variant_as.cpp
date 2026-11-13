#include <fc/variant.hpp>
#include <fc/variant_multiprecision.hpp>
#include <fc/variant_object.hpp>
#include <fc/exception/exception.hpp>
#include <fc/int128.hpp>
#include <fc/int256.hpp>
#include <fc/io/json.hpp>
#include <fc/io/varint.hpp>
#include <fc/reflect/variant.hpp>
#include <fc/safe.hpp>
#include <fc/time.hpp>

#include <boost/test/unit_test.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

using fc::variant;
using fc::variant_object;
using fc::mutable_variant_object;
using fc::variants;
using fc::blob;

namespace {
constexpr int64_t  int64_min  = std::numeric_limits<int64_t>::min();
constexpr int64_t  int64_max  = std::numeric_limits<int64_t>::max();
constexpr uint64_t uint64_max = std::numeric_limits<uint64_t>::max();
} // namespace

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

BOOST_AUTO_TEST_CASE(as_uint64_holds_its_whole_range) {
   BOOST_CHECK_EQUAL(variant{uint64_t{0}}.as_uint64(), 0u);
   BOOST_CHECK_EQUAL(variant{uint64_max}.as_uint64(), uint64_max);
   BOOST_CHECK_EQUAL(variant{int64_t{0}}.as_uint64(), 0u);
   BOOST_CHECK_EQUAL(variant{int64_max}.as_uint64(), static_cast<uint64_t>(int64_max));
}

// A negative integer is outside the range of uint64_t. It used to convert to its two's complement.
BOOST_AUTO_TEST_CASE(as_uint64_throws_on_a_negative_integer) {
   BOOST_CHECK_THROW(variant{int64_t{-1}}.as_uint64(), fc::parse_error_exception); // 2^64 - 1
   BOOST_CHECK_THROW(variant{int64_min}.as_uint64(), fc::parse_error_exception);   // 2^63
}

BOOST_AUTO_TEST_CASE(as_int64_holds_its_whole_range) {
   BOOST_CHECK_EQUAL(variant{int64_min}.as_int64(), int64_min);
   BOOST_CHECK_EQUAL(variant{int64_max}.as_int64(), int64_max);
   BOOST_CHECK_EQUAL(variant{uint64_t{0}}.as_int64(), 0);
   BOOST_CHECK_EQUAL(variant{static_cast<uint64_t>(int64_max)}.as_int64(), int64_max);
}

// A uint64 above the maximum of int64_t used to convert to a negative number.
BOOST_AUTO_TEST_CASE(as_int64_throws_on_a_uint64_above_its_maximum) {
   BOOST_CHECK_THROW(variant{static_cast<uint64_t>(int64_max) + 1}.as_int64(), fc::parse_error_exception); // -2^63
   BOOST_CHECK_THROW(variant{uint64_max}.as_int64(), fc::parse_error_exception);                           // -1
}

// The longer strings are held on the heap and the shorter ones in place; each is parsed the same way.
BOOST_AUTO_TEST_CASE(as_uint64_from_a_string) {
   BOOST_CHECK_EQUAL(variant{std::string{"0"}}.as_uint64(), 0u);
   BOOST_CHECK_EQUAL(variant{std::string{"18446744073709551615"}}.as_uint64(), uint64_max);
   BOOST_CHECK_EQUAL(variant{std::string{"+5"}}.as_uint64(), 5u);
   BOOST_CHECK_EQUAL(variant{std::string{"+18446744073709551615"}}.as_uint64(), uint64_max);
   BOOST_CHECK_EQUAL(variant{std::string{"-0"}}.as_uint64(), 0u);

   BOOST_CHECK_THROW(variant{std::string{"-1"}}.as_uint64(), fc::parse_error_exception);                    // 2^64 - 1
   BOOST_CHECK_THROW(variant{std::string{"-18446744073709551361"}}.as_uint64(), fc::parse_error_exception); // 255
   BOOST_CHECK_THROW(variant{std::string{"18446744073709551616"}}.as_uint64(), fc::parse_error_exception);
   BOOST_CHECK_THROW(variant{std::string{"1.5"}}.as_uint64(), fc::parse_error_exception);
   BOOST_CHECK_THROW(variant{std::string{""}}.as_uint64(), fc::parse_error_exception);
}

BOOST_AUTO_TEST_CASE(as_int64_from_a_string) {
   BOOST_CHECK_EQUAL(variant{std::string{"-9223372036854775808"}}.as_int64(), int64_min);
   BOOST_CHECK_EQUAL(variant{std::string{"9223372036854775807"}}.as_int64(), int64_max);
   BOOST_CHECK_EQUAL(variant{std::string{"+5"}}.as_int64(), 5);
   BOOST_CHECK_EQUAL(variant{std::string{"-0"}}.as_int64(), 0);

   BOOST_CHECK_THROW(variant{std::string{"9223372036854775808"}}.as_int64(), fc::parse_error_exception);
   BOOST_CHECK_THROW(variant{std::string{"18446744073709551615"}}.as_int64(), fc::parse_error_exception);
   BOOST_CHECK_THROW(variant{std::string{"-9223372036854775809"}}.as_int64(), fc::parse_error_exception);
   BOOST_CHECK_THROW(variant{std::string{"1.5"}}.as_int64(), fc::parse_error_exception);
}

// A double truncates toward zero and is then held to the range. Converting one outside it was undefined behaviour.
BOOST_AUTO_TEST_CASE(as_uint64_from_a_double) {
   BOOST_CHECK_EQUAL(variant{0.0}.as_uint64(), 0u);
   BOOST_CHECK_EQUAL(variant{-0.0}.as_uint64(), 0u);
   BOOST_CHECK_EQUAL(variant{-0.9}.as_uint64(), 0u);
   BOOST_CHECK_EQUAL(variant{1.9}.as_uint64(), 1u);
   BOOST_CHECK_EQUAL(variant{0x1p63}.as_uint64(), uint64_t{1} << 63);
   BOOST_CHECK_EQUAL(variant{std::nextafter(0x1p64, 0.0)}.as_uint64(), 18446744073709549568u); // the largest below 2^64

   BOOST_CHECK_THROW(variant{-1.0}.as_uint64(), fc::parse_error_exception);
   BOOST_CHECK_THROW(variant{0x1p64}.as_uint64(), fc::parse_error_exception);
   BOOST_CHECK_THROW(variant{1e30}.as_uint64(), fc::parse_error_exception);
   BOOST_CHECK_THROW(variant{-1e30}.as_uint64(), fc::parse_error_exception);
   BOOST_CHECK_THROW(variant{std::numeric_limits<double>::quiet_NaN()}.as_uint64(), fc::parse_error_exception);
   BOOST_CHECK_THROW(variant{std::numeric_limits<double>::infinity()}.as_uint64(), fc::parse_error_exception);
   BOOST_CHECK_THROW(variant{-std::numeric_limits<double>::infinity()}.as_uint64(), fc::parse_error_exception);
}

BOOST_AUTO_TEST_CASE(as_int64_from_a_double) {
   BOOST_CHECK_EQUAL(variant{-0.0}.as_int64(), 0);
   BOOST_CHECK_EQUAL(variant{-0.9}.as_int64(), 0);
   BOOST_CHECK_EQUAL(variant{-1.9}.as_int64(), -1);
   BOOST_CHECK_EQUAL(variant{-0x1p63}.as_int64(), int64_min);
   BOOST_CHECK_EQUAL(variant{std::nextafter(0x1p63, 0.0)}.as_int64(), 9223372036854774784); // the largest below 2^63

   BOOST_CHECK_THROW(variant{0x1p63}.as_int64(), fc::parse_error_exception);
   BOOST_CHECK_THROW(variant{std::nextafter(-0x1p63, -0x1p64)}.as_int64(), fc::parse_error_exception);
   BOOST_CHECK_THROW(variant{1e30}.as_int64(), fc::parse_error_exception);
   BOOST_CHECK_THROW(variant{-1e30}.as_int64(), fc::parse_error_exception);
   BOOST_CHECK_THROW(variant{std::numeric_limits<double>::quiet_NaN()}.as_int64(), fc::parse_error_exception);
   BOOST_CHECK_THROW(variant{std::numeric_limits<double>::infinity()}.as_int64(), fc::parse_error_exception);
   BOOST_CHECK_THROW(variant{-std::numeric_limits<double>::infinity()}.as_int64(), fc::parse_error_exception);
}

BOOST_AUTO_TEST_CASE(out_of_range_message_names_the_value_and_the_range) {
   const auto message_has = [](const std::string& text) {
      return [text](const fc::exception& e) { return e.to_detail_string().find(text) != std::string::npos; };
   };
   BOOST_CHECK_EXCEPTION(variant{int64_t{-1}}.as_uint64(), fc::parse_error_exception,
                         message_has("-1 is out of range for uint64_t [0, 18446744073709551615]"));
   BOOST_CHECK_EXCEPTION(
      variant{uint64_max}.as_int64(), fc::parse_error_exception,
      message_has("18446744073709551615 is out of range for int64_t [-9223372036854775808, 9223372036854775807]"));
   BOOST_CHECK_EXCEPTION(variant{1e30}.as_uint64(), fc::parse_error_exception,
                         message_has("1e+30 is out of range for uint64_t [0, 18446744073709551615]"));
}

// Every integer from_variant reads through as_int64() or as_uint64().
BOOST_AUTO_TEST_CASE(integer_from_variant_checks_the_64_bit_range) {
   const variant negative{int64_t{-1}};
   const variant huge{uint64_max};

   BOOST_CHECK_EQUAL(huge.as<uint64_t>(), uint64_max);
   BOOST_CHECK_EQUAL(variant{int64_min}.as<int64_t>(), int64_min);
   BOOST_CHECK_THROW(negative.as<uint64_t>(), fc::parse_error_exception);
   BOOST_CHECK_THROW(huge.as<int64_t>(), fc::parse_error_exception);

   BOOST_CHECK_THROW(negative.as<uint32_t>(), fc::parse_error_exception);
   BOOST_CHECK_THROW(negative.as<uint16_t>(), fc::parse_error_exception);
   BOOST_CHECK_THROW(negative.as<uint8_t>(), fc::parse_error_exception);
   BOOST_CHECK_THROW(huge.as<int32_t>(), fc::parse_error_exception);
   BOOST_CHECK_THROW(huge.as<int16_t>(), fc::parse_error_exception);
   BOOST_CHECK_THROW(huge.as<int8_t>(), fc::parse_error_exception);

   BOOST_CHECK_THROW(negative.as<fc::unsigned_int>(), fc::parse_error_exception);
   BOOST_CHECK_THROW(huge.as<fc::signed_int>(), fc::parse_error_exception);
   BOOST_CHECK_THROW(negative.as<fc::UInt<64>>(), fc::parse_error_exception);
   BOOST_CHECK_THROW(huge.as<fc::microseconds>(), fc::parse_error_exception);
   BOOST_CHECK_THROW(huge.as<variant::type_id>(), fc::parse_error_exception); // a reflected enum
   BOOST_CHECK_THROW(huge.as_enum_value<variant::type_id>(), fc::parse_error_exception);
   BOOST_CHECK_THROW(negative.as<std::optional<uint64_t>>(), fc::parse_error_exception);
   BOOST_CHECK_THROW((variant{variants{variant{uint64_t{1}}, negative}}.as<std::vector<uint64_t>>()),
                     fc::parse_error_exception);
   BOOST_CHECK_THROW((variant{variants{variant{int64_t{1}}, huge}}.as<std::vector<int64_t>>()),
                     fc::parse_error_exception);
}

BOOST_AUTO_TEST_CASE(safe_from_variant_reads_its_own_integer_type) {
   variant packed;
   fc::to_variant(fc::safe<int64_t>{-5}, packed);
   BOOST_CHECK_EQUAL(packed.as<fc::safe<int64_t>>().value, -5);
   BOOST_CHECK_EQUAL(variant{int64_min}.as<fc::safe<int64_t>>().value, int64_min);
   BOOST_CHECK_EQUAL(variant{uint64_max}.as<fc::safe<uint64_t>>().value, uint64_max);
   BOOST_CHECK_THROW(variant{int64_t{-1}}.as<fc::safe<uint64_t>>(), fc::parse_error_exception);
   BOOST_CHECK_THROW(variant{uint64_max}.as<fc::safe<int64_t>>(), fc::parse_error_exception);
}

// A JSON integer parses to a uint64, or to an int64 when it has a minus sign; one with a fraction to a double.
BOOST_AUTO_TEST_CASE(json_number_checks_the_64_bit_range) {
   BOOST_CHECK_EQUAL(fc::json::from_string("18446744073709551615").as<uint64_t>(), uint64_max);
   BOOST_CHECK_EQUAL(fc::json::from_string("-9223372036854775808").as<int64_t>(), int64_min);
   BOOST_CHECK_EQUAL(fc::json::from_string("-0").as<uint64_t>(), 0u);
   BOOST_CHECK_EQUAL(fc::json::from_string("-0.5").as<uint64_t>(), 0u);

   BOOST_CHECK_THROW(fc::json::from_string("-1").as<uint64_t>(), fc::parse_error_exception);
   BOOST_CHECK_THROW(fc::json::from_string("18446744073709551615").as<int64_t>(), fc::parse_error_exception);
   BOOST_CHECK_THROW(fc::json::from_string("-1.0").as<uint64_t>(), fc::parse_error_exception);
   BOOST_CHECK_THROW(fc::json::from_string("1000000000000000000000000000000.0").as<uint64_t>(),
                     fc::parse_error_exception);
   BOOST_CHECK_THROW(fc::json::from_string("1000000000000000000000000000000.0").as<int64_t>(),
                     fc::parse_error_exception);
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

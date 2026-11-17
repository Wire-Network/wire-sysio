#include <fc/exception/exception.hpp>
#include <fc/int128.hpp>
#include <fc/reflect/reflect.hpp>
#include <fc/reflect/variant.hpp>
#include <fc/variant.hpp>
#include <fc/variant_object.hpp>

#include <boost/mpl/list.hpp>
#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

using fc::mutable_variant_object;
using fc::variant;
using fc::variant_object;
using fc::variants;

namespace variant_integer_range_test {

/// The integer types narrower than the 64 bits a variant stores an integer in.
using narrow_integer_types = boost::mpl::list<uint8_t, int8_t, uint16_t, int16_t, uint32_t, int32_t>;
using narrow_unsigned_types = boost::mpl::list<uint8_t, uint16_t, uint32_t>;
using narrow_signed_types = boost::mpl::list<int8_t, int16_t, int32_t>;

/// A reflected struct, the shape request parameters and config objects are read through.
struct narrow_fields {
   uint8_t  u8  = 0;
   int16_t  i16 = 0;
   uint32_t u32 = 0;
};

/// `v` converted to T, widened so a failed check prints a number rather than a character.
template<typename T>
int64_t converted(const variant& v) {
   return int64_t{v.as<T>()};
}

/// 2^bits(T): adding it to a value leaves the low bits(T) bits, so an unchecked narrowing returns the value unchanged.
template<typename T>
constexpr int64_t modulus = int64_t{1} << std::numeric_limits<std::make_unsigned_t<T>>::digits;

} // namespace variant_integer_range_test

FC_REFLECT(variant_integer_range_test::narrow_fields, (u8)(i16)(u32))

using namespace variant_integer_range_test;

BOOST_AUTO_TEST_SUITE(variant_integer_range_suite)

BOOST_AUTO_TEST_CASE_TEMPLATE(boundaries_convert_from_every_numeric_source, T, narrow_integer_types) {
   constexpr int64_t min = std::numeric_limits<T>::min();
   constexpr int64_t max = std::numeric_limits<T>::max();

   BOOST_CHECK_EQUAL(converted<T>(variant{min}), min);
   BOOST_CHECK_EQUAL(converted<T>(variant{max}), max);
   BOOST_CHECK_EQUAL(converted<T>(variant{static_cast<uint64_t>(max)}), max);
   BOOST_CHECK_EQUAL(converted<T>(variant{uint64_t{0}}), 0);
   BOOST_CHECK_EQUAL(converted<T>(variant{static_cast<double>(min)}), min);
   BOOST_CHECK_EQUAL(converted<T>(variant{static_cast<double>(max)}), max);
   BOOST_CHECK_EQUAL(converted<T>(variant{std::to_string(min)}), min);
   BOOST_CHECK_EQUAL(converted<T>(variant{std::to_string(max)}), max);

   // A string too long for the variant to hold inline.
   BOOST_CHECK_EQUAL(converted<T>(variant{std::string{"00000000000000000042"}}), 42);
}

BOOST_AUTO_TEST_CASE_TEMPLATE(one_past_each_boundary_throws, T, narrow_integer_types) {
   constexpr int64_t below = int64_t{std::numeric_limits<T>::min()} - 1;
   constexpr int64_t above = int64_t{std::numeric_limits<T>::max()} + 1;

   BOOST_CHECK_THROW(variant{below}.as<T>(), fc::parse_error_exception);
   BOOST_CHECK_THROW(variant{above}.as<T>(), fc::parse_error_exception);
   BOOST_CHECK_THROW(variant{static_cast<uint64_t>(above)}.as<T>(), fc::parse_error_exception);
   BOOST_CHECK_THROW(variant{static_cast<double>(below)}.as<T>(), fc::parse_error_exception);
   BOOST_CHECK_THROW(variant{static_cast<double>(above)}.as<T>(), fc::parse_error_exception);
   BOOST_CHECK_THROW(variant{std::to_string(below)}.as<T>(), fc::parse_error_exception);
   BOOST_CHECK_THROW(variant{std::to_string(above)}.as<T>(), fc::parse_error_exception);
}

// Values an unchecked narrowing maps back into range: whole multiples of 2^bits(T) away from an in-range value, and
// the extremes of the 64-bit types.
BOOST_AUTO_TEST_CASE_TEMPLATE(large_values_throw_instead_of_wrapping, T, narrow_integer_types) {
   constexpr int64_t min = std::numeric_limits<T>::min();
   constexpr int64_t max = std::numeric_limits<T>::max();

   for( const int64_t in_range : {min, int64_t{0}, int64_t{4}, max} ) {
      for( const int64_t wraps : {in_range + modulus<T>, in_range - modulus<T>, in_range + 1000 * modulus<T>} ) {
         BOOST_TEST_CONTEXT(wraps << " wraps to " << in_range) {
            BOOST_CHECK_THROW(variant{wraps}.as<T>(), fc::parse_error_exception);
            BOOST_CHECK_THROW(variant{std::to_string(wraps)}.as<T>(), fc::parse_error_exception);
            BOOST_CHECK_THROW(variant{static_cast<double>(wraps)}.as<T>(), fc::parse_error_exception);
            if( wraps > 0 )
               BOOST_CHECK_THROW(variant{static_cast<uint64_t>(wraps)}.as<T>(), fc::parse_error_exception);
         }
      }
   }

   constexpr int64_t  int64_min  = std::numeric_limits<int64_t>::min();
   constexpr int64_t  int64_max  = std::numeric_limits<int64_t>::max();
   constexpr uint64_t uint64_max = std::numeric_limits<uint64_t>::max();
   BOOST_CHECK_THROW(variant{int64_min}.as<T>(), fc::parse_error_exception);
   BOOST_CHECK_THROW(variant{int64_max}.as<T>(), fc::parse_error_exception);
   BOOST_CHECK_THROW(variant{uint64_max}.as<T>(), fc::parse_error_exception);
   BOOST_CHECK_THROW(variant{uint64_t{1} << 63}.as<T>(), fc::parse_error_exception);
   BOOST_CHECK_THROW(variant{std::to_string(int64_min)}.as<T>(), fc::parse_error_exception);
   BOOST_CHECK_THROW(variant{std::to_string(int64_max)}.as<T>(), fc::parse_error_exception);
   BOOST_CHECK_THROW(variant{std::to_string(uint64_max)}.as<T>(), fc::parse_error_exception);
}

BOOST_AUTO_TEST_CASE_TEMPLATE(negative_into_unsigned_throws, T, narrow_unsigned_types) {
   constexpr uint64_t max = std::numeric_limits<T>::max();

   BOOST_CHECK_THROW(variant{int64_t{-1}}.as<T>(), fc::parse_error_exception);
   BOOST_CHECK_THROW(variant{-1.0}.as<T>(), fc::parse_error_exception);
   BOOST_CHECK_THROW(variant{std::string{"-1"}}.as<T>(), fc::parse_error_exception);

   // Parsed as an unsigned 64-bit value, "-n" is 2^64 - n: this one is the type's maximum.
   const std::string negative_of_max_complement = "-" + std::to_string(std::numeric_limits<uint64_t>::max() - max + 1);
   BOOST_CHECK_THROW(variant{negative_of_max_complement}.as<T>(), fc::parse_error_exception);

   // Zero is in range however it is signed.
   BOOST_CHECK_EQUAL(converted<T>(variant{std::string{"-0"}}), 0);
   BOOST_CHECK_EQUAL(converted<T>(variant{-0.0}), 0);
}

// A uint64 above the int64 maximum must not reach a signed type through its two's-complement value.
BOOST_AUTO_TEST_CASE_TEMPLATE(uint64_above_int64_max_into_signed_throws, T, narrow_signed_types) {
   constexpr uint64_t uint64_max = std::numeric_limits<uint64_t>::max();

   // As int64 these are -1 and the type's minimum.
   BOOST_CHECK_THROW(variant{uint64_max}.as<T>(), fc::parse_error_exception);
   BOOST_CHECK_THROW(variant{uint64_max - std::numeric_limits<T>::max()}.as<T>(), fc::parse_error_exception);
   BOOST_CHECK_THROW(variant{std::to_string(uint64_max)}.as<T>(), fc::parse_error_exception);
}

// A double converts by truncating toward zero, as it does for the 64-bit types; the truncated value is what is
// range-checked.
BOOST_AUTO_TEST_CASE_TEMPLATE(double_truncates_then_range_checks, T, narrow_integer_types) {
   constexpr int64_t min = std::numeric_limits<T>::min();
   constexpr int64_t max = std::numeric_limits<T>::max();

   BOOST_CHECK_EQUAL(converted<T>(variant{42.9}), 42);
   BOOST_CHECK_EQUAL(converted<T>(variant{static_cast<double>(max) + 0.5}), max);
   BOOST_CHECK_EQUAL(converted<T>(variant{static_cast<double>(min) - 0.5}), min);

   BOOST_CHECK_THROW(variant{static_cast<double>(max) + 1.0}.as<T>(), fc::parse_error_exception);
   BOOST_CHECK_THROW(variant{static_cast<double>(min) - 1.0}.as<T>(), fc::parse_error_exception);
   BOOST_CHECK_THROW(variant{1e30}.as<T>(), fc::parse_error_exception);
   BOOST_CHECK_THROW(variant{-1e30}.as<T>(), fc::parse_error_exception);
   BOOST_CHECK_THROW(variant{std::numeric_limits<double>::infinity()}.as<T>(), fc::parse_error_exception);
   BOOST_CHECK_THROW(variant{-std::numeric_limits<double>::infinity()}.as<T>(), fc::parse_error_exception);
   BOOST_CHECK_THROW(variant{std::numeric_limits<double>::quiet_NaN()}.as<T>(), fc::parse_error_exception);
}

BOOST_AUTO_TEST_CASE_TEMPLATE(non_integer_strings_throw, T, narrow_integer_types) {
   for( const std::string text : {"", "-", "abc", "1.5", "1e2", "0x10", " 1", "1 ", "12abc"} ) {
      BOOST_TEST_CONTEXT("\"" << text << "\"") {
         BOOST_CHECK_THROW(variant{text}.as<T>(), fc::parse_error_exception);
      }
   }
}

// What is not a number converts as it does for the 64-bit types.
BOOST_AUTO_TEST_CASE_TEMPLATE(non_numeric_sources_are_unchanged, T, narrow_integer_types) {
   BOOST_CHECK_EQUAL(converted<T>(variant{true}), 1);
   BOOST_CHECK_EQUAL(converted<T>(variant{false}), 0);
   BOOST_CHECK_EQUAL(converted<T>(variant{}), 0);

   BOOST_CHECK_THROW(variant{variants{}}.as<T>(), fc::bad_cast_exception);
   BOOST_CHECK_THROW(variant{variant_object{mutable_variant_object()}}.as<T>(), fc::bad_cast_exception);
   BOOST_CHECK_THROW(variant{fc::blob{{'a'}}}.as<T>(), fc::bad_cast_exception);
   BOOST_CHECK_THROW(variant{fc::int128{1}}.as<T>(), fc::bad_cast_exception);
   BOOST_CHECK_THROW(variant{fc::uint128{1}}.as<T>(), fc::bad_cast_exception);
}

BOOST_AUTO_TEST_CASE(out_of_range_message_names_the_value_and_the_range) {
   try {
      variant{uint64_t{260}}.as<uint8_t>();
      BOOST_FAIL("260 converted to uint8_t");
   } catch( const fc::parse_error_exception& e ) {
      BOOST_CHECK_EQUAL(e.top_message(), "260 is out of range for uint8_t [0, 255]");
   }
   try {
      variant{int64_t{-129}}.as<int8_t>();
      BOOST_FAIL("-129 converted to int8_t");
   } catch( const fc::parse_error_exception& e ) {
      BOOST_CHECK_EQUAL(e.top_message(), "-129 is out of range for int8_t [-128, 127]");
   }
}

// The containers and reflected structs that convert element by element reject an out-of-range element.
BOOST_AUTO_TEST_CASE(containers_and_reflected_structs_range_check_their_elements) {
   const variant in_range{variants{variant{uint64_t{0}}, variant{uint64_t{255}}}};
   const std::vector<uint8_t> bytes = in_range.as<std::vector<uint8_t>>();
   BOOST_REQUIRE_EQUAL(bytes.size(), 2u);
   BOOST_CHECK_EQUAL(int64_t{bytes[0]}, 0);
   BOOST_CHECK_EQUAL(int64_t{bytes[1]}, 255);

   const variant one_out_of_range{variants{variant{uint64_t{0}}, variant{uint64_t{256}}}};
   BOOST_CHECK_THROW(one_out_of_range.as<std::vector<uint8_t>>(), fc::parse_error_exception);

   BOOST_CHECK(!variant{}.as<std::optional<uint16_t>>().has_value());
   BOOST_CHECK_EQUAL(variant{uint64_t{65535}}.as<std::optional<uint16_t>>().value(), 65535);
   BOOST_CHECK_THROW(variant{uint64_t{65536}}.as<std::optional<uint16_t>>(), fc::parse_error_exception);

   const auto fields = [](int64_t u8, int64_t i16, int64_t u32) {
      return variant{mutable_variant_object()("u8", u8)("i16", i16)("u32", u32)};
   };
   const narrow_fields accepted = fields(255, -32768, 4294967295).as<narrow_fields>();
   BOOST_CHECK_EQUAL(int64_t{accepted.u8}, 255);
   BOOST_CHECK_EQUAL(accepted.i16, -32768);
   BOOST_CHECK_EQUAL(accepted.u32, 4294967295u);

   BOOST_CHECK_THROW(fields(256, 0, 0).as<narrow_fields>(), fc::parse_error_exception);
   BOOST_CHECK_THROW(fields(0, 32768, 0).as<narrow_fields>(), fc::parse_error_exception);
   BOOST_CHECK_THROW(fields(0, 0, 4294967296).as<narrow_fields>(), fc::parse_error_exception);
   BOOST_CHECK_THROW(fields(0, 0, -1).as<narrow_fields>(), fc::parse_error_exception);
}

BOOST_AUTO_TEST_SUITE_END()

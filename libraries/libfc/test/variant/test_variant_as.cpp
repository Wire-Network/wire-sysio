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

#include <boost/test/unit_test.hpp>

#include <fc/io/json.hpp>
#include <fc/int128.hpp>
#include <fc/int256.hpp>
#include <fc/variant.hpp>
#include <fc/variant_object.hpp>

#include <limits>

using namespace fc;

BOOST_AUTO_TEST_SUITE(json_variant_test_suite)

BOOST_AUTO_TEST_CASE(variant_object_from_raw_json) {
   std::string raw = R"({"name":"alice","address":"0xABCD1234"})";

   variant v = json::from_string(raw);
   BOOST_REQUIRE(v.is_object());

   auto obj = v.get_object();
   BOOST_CHECK_EQUAL(obj.size(), 2u);
   BOOST_CHECK_EQUAL(obj["name"].as_string(), "alice");
   BOOST_CHECK_EQUAL(obj["address"].as_string(), "0xABCD1234");
}

BOOST_AUTO_TEST_CASE(variant_object_roundtrip) {
   mutable_variant_object mvo;
   mvo("name", "alice");
   mvo("balance", 100);
   mvo("active", true);

   variant v(mvo);
   std::string json_str = json::to_pretty_string(v);

   variant parsed = json::from_string(json_str);
   BOOST_REQUIRE(parsed.is_object());

   auto obj = parsed.get_object();
   BOOST_CHECK_EQUAL(obj["name"].as_string(), "alice");
   BOOST_CHECK_EQUAL(obj["balance"].as_int64(), 100);
   BOOST_CHECK_EQUAL(obj["active"].as<bool>(), true);
}

BOOST_AUTO_TEST_CASE(map_serializes_as_array_of_pairs) {
   // std::map serializes as [["key","value"],...]
   std::map<std::string, std::string> m = {{"a", "1"}, {"b", "2"}};

   variant v;
   to_variant(m, v);
   BOOST_REQUIRE(v.is_array());
   BOOST_CHECK_EQUAL(v.size(), 2u);

   for (const auto& e : v.get_array()) {
      BOOST_CHECK_EQUAL(e.size(), 2u);
      if (e[size_t(0)].as_string() == "a")
         BOOST_CHECK_EQUAL(e[size_t(1)].as_string(), "1");
      else if (e[size_t(0)].as_string() == "b")
         BOOST_CHECK_EQUAL(e[size_t(1)].as_string(), "2");
   }

   // Each pair serializes as a 2-element array: [["a","1"],["b","2"]]
   auto deadline = fc::time_point::maximum();
   std::string json_str = json::to_string(v, deadline);
   BOOST_CHECK_EQUAL(json_str, R"([["a","1"],["b","2"]])");
}

BOOST_AUTO_TEST_CASE(nested_object_from_raw_json) {
   std::string raw = R"({"user":{"name":"bob","id":42},"tags":["admin","active"]})";

   variant v = json::from_string(raw);
   BOOST_REQUIRE(v.is_object());

   auto obj = v.get_object();
   BOOST_REQUIRE(obj["user"].is_object());
   BOOST_CHECK_EQUAL(obj["user"]["name"].as_string(), "bob");
   BOOST_CHECK_EQUAL(obj["user"]["id"].as_int64(), 42);

   BOOST_REQUIRE(obj["tags"].is_array());
   BOOST_CHECK_EQUAL(obj["tags"].size(), 2u);
   BOOST_CHECK_EQUAL(obj["tags"][size_t(0)].as_string(), "admin");
   BOOST_CHECK_EQUAL(obj["tags"][size_t(1)].as_string(), "active");
}

BOOST_AUTO_TEST_CASE(mutable_variant_object_merge) {
   mutable_variant_object a;
   a("x", 1)("y", 2);

   mutable_variant_object b;
   b("y", 99)("z", 3);

   // Merging b into a overwrites "y"
   a(b);
   variant v(a);
   auto obj = v.get_object();
   BOOST_CHECK_EQUAL(obj["x"].as_int64(), 1);
   BOOST_CHECK_EQUAL(obj["y"].as_int64(), 99);
   BOOST_CHECK_EQUAL(obj["z"].as_int64(), 3);
}

BOOST_AUTO_TEST_CASE(variant_type_checks) {
   BOOST_CHECK(variant(42).is_integer());
   BOOST_CHECK(variant("hello").is_string());
   BOOST_CHECK(variant(true).is_bool());
   BOOST_CHECK(variant(3.14).is_double());
   BOOST_CHECK(variant().is_null());

   variants arr;
   arr.push_back(variant(1));
   arr.push_back(variant(2));
   BOOST_CHECK(variant(arr).is_array());

   BOOST_CHECK(variant(mutable_variant_object()).is_object());
}

BOOST_AUTO_TEST_CASE(variant_object_missing_key) {
   std::string raw = R"({"only_key":"value"})";
   variant v = json::from_string(raw);
   BOOST_REQUIRE(v.is_object());
   auto obj = v.get_object();

   // operator[] throws key_not_found_exception for missing keys
   BOOST_CHECK_THROW(obj["missing"], fc::key_not_found_exception);
   BOOST_CHECK(!obj.contains("missing"));
   BOOST_CHECK(obj.contains("only_key"));
}

BOOST_AUTO_TEST_CASE(empty_object_and_array) {
   variant empty_obj = json::from_string("{}");
   BOOST_REQUIRE(empty_obj.is_object());
   BOOST_CHECK_EQUAL(empty_obj.get_object().size(), 0u);

   variant empty_arr = json::from_string("[]");
   BOOST_REQUIRE(empty_arr.is_array());
   BOOST_CHECK_EQUAL(empty_arr.size(), 0u);
}

BOOST_AUTO_TEST_CASE(variant_numeric_conversions) {
   std::string raw = R"({"i":"-100","u":"200","d":"3.14"})";
   variant v = json::from_string(raw);
   auto obj = v.get_object();

   // String values can be converted to numeric types
   BOOST_CHECK_EQUAL(obj["i"].as_int64(), -100);
   BOOST_CHECK_EQUAL(obj["u"].as_uint64(), 200u);
   BOOST_CHECK_CLOSE(obj["d"].as_double(), 3.14, 0.001);
}

// ---------------------------------------------------------------------------
// number_from_stream - negative integer type boundaries
//
// The parser strips the minus sign and leading zeros, leaving the absolute
// value string (str). Routing is binary (no int128 variant):
//   str.size() < 19  OR  (size==19 AND str <= "9223372036854775808") -> int64
//   str.size() > 78  OR  (size==78 AND str >  int256_max_str)        -> throws
//                                                                       (exceeds int256 magnitude)
//   otherwise -> int256
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(number_from_stream_negative_small) {
   variant v = json::from_string("-1");
   BOOST_CHECK(v.is_int64());
   BOOST_CHECK_EQUAL(v.as_int64(), -1LL);
}

BOOST_AUTO_TEST_CASE(number_from_stream_negative_int64_max) {
   // -INT64_MAX (abs one less than the threshold) -> int64
   variant v = json::from_string("-9223372036854775807");
   BOOST_CHECK(v.is_int64());
   BOOST_CHECK_EQUAL(v.as_int64(), -9223372036854775807LL);
}

BOOST_AUTO_TEST_CASE(number_from_stream_negative_int64_min) {
   // INT64_MIN = -9223372036854775808 (abs exactly equal to threshold) -> int64
   variant v = json::from_string("-9223372036854775808");
   BOOST_CHECK(v.is_int64());
   BOOST_CHECK_EQUAL(v.as_int64(), std::numeric_limits<int64_t>::min());
}

BOOST_AUTO_TEST_CASE(number_from_stream_negative_int64_min_minus_one) {
   // INT64_MIN - 1 = -9223372036854775809 (abs one past threshold) -> int256
   variant v = json::from_string("-9223372036854775809");
   BOOST_CHECK(v.is_int256());
}

BOOST_AUTO_TEST_CASE(number_from_stream_negative_near_int256_max) {
   // 39-digit value well within int256 magnitude -> int256
   variant v = json::from_string("-170141183460469231731687303715884105727");
   BOOST_CHECK(v.is_int256());
}

BOOST_AUTO_TEST_CASE(number_from_stream_negative_int256_min) {
   // INT256_MIN magnitude = 2^255 = 77 digits -> int256
   variant v = json::from_string("-57896044618658097711785492504343953926634992332820282019728792003956564819968");
   BOOST_CHECK(v.is_int256());
}

BOOST_AUTO_TEST_CASE(number_from_stream_negative_exceeds_int256_throws) {
   // 78 digits, first digit >= 6 -> magnitude > INT256_MIN magnitude -> throws
   BOOST_CHECK_THROW(json::from_string("-60000000000000000000000000000000000000000000000000000000000000000000000000000"),
                     fc::parse_error_exception);
}

BOOST_AUTO_TEST_CASE(number_from_stream_negative_length_ceiling_throws) {
   // 79-digit magnitude exceeds any int256 value -> throws
   BOOST_CHECK_THROW(json::from_string("-1000000000000000000000000000000000000000000000000000000000000000000000000000000"),
                     fc::parse_error_exception);
}

// ---------------------------------------------------------------------------
// number_from_stream - positive integer type boundaries
//
// Routing is binary (no uint128 variant):
//   str.size() < 20  OR  (size==20 AND str <= "18446744073709551615") -> uint64
//   str.size() > 78  OR  (size==78 AND str >  uint256_max_str)        -> throws
//                                                                       (exceeds uint256)
//   otherwise -> uint256
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(number_from_stream_positive_small) {
   variant v = json::from_string("1");
   BOOST_CHECK(v.is_uint64());
   BOOST_CHECK_EQUAL(v.as_uint64(), 1ull);
}

BOOST_AUTO_TEST_CASE(number_from_stream_positive_uint64_max) {
   // UINT64_MAX = 18446744073709551615  (exactly at threshold) → uint64
   const auto max = std::numeric_limits<uint64_t>::max();
   const auto str = std::to_string(max);
   variant v = json::from_string(str);
   BOOST_CHECK(v.is_uint64());
   BOOST_CHECK_EQUAL(v.as_uint64(), max);
}

BOOST_AUTO_TEST_CASE(number_from_stream_positive_uint64_max_plus_one) {
   // UINT64_MAX + 1 = 18446744073709551616 (one past threshold) -> uint256
   fc::uint256 max_plus = fc::uint256(std::numeric_limits<uint64_t>::max()) + 1;
   variant v = json::from_string(max_plus.str());
   BOOST_CHECK(v.is_uint256());
   BOOST_CHECK_EQUAL(v.as_uint256(), max_plus);
}

BOOST_AUTO_TEST_CASE(number_from_stream_positive_far_past_uint64) {
   // 39-digit value well within uint256 -> uint256
   const auto max = std::numeric_limits<uint128_t>::max();
   variant v = json::from_string(fc::to_string(max));
   BOOST_CHECK(v.is_uint256());
}

BOOST_AUTO_TEST_CASE(number_from_stream_positive_uint256_max) {
   // UINT256_MAX = 2^256 - 1 = 78 digits, exactly at the throwing boundary
   variant v = json::from_string("115792089237316195423570985008687907853269984665640564039457584007913129639935");
   BOOST_CHECK(v.is_uint256());
}

BOOST_AUTO_TEST_CASE(number_from_stream_positive_uint256_max_plus_one_throws) {
   // UINT256_MAX + 1 = 2^256 -> throws, same length but lexicographically greater than max
   BOOST_CHECK_THROW(json::from_string("115792089237316195423570985008687907853269984665640564039457584007913129639936"),
                     fc::parse_error_exception);
}

BOOST_AUTO_TEST_CASE(number_from_stream_positive_length_ceiling_throws) {
   // 79-digit input -> throws regardless of value
   BOOST_CHECK_THROW(json::from_string("9999999999999999999999999999999999999999999999999999999999999999999999999999999"),
                     fc::parse_error_exception);
}

// ---------------------------------------------------------------------------
// number_from_stream - zero-handling / leading-zero edge cases
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(number_from_stream_plain_zero) {
   variant v = json::from_string("0");
   BOOST_CHECK(v.is_uint64());
   BOOST_CHECK_EQUAL(v.as_uint64(), 0ull);
}

BOOST_AUTO_TEST_CASE(number_from_stream_double_zero) {
   variant v = json::from_string("00");
   BOOST_CHECK(v.is_uint64());
   BOOST_CHECK_EQUAL(v.as_uint64(), 0ull);
}

BOOST_AUTO_TEST_CASE(number_from_stream_negative_zero) {
   variant v = json::from_string("-0");
   BOOST_CHECK(v.is_uint64());
   BOOST_CHECK_EQUAL(v.as_uint64(), 0ull);
}

BOOST_AUTO_TEST_CASE(number_from_stream_many_zeros) {
   // 30 zeros: all digits stripped, should parse as uint64(0), not promote to int256.
   variant v = json::from_string("000000000000000000000000000000");
   BOOST_CHECK(v.is_uint64());
   BOOST_CHECK_EQUAL(v.as_uint64(), 0ull);
}

BOOST_AUTO_TEST_CASE(number_from_stream_leading_zeros_fit_uint64) {
   // Leading zeros stripped before length check -> should route to uint64
   variant v = json::from_string("0000000000000000000000042");
   BOOST_CHECK(v.is_uint64());
   BOOST_CHECK_EQUAL(v.as_uint64(), 42ull);
}

BOOST_AUTO_TEST_CASE(number_from_stream_negative_leading_zeros_fit_int64) {
   // Leading zeros stripped before length check -> should route to uint64
   variant v = json::from_string("-0000000000000000000000042");
   BOOST_CHECK(v.is_int64());
   BOOST_CHECK_EQUAL(v.as_int64(), -42ll);
}

BOOST_AUTO_TEST_CASE(number_from_stream_bare_minus_throws) {
   BOOST_CHECK_THROW(json::from_string("-"), fc::parse_error_exception);
}

BOOST_AUTO_TEST_CASE(number_from_stream_bare_minus_in_array_throws) {
   BOOST_CHECK_THROW(json::from_string("[-,1]"), fc::parse_error_exception);
}

BOOST_AUTO_TEST_SUITE_END()

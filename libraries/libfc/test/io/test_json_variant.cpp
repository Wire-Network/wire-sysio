#include <boost/test/unit_test.hpp>

#include <fc/io/json.hpp>
#include <fc/variant.hpp>
#include <fc/variant_object.hpp>

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

BOOST_AUTO_TEST_SUITE_END()
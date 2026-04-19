#include <boost/test/unit_test.hpp>

#include <fc/reflect/json_stream.hpp>

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace {

struct point_t {
   int32_t     x = 0;
   int32_t     y = 0;
   std::string label;
};

struct nested_t {
   std::string           name;
   std::vector<int32_t>  values;
   std::optional<int32_t> maybe;
   point_t               where;
};

enum class color_t { red, green, blue };

struct with_optional_t {
   int32_t                  id = 0;
   std::optional<std::string> note;
};

struct with_map_t {
   std::map<std::string, int32_t> counts;
};

} // namespace

FC_REFLECT(point_t, (x)(y)(label))
FC_REFLECT(nested_t, (name)(values)(maybe)(where))
FC_REFLECT_ENUM(color_t, (red)(green)(blue))
FC_REFLECT(with_optional_t, (id)(note))
FC_REFLECT(with_map_t, (counts))

BOOST_AUTO_TEST_SUITE(json_stream_test)

BOOST_AUTO_TEST_CASE(primitives) {
   // Scalars serialize the same way fc::json::to_string(variant(v)) would.
   BOOST_CHECK_EQUAL(fc::to_json_string(true),         "true");
   BOOST_CHECK_EQUAL(fc::to_json_string(false),        "false");
   BOOST_CHECK_EQUAL(fc::to_json_string(int32_t{0}),   "0");
   BOOST_CHECK_EQUAL(fc::to_json_string(int32_t{-7}),  "-7");
   BOOST_CHECK_EQUAL(fc::to_json_string(uint64_t{42}), "42");
   BOOST_CHECK_EQUAL(fc::to_json_string(std::string{"hi"}), "\"hi\"");
}

BOOST_AUTO_TEST_CASE(string_escaping) {
   // Control characters must be JSON-escaped.
   BOOST_CHECK_EQUAL(fc::to_json_string(std::string{"a\"b"}),  "\"a\\\"b\"");
   BOOST_CHECK_EQUAL(fc::to_json_string(std::string{"a\nb"}),  "\"a\\nb\"");
   BOOST_CHECK_EQUAL(fc::to_json_string(std::string{"\t"}),     "\"\\t\"");
}

BOOST_AUTO_TEST_CASE(vector_and_optional) {
   std::vector<int32_t> v{1, 2, 3};
   BOOST_CHECK_EQUAL(fc::to_json_string(v), "[1,2,3]");

   std::optional<int32_t> unset;
   BOOST_CHECK_EQUAL(fc::to_json_string(unset), "null");

   std::optional<int32_t> set{7};
   BOOST_CHECK_EQUAL(fc::to_json_string(set), "7");
}

BOOST_AUTO_TEST_CASE(reflected_struct) {
   point_t p{.x = 3, .y = -4, .label = "origin"};
   BOOST_CHECK_EQUAL(fc::to_json_string(p), "{\"x\":3,\"y\":-4,\"label\":\"origin\"}");
}

BOOST_AUTO_TEST_CASE(nested_struct) {
   nested_t n{
      .name   = "n",
      .values = {1, 2},
      .maybe  = std::nullopt,
      .where  = {.x = 0, .y = 1, .label = "p"}
   };
   // `maybe` is std::nullopt so it's omitted per to_variant_visitor::add semantics.
   BOOST_CHECK_EQUAL(
      fc::to_json_string(n),
      "{\"name\":\"n\",\"values\":[1,2],\"where\":{\"x\":0,\"y\":1,\"label\":\"p\"}}");
}

BOOST_AUTO_TEST_CASE(nested_optional_present) {
   nested_t n{
      .name   = "n",
      .values = {},
      .maybe  = 5,
      .where  = {.x = 0, .y = 0, .label = ""}
   };
   BOOST_CHECK_EQUAL(
      fc::to_json_string(n),
      "{\"name\":\"n\",\"values\":[],\"maybe\":5,\"where\":{\"x\":0,\"y\":0,\"label\":\"\"}}");
}

BOOST_AUTO_TEST_CASE(enum_value) {
   // FC_REFLECT_ENUM emits the enum name via reflector::to_fc_string.
   BOOST_CHECK_EQUAL(fc::to_json_string(color_t::red),   "\"red\"");
   BOOST_CHECK_EQUAL(fc::to_json_string(color_t::green), "\"green\"");
}

BOOST_AUTO_TEST_CASE(optional_field_omitted) {
   // Unset optional field on a reflected struct is omitted entirely.
   with_optional_t o{.id = 1, .note = std::nullopt};
   BOOST_CHECK_EQUAL(fc::to_json_string(o), "{\"id\":1}");
}

BOOST_AUTO_TEST_CASE(map_object_emission) {
   with_map_t m;
   m.counts["a"] = 1;
   m.counts["b"] = 2;
   // std::map iterates in sorted order so output is deterministic.
   BOOST_CHECK_EQUAL(fc::to_json_string(m), "{\"counts\":{\"a\":1,\"b\":2}}");
}

BOOST_AUTO_TEST_CASE(raw_value_embeds_fragment) {
   // raw_value should splice a pre-serialized JSON fragment at a value position.
   std::string out;
   {
      fc::json_writer w(out);
      w.begin_object();
      w.key("a");
      w.value_int32(1);
      w.key("b");
      w.raw_value("{\"x\":42}");
      w.end_object();
   }
   BOOST_CHECK_EQUAL(out, "{\"a\":1,\"b\":{\"x\":42}}");
}

BOOST_AUTO_TEST_CASE(array_of_reflected) {
   std::vector<point_t> pts{
      {.x = 1, .y = 2, .label = "a"},
      {.x = 3, .y = 4, .label = "b"},
   };
   BOOST_CHECK_EQUAL(
      fc::to_json_string(pts),
      "[{\"x\":1,\"y\":2,\"label\":\"a\"},{\"x\":3,\"y\":4,\"label\":\"b\"}]");
}

BOOST_AUTO_TEST_SUITE_END()

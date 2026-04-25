#include <fc/variant.hpp>
#include <fc/variant_object.hpp>

#include <boost/test/unit_test.hpp>

#include <string>

using fc::variant;
using fc::variant_object;
using fc::mutable_variant_object;
using fc::variants;
using fc::blob;

BOOST_AUTO_TEST_SUITE(variant_assign_suite)

BOOST_AUTO_TEST_CASE(self_assign_copy) {
   variant v{std::string{"x"}};
   v = static_cast<const variant&>(v);
   BOOST_CHECK(v.is_string());
   BOOST_CHECK_EQUAL(v.get_string(), "x");
}

BOOST_AUTO_TEST_CASE(self_assign_move) {
   variant v{std::string{"y"}};
   v = std::move(v);
   BOOST_CHECK(v.is_string());
   BOOST_CHECK_EQUAL(v.get_string(), "y");
}

BOOST_AUTO_TEST_CASE(copy_assign_clears_existing_heap_object) {
   variant a{std::string{"first"}};
   variant b{int64_t{7}};

   a = b;
   BOOST_CHECK(a.is_int64());
   BOOST_CHECK_EQUAL(a.as_int64(), 7);
}

BOOST_AUTO_TEST_CASE(move_assign_leaves_source_null) {
   variant a{std::string{"alpha"}};
   variant b;

   b = std::move(a);
   BOOST_CHECK(a.is_null());
   BOOST_CHECK(b.is_string());
   BOOST_CHECK_EQUAL(b.get_string(), "alpha");
}

BOOST_AUTO_TEST_CASE(move_assign_over_existing_heap_object) {
   variant a{std::string{"new"}};
   variant b{std::string{"old"}};

   b = std::move(a);
   BOOST_CHECK(a.is_null());
   BOOST_CHECK_EQUAL(b.get_string(), "new");
}

BOOST_AUTO_TEST_CASE(template_assign_from_t) {
   variant v;
   v = std::string{"templated"};
   BOOST_CHECK(v.is_string());
   BOOST_CHECK_EQUAL(v.get_string(), "templated");

   v = int64_t{55};
   BOOST_CHECK(v.is_int64());
   BOOST_CHECK_EQUAL(v.as_int64(), 55);
}

BOOST_AUTO_TEST_CASE(cross_type_assign_object_to_array) {
   variant v{variant_object{mutable_variant_object("k", 1)}};
   BOOST_CHECK(v.is_object());

   v = variants{variant{int64_t{1}}, variant{int64_t{2}}};
   BOOST_CHECK(v.is_array());
   BOOST_CHECK_EQUAL(v.size(), 2u);
}

BOOST_AUTO_TEST_CASE(cross_type_assign_array_to_blob) {
   variant v{variants{variant{int64_t{1}}}};
   BOOST_CHECK(v.is_array());

   v = blob{{'q'}};
   BOOST_CHECK(v.is_blob());
   BOOST_CHECK_EQUAL(v.get_blob().data[0], 'q');
}

BOOST_AUTO_TEST_CASE(cross_type_assign_string_to_null) {
   variant v{std::string{"x"}};
   v = variant{};
   BOOST_CHECK(v.is_null());
}

BOOST_AUTO_TEST_CASE(same_type_string_reassign) {
   // Documents current behaviour: same-type reassignment goes through
   // clear() (delete) + new (allocate).  Phase B item 5 may change this
   // to reuse the existing heap object; this test will need updating
   // there but should still pass observationally.
   variant v{std::string{"first"}};
   v = std::string{"second"};
   BOOST_CHECK_EQUAL(v.get_string(), "second");
}

BOOST_AUTO_TEST_CASE(same_type_object_reassign) {
   variant v{variant_object{mutable_variant_object("k", 1)}};
   v = variant_object{mutable_variant_object("k", 2)};
   BOOST_CHECK_EQUAL(v.get_object()["k"].as_int64(), 2);
}

BOOST_AUTO_TEST_SUITE_END()

#include <fc/variant.hpp>
#include <fc/variant_object.hpp>
#include <fc/int128.hpp>
#include <fc/int256.hpp>

#include <boost/test/unit_test.hpp>

#include <string>

using fc::variant;
using fc::variant_object;
using fc::mutable_variant_object;
using fc::variants;
using fc::blob;

namespace {

struct recording_visitor : public variant::visitor {
   mutable variant::type_id last = variant::null_type;
   mutable int call_count = 0;

   void note(variant::type_id t) const {
      last = t;
      ++call_count;
   }

   void handle() const override                              { note(variant::null_type); }
   void handle(const int64_t&) const override                { note(variant::int64_type); }
   void handle(const uint64_t&) const override               { note(variant::uint64_type); }
   void handle(const fc::int128_t&) const override           { note(variant::int128_type); }
   void handle(const fc::uint128_t&) const override          { note(variant::uint128_type); }
   void handle(const fc::int256_t&) const override           { note(variant::int256_type); }
   void handle(const fc::uint256_t&) const override          { note(variant::uint256_type); }
   void handle(const double&) const override                 { note(variant::double_type); }
   void handle(const bool&) const override                   { note(variant::bool_type); }
   void handle(const std::string&) const override            { note(variant::string_type); }
   void handle(const variant_object&) const override         { note(variant::object_type); }
   void handle(const variants&) const override               { note(variant::array_type); }
   void handle(const blob&) const override                   { note(variant::blob_type); }
};

void check_visit(const variant& v, variant::type_id expected) {
   recording_visitor rv;
   v.visit(rv);
   BOOST_CHECK_EQUAL(rv.last, expected);
   BOOST_CHECK_EQUAL(rv.call_count, 1);
}

} // namespace

BOOST_AUTO_TEST_SUITE(variant_visitor_suite)

BOOST_AUTO_TEST_CASE(dispatch_null) {
   check_visit(variant{}, variant::null_type);
}

BOOST_AUTO_TEST_CASE(dispatch_int64) {
   check_visit(variant{int64_t{-1}}, variant::int64_type);
}

BOOST_AUTO_TEST_CASE(dispatch_uint64) {
   check_visit(variant{uint64_t{1}}, variant::uint64_type);
}

// int128 / uint128 / int256 / uint256 are stored as heap-resident
// std::string (their decimal representation); variant::visit() for these
// type tags calls handle(const std::string&), NOT the typed
// handle(int128_t&) / handle(uint128_t&) / handle(int256_t&) /
// handle(uint256_t&) overloads.  Those typed handlers are declared on
// the visitor interface but never reachable through visit().  The tests
// below document the as-observed dispatch.

BOOST_AUTO_TEST_CASE(dispatch_int128_routes_to_string_handler) {
   check_visit(variant{fc::int128{-1}}, variant::string_type);
}

BOOST_AUTO_TEST_CASE(dispatch_uint128_routes_to_string_handler) {
   check_visit(variant{fc::uint128{1}}, variant::string_type);
}

BOOST_AUTO_TEST_CASE(dispatch_int256_routes_to_string_handler) {
   check_visit(variant{fc::int256(-1)}, variant::string_type);
}

BOOST_AUTO_TEST_CASE(dispatch_uint256_routes_to_string_handler) {
   check_visit(variant{fc::uint256(1)}, variant::string_type);
}

BOOST_AUTO_TEST_CASE(dispatch_double) {
   check_visit(variant{1.5}, variant::double_type);
}

BOOST_AUTO_TEST_CASE(dispatch_bool) {
   check_visit(variant{true}, variant::bool_type);
}

BOOST_AUTO_TEST_CASE(dispatch_string) {
   check_visit(variant{std::string{"x"}}, variant::string_type);
}

BOOST_AUTO_TEST_CASE(dispatch_array) {
   check_visit(variant{variants{}}, variant::array_type);
}

BOOST_AUTO_TEST_CASE(dispatch_object) {
   check_visit(variant{variant_object{mutable_variant_object()}}, variant::object_type);
}

BOOST_AUTO_TEST_CASE(dispatch_blob) {
   check_visit(variant{blob{}}, variant::blob_type);
}

BOOST_AUTO_TEST_SUITE_END()

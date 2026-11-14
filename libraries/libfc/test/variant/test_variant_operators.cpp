#include <fc/variant.hpp>
#include <fc/variant_object.hpp>
#include <fc/exception/exception.hpp>

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <limits>
#include <string>

using fc::variant;
using fc::variant_object;
using fc::mutable_variant_object;
using fc::variants;

BOOST_AUTO_TEST_SUITE(variant_operators_suite)

// Note: arithmetic operators (+/-/*//) on fc::variant are `= delete`
// (see libraries/libfc/include/fc/variant.hpp).  Any caller that
// attempts `var1 - var2` will fail at compile time, so there is
// nothing to test for them here.

BOOST_AUTO_TEST_CASE(equality_int64) {
   BOOST_CHECK(variant{int64_t{1}} == variant{int64_t{1}});
   BOOST_CHECK(variant{int64_t{1}} != variant{int64_t{2}});
}

BOOST_AUTO_TEST_CASE(equality_uint64) {
   BOOST_CHECK(variant{uint64_t{5}} == variant{uint64_t{5}});
   BOOST_CHECK(variant{uint64_t{5}} != variant{uint64_t{6}});
}

BOOST_AUTO_TEST_CASE(equality_double) {
   BOOST_CHECK(variant{1.5} == variant{1.5});
   BOOST_CHECK(variant{1.5} != variant{1.6});
}

BOOST_AUTO_TEST_CASE(equality_string) {
   BOOST_CHECK(variant{std::string{"x"}} == variant{std::string{"x"}});
   BOOST_CHECK(variant{std::string{"x"}} != variant{std::string{"y"}});
}

BOOST_AUTO_TEST_CASE(equality_array) {
   variant a{variants{variant{int64_t{1}}, variant{int64_t{2}}}};
   variant b{variants{variant{int64_t{1}}, variant{int64_t{2}}}};
   variant c{variants{variant{int64_t{1}}, variant{int64_t{3}}}};

   BOOST_CHECK(a == b);
   BOOST_CHECK(a != c);
}

BOOST_AUTO_TEST_CASE(equality_cross_type_string_coerces) {
   // Documents current behaviour: when one side is string, both sides are
   // coerced to string before comparing.  variant{1} == variant{"1"} is
   // therefore true.
   BOOST_CHECK(variant{int64_t{1}} == variant{std::string{"1"}});
}

BOOST_AUTO_TEST_CASE(less_and_greater_int64) {
   BOOST_CHECK(variant{int64_t{1}} < variant{int64_t{2}});
   BOOST_CHECK(variant{int64_t{2}} > variant{int64_t{1}});
   BOOST_CHECK(!(variant{int64_t{2}} < variant{int64_t{2}}));
}

// An int64 and a uint64 compare by value. The uint64 used to be read as an int64, so 2^64 - 1 compared as -1.
BOOST_AUTO_TEST_CASE(int64_and_uint64_compare_by_value) {
   const variant negative{int64_t{-1}};
   const variant huge{std::numeric_limits<uint64_t>::max()};

   BOOST_CHECK(negative != huge);
   BOOST_CHECK(huge != negative);
   BOOST_CHECK(negative < huge);
   BOOST_CHECK(huge > negative);
   BOOST_CHECK(!(huge < negative));
   BOOST_CHECK(!(negative > huge));

   BOOST_CHECK(variant{int64_t{5}} == variant{uint64_t{5}});
   BOOST_CHECK(variant{uint64_t{5}} == variant{int64_t{5}});
   BOOST_CHECK(variant{int64_t{5}} < variant{uint64_t{1} << 63});
   BOOST_CHECK(variant{uint64_t{1} << 63} > variant{int64_t{5}});
   BOOST_CHECK(variant{std::numeric_limits<int64_t>::min()} < variant{uint64_t{0}});

   // A bool or null beside an int64 still compares as 0 or 1.
   BOOST_CHECK(variant{int64_t{1}} == variant{true});
   BOOST_CHECK(variant{int64_t{0}} == variant{});
   BOOST_CHECK(variant{int64_t{-1}} < variant{});
}

BOOST_AUTO_TEST_CASE(less_and_greater_string) {
   BOOST_CHECK(variant{std::string{"a"}} < variant{std::string{"b"}});
   BOOST_CHECK(variant{std::string{"b"}} > variant{std::string{"a"}});
}

BOOST_AUTO_TEST_CASE(less_throws_on_objects) {
   variant a{variant_object{mutable_variant_object("k", 1)}};
   variant b{variant_object{mutable_variant_object("k", 2)}};
   // Two object variants share neither string/double/int/uint -> hits
   // the FC_ASSERT(false, ...) trailing branch.
   auto compare = [&] { (void)(a < b); };
   BOOST_CHECK_THROW(compare(), fc::exception);
}

BOOST_AUTO_TEST_CASE(operator_not_negates_as_bool) {
   BOOST_CHECK(!variant{false});
   BOOST_CHECK(!variant{int64_t{0}});
   BOOST_CHECK(!variant{uint64_t{0}});
   BOOST_CHECK(!variant{0.0});
   BOOST_CHECK(!variant{});
   BOOST_CHECK(!(!variant{true}));
   BOOST_CHECK(!(!variant{int64_t{1}}));
}

BOOST_AUTO_TEST_SUITE_END()

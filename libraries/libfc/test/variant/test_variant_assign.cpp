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

// fc::variant operator=(const variant&) treats aliased rhs (rhs referring to storage owned by lhs, e.g.
// v = v.get_array()[i] / v = v.get_object()["k"]) as undefined behaviour, matching the previous clear()-then-new
// pattern.  Debug builds catch the common direct-aliasing cases via an assertion in variant.cpp; deeper nesting
// remains UB.  No tests exercise aliased self-assign here -- a test cannot pin UB.

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

// int128 / uint128 / int256 / uint256 store as new std::string(...) on the heap.
// These cases pin the leak fix in clear() and the deep-copy fix in the copy ctor;
// without both, ASAN reports a leak (clear()) or a use-after-free / double-free
// (copy ctor sharing the std::string* pointer).
BOOST_AUTO_TEST_CASE(int128_destructor_frees_heap_string) {
   { variant v{fc::int128{42}};  }       // ASAN-leak if clear() doesn't handle int128_type
   { variant v{fc::uint128{42}}; }       // same for uint128_type
   { variant v{fc::int256{42}};  }       // same for int256_type
   { variant v{fc::uint256{42}}; }       // same for uint256_type
}

BOOST_AUTO_TEST_CASE(int128_copy_ctor_deep_copies) {
   {
      variant a{fc::int128{-12345}};
      variant b(a);                         // shallow-copy bug -> shared pointer
      BOOST_CHECK_EQUAL(a.as_string(), b.as_string());
      // Both destructors run at scope exit; double-free if shared pointer.
   }
   {
      variant a{fc::uint128{0xABCDEF0123456789ULL}};
      variant b(a);
      BOOST_CHECK_EQUAL(a.as_string(), b.as_string());
   }
   {
      variant a{fc::int256{-9999}};
      variant b(a);
      BOOST_CHECK_EQUAL(a.as_string(), b.as_string());
   }
   {
      variant a{fc::uint256{0xDEADBEEFCAFEBABEULL}};
      variant b(a);
      BOOST_CHECK_EQUAL(a.as_string(), b.as_string());
   }
}

BOOST_AUTO_TEST_CASE(int128_op_assign_same_and_cross_type) {
   // Non-aliased same-type op= for the four std::string-backed multi-precision types: pins the same-type fast path's
   // heap-string reuse.  The aliased-self-assign variant of this case (lhs object containing rhs as an entry value)
   // is UB and intentionally not exercised here.
   {
      variant a{fc::int128{42}};
      a = variant{fc::int128{99}};
      BOOST_CHECK(a.get_type() == variant::int128_type);
      BOOST_CHECK_EQUAL(a.as_string(), "99");
   }
   {
      variant a{fc::uint128{1}};
      a = variant{fc::uint128{0xABCD}};
      BOOST_CHECK(a.get_type() == variant::uint128_type);
   }
   {
      variant a{fc::int256{-1}};
      a = variant{fc::int256{-12345}};
      BOOST_CHECK(a.get_type() == variant::int256_type);
      BOOST_CHECK_EQUAL(a.as_string(), "-12345");
   }
   {
      variant a{fc::uint256{0}};
      a = variant{fc::uint256{0xDEADBEEF}};
      BOOST_CHECK(a.get_type() == variant::uint256_type);
   }
   // Cross-type op= TO int128/etc exercises the clear()+new heap-string allocation.
   {
      variant a{int64_t{1}};
      a = variant{fc::uint256{0xABCDEF}};
      BOOST_CHECK(a.get_type() == variant::uint256_type);
   }
}

BOOST_AUTO_TEST_SUITE_END()

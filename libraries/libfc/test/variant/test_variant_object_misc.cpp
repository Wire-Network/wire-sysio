#include <fc/variant.hpp>
#include <fc/variant_object.hpp>
#include <fc/exception/exception.hpp>

#include <boost/test/unit_test.hpp>

#include <string>

using fc::variant;
using fc::variant_object;
using fc::mutable_variant_object;
using fc::variants;

BOOST_AUTO_TEST_SUITE(variant_object_misc_suite)

BOOST_AUTO_TEST_CASE(default_variant_object_is_empty) {
   variant_object vo;
   BOOST_CHECK_EQUAL(vo.size(), 0u);
   BOOST_CHECK(vo.begin() == vo.end());
   BOOST_CHECK(vo.find("anything") == vo.end());
   BOOST_CHECK(!vo.contains("anything"));
}

BOOST_AUTO_TEST_CASE(default_mutable_variant_object_is_empty) {
   mutable_variant_object mvo;
   BOOST_CHECK_EQUAL(mvo.size(), 0u);
   BOOST_CHECK(mvo.begin() == mvo.end());
}

BOOST_AUTO_TEST_CASE(insertion_order_is_preserved) {
   mutable_variant_object mvo;
   mvo("z", 1)("a", 2)("m", 3);

   variant_object vo{mvo};
   auto it = vo.begin();
   BOOST_CHECK_EQUAL(it->key(), "z");
   ++it;
   BOOST_CHECK_EQUAL(it->key(), "a");
   ++it;
   BOOST_CHECK_EQUAL(it->key(), "m");
}

BOOST_AUTO_TEST_CASE(operator_brackets_throws_on_missing_key) {
   variant_object vo{mutable_variant_object("only", 1)};
   BOOST_CHECK_THROW(vo["missing"], fc::key_not_found_exception);
}

BOOST_AUTO_TEST_CASE(contains_and_find_consistent) {
   variant_object vo{mutable_variant_object("a", 1)("b", 2)};
   BOOST_CHECK(vo.contains("a"));
   BOOST_CHECK(vo.contains("b"));
   BOOST_CHECK(!vo.contains("c"));
   BOOST_CHECK(vo.find("a") != vo.end());
   BOOST_CHECK(vo.find("c") == vo.end());
}

BOOST_AUTO_TEST_CASE(copy_then_modify_via_mvo_does_not_affect_original) {
   variant_object original{mutable_variant_object("k", 1)};
   variant_object copy = original;

   // Convert the copy back through mvo, mutate it, and rewrap.  The
   // original variant_object should not observe the change.
   mutable_variant_object mut{copy};
   mut("k", 999);
   variant_object new_copy{mut};

   BOOST_CHECK_EQUAL(original["k"].as_int64(), 1);
   BOOST_CHECK_EQUAL(new_copy["k"].as_int64(), 999);
}

BOOST_AUTO_TEST_CASE(mvo_set_replaces_existing_key) {
   mutable_variant_object mvo;
   mvo.set("k", variant{1});
   mvo.set("k", variant{2});

   BOOST_CHECK_EQUAL(mvo.size(), 1u);
   BOOST_CHECK_EQUAL(mvo["k"].as_int64(), 2);
}

BOOST_AUTO_TEST_CASE(mvo_op_paren_with_variant_appends_without_dedup) {
   // The non-template `operator()(std::string, variant)` overload is
   // documented to be append-only; duplicate keys are observable.  Note
   // that the template overload `operator()(std::string, T&&)` for
   // non-variant T routes through set(), which DOES dedup -- so
   // `mvo("k", 1)("k", 2)` collapses to one entry but
   // `mvo("k", variant{1})("k", variant{2})` keeps both.
   mutable_variant_object mvo;
   mvo("k", variant{1})("k", variant{2});
   BOOST_CHECK_EQUAL(mvo.size(), 2u);
}

BOOST_AUTO_TEST_CASE(mvo_op_paren_with_non_variant_dedups_via_set) {
   mutable_variant_object mvo;
   mvo("k", 1)("k", 2);
   BOOST_CHECK_EQUAL(mvo.size(), 1u);
   BOOST_CHECK_EQUAL(mvo["k"].as_int64(), 2);
}

BOOST_AUTO_TEST_CASE(mvo_op_brackets_inserts_default_on_miss) {
   mutable_variant_object mvo;
   variant& v = mvo["new_key"];
   BOOST_CHECK(v.is_null());
   BOOST_CHECK(mvo.contains("new_key"));
}

BOOST_AUTO_TEST_CASE(mvo_erase_present_and_absent) {
   mutable_variant_object mvo;
   mvo("a", 1)("b", 2);

   mvo.erase("a");
   BOOST_CHECK_EQUAL(mvo.size(), 1u);
   BOOST_CHECK(!mvo.contains("a"));

   // erase of an absent key is a silent no-op.
   mvo.erase("never_existed");
   BOOST_CHECK_EQUAL(mvo.size(), 1u);
}

BOOST_AUTO_TEST_CASE(mvo_merge_via_op_paren_dedups_through_set) {
   mutable_variant_object a;
   a("x", 1)("y", 2);

   mutable_variant_object b;
   b("y", 99)("z", 3);

   a(b);
   BOOST_CHECK_EQUAL(a.size(), 3u);
   BOOST_CHECK_EQUAL(a["y"].as_int64(), 99);
}

BOOST_AUTO_TEST_CASE(mvo_self_merge_is_noop) {
   mutable_variant_object mvo;
   mvo("k", 1);
   mvo(mvo);
   BOOST_CHECK_EQUAL(mvo.size(), 1u);
}

BOOST_AUTO_TEST_CASE(mvo_reserve_does_not_change_size) {
   mutable_variant_object mvo;
   mvo.reserve(64);
   BOOST_CHECK_EQUAL(mvo.size(), 0u);
}

BOOST_AUTO_TEST_CASE(entry_set_swaps_value) {
   mutable_variant_object mvo;
   mvo("k", 1);
   auto it = mvo.find("k");
   BOOST_REQUIRE(it != mvo.end());
   it->set(variant{42});
   BOOST_CHECK_EQUAL(mvo["k"].as_int64(), 42);
}

BOOST_AUTO_TEST_SUITE_END()

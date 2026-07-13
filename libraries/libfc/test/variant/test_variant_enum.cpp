#include <fc/variant.hpp>
#include <fc/variant_object.hpp>

#include <boost/test/unit_test.hpp>

#include <stdexcept>
#include <string>

using fc::variant;
using fc::variant_object;
using fc::mutable_variant_object;
using fc::variants;

namespace {

enum class color : int {
   red = 0,
   green = 1,
   blue = 2,
   negative = -1,
};

enum class chain_kind_like : uint8_t {
   none = 0,
   ethereum = 1,
   solana = 2,
};

} // namespace

BOOST_AUTO_TEST_SUITE(variant_enum_suite)

BOOST_AUTO_TEST_CASE(int_source_returns_enum_directly) {
   BOOST_CHECK(variant{int64_t{1}}.as_enum_value<color>() == color::green);
   BOOST_CHECK(variant{int64_t{2}}.as_enum_value<color>() == color::blue);
}

BOOST_AUTO_TEST_CASE(int_source_negative_for_signed_enum) {
   BOOST_CHECK(variant{int64_t{-1}}.as_enum_value<color>() == color::negative);
}

BOOST_AUTO_TEST_CASE(uint_source_returns_enum) {
   BOOST_CHECK(variant{uint64_t{1}}.as_enum_value<chain_kind_like>() == chain_kind_like::ethereum);
}

BOOST_AUTO_TEST_CASE(bool_source_returns_enum) {
   // bool is integer-shaped.  ABI serializer should never emit bool for
   // an enum field, but the contract is "any integer-or-numeric source",
   // so cover it.
   BOOST_CHECK(variant{true}.as_enum_value<color>() == color::green);
   BOOST_CHECK(variant{false}.as_enum_value<color>() == color::red);
}

BOOST_AUTO_TEST_CASE(string_source_with_valid_integer_text) {
   BOOST_CHECK(variant{std::string{"2"}}.as_enum_value<color>() == color::blue);
   BOOST_CHECK(variant{std::string{"-1"}}.as_enum_value<color>() == color::negative);
}

BOOST_AUTO_TEST_CASE(string_source_with_invalid_text_throws_runtime_error) {
   BOOST_CHECK_THROW(variant{std::string{"not_a_number"}}.as_enum_value<color>(),
                     std::runtime_error);
   BOOST_CHECK_THROW(variant{std::string{""}}.as_enum_value<color>(),
                     std::runtime_error);
}

BOOST_AUTO_TEST_CASE(string_source_with_trailing_garbage_accepted) {
   // "1abc" -- from_chars reads 1, stops at 'a', returns ec=std::errc{}.  The helper does not validate that the
   // entire string was consumed, matching the previous std::stoll behaviour (which silently ignored the suffix too).
   BOOST_CHECK(variant{std::string{"1abc"}}.as_enum_value<color>() == color::green);
}

BOOST_AUTO_TEST_CASE(string_source_with_leading_whitespace_throws) {
   // Stricter than the previous std::stoll-based code, which accepted leading whitespace and returned 1 for " 1".
   // std::from_chars rejects whitespace; the ABI serializer never emits such input.
   BOOST_CHECK_THROW(variant{std::string{" 1"}}.as_enum_value<color>(),
                     std::runtime_error);
   BOOST_CHECK_THROW(variant{std::string{"\t1"}}.as_enum_value<color>(),
                     std::runtime_error);
}

BOOST_AUTO_TEST_CASE(string_source_with_leading_plus_throws) {
   // Stricter than the previous std::stoll-based code, which accepted "+1" and returned 1.  std::from_chars rejects
   // a leading '+'; the ABI serializer never emits such input.
   BOOST_CHECK_THROW(variant{std::string{"+1"}}.as_enum_value<color>(),
                     std::runtime_error);
}

BOOST_AUTO_TEST_CASE(object_source_throws) {
   variant obj{variant_object{mutable_variant_object("k", 1)}};
   BOOST_CHECK_THROW(obj.as_enum_value<color>(), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(array_source_throws) {
   variant arr{variants{}};
   BOOST_CHECK_THROW(arr.as_enum_value<color>(), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(blob_source_throws) {
   variant b{fc::blob{}};
   BOOST_CHECK_THROW(b.as_enum_value<color>(), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(double_source_returns_enum_via_truncation) {
   // is_numeric() includes double; ::as_int64() truncates toward zero.
   // Cover the path so a future change does not silently break it.
   BOOST_CHECK(variant{2.9}.as_enum_value<color>() == color::blue);
}

BOOST_AUTO_TEST_SUITE_END()

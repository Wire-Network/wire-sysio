#include <fc/int128.hpp>
#include <fc/int256.hpp>
#include <fc/int256_fwd.hpp>
#include <fc/variant.hpp>
#include <fc/variant_multiprecision.hpp>
#include <fc/exception/exception.hpp>
#include <fc/io/datastream.hpp>

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <limits>
#include <vector>

using namespace fc;

namespace {
   constexpr unsigned __int128 UINT128_MAX_VAL = ~static_cast<unsigned __int128>(0);
   constexpr __int128           INT128_MAX_VAL = static_cast<__int128>(UINT128_MAX_VAL >> 1);
   constexpr __int128           INT128_MIN_VAL = -INT128_MAX_VAL - 1;
}

// ============================================================================
// fc::to_string / fc::*_from_string
// ============================================================================

BOOST_AUTO_TEST_SUITE(int128_tests)

BOOST_AUTO_TEST_CASE(uint128_to_string_values) {
   BOOST_CHECK_EQUAL(fc::to_string(static_cast<unsigned __int128>(0)), "0");
   BOOST_CHECK_EQUAL(fc::to_string(static_cast<unsigned __int128>(1)), "1");
   BOOST_CHECK_EQUAL(fc::to_string(static_cast<unsigned __int128>(999)), "999");
   BOOST_CHECK_EQUAL(fc::to_string(static_cast<unsigned __int128>(UINT64_MAX)),
                     "18446744073709551615");
   unsigned __int128 beyond64 = (static_cast<unsigned __int128>(1) << 64) | 1;
   BOOST_CHECK_EQUAL(fc::to_string(beyond64), "18446744073709551617");
   BOOST_CHECK_EQUAL(fc::to_string(UINT128_MAX_VAL),
                     "340282366920938463463374607431768211455");
}

BOOST_AUTO_TEST_CASE(int128_to_string_values) {
   BOOST_CHECK_EQUAL(fc::to_string(static_cast<__int128>(0)), "0");
   BOOST_CHECK_EQUAL(fc::to_string(static_cast<__int128>(1)), "1");
   BOOST_CHECK_EQUAL(fc::to_string(static_cast<__int128>(-1)), "-1");
   BOOST_CHECK_EQUAL(fc::to_string(static_cast<__int128>(-42)), "-42");
   BOOST_CHECK_EQUAL(fc::to_string(INT128_MAX_VAL),
                     "170141183460469231731687303715884105727");
   // INT128_MIN -- exercises the UB fix (negation in unsigned domain)
   BOOST_CHECK_EQUAL(fc::to_string(INT128_MIN_VAL),
                     "-170141183460469231731687303715884105728");
}

BOOST_AUTO_TEST_CASE(uint128_from_string_decimal) {
   BOOST_CHECK(fc::uint128_from_string("0") == 0);
   BOOST_CHECK(fc::uint128_from_string("1") == 1);
   BOOST_CHECK(fc::uint128_from_string("18446744073709551615") ==
               static_cast<unsigned __int128>(UINT64_MAX));
   BOOST_CHECK(fc::uint128_from_string("340282366920938463463374607431768211455") ==
               UINT128_MAX_VAL);
}

BOOST_AUTO_TEST_CASE(uint128_from_string_hex) {
   BOOST_CHECK(fc::uint128_from_string("0x0") == 0);
   BOOST_CHECK(fc::uint128_from_string("0x1") == 1);
   BOOST_CHECK(fc::uint128_from_string("0xff") == 255);
   BOOST_CHECK(fc::uint128_from_string("0xFF") == 255);
   BOOST_CHECK(fc::uint128_from_string("0XFF") == 255);
   BOOST_CHECK(fc::uint128_from_string("0xaBcDeF") == 0xABCDEF);
   BOOST_CHECK(fc::uint128_from_string("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF") ==
               UINT128_MAX_VAL);
}

BOOST_AUTO_TEST_CASE(uint128_from_string_invalid) {
   BOOST_CHECK_THROW(fc::uint128_from_string("xyz"), fc::exception);
   BOOST_CHECK_THROW(fc::uint128_from_string("0xGG"), fc::exception);
}

BOOST_AUTO_TEST_CASE(int128_from_string_values) {
   BOOST_CHECK(fc::int128_from_string("0") == 0);
   BOOST_CHECK(fc::int128_from_string("1") == 1);
   BOOST_CHECK(fc::int128_from_string("-1") == -1);
   BOOST_CHECK(fc::int128_from_string("-42") == -42);
   BOOST_CHECK(fc::int128_from_string("") == 0);
   BOOST_CHECK(fc::int128_from_string("170141183460469231731687303715884105727") ==
               INT128_MAX_VAL);
   // INT128_MIN -- exercises the UB fix (negation in unsigned domain)
   BOOST_CHECK(fc::int128_from_string("-170141183460469231731687303715884105728") ==
               INT128_MIN_VAL);
   BOOST_CHECK(fc::int128_from_string("-0xff") == -255);
}

BOOST_AUTO_TEST_CASE(uint128_string_roundtrip) {
   auto check = [](unsigned __int128 v) {
      BOOST_CHECK(fc::uint128_from_string(fc::to_string(v)) == v);
   };
   check(0);
   check(1);
   check(UINT64_MAX);
   check(static_cast<unsigned __int128>(UINT64_MAX) + 1);
   check(UINT128_MAX_VAL);
}

BOOST_AUTO_TEST_CASE(int128_string_roundtrip) {
   auto check = [](__int128 v) {
      BOOST_CHECK(fc::int128_from_string(fc::to_string(v)) == v);
   };
   check(0);
   check(1);
   check(-1);
   check(INT128_MAX_VAL);
   check(INT128_MIN_VAL); // exercises both to_string and from_string UB fixes
}

// ============================================================================
// fc::to_uint128 helper
// ============================================================================

BOOST_AUTO_TEST_CASE(to_uint128_hilo) {
   BOOST_CHECK(fc::to_uint128(0, 0) == 0);
   BOOST_CHECK(fc::to_uint128(0, 1) == 1);
   BOOST_CHECK(fc::to_uint128(0, UINT64_MAX) == UINT64_MAX);
   BOOST_CHECK(fc::to_uint128(1, 0) == (static_cast<unsigned __int128>(1) << 64));
   BOOST_CHECK(fc::to_uint128(UINT64_MAX, UINT64_MAX) == UINT128_MAX_VAL);
   unsigned __int128 expected = (static_cast<unsigned __int128>(0xDEADBEEFULL) << 64) | 0xCAFEBABEULL;
   BOOST_CHECK(fc::to_uint128(0xDEADBEEFULL, 0xCAFEBABEULL) == expected);
}

// ============================================================================
// fc::variant roundtrips for int128/uint128
// ============================================================================

BOOST_AUTO_TEST_CASE(variant_uint128_roundtrip) {
   auto check = [](unsigned __int128 v) {
      fc::variant var;
      fc::to_variant(v, var);
      unsigned __int128 result = 0;
      fc::from_variant(var, result);
      BOOST_CHECK(result == v);
   };
   check(0);
   check(1);
   check(UINT64_MAX);
   check(static_cast<unsigned __int128>(UINT64_MAX) + 1);
   check(UINT128_MAX_VAL);
}

BOOST_AUTO_TEST_CASE(variant_int128_roundtrip) {
   auto check = [](__int128 v) {
      fc::variant var;
      fc::to_variant(v, var);
      __int128 result = 0;
      fc::from_variant(var, result);
      BOOST_CHECK(result == v);
   };
   check(0);
   check(1);
   check(-1);
   check(INT128_MAX_VAL);
   check(INT128_MIN_VAL);
}

BOOST_AUTO_TEST_CASE(variant_uint128_type_id) {
   fc::variant var(static_cast<fc::uint128>(42));
   BOOST_CHECK(var.is_uint128());
   BOOST_CHECK(var.as_uint128() == 42);
}

BOOST_AUTO_TEST_CASE(variant_int128_type_id) {
   fc::variant var(static_cast<fc::int128>(-42));
   BOOST_CHECK(var.is_int128());
   BOOST_CHECK(var.as_int128() == -42);
}

BOOST_AUTO_TEST_CASE(from_variant_uint64_to_uint128) {
   fc::variant var(static_cast<uint64_t>(42));
   unsigned __int128 result = 0;
   fc::from_variant(var, result);
   BOOST_CHECK(result == 42);
}

BOOST_AUTO_TEST_CASE(from_variant_int64_to_uint128) {
   fc::variant var(static_cast<int64_t>(42));
   unsigned __int128 result = 0;
   fc::from_variant(var, result);
   BOOST_CHECK(result == 42);
}

BOOST_AUTO_TEST_CASE(from_variant_string_to_uint128) {
   fc::variant var(std::string("340282366920938463463374607431768211455"));
   unsigned __int128 result = 0;
   fc::from_variant(var, result);
   BOOST_CHECK(result == UINT128_MAX_VAL);
}

BOOST_AUTO_TEST_CASE(from_variant_string_to_int128) {
   fc::variant var(std::string("-170141183460469231731687303715884105728"));
   __int128 result = 0;
   fc::from_variant(var, result);
   BOOST_CHECK(result == INT128_MIN_VAL);
}

BOOST_AUTO_TEST_CASE(from_variant_invalid_type_throws) {
   fc::variant var(fc::variants{});
   unsigned __int128 uresult = 0;
   BOOST_CHECK_THROW(fc::from_variant(var, uresult), fc::bad_cast_exception);
   __int128 sresult = 0;
   BOOST_CHECK_THROW(fc::from_variant(var, sresult), fc::bad_cast_exception);
}

// ============================================================================
// Binary pack/unpack (fc::raw)
// ============================================================================

BOOST_AUTO_TEST_CASE(pack_unpack_uint128) {
   auto check = [](unsigned __int128 v) {
      std::vector<char> buf(sizeof(v));
      fc::datastream<char*> ds(buf.data(), buf.size());
      fc::raw::pack(ds, v);
      unsigned __int128 result = 0;
      fc::datastream<const char*> ds2(buf.data(), buf.size());
      fc::raw::unpack(ds2, result);
      BOOST_CHECK(result == v);
   };
   check(0);
   check(1);
   check(UINT64_MAX);
   check(UINT128_MAX_VAL);
   check((static_cast<unsigned __int128>(0xDEADBEEFCAFEBABEULL) << 64) | 0x0123456789ABCDEFULL);
}

// ============================================================================
// fc::int256 / fc::uint256 wrapper classes
// ============================================================================

BOOST_AUTO_TEST_CASE(uint256_construction_and_comparison) {
   fc::uint256 a;
   BOOST_CHECK(a == 0);

   fc::uint256 b(42);
   BOOST_CHECK(b == 42);
   BOOST_CHECK(b != 43);
   BOOST_CHECK(b < 100);
   BOOST_CHECK(b > 10);
   BOOST_CHECK(b <= 42);
   BOOST_CHECK(b >= 42);

   boost::multiprecision::uint256_t base(123);
   fc::uint256 c(base);
   BOOST_CHECK(c == 123);

   fc::uint256 d(std::move(base));
   BOOST_CHECK(d == 123);
}

BOOST_AUTO_TEST_CASE(uint256_comparison_between_instances) {
   fc::uint256 a(10), b(20), c(10);
   BOOST_CHECK(a == c);
   BOOST_CHECK(a != b);
   BOOST_CHECK(a < b);
   BOOST_CHECK(b > a);
   BOOST_CHECK(a <= c);
   BOOST_CHECK(a <= b);
   BOOST_CHECK(b >= a);
}

BOOST_AUTO_TEST_CASE(int256_construction_and_comparison) {
   fc::int256 a;
   BOOST_CHECK(a == 0);

   fc::int256 b(-42);
   BOOST_CHECK(b == -42);
   BOOST_CHECK(b < 0);
   BOOST_CHECK(b != 42);

   fc::int256 c(-10), d(20), e(-10);
   BOOST_CHECK(c == e);
   BOOST_CHECK(c != d);
   BOOST_CHECK(c < d);
   BOOST_CHECK(d > c);
}

BOOST_AUTO_TEST_CASE(uint256_variant_roundtrip) {
   fc::uint256 original("115792089237316195423570985008687907853269984665640564039457584007913129639935");
   fc::variant var;
   fc::to_variant(original, var);
   fc::uint256 result;
   fc::from_variant(var, result);
   BOOST_CHECK(result == original);
}

BOOST_AUTO_TEST_CASE(int256_variant_roundtrip) {
   fc::int256 original(-999999);
   fc::variant var;
   fc::to_variant(original, var);
   fc::int256 result;
   fc::from_variant(var, result);
   BOOST_CHECK(result == original);
}

// UInt<N>/Int<N> aliases from variant_multiprecision.hpp still work
BOOST_AUTO_TEST_CASE(uint_alias_variant_roundtrip) {
   fc::UInt<256> original("12345678901234567890");
   fc::variant var;
   fc::to_variant(original, var);
   fc::UInt<256> result;
   fc::from_variant(var, result);
   BOOST_CHECK(result == original);
}


BOOST_AUTO_TEST_SUITE_END()

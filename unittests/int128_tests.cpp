#include <fc/int128.hpp>
#include <fc/int256.hpp>
#include <fc/int256_fwd.hpp>
#include <fc/variant.hpp>
#include <fc/variant_multiprecision.hpp>
#include <fc/exception/exception.hpp>
#include <fc/io/datastream.hpp>

#include <sysio/chain/apply_context.hpp>
#include <sysio/chain/webassembly/interface.hpp>

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <limits>
#include <vector>

using namespace fc;
using namespace sysio::chain;
using namespace sysio::chain::webassembly;

namespace {
   constexpr unsigned __int128 UINT128_MAX_VAL = ~static_cast<unsigned __int128>(0);
   constexpr __int128           INT128_MAX_VAL = static_cast<__int128>(UINT128_MAX_VAL >> 1);
   constexpr __int128           INT128_MIN_VAL = -INT128_MAX_VAL - 1;

   // The compiler builtin methods on interface are const and never access this->context.
   // Provide a minimal harness that satisfies the constructor without needing a real chain.
   struct builtin_harness {
      alignas(apply_context) char storage[sizeof(apply_context)]{};
      interface iface{reinterpret_cast<apply_context&>(storage)};

      __int128 ashrti3(uint64_t low, uint64_t high, uint32_t shift) {
         __int128 result{};
         iface.__ashrti3(legacy_ptr<__int128>(static_cast<void*>(&result)), low, high, shift);
         return result;
      }
      __int128 ashlti3(uint64_t low, uint64_t high, uint32_t shift) {
         __int128 result{};
         iface.__ashlti3(legacy_ptr<__int128>(static_cast<void*>(&result)), low, high, shift);
         return result;
      }
      __int128 lshlti3(uint64_t low, uint64_t high, uint32_t shift) {
         __int128 result{};
         iface.__lshlti3(legacy_ptr<__int128>(static_cast<void*>(&result)), low, high, shift);
         return result;
      }
      __int128 lshrti3(uint64_t low, uint64_t high, uint32_t shift) {
         __int128 result{};
         iface.__lshrti3(legacy_ptr<__int128>(static_cast<void*>(&result)), low, high, shift);
         return result;
      }
      __int128 divti3(uint64_t la, uint64_t ha, uint64_t lb, uint64_t hb) {
         __int128 result{};
         iface.__divti3(legacy_ptr<__int128>(static_cast<void*>(&result)), la, ha, lb, hb);
         return result;
      }
      unsigned __int128 udivti3(uint64_t la, uint64_t ha, uint64_t lb, uint64_t hb) {
         unsigned __int128 result{};
         iface.__udivti3(legacy_ptr<unsigned __int128>(static_cast<void*>(&result)), la, ha, lb, hb);
         return result;
      }
      unsigned __int128 multi3(uint64_t la, uint64_t ha, uint64_t lb, uint64_t hb) {
         unsigned __int128 result{};
         iface.__multi3(legacy_ptr<unsigned __int128>(static_cast<void*>(&result)), la, ha, lb, hb);
         return result;
      }
      __int128 modti3(uint64_t la, uint64_t ha, uint64_t lb, uint64_t hb) {
         __int128 result{};
         iface.__modti3(legacy_ptr<__int128>(static_cast<void*>(&result)), la, ha, lb, hb);
         return result;
      }
      unsigned __int128 umodti3(uint64_t la, uint64_t ha, uint64_t lb, uint64_t hb) {
         unsigned __int128 result{};
         iface.__umodti3(legacy_ptr<unsigned __int128>(static_cast<void*>(&result)), la, ha, lb, hb);
         return result;
      }

      // Helper: split a signed 128-bit value into lo/hi uint64_t parts
      static std::pair<uint64_t,uint64_t> split(__int128 v) {
         return { static_cast<uint64_t>(v), static_cast<uint64_t>(static_cast<unsigned __int128>(v) >> 64) };
      }
      static std::pair<uint64_t,uint64_t> usplit(unsigned __int128 v) {
         return { static_cast<uint64_t>(v), static_cast<uint64_t>(v >> 64) };
      }
   };
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

// ============================================================================
// Compiler builtins: __ashrti3 (arithmetic right shift)
// Calls the actual interface methods from compiler_builtins.cpp
// ============================================================================

BOOST_AUTO_TEST_CASE(ashrti3_positive_values) {
   builtin_harness h;
   BOOST_CHECK(h.ashrti3(100, 0, 0) == 100);
   BOOST_CHECK(h.ashrti3(100, 0, 1) == 50);
   BOOST_CHECK(h.ashrti3(100, 0, 7) == 0);
   BOOST_CHECK(h.ashrti3(100, 0, 64) == 0);
   BOOST_CHECK(h.ashrti3(100, 0, 127) == 0);
   // shift >= 128: returns 0 for non-negative
   BOOST_CHECK(h.ashrti3(100, 0, 128) == 0);
   BOOST_CHECK(h.ashrti3(100, 0, 200) == 0);
   BOOST_CHECK(h.ashrti3(100, 0, UINT32_MAX) == 0);
}

BOOST_AUTO_TEST_CASE(ashrti3_int128_min) {
   builtin_harness h;
   uint64_t low = 0, high = 0x8000000000000000ULL; // INT128_MIN

   BOOST_CHECK(h.ashrti3(low, high, 0) == INT128_MIN_VAL);
   BOOST_CHECK(h.ashrti3(low, high, 1) == INT128_MIN_VAL / 2);

   __int128 neg_2_63 = -static_cast<__int128>(static_cast<unsigned __int128>(1) << 63);
   BOOST_CHECK(h.ashrti3(low, high, 64) == neg_2_63);

   BOOST_CHECK(h.ashrti3(low, high, 127) == -1);
   // shift >= 128: saturates to -1 for negative
   BOOST_CHECK(h.ashrti3(low, high, 128) == -1);
   BOOST_CHECK(h.ashrti3(low, high, 200) == -1);
   BOOST_CHECK(h.ashrti3(low, high, UINT32_MAX) == -1);
}

BOOST_AUTO_TEST_CASE(ashrti3_negative_one) {
   builtin_harness h;
   // -1 >> n == -1 for all n (arithmetic right shift preserves sign)
   BOOST_CHECK(h.ashrti3(UINT64_MAX, UINT64_MAX, 0) == -1);
   BOOST_CHECK(h.ashrti3(UINT64_MAX, UINT64_MAX, 1) == -1);
   BOOST_CHECK(h.ashrti3(UINT64_MAX, UINT64_MAX, 64) == -1);
   BOOST_CHECK(h.ashrti3(UINT64_MAX, UINT64_MAX, 127) == -1);
   BOOST_CHECK(h.ashrti3(UINT64_MAX, UINT64_MAX, 128) == -1);
}

BOOST_AUTO_TEST_CASE(ashrti3_zero) {
   builtin_harness h;
   BOOST_CHECK(h.ashrti3(0, 0, 0) == 0);
   BOOST_CHECK(h.ashrti3(0, 0, 64) == 0);
   BOOST_CHECK(h.ashrti3(0, 0, 128) == 0);
   BOOST_CHECK(h.ashrti3(0, 0, UINT32_MAX) == 0);
}

// Cross-check against values from existing WASM test (test_compiler_builtins.cpp)
BOOST_AUTO_TEST_CASE(ashrti3_matches_wasm_expectations) {
   builtin_harness h;
   uint64_t low = 0, high = 0x8000000000000000ULL;

   __int128 test = static_cast<__int128>(1);
   test <<= 127;
   BOOST_CHECK(h.ashrti3(low, high, 2)   == (test >> 2));
   BOOST_CHECK(h.ashrti3(low, high, 64)  == (test >> 64));
   BOOST_CHECK(h.ashrti3(low, high, 95)  == (test >> 95));
   BOOST_CHECK(h.ashrti3(low, high, 127) == (test >> 127));
}

// ============================================================================
// Compiler builtins: unsigned shift overflow guards
// ============================================================================

BOOST_AUTO_TEST_CASE(ashlti3_overflow_guard) {
   builtin_harness h;
   // shift >= 128 returns 0
   BOOST_CHECK(h.ashlti3(1, 0, 128) == 0);
   BOOST_CHECK(h.ashlti3(UINT64_MAX, UINT64_MAX, 128) == 0);
   BOOST_CHECK(h.ashlti3(1, 0, 200) == 0);
   BOOST_CHECK(h.ashlti3(1, 0, UINT32_MAX) == 0);
   // Normal shifts work
   BOOST_CHECK(h.ashlti3(1, 0, 0) == 1);
   BOOST_CHECK(h.ashlti3(1, 0, 1) == 2);
   BOOST_CHECK(h.ashlti3(1, 0, 64) == static_cast<__int128>(static_cast<unsigned __int128>(1) << 64));
}

BOOST_AUTO_TEST_CASE(lshlti3_overflow_guard) {
   builtin_harness h;
   BOOST_CHECK(h.lshlti3(1, 0, 128) == 0);
   BOOST_CHECK(h.lshlti3(1, 0, 200) == 0);
   BOOST_CHECK(h.lshlti3(1, 0, UINT32_MAX) == 0);
   BOOST_CHECK(h.lshlti3(1, 0, 0) == 1);
   BOOST_CHECK(h.lshlti3(1, 0, 1) == 2);
}

BOOST_AUTO_TEST_CASE(lshrti3_overflow_guard) {
   builtin_harness h;
   uint64_t high = 0x8000000000000000ULL;
   // shift >= 128 returns 0
   BOOST_CHECK(h.lshrti3(0, high, 128) == 0);
   BOOST_CHECK(h.lshrti3(0, high, 200) == 0);
   BOOST_CHECK(h.lshrti3(0, high, UINT32_MAX) == 0);
   // Normal shifts work
   BOOST_CHECK(h.lshrti3(0, high, 0) == static_cast<__int128>(static_cast<unsigned __int128>(1) << 127));
   BOOST_CHECK(h.lshrti3(0, high, 127) == 1);
}

// ============================================================================
// Compiler builtins: __multi3 (128-bit multiplication)
// ============================================================================

BOOST_AUTO_TEST_CASE(multi3_basic) {
   builtin_harness h;
   // 100 * 30 = 3000
   BOOST_CHECK(h.multi3(100, 0, 30, 0) == 3000);
   // -30 * 100 = -3000
   auto [la, ha] = builtin_harness::split(-30);
   BOOST_CHECK(h.multi3(la, ha, 100, 0) == static_cast<unsigned __int128>(static_cast<__int128>(-3000)));
   // 100 * -30 = -3000
   BOOST_CHECK(h.multi3(100, 0, la, ha) == static_cast<unsigned __int128>(static_cast<__int128>(-3000)));
   // -30 * -30 = 900
   BOOST_CHECK(h.multi3(la, ha, la, ha) == 900);
   // identity: 1 * x = x
   BOOST_CHECK(h.multi3(1, 0, 100, 0) == 100);
   // zero: 0 * x = 0
   BOOST_CHECK(h.multi3(0, 0, 100, 0) == 0);
}

BOOST_AUTO_TEST_CASE(multi3_large) {
   builtin_harness h;
   // (2^64) * (2^63) = 2^127
   unsigned __int128 expected = static_cast<unsigned __int128>(1) << 127;
   BOOST_CHECK(h.multi3(0, 1, 0x8000000000000000ULL, 0) == expected);
}

// ============================================================================
// Compiler builtins: __divti3 (signed 128-bit division)
// ============================================================================

BOOST_AUTO_TEST_CASE(divti3_basic) {
   builtin_harness h;
   auto [la, ha] = builtin_harness::split(-30);

   // 100 / -30 = -3
   BOOST_CHECK(h.divti3(100, 0, la, ha) == -3);
   // -30 / 100 = 0
   BOOST_CHECK(h.divti3(la, ha, 100, 0) == 0);
   // -30 / -30 = 1
   BOOST_CHECK(h.divti3(la, ha, la, ha) == 1);
   // 3333 / 100 = 33
   BOOST_CHECK(h.divti3(3333, 0, 100, 0) == 33);
   // identity: x / 1 = x
   BOOST_CHECK(h.divti3(100, 0, 1, 0) == 100);
   BOOST_CHECK(h.divti3(la, ha, 1, 0) == -30);
}

BOOST_AUTO_TEST_CASE(divti3_by_zero) {
   builtin_harness h;
   BOOST_CHECK_EXCEPTION(h.divti3(100, 0, 0, 0), arithmetic_exception,
      [](const auto& e) { return std::string(e.what()).find("divide by zero") != std::string::npos; });
}

BOOST_AUTO_TEST_CASE(divti3_overflow) {
   builtin_harness h;
   // INT128_MIN / -1 must return INT128_MIN (not UB)
   auto [la, ha] = builtin_harness::split(INT128_MIN_VAL);
   auto [lb, hb] = builtin_harness::split(-1);
   BOOST_CHECK(h.divti3(la, ha, lb, hb) == INT128_MIN_VAL);
}

// ============================================================================
// Compiler builtins: __udivti3 (unsigned 128-bit division)
// ============================================================================

BOOST_AUTO_TEST_CASE(udivti3_basic) {
   builtin_harness h;
   BOOST_CHECK(h.udivti3(100, 0, 30, 0) == 3);
   BOOST_CHECK(h.udivti3(3333, 0, 100, 0) == 33);
   // identity: x / 1 = x
   BOOST_CHECK(h.udivti3(100, 0, 1, 0) == 100);
   // large / large
   auto [la, ha] = builtin_harness::usplit(UINT128_MAX_VAL);
   BOOST_CHECK(h.udivti3(la, ha, la, ha) == 1);
}

BOOST_AUTO_TEST_CASE(udivti3_by_zero) {
   builtin_harness h;
   BOOST_CHECK_EXCEPTION(h.udivti3(100, 0, 0, 0), arithmetic_exception,
      [](const auto& e) { return std::string(e.what()).find("divide by zero") != std::string::npos; });
}

// ============================================================================
// Compiler builtins: __modti3 (signed 128-bit modulo)
// ============================================================================

BOOST_AUTO_TEST_CASE(modti3_basic) {
   builtin_harness h;
   auto [la, ha] = builtin_harness::split(-30);
   auto [lb, hb] = builtin_harness::split(-100);

   // -30 % 100 = -30
   BOOST_CHECK(h.modti3(la, ha, 100, 0) == -30);
   // 30 % -100 = 30
   BOOST_CHECK(h.modti3(30, 0, lb, hb) == 30);
   // -30 % -100 = -30
   BOOST_CHECK(h.modti3(la, ha, lb, hb) == -30);
   // 100 % 30 = 10
   BOOST_CHECK(h.modti3(100, 0, 30, 0) == 10);
   // 100 % 100 = 0
   BOOST_CHECK(h.modti3(100, 0, 100, 0) == 0);
   // 0 % 100 = 0
   BOOST_CHECK(h.modti3(0, 0, 100, 0) == 0);
}

BOOST_AUTO_TEST_CASE(modti3_by_zero) {
   builtin_harness h;
   BOOST_CHECK_EXCEPTION(h.modti3(100, 0, 0, 0), arithmetic_exception,
      [](const auto& e) { return std::string(e.what()).find("divide by zero") != std::string::npos; });
}

BOOST_AUTO_TEST_CASE(modti3_overflow) {
   builtin_harness h;
   // INT128_MIN % -1 must return 0 (not UB)
   auto [la, ha] = builtin_harness::split(INT128_MIN_VAL);
   auto [lb, hb] = builtin_harness::split(-1);
   BOOST_CHECK(h.modti3(la, ha, lb, hb) == 0);
}

// ============================================================================
// Compiler builtins: __umodti3 (unsigned 128-bit modulo)
// ============================================================================

BOOST_AUTO_TEST_CASE(umodti3_basic) {
   builtin_harness h;
   BOOST_CHECK(h.umodti3(100, 0, 30, 0) == 10);
   BOOST_CHECK(h.umodti3(100, 0, 100, 0) == 0);
   BOOST_CHECK(h.umodti3(0, 0, 100, 0) == 0);
   // large values
   auto [la, ha] = builtin_harness::usplit(UINT128_MAX_VAL);
   BOOST_CHECK(h.umodti3(la, ha, la, ha) == 0);
}

BOOST_AUTO_TEST_CASE(umodti3_by_zero) {
   builtin_harness h;
   BOOST_CHECK_EXCEPTION(h.umodti3(100, 0, 0, 0), arithmetic_exception,
      [](const auto& e) { return std::string(e.what()).find("divide by zero") != std::string::npos; });
}

BOOST_AUTO_TEST_SUITE_END()

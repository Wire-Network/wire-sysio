/**
 * Golden-value tests for all 34 float128 compiler builtin host functions.
 *
 * These functions are consensus-critical: if any function changes behavior
 * (compiler upgrade, softfloat update, platform difference), nodes will fork.
 * Tests assert exact bit-pattern outputs for known inputs.
 *
 * Uses the builtin_harness pattern (fake apply_context) because all compiler
 * builtin methods are const and never dereference the context pointer.
 */
#include <sysio/chain/apply_context.hpp>
#include <sysio/chain/webassembly/interface.hpp>

#include <boost/test/unit_test.hpp>

#include <cmath>
#include <cstring>
#include <cstdint>
#include <limits>

using namespace sysio::chain;
using namespace sysio::chain::webassembly;

namespace {

// ============================================================================
// IEEE 754 quadruple-precision (binary128) bit patterns
// Layout: float128_t = { uint64_t v[2] } where v[0]=lo, v[1]=hi
// Hi word: [sign:1][exponent:15][significand_hi:48]
// Lo word: [significand_lo:64]
// Exponent bias: 16383
// ============================================================================

struct f128 { uint64_t lo, hi; };

constexpr f128 F128_ZERO         = {0, 0x0000000000000000ULL};
constexpr f128 F128_NEG_ZERO     = {0, 0x8000000000000000ULL};
constexpr f128 F128_ONE          = {0, 0x3FFF000000000000ULL};
constexpr f128 F128_NEG_ONE      = {0, 0xBFFF000000000000ULL};
constexpr f128 F128_TWO          = {0, 0x4000000000000000ULL};
constexpr f128 F128_NEG_TWO      = {0, 0xC000000000000000ULL};
constexpr f128 F128_THREE        = {0, 0x4000800000000000ULL};
constexpr f128 F128_HALF         = {0, 0x3FFE000000000000ULL};
constexpr f128 F128_SIX          = {0, 0x4001800000000000ULL};
constexpr f128 F128_TEN          = {0, 0x4002400000000000ULL};
constexpr f128 F128_POS_INF      = {0, 0x7FFF000000000000ULL};
constexpr f128 F128_NEG_INF      = {0, 0xFFFF000000000000ULL};
constexpr f128 F128_QNAN         = {0, 0x7FFF800000000000ULL};
constexpr f128 F128_MIN_SUBNORM  = {1, 0x0000000000000000ULL}; // smallest positive subnormal

// Float/double bit-pattern constants
constexpr uint32_t F32_ONE     = 0x3F800000u;
constexpr uint32_t F32_TWO     = 0x40000000u;
constexpr uint32_t F32_NEG_ONE = 0xBF800000u;
constexpr uint32_t F32_ZERO    = 0x00000000u;
constexpr uint32_t F32_POS_INF = 0x7F800000u;

constexpr uint64_t F64_ONE     = 0x3FF0000000000000ULL;
constexpr uint64_t F64_TWO     = 0x4000000000000000ULL;
constexpr uint64_t F64_NEG_ONE = 0xBFF0000000000000ULL;
constexpr uint64_t F64_ZERO    = 0x0000000000000000ULL;
constexpr uint64_t F64_POS_INF = 0x7FF0000000000000ULL;

// ============================================================================
// Helpers
// ============================================================================

uint32_t to_f32_bits(float f) {
   uint32_t bits;
   std::memcpy(&bits, &f, sizeof(bits));
   return bits;
}

float from_f32_bits(uint32_t bits) {
   float f;
   std::memcpy(&f, &bits, sizeof(f));
   return f;
}

uint64_t to_f64_bits(double d) {
   uint64_t bits;
   std::memcpy(&bits, &d, sizeof(bits));
   return bits;
}

double from_f64_bits(uint64_t bits) {
   double d;
   std::memcpy(&d, &bits, sizeof(d));
   return d;
}

bool is_f128_nan(uint64_t lo, uint64_t hi) {
   return ((hi & 0x7FFF000000000000ULL) == 0x7FFF000000000000ULL) &&
          ((hi & 0x0000FFFFFFFFFFFFULL) != 0 || lo != 0);
}

static f128 f128_neg(f128 a) { return {a.lo, a.hi ^ 0x8000000000000000ULL}; }

// ============================================================================
// Minimal test harness – compiler builtins are const and never touch context
// ============================================================================

struct builtin_harness {
   alignas(apply_context) char storage[sizeof(apply_context)]{};
   interface iface{reinterpret_cast<apply_context&>(storage)};

   // --- Float128 arithmetic (5 functions) ---

   f128 addtf3(f128 a, f128 b) {
      float128_t r;
      iface.__addtf3(legacy_ptr<float128_t>(static_cast<void*>(&r)), a.lo, a.hi, b.lo, b.hi);
      return {r.v[0], r.v[1]};
   }

   f128 subtf3(f128 a, f128 b) {
      float128_t r;
      iface.__subtf3(legacy_ptr<float128_t>(static_cast<void*>(&r)), a.lo, a.hi, b.lo, b.hi);
      return {r.v[0], r.v[1]};
   }

   f128 multf3(f128 a, f128 b) {
      float128_t r;
      iface.__multf3(legacy_ptr<float128_t>(static_cast<void*>(&r)), a.lo, a.hi, b.lo, b.hi);
      return {r.v[0], r.v[1]};
   }

   f128 divtf3(f128 a, f128 b) {
      float128_t r;
      iface.__divtf3(legacy_ptr<float128_t>(static_cast<void*>(&r)), a.lo, a.hi, b.lo, b.hi);
      return {r.v[0], r.v[1]};
   }

   f128 negtf2(f128 a) {
      float128_t r;
      iface.__negtf2(legacy_ptr<float128_t>(static_cast<void*>(&r)), a.lo, a.hi);
      return {r.v[0], r.v[1]};
   }

   // --- Float/Double <-> Float128 conversions (4 functions) ---

   f128 extendsftf2(float f) {
      float128_t r;
      iface.__extendsftf2(legacy_ptr<float128_t>(static_cast<void*>(&r)), f);
      return {r.v[0], r.v[1]};
   }

   f128 extenddftf2(double d) {
      float128_t r;
      iface.__extenddftf2(legacy_ptr<float128_t>(static_cast<void*>(&r)), d);
      return {r.v[0], r.v[1]};
   }

   double trunctfdf2(f128 a) { return iface.__trunctfdf2(a.lo, a.hi); }
   float  trunctfsf2(f128 a) { return iface.__trunctfsf2(a.lo, a.hi); }

   // --- Float128 -> Integer (6 functions) ---

   int32_t  fixtfsi(f128 a)   { return iface.__fixtfsi(a.lo, a.hi); }
   int64_t  fixtfdi(f128 a)   { return iface.__fixtfdi(a.lo, a.hi); }
   uint32_t fixunstfsi(f128 a) { return iface.__fixunstfsi(a.lo, a.hi); }
   uint64_t fixunstfdi(f128 a) { return iface.__fixunstfdi(a.lo, a.hi); }

   __int128 fixtfti(f128 a) {
      __int128 r = 0;
      iface.__fixtfti(legacy_ptr<__int128>(static_cast<void*>(&r)), a.lo, a.hi);
      return r;
   }

   unsigned __int128 fixunstfti(f128 a) {
      unsigned __int128 r = 0;
      iface.__fixunstfti(legacy_ptr<unsigned __int128>(static_cast<void*>(&r)), a.lo, a.hi);
      return r;
   }

   // --- Float/Double -> Int128 (4 functions) ---

   __int128 fixsfti(float a) {
      __int128 r = 0;
      iface.__fixsfti(legacy_ptr<__int128>(static_cast<void*>(&r)), a);
      return r;
   }

   __int128 fixdfti(double a) {
      __int128 r = 0;
      iface.__fixdfti(legacy_ptr<__int128>(static_cast<void*>(&r)), a);
      return r;
   }

   unsigned __int128 fixunssfti(float a) {
      unsigned __int128 r = 0;
      iface.__fixunssfti(legacy_ptr<unsigned __int128>(static_cast<void*>(&r)), a);
      return r;
   }

   unsigned __int128 fixunsdfti(double a) {
      unsigned __int128 r = 0;
      iface.__fixunsdfti(legacy_ptr<unsigned __int128>(static_cast<void*>(&r)), a);
      return r;
   }

   // --- Integer -> Float/Float128 (7 functions) ---

   double floatsidf(int32_t i) { return iface.__floatsidf(i); }

   f128 floatsitf(int32_t i) {
      float128_t r;
      iface.__floatsitf(legacy_ptr<float128_t>(static_cast<void*>(&r)), i);
      return {r.v[0], r.v[1]};
   }

   f128 floatditf(uint64_t a) {
      float128_t r;
      iface.__floatditf(legacy_ptr<float128_t>(static_cast<void*>(&r)), a);
      return {r.v[0], r.v[1]};
   }

   f128 floatunsitf(uint32_t i) {
      float128_t r;
      iface.__floatunsitf(legacy_ptr<float128_t>(static_cast<void*>(&r)), i);
      return {r.v[0], r.v[1]};
   }

   f128 floatunditf(uint64_t a) {
      float128_t r;
      iface.__floatunditf(legacy_ptr<float128_t>(static_cast<void*>(&r)), a);
      return {r.v[0], r.v[1]};
   }

   double floattidf(uint64_t lo, uint64_t hi)   { return iface.__floattidf(lo, hi); }
   double floatuntidf(uint64_t lo, uint64_t hi)  { return iface.__floatuntidf(lo, hi); }

   // --- Float128 comparisons (8 functions) ---

   int eqtf2(f128 a, f128 b)    { return iface.__eqtf2(a.lo, a.hi, b.lo, b.hi); }
   int netf2(f128 a, f128 b)    { return iface.__netf2(a.lo, a.hi, b.lo, b.hi); }
   int getf2(f128 a, f128 b)    { return iface.__getf2(a.lo, a.hi, b.lo, b.hi); }
   int gttf2(f128 a, f128 b)    { return iface.__gttf2(a.lo, a.hi, b.lo, b.hi); }
   int letf2(f128 a, f128 b)    { return iface.__letf2(a.lo, a.hi, b.lo, b.hi); }
   int lttf2(f128 a, f128 b)    { return iface.__lttf2(a.lo, a.hi, b.lo, b.hi); }
   int cmptf2(f128 a, f128 b)   { return iface.__cmptf2(a.lo, a.hi, b.lo, b.hi); }
   int unordtf2(f128 a, f128 b) { return iface.__unordtf2(a.lo, a.hi, b.lo, b.hi); }
};

// Convenience macros for checking f128 bit patterns
#define CHECK_F128(actual, expected_lo, expected_hi)  \
   BOOST_CHECK_EQUAL((actual).lo, (expected_lo));     \
   BOOST_CHECK_EQUAL((actual).hi, (expected_hi))

#define CHECK_F128_VAL(actual, expected)              \
   CHECK_F128(actual, (expected).lo, (expected).hi)

#define CHECK_F128_NAN(actual)                        \
   BOOST_CHECK(is_f128_nan((actual).lo, (actual).hi))

} // anonymous namespace

// ============================================================================
//  Float128 Arithmetic: __addtf3, __subtf3, __multf3, __divtf3, __negtf2
// ============================================================================

BOOST_AUTO_TEST_SUITE(float128_builtin_tests)

BOOST_AUTO_TEST_CASE(addtf3_basic) {
   builtin_harness h;
   CHECK_F128_VAL(h.addtf3(F128_ONE, F128_TWO), F128_THREE);
   CHECK_F128_VAL(h.addtf3(F128_ONE, F128_NEG_ONE), F128_ZERO);
   CHECK_F128_VAL(h.addtf3(F128_ZERO, F128_ZERO), F128_ZERO);
   CHECK_F128_VAL(h.addtf3(F128_ONE, F128_ZERO), F128_ONE);
   CHECK_F128_VAL(h.addtf3(F128_HALF, F128_HALF), F128_ONE);
}

BOOST_AUTO_TEST_CASE(addtf3_special) {
   builtin_harness h;
   CHECK_F128_VAL(h.addtf3(F128_POS_INF, F128_ONE), F128_POS_INF);
   CHECK_F128_VAL(h.addtf3(F128_NEG_INF, F128_NEG_INF), F128_NEG_INF);
   CHECK_F128_NAN(h.addtf3(F128_POS_INF, F128_NEG_INF));
   CHECK_F128_NAN(h.addtf3(F128_QNAN, F128_ONE));
}

BOOST_AUTO_TEST_CASE(addtf3_subnormal) {
   builtin_harness h;
   // Subnormal + 0 must not flush to zero (hardware often does, softfloat must not)
   CHECK_F128_VAL(h.addtf3(F128_MIN_SUBNORM, F128_ZERO), F128_MIN_SUBNORM);
}

BOOST_AUTO_TEST_CASE(subtf3_basic) {
   builtin_harness h;
   CHECK_F128_VAL(h.subtf3(F128_THREE, F128_TWO), F128_ONE);
   CHECK_F128_VAL(h.subtf3(F128_ONE, F128_TWO), F128_NEG_ONE);
   CHECK_F128_VAL(h.subtf3(F128_ONE, F128_ONE), F128_ZERO);
   CHECK_F128_VAL(h.subtf3(F128_ZERO, F128_ONE), F128_NEG_ONE);
}

BOOST_AUTO_TEST_CASE(subtf3_special) {
   builtin_harness h;
   CHECK_F128_VAL(h.subtf3(F128_POS_INF, F128_ONE), F128_POS_INF);
   CHECK_F128_NAN(h.subtf3(F128_POS_INF, F128_POS_INF));
   CHECK_F128_NAN(h.subtf3(F128_QNAN, F128_ONE));
}

BOOST_AUTO_TEST_CASE(multf3_basic) {
   builtin_harness h;
   CHECK_F128_VAL(h.multf3(F128_TWO, F128_THREE), F128_SIX);
   CHECK_F128_VAL(h.multf3(F128_ONE, F128_ONE), F128_ONE);
   CHECK_F128_VAL(h.multf3(F128_NEG_ONE, F128_NEG_ONE), F128_ONE);
   CHECK_F128_VAL(h.multf3(F128_TWO, F128_HALF), F128_ONE);
   CHECK_F128_VAL(h.multf3(F128_TWO, F128_ZERO), F128_ZERO);
   CHECK_F128_VAL(h.multf3(F128_NEG_ONE, F128_TWO), F128_NEG_TWO);
}

BOOST_AUTO_TEST_CASE(multf3_special) {
   builtin_harness h;
   CHECK_F128_VAL(h.multf3(F128_POS_INF, F128_TWO), F128_POS_INF);
   CHECK_F128_NAN(h.multf3(F128_POS_INF, F128_ZERO));
   CHECK_F128_NAN(h.multf3(F128_QNAN, F128_ONE));
}

BOOST_AUTO_TEST_CASE(divtf3_basic) {
   builtin_harness h;
   CHECK_F128_VAL(h.divtf3(F128_SIX, F128_TWO), F128_THREE);
   CHECK_F128_VAL(h.divtf3(F128_ONE, F128_TWO), F128_HALF);
   CHECK_F128_VAL(h.divtf3(F128_ONE, F128_ONE), F128_ONE);
   CHECK_F128_VAL(h.divtf3(f128_neg(F128_SIX), F128_TWO), f128_neg(F128_THREE));
}

BOOST_AUTO_TEST_CASE(divtf3_special) {
   builtin_harness h;
   // Float128 division by zero follows IEEE 754 (no exception, returns Inf)
   CHECK_F128_VAL(h.divtf3(F128_ONE, F128_ZERO), F128_POS_INF);
   CHECK_F128_VAL(h.divtf3(F128_NEG_ONE, F128_ZERO), F128_NEG_INF);
   CHECK_F128_NAN(h.divtf3(F128_ZERO, F128_ZERO));
   CHECK_F128_NAN(h.divtf3(F128_POS_INF, F128_POS_INF));
   CHECK_F128_NAN(h.divtf3(F128_QNAN, F128_ONE));
}

BOOST_AUTO_TEST_CASE(negtf2_basic) {
   builtin_harness h;
   CHECK_F128_VAL(h.negtf2(F128_ONE), F128_NEG_ONE);
   CHECK_F128_VAL(h.negtf2(F128_NEG_ONE), F128_ONE);
   CHECK_F128_VAL(h.negtf2(F128_ZERO), F128_NEG_ZERO);
   CHECK_F128_VAL(h.negtf2(F128_NEG_ZERO), F128_ZERO);
   CHECK_F128_VAL(h.negtf2(F128_POS_INF), F128_NEG_INF);
   CHECK_F128_VAL(h.negtf2(F128_NEG_INF), F128_POS_INF);
   // Double negate = identity
   CHECK_F128_VAL(h.negtf2(h.negtf2(F128_TWO)), F128_TWO);
}

// ============================================================================
//  Float128 Conversions: extend, truncate
// ============================================================================

BOOST_AUTO_TEST_CASE(extendsftf2_basic) {
   builtin_harness h;
   CHECK_F128_VAL(h.extendsftf2(1.0f), F128_ONE);
   CHECK_F128_VAL(h.extendsftf2(-1.0f), F128_NEG_ONE);
   CHECK_F128_VAL(h.extendsftf2(0.0f), F128_ZERO);
   CHECK_F128_VAL(h.extendsftf2(2.0f), F128_TWO);
   CHECK_F128_VAL(h.extendsftf2(from_f32_bits(F32_POS_INF)), F128_POS_INF);
   // NaN extends to NaN
   CHECK_F128_NAN(h.extendsftf2(std::nanf("")));
}

BOOST_AUTO_TEST_CASE(extenddftf2_basic) {
   builtin_harness h;
   CHECK_F128_VAL(h.extenddftf2(1.0), F128_ONE);
   CHECK_F128_VAL(h.extenddftf2(-1.0), F128_NEG_ONE);
   CHECK_F128_VAL(h.extenddftf2(0.0), F128_ZERO);
   CHECK_F128_VAL(h.extenddftf2(2.0), F128_TWO);
   CHECK_F128_VAL(h.extenddftf2(from_f64_bits(F64_POS_INF)), F128_POS_INF);
   CHECK_F128_NAN(h.extenddftf2(std::nan("")));
}

BOOST_AUTO_TEST_CASE(trunctfdf2_basic) {
   builtin_harness h;
   BOOST_CHECK_EQUAL(to_f64_bits(h.trunctfdf2(F128_ONE)), F64_ONE);
   BOOST_CHECK_EQUAL(to_f64_bits(h.trunctfdf2(F128_NEG_ONE)), F64_NEG_ONE);
   BOOST_CHECK_EQUAL(to_f64_bits(h.trunctfdf2(F128_ZERO)), F64_ZERO);
   BOOST_CHECK_EQUAL(to_f64_bits(h.trunctfdf2(F128_TWO)), F64_TWO);
   BOOST_CHECK_EQUAL(to_f64_bits(h.trunctfdf2(F128_POS_INF)), F64_POS_INF);
   BOOST_CHECK(std::isnan(h.trunctfdf2(F128_QNAN)));
}

BOOST_AUTO_TEST_CASE(trunctfsf2_basic) {
   builtin_harness h;
   BOOST_CHECK_EQUAL(to_f32_bits(h.trunctfsf2(F128_ONE)), F32_ONE);
   BOOST_CHECK_EQUAL(to_f32_bits(h.trunctfsf2(F128_NEG_ONE)), F32_NEG_ONE);
   BOOST_CHECK_EQUAL(to_f32_bits(h.trunctfsf2(F128_ZERO)), F32_ZERO);
   BOOST_CHECK_EQUAL(to_f32_bits(h.trunctfsf2(F128_TWO)), F32_TWO);
   BOOST_CHECK_EQUAL(to_f32_bits(h.trunctfsf2(F128_POS_INF)), F32_POS_INF);
   BOOST_CHECK(std::isnan(h.trunctfsf2(F128_QNAN)));
}

BOOST_AUTO_TEST_CASE(float_f128_roundtrip) {
   builtin_harness h;
   auto check_f32 = [&](float f) {
      BOOST_CHECK_EQUAL(to_f32_bits(h.trunctfsf2(h.extendsftf2(f))), to_f32_bits(f));
   };
   check_f32(1.0f);
   check_f32(-1.0f);
   check_f32(0.0f);
   check_f32(3.14159f);
   check_f32(1e30f);
   check_f32(-1e-30f);

   auto check_f64 = [&](double d) {
      BOOST_CHECK_EQUAL(to_f64_bits(h.trunctfdf2(h.extenddftf2(d))), to_f64_bits(d));
   };
   check_f64(1.0);
   check_f64(-1.0);
   check_f64(0.0);
   check_f64(3.141592653589793);
   check_f64(1e300);
   check_f64(-1e-300);
}

// ============================================================================
//  Float128 -> Integer conversions
// ============================================================================

BOOST_AUTO_TEST_CASE(fixtfsi_basic) {
   builtin_harness h;
   BOOST_CHECK_EQUAL(h.fixtfsi(F128_ZERO), 0);
   BOOST_CHECK_EQUAL(h.fixtfsi(F128_ONE), 1);
   BOOST_CHECK_EQUAL(h.fixtfsi(F128_NEG_ONE), -1);
   BOOST_CHECK_EQUAL(h.fixtfsi(F128_TWO), 2);
   BOOST_CHECK_EQUAL(h.fixtfsi(F128_THREE), 3);
   BOOST_CHECK_EQUAL(h.fixtfsi(F128_HALF), 0);  // truncation toward zero
   BOOST_CHECK_EQUAL(h.fixtfsi(F128_TEN), 10);
}

BOOST_AUTO_TEST_CASE(fixtfdi_basic) {
   builtin_harness h;
   BOOST_CHECK_EQUAL(h.fixtfdi(F128_ZERO), 0);
   BOOST_CHECK_EQUAL(h.fixtfdi(F128_ONE), 1);
   BOOST_CHECK_EQUAL(h.fixtfdi(F128_NEG_ONE), -1);
   BOOST_CHECK_EQUAL(h.fixtfdi(F128_TEN), 10);
   BOOST_CHECK_EQUAL(h.fixtfdi(F128_HALF), 0);
}

BOOST_AUTO_TEST_CASE(fixtfti_basic) {
   builtin_harness h;
   BOOST_CHECK(h.fixtfti(F128_ZERO) == 0);
   BOOST_CHECK(h.fixtfti(F128_ONE) == 1);
   BOOST_CHECK(h.fixtfti(F128_NEG_ONE) == -1);
   BOOST_CHECK(h.fixtfti(F128_TEN) == 10);
   BOOST_CHECK(h.fixtfti(F128_NEG_TWO) == -2);
}

BOOST_AUTO_TEST_CASE(fixunstfsi_basic) {
   builtin_harness h;
   BOOST_CHECK_EQUAL(h.fixunstfsi(F128_ZERO), 0u);
   BOOST_CHECK_EQUAL(h.fixunstfsi(F128_ONE), 1u);
   BOOST_CHECK_EQUAL(h.fixunstfsi(F128_TWO), 2u);
   BOOST_CHECK_EQUAL(h.fixunstfsi(F128_TEN), 10u);
   BOOST_CHECK_EQUAL(h.fixunstfsi(F128_HALF), 0u);
}

BOOST_AUTO_TEST_CASE(fixunstfdi_basic) {
   builtin_harness h;
   BOOST_CHECK_EQUAL(h.fixunstfdi(F128_ZERO), 0ULL);
   BOOST_CHECK_EQUAL(h.fixunstfdi(F128_ONE), 1ULL);
   BOOST_CHECK_EQUAL(h.fixunstfdi(F128_TEN), 10ULL);
}

BOOST_AUTO_TEST_CASE(fixunstfti_basic) {
   builtin_harness h;
   BOOST_CHECK(h.fixunstfti(F128_ZERO) == 0);
   BOOST_CHECK(h.fixunstfti(F128_ONE) == 1);
   BOOST_CHECK(h.fixunstfti(F128_TEN) == 10);
}

// ============================================================================
//  Float/Double -> Int128 conversions
// ============================================================================

BOOST_AUTO_TEST_CASE(fixsfti_basic) {
   builtin_harness h;
   BOOST_CHECK(h.fixsfti(0.0f) == 0);
   BOOST_CHECK(h.fixsfti(1.0f) == 1);
   BOOST_CHECK(h.fixsfti(-1.0f) == -1);
   BOOST_CHECK(h.fixsfti(42.0f) == 42);
   BOOST_CHECK(h.fixsfti(-42.0f) == -42);
   BOOST_CHECK(h.fixsfti(3.14f) == 3);
}

BOOST_AUTO_TEST_CASE(fixdfti_basic) {
   builtin_harness h;
   BOOST_CHECK(h.fixdfti(0.0) == 0);
   BOOST_CHECK(h.fixdfti(1.0) == 1);
   BOOST_CHECK(h.fixdfti(-1.0) == -1);
   BOOST_CHECK(h.fixdfti(42.0) == 42);
   BOOST_CHECK(h.fixdfti(-42.0) == -42);
   BOOST_CHECK(h.fixdfti(3.14) == 3);
}

BOOST_AUTO_TEST_CASE(fixunssfti_basic) {
   builtin_harness h;
   BOOST_CHECK(h.fixunssfti(0.0f) == 0);
   BOOST_CHECK(h.fixunssfti(1.0f) == 1);
   BOOST_CHECK(h.fixunssfti(42.0f) == 42);
   BOOST_CHECK(h.fixunssfti(3.14f) == 3);
}

BOOST_AUTO_TEST_CASE(fixunsdfti_basic) {
   builtin_harness h;
   BOOST_CHECK(h.fixunsdfti(0.0) == 0);
   BOOST_CHECK(h.fixunsdfti(1.0) == 1);
   BOOST_CHECK(h.fixunsdfti(42.0) == 42);
   BOOST_CHECK(h.fixunsdfti(3.14) == 3);
}

// ============================================================================
//  Integer -> Float128/Double conversions
// ============================================================================

BOOST_AUTO_TEST_CASE(floatsidf_basic) {
   builtin_harness h;
   BOOST_CHECK_EQUAL(to_f64_bits(h.floatsidf(0)), F64_ZERO);
   BOOST_CHECK_EQUAL(to_f64_bits(h.floatsidf(1)), F64_ONE);
   BOOST_CHECK_EQUAL(to_f64_bits(h.floatsidf(-1)), F64_NEG_ONE);
   BOOST_CHECK_EQUAL(to_f64_bits(h.floatsidf(2)), F64_TWO);
}

BOOST_AUTO_TEST_CASE(floatsitf_basic) {
   builtin_harness h;
   CHECK_F128_VAL(h.floatsitf(0), F128_ZERO);
   CHECK_F128_VAL(h.floatsitf(1), F128_ONE);
   CHECK_F128_VAL(h.floatsitf(-1), F128_NEG_ONE);
   CHECK_F128_VAL(h.floatsitf(2), F128_TWO);
   CHECK_F128_VAL(h.floatsitf(10), F128_TEN);
}

BOOST_AUTO_TEST_CASE(floatditf_basic) {
   builtin_harness h;
   CHECK_F128_VAL(h.floatditf(0), F128_ZERO);
   CHECK_F128_VAL(h.floatditf(1), F128_ONE);
   CHECK_F128_VAL(h.floatditf(2), F128_TWO);
   CHECK_F128_VAL(h.floatditf(10), F128_TEN);
   // floatditf interprets uint64_t as signed int64; UINT64_MAX = -1 as int64
   CHECK_F128_VAL(h.floatditf(UINT64_MAX), F128_NEG_ONE);
}

BOOST_AUTO_TEST_CASE(floatunsitf_basic) {
   builtin_harness h;
   CHECK_F128_VAL(h.floatunsitf(0), F128_ZERO);
   CHECK_F128_VAL(h.floatunsitf(1), F128_ONE);
   CHECK_F128_VAL(h.floatunsitf(2), F128_TWO);
   CHECK_F128_VAL(h.floatunsitf(10), F128_TEN);
}

BOOST_AUTO_TEST_CASE(floatunditf_basic) {
   builtin_harness h;
   CHECK_F128_VAL(h.floatunditf(0), F128_ZERO);
   CHECK_F128_VAL(h.floatunditf(1), F128_ONE);
   CHECK_F128_VAL(h.floatunditf(2), F128_TWO);
   CHECK_F128_VAL(h.floatunditf(10), F128_TEN);
}

BOOST_AUTO_TEST_CASE(floattidf_basic) {
   builtin_harness h;
   BOOST_CHECK_EQUAL(to_f64_bits(h.floattidf(0, 0)), F64_ZERO);
   BOOST_CHECK_EQUAL(to_f64_bits(h.floattidf(1, 0)), F64_ONE);
   BOOST_CHECK_EQUAL(to_f64_bits(h.floattidf(2, 0)), F64_TWO);
   // -1 as int128 = {UINT64_MAX, UINT64_MAX}
   BOOST_CHECK_EQUAL(to_f64_bits(h.floattidf(UINT64_MAX, UINT64_MAX)), F64_NEG_ONE);
}

BOOST_AUTO_TEST_CASE(floatuntidf_basic) {
   builtin_harness h;
   BOOST_CHECK_EQUAL(to_f64_bits(h.floatuntidf(0, 0)), F64_ZERO);
   BOOST_CHECK_EQUAL(to_f64_bits(h.floatuntidf(1, 0)), F64_ONE);
   BOOST_CHECK_EQUAL(to_f64_bits(h.floatuntidf(2, 0)), F64_TWO);
}

// Round-trip: int -> float128 -> int
BOOST_AUTO_TEST_CASE(int_f128_roundtrip) {
   builtin_harness h;
   auto check_i32 = [&](int32_t i) {
      BOOST_CHECK_EQUAL(h.fixtfsi(h.floatsitf(i)), i);
   };
   check_i32(0);
   check_i32(1);
   check_i32(-1);
   check_i32(42);
   check_i32(-42);
   check_i32(std::numeric_limits<int32_t>::max());
   check_i32(std::numeric_limits<int32_t>::min());

   auto check_u32 = [&](uint32_t i) {
      BOOST_CHECK_EQUAL(h.fixunstfsi(h.floatunsitf(i)), i);
   };
   check_u32(0);
   check_u32(1);
   check_u32(42);
   check_u32(std::numeric_limits<uint32_t>::max());
}

// ============================================================================
//  Float128 Comparisons
//
//  All comparison functions (except __unordtf2) use cmptf2_impl which returns:
//    -1 if a < b,  0 if a == b,  1 if a > b
//  They differ only in their NaN return value:
//    __eqtf2: NaN→1   __netf2: NaN→1   __getf2: NaN→-1  __gttf2: NaN→0
//    __letf2: NaN→1   __lttf2: NaN→0   __cmptf2: NaN→1  __unordtf2: NaN→1
// ============================================================================

BOOST_AUTO_TEST_CASE(eqtf2_basic) {
   builtin_harness h;
   BOOST_CHECK_EQUAL(h.eqtf2(F128_ONE, F128_ONE), 0);
   BOOST_CHECK_EQUAL(h.eqtf2(F128_ZERO, F128_ZERO), 0);
   BOOST_CHECK_EQUAL(h.eqtf2(F128_ZERO, F128_NEG_ZERO), 0); // +0 == -0
   BOOST_CHECK_NE(h.eqtf2(F128_ONE, F128_TWO), 0);
   // NaN → 1
   BOOST_CHECK_EQUAL(h.eqtf2(F128_QNAN, F128_ONE), 1);
   BOOST_CHECK_EQUAL(h.eqtf2(F128_ONE, F128_QNAN), 1);
   BOOST_CHECK_EQUAL(h.eqtf2(F128_QNAN, F128_QNAN), 1);
}

BOOST_AUTO_TEST_CASE(netf2_basic) {
   builtin_harness h;
   BOOST_CHECK_EQUAL(h.netf2(F128_ONE, F128_ONE), 0);
   BOOST_CHECK_NE(h.netf2(F128_ONE, F128_TWO), 0);
   BOOST_CHECK_EQUAL(h.netf2(F128_QNAN, F128_ONE), 1);
}

BOOST_AUTO_TEST_CASE(getf2_basic) {
   builtin_harness h;
   BOOST_CHECK_EQUAL(h.getf2(F128_TWO, F128_ONE), 1);
   BOOST_CHECK_EQUAL(h.getf2(F128_ONE, F128_ONE), 0);
   BOOST_CHECK_EQUAL(h.getf2(F128_ONE, F128_TWO), -1);
   BOOST_CHECK_EQUAL(h.getf2(F128_QNAN, F128_ONE), -1);
}

BOOST_AUTO_TEST_CASE(gttf2_basic) {
   builtin_harness h;
   BOOST_CHECK_EQUAL(h.gttf2(F128_TWO, F128_ONE), 1);
   BOOST_CHECK_EQUAL(h.gttf2(F128_ONE, F128_ONE), 0);
   BOOST_CHECK_EQUAL(h.gttf2(F128_ONE, F128_TWO), -1);
   BOOST_CHECK_EQUAL(h.gttf2(F128_QNAN, F128_ONE), 0);
}

BOOST_AUTO_TEST_CASE(letf2_basic) {
   builtin_harness h;
   BOOST_CHECK_EQUAL(h.letf2(F128_ONE, F128_TWO), -1);
   BOOST_CHECK_EQUAL(h.letf2(F128_ONE, F128_ONE), 0);
   BOOST_CHECK_EQUAL(h.letf2(F128_TWO, F128_ONE), 1);
   BOOST_CHECK_EQUAL(h.letf2(F128_QNAN, F128_ONE), 1);
}

BOOST_AUTO_TEST_CASE(lttf2_basic) {
   builtin_harness h;
   BOOST_CHECK_EQUAL(h.lttf2(F128_ONE, F128_TWO), -1);
   BOOST_CHECK_EQUAL(h.lttf2(F128_ONE, F128_ONE), 0);
   BOOST_CHECK_EQUAL(h.lttf2(F128_TWO, F128_ONE), 1);
   BOOST_CHECK_EQUAL(h.lttf2(F128_QNAN, F128_ONE), 0);
}

BOOST_AUTO_TEST_CASE(cmptf2_basic) {
   builtin_harness h;
   BOOST_CHECK_EQUAL(h.cmptf2(F128_ONE, F128_TWO), -1);
   BOOST_CHECK_EQUAL(h.cmptf2(F128_ONE, F128_ONE), 0);
   BOOST_CHECK_EQUAL(h.cmptf2(F128_TWO, F128_ONE), 1);
   BOOST_CHECK_EQUAL(h.cmptf2(F128_QNAN, F128_ONE), 1);
}

BOOST_AUTO_TEST_CASE(unordtf2_basic) {
   builtin_harness h;
   BOOST_CHECK_EQUAL(h.unordtf2(F128_ONE, F128_TWO), 0);
   BOOST_CHECK_EQUAL(h.unordtf2(F128_ZERO, F128_ZERO), 0);
   BOOST_CHECK_EQUAL(h.unordtf2(F128_POS_INF, F128_NEG_INF), 0);
   BOOST_CHECK_NE(h.unordtf2(F128_QNAN, F128_ONE), 0);
   BOOST_CHECK_NE(h.unordtf2(F128_ONE, F128_QNAN), 0);
   BOOST_CHECK_NE(h.unordtf2(F128_QNAN, F128_QNAN), 0);
}

BOOST_AUTO_TEST_CASE(comparison_edge_cases) {
   builtin_harness h;
   BOOST_CHECK_EQUAL(h.cmptf2(F128_POS_INF, F128_ONE), 1);
   BOOST_CHECK_EQUAL(h.cmptf2(F128_NEG_INF, F128_ONE), -1);
   BOOST_CHECK_EQUAL(h.eqtf2(F128_POS_INF, F128_POS_INF), 0);
   BOOST_CHECK_EQUAL(h.eqtf2(F128_NEG_INF, F128_NEG_INF), 0);
   BOOST_CHECK_EQUAL(h.cmptf2(F128_NEG_ONE, F128_ONE), -1);
   BOOST_CHECK_EQUAL(h.cmptf2(F128_ONE, F128_NEG_ONE), 1);
}

BOOST_AUTO_TEST_SUITE_END()

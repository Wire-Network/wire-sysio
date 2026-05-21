/**
 * @file test_f128_builtins.cpp
 *
 * Float128 / softfloat compiler-builtin coverage, exercised through WASM
 * execution. Each action calls a builtin via plain extern "C" (no
 * sysio_wasm_import) so the call resolves to the libsf copy linked into the
 * contract module via --use-rt. The host registrations for these intrinsics
 * were dropped; this test verifies that the produced WASM (containing librt
 * + libsf) is converted deterministically by sys-vm, sys-vm-jit, and
 * sys-vm-oc.
 *
 * Replaces the deleted unittests/float128_builtin_tests.cpp host-direct-call
 * golden-value test suite. Same operations, same golden bit patterns,
 * exercised through the WASM runtimes that matter for consensus.
 */
#include <sysio/sysio.hpp>

#include "test_api.hpp"

extern "C" {
   // Arithmetic
   void __addtf3(__float128&, uint64_t, uint64_t, uint64_t, uint64_t);
   void __subtf3(__float128&, uint64_t, uint64_t, uint64_t, uint64_t);
   void __multf3(__float128&, uint64_t, uint64_t, uint64_t, uint64_t);
   void __divtf3(__float128&, uint64_t, uint64_t, uint64_t, uint64_t);
   void __negtf2(__float128&, uint64_t, uint64_t);

   // Conversion (extend / truncate between f32, f64, f128)
   void __extendsftf2(__float128&, float);
   void __extenddftf2(__float128&, double);
   double __trunctfdf2(uint64_t, uint64_t);
   float  __trunctfsf2(uint64_t, uint64_t);

   // f128 -> int
   int32_t  __fixtfsi (uint64_t, uint64_t);
   int64_t  __fixtfdi (uint64_t, uint64_t);
   void     __fixtfti (__int128&, uint64_t, uint64_t);
   uint32_t __fixunstfsi(uint64_t, uint64_t);
   uint64_t __fixunstfdi(uint64_t, uint64_t);
   void     __fixunstfti(unsigned __int128&, uint64_t, uint64_t);

   // f32/f64 -> int128
   void __fixsfti  (__int128&, float);
   void __fixdfti  (__int128&, double);
   void __fixunssfti(unsigned __int128&, float);
   void __fixunsdfti(unsigned __int128&, double);

   // int -> f64 / f128
   double __floatsidf  (int32_t);
   void   __floatsitf  (__float128&, int32_t);
   void   __floatditf  (__float128&, uint64_t);
   void   __floatunsitf(__float128&, uint32_t);
   void   __floatunditf(__float128&, uint64_t);
   double __floattidf  (uint64_t, uint64_t);
   double __floatuntidf(uint64_t, uint64_t);

   // Comparisons
   int32_t __eqtf2   (uint64_t, uint64_t, uint64_t, uint64_t);
   int32_t __netf2   (uint64_t, uint64_t, uint64_t, uint64_t);
   int32_t __getf2   (uint64_t, uint64_t, uint64_t, uint64_t);
   int32_t __gttf2   (uint64_t, uint64_t, uint64_t, uint64_t);
   int32_t __letf2   (uint64_t, uint64_t, uint64_t, uint64_t);
   int32_t __lttf2   (uint64_t, uint64_t, uint64_t, uint64_t);
   int32_t __cmptf2  (uint64_t, uint64_t, uint64_t, uint64_t);
   int32_t __unordtf2(uint64_t, uint64_t, uint64_t, uint64_t);
}

// Bit-pattern helpers. f128 layout is { uint64_t lo, uint64_t hi } with hi
// holding sign:1 / exponent:15 / significand_hi:48, lo holding significand_lo:64.
namespace {
   struct f128 { uint64_t lo, hi; };

   // Selected golden f128 values (binary128, exponent bias 16383).
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
   constexpr f128 F128_MIN_SUBNORM  = {1, 0x0000000000000000ULL};

   // f32 / f64 bit patterns
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

   inline uint32_t to_f32_bits(float f) {
      uint32_t u = 0; __builtin_memcpy(&u, &f, sizeof(u)); return u;
   }
   inline float from_f32_bits(uint32_t u) {
      float f = 0; __builtin_memcpy(&f, &u, sizeof(f)); return f;
   }
   inline uint64_t to_f64_bits(double d) {
      uint64_t u = 0; __builtin_memcpy(&u, &d, sizeof(u)); return u;
   }
   inline double from_f64_bits(uint64_t u) {
      double d = 0; __builtin_memcpy(&d, &u, sizeof(d)); return d;
   }

   inline f128 to_f128(__float128 r) {
      f128 out = {0, 0};
      __builtin_memcpy(&out, &r, sizeof(out));
      return out;
   }

   inline bool eq(f128 a, f128 b) { return a.lo == b.lo && a.hi == b.hi; }
   inline bool is_nan(f128 a) {
      return ((a.hi & 0x7FFF000000000000ULL) == 0x7FFF000000000000ULL) &&
             ((a.hi & 0x0000FFFFFFFFFFFFULL) != 0 || a.lo != 0);
   }
   inline bool is_nan_f32(float f) {
      uint32_t b = to_f32_bits(f);
      return (b & 0x7F800000u) == 0x7F800000u && (b & 0x007FFFFFu) != 0;
   }
   inline bool is_nan_f64(double d) {
      uint64_t b = to_f64_bits(d);
      return (b & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL && (b & 0x000FFFFFFFFFFFFFULL) != 0;
   }

   inline f128 op_add(f128 a, f128 b) { __float128 r; __addtf3(r, a.lo, a.hi, b.lo, b.hi); return to_f128(r); }
   inline f128 op_sub(f128 a, f128 b) { __float128 r; __subtf3(r, a.lo, a.hi, b.lo, b.hi); return to_f128(r); }
   inline f128 op_mul(f128 a, f128 b) { __float128 r; __multf3(r, a.lo, a.hi, b.lo, b.hi); return to_f128(r); }
   inline f128 op_div(f128 a, f128 b) { __float128 r; __divtf3(r, a.lo, a.hi, b.lo, b.hi); return to_f128(r); }
   inline f128 op_neg(f128 a)         { __float128 r; __negtf2(r, a.lo, a.hi); return to_f128(r); }
}

// Arithmetic
void test_f128_builtins::test_addtf3() {
   sysio_assert(eq(op_add(F128_ONE, F128_TWO), F128_THREE),     "addtf3 1+2=3");
   sysio_assert(eq(op_add(F128_ONE, F128_NEG_ONE), F128_ZERO),  "addtf3 1+(-1)=0");
   sysio_assert(eq(op_add(F128_ZERO, F128_ZERO), F128_ZERO),    "addtf3 0+0=0");
   sysio_assert(eq(op_add(F128_ONE, F128_ZERO), F128_ONE),      "addtf3 1+0=1");
   sysio_assert(eq(op_add(F128_HALF, F128_HALF), F128_ONE),     "addtf3 0.5+0.5=1");
   // special values
   sysio_assert(eq(op_add(F128_POS_INF, F128_ONE),     F128_POS_INF), "addtf3 +inf+1");
   sysio_assert(eq(op_add(F128_NEG_INF, F128_NEG_INF), F128_NEG_INF), "addtf3 -inf+-inf");
   sysio_assert(is_nan(op_add(F128_POS_INF, F128_NEG_INF)),     "addtf3 +inf+-inf=NaN");
   sysio_assert(is_nan(op_add(F128_QNAN,   F128_ONE)),          "addtf3 NaN+1=NaN");
   // subnormal must not flush to zero
   sysio_assert(eq(op_add(F128_MIN_SUBNORM, F128_ZERO), F128_MIN_SUBNORM),
                "addtf3 subnormal+0 must not flush");
}

void test_f128_builtins::test_subtf3() {
   sysio_assert(eq(op_sub(F128_THREE, F128_TWO), F128_ONE),    "subtf3 3-2=1");
   sysio_assert(eq(op_sub(F128_ONE, F128_TWO),   F128_NEG_ONE),"subtf3 1-2=-1");
   sysio_assert(eq(op_sub(F128_ONE, F128_ONE),   F128_ZERO),   "subtf3 1-1=0");
   sysio_assert(eq(op_sub(F128_ZERO, F128_ONE),  F128_NEG_ONE),"subtf3 0-1=-1");
   sysio_assert(eq(op_sub(F128_POS_INF, F128_ONE), F128_POS_INF), "subtf3 +inf-1");
   sysio_assert(is_nan(op_sub(F128_POS_INF, F128_POS_INF)),    "subtf3 +inf-+inf=NaN");
   sysio_assert(is_nan(op_sub(F128_QNAN,   F128_ONE)),         "subtf3 NaN-1=NaN");
}

void test_f128_builtins::test_multf3() {
   sysio_assert(eq(op_mul(F128_TWO,     F128_THREE), F128_SIX),     "multf3 2*3=6");
   sysio_assert(eq(op_mul(F128_ONE,     F128_ONE),   F128_ONE),     "multf3 1*1=1");
   sysio_assert(eq(op_mul(F128_NEG_ONE, F128_NEG_ONE), F128_ONE),   "multf3 -1*-1=1");
   sysio_assert(eq(op_mul(F128_TWO,     F128_HALF),  F128_ONE),     "multf3 2*0.5=1");
   sysio_assert(eq(op_mul(F128_TWO,     F128_ZERO),  F128_ZERO),    "multf3 2*0=0");
   sysio_assert(eq(op_mul(F128_NEG_ONE, F128_TWO),   F128_NEG_TWO), "multf3 -1*2=-2");
   sysio_assert(eq(op_mul(F128_POS_INF, F128_TWO),   F128_POS_INF), "multf3 +inf*2");
   sysio_assert(is_nan(op_mul(F128_POS_INF, F128_ZERO)),            "multf3 +inf*0=NaN");
   sysio_assert(is_nan(op_mul(F128_QNAN,    F128_ONE)),             "multf3 NaN*1=NaN");
}

void test_f128_builtins::test_divtf3() {
   sysio_assert(eq(op_div(F128_SIX, F128_TWO), F128_THREE),  "divtf3 6/2=3");
   sysio_assert(eq(op_div(F128_ONE, F128_TWO), F128_HALF),   "divtf3 1/2=0.5");
   sysio_assert(eq(op_div(F128_ONE, F128_ONE), F128_ONE),    "divtf3 1/1=1");
   sysio_assert(eq(op_div(op_neg(F128_SIX), F128_TWO), op_neg(F128_THREE)),
                "divtf3 -6/2=-3");
   // Division by zero follows IEEE 754 (no trap, returns +/-inf or NaN)
   sysio_assert(eq(op_div(F128_ONE,     F128_ZERO), F128_POS_INF),  "divtf3 1/0=+inf");
   sysio_assert(eq(op_div(F128_NEG_ONE, F128_ZERO), F128_NEG_INF),  "divtf3 -1/0=-inf");
   sysio_assert(is_nan(op_div(F128_ZERO,    F128_ZERO)),            "divtf3 0/0=NaN");
   sysio_assert(is_nan(op_div(F128_POS_INF, F128_POS_INF)),         "divtf3 +inf/+inf=NaN");
   sysio_assert(is_nan(op_div(F128_QNAN,    F128_ONE)),             "divtf3 NaN/1=NaN");
}

void test_f128_builtins::test_negtf2() {
   sysio_assert(eq(op_neg(F128_ONE),       F128_NEG_ONE),  "negtf2 -1");
   sysio_assert(eq(op_neg(F128_NEG_ONE),   F128_ONE),      "negtf2 -(-1)=1");
   sysio_assert(eq(op_neg(F128_ZERO),      F128_NEG_ZERO), "negtf2 -0");
   sysio_assert(eq(op_neg(F128_NEG_ZERO),  F128_ZERO),     "negtf2 -(-0)=0");
   sysio_assert(eq(op_neg(F128_POS_INF),   F128_NEG_INF),  "negtf2 -inf");
   sysio_assert(eq(op_neg(F128_NEG_INF),   F128_POS_INF),  "negtf2 -(-inf)=+inf");
   sysio_assert(eq(op_neg(op_neg(F128_TWO)), F128_TWO),    "negtf2 double-negate");
}

// Conversions: extend / truncate
void test_f128_builtins::test_extendsftf2() {
   __float128 r;
   __extendsftf2(r, 1.0f);   sysio_assert(eq(to_f128(r), F128_ONE),     "extendsf 1");
   __extendsftf2(r, -1.0f);  sysio_assert(eq(to_f128(r), F128_NEG_ONE), "extendsf -1");
   __extendsftf2(r, 0.0f);   sysio_assert(eq(to_f128(r), F128_ZERO),    "extendsf 0");
   __extendsftf2(r, 2.0f);   sysio_assert(eq(to_f128(r), F128_TWO),     "extendsf 2");
   __extendsftf2(r, from_f32_bits(F32_POS_INF));
   sysio_assert(eq(to_f128(r), F128_POS_INF), "extendsf +inf");
   __extendsftf2(r, __builtin_nanf(""));
   sysio_assert(is_nan(to_f128(r)),           "extendsf NaN");
}

void test_f128_builtins::test_extenddftf2() {
   __float128 r;
   __extenddftf2(r, 1.0);   sysio_assert(eq(to_f128(r), F128_ONE),     "extenddf 1");
   __extenddftf2(r, -1.0);  sysio_assert(eq(to_f128(r), F128_NEG_ONE), "extenddf -1");
   __extenddftf2(r, 0.0);   sysio_assert(eq(to_f128(r), F128_ZERO),    "extenddf 0");
   __extenddftf2(r, 2.0);   sysio_assert(eq(to_f128(r), F128_TWO),     "extenddf 2");
   __extenddftf2(r, from_f64_bits(F64_POS_INF));
   sysio_assert(eq(to_f128(r), F128_POS_INF), "extenddf +inf");
   __extenddftf2(r, __builtin_nan(""));
   sysio_assert(is_nan(to_f128(r)),           "extenddf NaN");
}

void test_f128_builtins::test_trunctfdf2() {
   sysio_assert(to_f64_bits(__trunctfdf2(F128_ONE.lo,     F128_ONE.hi))     == F64_ONE,     "trunctfdf 1");
   sysio_assert(to_f64_bits(__trunctfdf2(F128_NEG_ONE.lo, F128_NEG_ONE.hi)) == F64_NEG_ONE, "trunctfdf -1");
   sysio_assert(to_f64_bits(__trunctfdf2(F128_ZERO.lo,    F128_ZERO.hi))    == F64_ZERO,    "trunctfdf 0");
   sysio_assert(to_f64_bits(__trunctfdf2(F128_TWO.lo,     F128_TWO.hi))     == F64_TWO,     "trunctfdf 2");
   sysio_assert(to_f64_bits(__trunctfdf2(F128_POS_INF.lo, F128_POS_INF.hi)) == F64_POS_INF, "trunctfdf +inf");
   sysio_assert(is_nan_f64(__trunctfdf2(F128_QNAN.lo, F128_QNAN.hi)), "trunctfdf NaN");
}

void test_f128_builtins::test_trunctfsf2() {
   sysio_assert(to_f32_bits(__trunctfsf2(F128_ONE.lo,     F128_ONE.hi))     == F32_ONE,     "trunctfsf 1");
   sysio_assert(to_f32_bits(__trunctfsf2(F128_NEG_ONE.lo, F128_NEG_ONE.hi)) == F32_NEG_ONE, "trunctfsf -1");
   sysio_assert(to_f32_bits(__trunctfsf2(F128_ZERO.lo,    F128_ZERO.hi))    == F32_ZERO,    "trunctfsf 0");
   sysio_assert(to_f32_bits(__trunctfsf2(F128_TWO.lo,     F128_TWO.hi))     == F32_TWO,     "trunctfsf 2");
   sysio_assert(to_f32_bits(__trunctfsf2(F128_POS_INF.lo, F128_POS_INF.hi)) == F32_POS_INF, "trunctfsf +inf");
   sysio_assert(is_nan_f32(__trunctfsf2(F128_QNAN.lo, F128_QNAN.hi)), "trunctfsf NaN");
}

void test_f128_builtins::test_float_f128_roundtrip() {
   auto check_f32 = [](float f) {
      __float128 ext;
      __extendsftf2(ext, f);
      f128 e = to_f128(ext);
      sysio_assert(to_f32_bits(__trunctfsf2(e.lo, e.hi)) == to_f32_bits(f),
                   "f32 -> f128 -> f32 roundtrip");
   };
   check_f32(1.0f); check_f32(-1.0f); check_f32(0.0f);
   check_f32(3.14159f); check_f32(1e30f); check_f32(-1e-30f);

   auto check_f64 = [](double d) {
      __float128 ext;
      __extenddftf2(ext, d);
      f128 e = to_f128(ext);
      sysio_assert(to_f64_bits(__trunctfdf2(e.lo, e.hi)) == to_f64_bits(d),
                   "f64 -> f128 -> f64 roundtrip");
   };
   check_f64(1.0); check_f64(-1.0); check_f64(0.0);
   check_f64(3.141592653589793); check_f64(1e300); check_f64(-1e-300);
}

// f128 -> int conversions
void test_f128_builtins::test_fixtfsi() {
   sysio_assert(__fixtfsi(F128_ZERO.lo,     F128_ZERO.hi)     == 0,  "fixtfsi 0");
   sysio_assert(__fixtfsi(F128_ONE.lo,      F128_ONE.hi)      == 1,  "fixtfsi 1");
   sysio_assert(__fixtfsi(F128_NEG_ONE.lo,  F128_NEG_ONE.hi)  == -1, "fixtfsi -1");
   sysio_assert(__fixtfsi(F128_TWO.lo,      F128_TWO.hi)      == 2,  "fixtfsi 2");
   sysio_assert(__fixtfsi(F128_THREE.lo,    F128_THREE.hi)    == 3,  "fixtfsi 3");
   sysio_assert(__fixtfsi(F128_HALF.lo,     F128_HALF.hi)     == 0,  "fixtfsi 0.5 truncates to 0");
   sysio_assert(__fixtfsi(F128_TEN.lo,      F128_TEN.hi)      == 10, "fixtfsi 10");
}

void test_f128_builtins::test_fixtfdi() {
   sysio_assert(__fixtfdi(F128_ZERO.lo,    F128_ZERO.hi)    == 0,  "fixtfdi 0");
   sysio_assert(__fixtfdi(F128_ONE.lo,     F128_ONE.hi)     == 1,  "fixtfdi 1");
   sysio_assert(__fixtfdi(F128_NEG_ONE.lo, F128_NEG_ONE.hi) == -1, "fixtfdi -1");
   sysio_assert(__fixtfdi(F128_TEN.lo,     F128_TEN.hi)     == 10, "fixtfdi 10");
   sysio_assert(__fixtfdi(F128_HALF.lo,    F128_HALF.hi)    == 0,  "fixtfdi 0.5 truncates");
}

void test_f128_builtins::test_fixtfti() {
   __int128 r = 0;
   __fixtfti(r, F128_ZERO.lo,     F128_ZERO.hi);     sysio_assert(r == 0,  "fixtfti 0");
   __fixtfti(r, F128_ONE.lo,      F128_ONE.hi);      sysio_assert(r == 1,  "fixtfti 1");
   __fixtfti(r, F128_NEG_ONE.lo,  F128_NEG_ONE.hi);  sysio_assert(r == -1, "fixtfti -1");
   __fixtfti(r, F128_TEN.lo,      F128_TEN.hi);      sysio_assert(r == 10, "fixtfti 10");
   __fixtfti(r, F128_NEG_TWO.lo,  F128_NEG_TWO.hi);  sysio_assert(r == -2, "fixtfti -2");
}

void test_f128_builtins::test_fixunstfsi() {
   sysio_assert(__fixunstfsi(F128_ZERO.lo, F128_ZERO.hi) == 0u,  "fixunstfsi 0");
   sysio_assert(__fixunstfsi(F128_ONE.lo,  F128_ONE.hi)  == 1u,  "fixunstfsi 1");
   sysio_assert(__fixunstfsi(F128_TWO.lo,  F128_TWO.hi)  == 2u,  "fixunstfsi 2");
   sysio_assert(__fixunstfsi(F128_TEN.lo,  F128_TEN.hi)  == 10u, "fixunstfsi 10");
   sysio_assert(__fixunstfsi(F128_HALF.lo, F128_HALF.hi) == 0u,  "fixunstfsi 0.5 truncates");
}

void test_f128_builtins::test_fixunstfdi() {
   sysio_assert(__fixunstfdi(F128_ZERO.lo, F128_ZERO.hi) == 0ULL,  "fixunstfdi 0");
   sysio_assert(__fixunstfdi(F128_ONE.lo,  F128_ONE.hi)  == 1ULL,  "fixunstfdi 1");
   sysio_assert(__fixunstfdi(F128_TEN.lo,  F128_TEN.hi)  == 10ULL, "fixunstfdi 10");
}

void test_f128_builtins::test_fixunstfti() {
   unsigned __int128 r = 0;
   __fixunstfti(r, F128_ZERO.lo, F128_ZERO.hi); sysio_assert(r == 0,  "fixunstfti 0");
   __fixunstfti(r, F128_ONE.lo,  F128_ONE.hi);  sysio_assert(r == 1,  "fixunstfti 1");
   __fixunstfti(r, F128_TEN.lo,  F128_TEN.hi);  sysio_assert(r == 10, "fixunstfti 10");
}

// f32/f64 -> int128
void test_f128_builtins::test_fixsfti() {
   __int128 r = 0;
   __fixsfti(r, 0.0f);   sysio_assert(r == 0,    "fixsfti 0.0");
   __fixsfti(r, 1.0f);   sysio_assert(r == 1,    "fixsfti 1.0");
   __fixsfti(r, -1.0f);  sysio_assert(r == -1,   "fixsfti -1.0");
   __fixsfti(r, 42.0f);  sysio_assert(r == 42,   "fixsfti 42.0");
   __fixsfti(r, -42.0f); sysio_assert(r == -42,  "fixsfti -42.0");
   __fixsfti(r, 3.14f);  sysio_assert(r == 3,    "fixsfti 3.14 truncates");
}

void test_f128_builtins::test_fixdfti() {
   __int128 r = 0;
   __fixdfti(r, 0.0);   sysio_assert(r == 0,   "fixdfti 0.0");
   __fixdfti(r, 1.0);   sysio_assert(r == 1,   "fixdfti 1.0");
   __fixdfti(r, -1.0);  sysio_assert(r == -1,  "fixdfti -1.0");
   __fixdfti(r, 42.0);  sysio_assert(r == 42,  "fixdfti 42.0");
   __fixdfti(r, -42.0); sysio_assert(r == -42, "fixdfti -42.0");
   __fixdfti(r, 3.14);  sysio_assert(r == 3,   "fixdfti 3.14 truncates");
}

void test_f128_builtins::test_fixunssfti() {
   unsigned __int128 r = 0;
   __fixunssfti(r, 0.0f);  sysio_assert(r == 0,  "fixunssfti 0.0");
   __fixunssfti(r, 1.0f);  sysio_assert(r == 1,  "fixunssfti 1.0");
   __fixunssfti(r, 42.0f); sysio_assert(r == 42, "fixunssfti 42.0");
   __fixunssfti(r, 3.14f); sysio_assert(r == 3,  "fixunssfti 3.14 truncates");
}

void test_f128_builtins::test_fixunsdfti() {
   unsigned __int128 r = 0;
   __fixunsdfti(r, 0.0);  sysio_assert(r == 0,  "fixunsdfti 0.0");
   __fixunsdfti(r, 1.0);  sysio_assert(r == 1,  "fixunsdfti 1.0");
   __fixunsdfti(r, 42.0); sysio_assert(r == 42, "fixunsdfti 42.0");
   __fixunsdfti(r, 3.14); sysio_assert(r == 3,  "fixunsdfti 3.14 truncates");
}

// int -> f64 / f128
void test_f128_builtins::test_floatsidf() {
   sysio_assert(to_f64_bits(__floatsidf(0))  == F64_ZERO,    "floatsidf 0");
   sysio_assert(to_f64_bits(__floatsidf(1))  == F64_ONE,     "floatsidf 1");
   sysio_assert(to_f64_bits(__floatsidf(-1)) == F64_NEG_ONE, "floatsidf -1");
   sysio_assert(to_f64_bits(__floatsidf(2))  == F64_TWO,     "floatsidf 2");
}

void test_f128_builtins::test_floatsitf() {
   __float128 r;
   __floatsitf(r, 0);  sysio_assert(eq(to_f128(r), F128_ZERO),     "floatsitf 0");
   __floatsitf(r, 1);  sysio_assert(eq(to_f128(r), F128_ONE),      "floatsitf 1");
   __floatsitf(r, -1); sysio_assert(eq(to_f128(r), F128_NEG_ONE),  "floatsitf -1");
   __floatsitf(r, 2);  sysio_assert(eq(to_f128(r), F128_TWO),      "floatsitf 2");
   __floatsitf(r, 10); sysio_assert(eq(to_f128(r), F128_TEN),      "floatsitf 10");
}

void test_f128_builtins::test_floatditf() {
   __float128 r;
   __floatditf(r, 0);  sysio_assert(eq(to_f128(r), F128_ZERO), "floatditf 0");
   __floatditf(r, 1);  sysio_assert(eq(to_f128(r), F128_ONE),  "floatditf 1");
   __floatditf(r, 2);  sysio_assert(eq(to_f128(r), F128_TWO),  "floatditf 2");
   __floatditf(r, 10); sysio_assert(eq(to_f128(r), F128_TEN),  "floatditf 10");
   // floatditf interprets uint64_t as signed int64; UINT64_MAX = -1 as int64
   __floatditf(r, 0xFFFFFFFFFFFFFFFFULL);
   sysio_assert(eq(to_f128(r), F128_NEG_ONE), "floatditf UINT64_MAX -> -1");
}

void test_f128_builtins::test_floatunsitf() {
   __float128 r;
   __floatunsitf(r, 0u);  sysio_assert(eq(to_f128(r), F128_ZERO), "floatunsitf 0");
   __floatunsitf(r, 1u);  sysio_assert(eq(to_f128(r), F128_ONE),  "floatunsitf 1");
   __floatunsitf(r, 2u);  sysio_assert(eq(to_f128(r), F128_TWO),  "floatunsitf 2");
   __floatunsitf(r, 10u); sysio_assert(eq(to_f128(r), F128_TEN),  "floatunsitf 10");
}

void test_f128_builtins::test_floatunditf() {
   __float128 r;
   __floatunditf(r, 0u);  sysio_assert(eq(to_f128(r), F128_ZERO), "floatunditf 0");
   __floatunditf(r, 1u);  sysio_assert(eq(to_f128(r), F128_ONE),  "floatunditf 1");
   __floatunditf(r, 2u);  sysio_assert(eq(to_f128(r), F128_TWO),  "floatunditf 2");
   __floatunditf(r, 10u); sysio_assert(eq(to_f128(r), F128_TEN),  "floatunditf 10");
}

void test_f128_builtins::test_floattidf() {
   sysio_assert(to_f64_bits(__floattidf(0, 0)) == F64_ZERO, "floattidf 0");
   sysio_assert(to_f64_bits(__floattidf(1, 0)) == F64_ONE,  "floattidf 1");
   sysio_assert(to_f64_bits(__floattidf(2, 0)) == F64_TWO,  "floattidf 2");
   // -1 as int128 = {UINT64_MAX, UINT64_MAX}
   sysio_assert(to_f64_bits(__floattidf(0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL)) == F64_NEG_ONE,
                "floattidf -1");
}

void test_f128_builtins::test_floatuntidf() {
   sysio_assert(to_f64_bits(__floatuntidf(0, 0)) == F64_ZERO, "floatuntidf 0");
   sysio_assert(to_f64_bits(__floatuntidf(1, 0)) == F64_ONE,  "floatuntidf 1");
   sysio_assert(to_f64_bits(__floatuntidf(2, 0)) == F64_TWO,  "floatuntidf 2");
}

void test_f128_builtins::test_int_f128_roundtrip() {
   // signed int32 roundtrip
   auto check_i32 = [](int32_t i) {
      __float128 r; __floatsitf(r, i);
      f128 v = to_f128(r);
      sysio_assert(__fixtfsi(v.lo, v.hi) == i, "i32 -> f128 -> i32 roundtrip");
   };
   check_i32(0); check_i32(1); check_i32(-1); check_i32(42); check_i32(-42);
   check_i32(0x7FFFFFFF); check_i32(static_cast<int32_t>(0x80000000));

   // unsigned uint32 roundtrip
   auto check_u32 = [](uint32_t i) {
      __float128 r; __floatunsitf(r, i);
      f128 v = to_f128(r);
      sysio_assert(__fixunstfsi(v.lo, v.hi) == i, "u32 -> f128 -> u32 roundtrip");
   };
   check_u32(0); check_u32(1); check_u32(42); check_u32(0xFFFFFFFFu);
}

// Comparisons (shape from cmptf2_impl: -1 if a<b, 0 if a==b, 1 if a>b; per-op
// NaN return value differs). Test uses BOOST_CHECK-style checks of returned
// integer codes, asserted via sysio_assert for in-WASM execution.
void test_f128_builtins::test_eqtf2() {
   sysio_assert(__eqtf2(F128_ONE.lo,  F128_ONE.hi,  F128_ONE.lo,  F128_ONE.hi)  == 0, "eqtf2 1==1");
   sysio_assert(__eqtf2(F128_ZERO.lo, F128_ZERO.hi, F128_ZERO.lo, F128_ZERO.hi) == 0, "eqtf2 0==0");
   sysio_assert(__eqtf2(F128_ZERO.lo, F128_ZERO.hi, F128_NEG_ZERO.lo, F128_NEG_ZERO.hi) == 0, "eqtf2 +0==-0");
   sysio_assert(__eqtf2(F128_ONE.lo,  F128_ONE.hi,  F128_TWO.lo,  F128_TWO.hi)  != 0, "eqtf2 1!=2");
   // NaN -> 1
   sysio_assert(__eqtf2(F128_QNAN.lo, F128_QNAN.hi, F128_ONE.lo,  F128_ONE.hi)  == 1, "eqtf2 NaN,1");
   sysio_assert(__eqtf2(F128_ONE.lo,  F128_ONE.hi,  F128_QNAN.lo, F128_QNAN.hi) == 1, "eqtf2 1,NaN");
   sysio_assert(__eqtf2(F128_QNAN.lo, F128_QNAN.hi, F128_QNAN.lo, F128_QNAN.hi) == 1, "eqtf2 NaN,NaN");
}

void test_f128_builtins::test_netf2() {
   sysio_assert(__netf2(F128_ONE.lo, F128_ONE.hi, F128_ONE.lo, F128_ONE.hi) == 0, "netf2 1==1 -> 0");
   sysio_assert(__netf2(F128_ONE.lo, F128_ONE.hi, F128_TWO.lo, F128_TWO.hi) != 0, "netf2 1!=2");
   sysio_assert(__netf2(F128_QNAN.lo, F128_QNAN.hi, F128_ONE.lo, F128_ONE.hi) == 1, "netf2 NaN -> 1");
}

void test_f128_builtins::test_getf2() {
   sysio_assert(__getf2(F128_TWO.lo,  F128_TWO.hi,  F128_ONE.lo, F128_ONE.hi)  == 1,  "getf2 2>1 -> 1");
   sysio_assert(__getf2(F128_ONE.lo,  F128_ONE.hi,  F128_ONE.lo, F128_ONE.hi)  == 0,  "getf2 1==1 -> 0");
   sysio_assert(__getf2(F128_ONE.lo,  F128_ONE.hi,  F128_TWO.lo, F128_TWO.hi)  == -1, "getf2 1<2 -> -1");
   // NaN -> -1
   sysio_assert(__getf2(F128_QNAN.lo, F128_QNAN.hi, F128_ONE.lo, F128_ONE.hi)  == -1, "getf2 NaN -> -1");
}

void test_f128_builtins::test_gttf2() {
   sysio_assert(__gttf2(F128_TWO.lo,  F128_TWO.hi,  F128_ONE.lo, F128_ONE.hi) == 1,  "gttf2 2>1");
   sysio_assert(__gttf2(F128_ONE.lo,  F128_ONE.hi,  F128_ONE.lo, F128_ONE.hi) == 0,  "gttf2 1==1");
   sysio_assert(__gttf2(F128_ONE.lo,  F128_ONE.hi,  F128_TWO.lo, F128_TWO.hi) == -1, "gttf2 1<2");
   // NaN -> 0
   sysio_assert(__gttf2(F128_QNAN.lo, F128_QNAN.hi, F128_ONE.lo, F128_ONE.hi) == 0,  "gttf2 NaN -> 0");
}

void test_f128_builtins::test_letf2() {
   sysio_assert(__letf2(F128_ONE.lo,  F128_ONE.hi,  F128_TWO.lo, F128_TWO.hi) == -1, "letf2 1<2");
   sysio_assert(__letf2(F128_ONE.lo,  F128_ONE.hi,  F128_ONE.lo, F128_ONE.hi) == 0,  "letf2 1==1");
   sysio_assert(__letf2(F128_TWO.lo,  F128_TWO.hi,  F128_ONE.lo, F128_ONE.hi) == 1,  "letf2 2>1");
   // NaN -> 1
   sysio_assert(__letf2(F128_QNAN.lo, F128_QNAN.hi, F128_ONE.lo, F128_ONE.hi) == 1,  "letf2 NaN -> 1");
}

void test_f128_builtins::test_lttf2() {
   sysio_assert(__lttf2(F128_ONE.lo,  F128_ONE.hi,  F128_TWO.lo, F128_TWO.hi) == -1, "lttf2 1<2");
   sysio_assert(__lttf2(F128_ONE.lo,  F128_ONE.hi,  F128_ONE.lo, F128_ONE.hi) == 0,  "lttf2 1==1");
   sysio_assert(__lttf2(F128_TWO.lo,  F128_TWO.hi,  F128_ONE.lo, F128_ONE.hi) == 1,  "lttf2 2>1");
   // NaN -> 0
   sysio_assert(__lttf2(F128_QNAN.lo, F128_QNAN.hi, F128_ONE.lo, F128_ONE.hi) == 0,  "lttf2 NaN -> 0");
}

void test_f128_builtins::test_cmptf2() {
   sysio_assert(__cmptf2(F128_ONE.lo,  F128_ONE.hi,  F128_TWO.lo, F128_TWO.hi) == -1, "cmptf2 1<2");
   sysio_assert(__cmptf2(F128_ONE.lo,  F128_ONE.hi,  F128_ONE.lo, F128_ONE.hi) == 0,  "cmptf2 1==1");
   sysio_assert(__cmptf2(F128_TWO.lo,  F128_TWO.hi,  F128_ONE.lo, F128_ONE.hi) == 1,  "cmptf2 2>1");
   // NaN -> 1
   sysio_assert(__cmptf2(F128_QNAN.lo, F128_QNAN.hi, F128_ONE.lo, F128_ONE.hi) == 1,  "cmptf2 NaN -> 1");
   // Edge cases
   sysio_assert(__cmptf2(F128_POS_INF.lo, F128_POS_INF.hi, F128_ONE.lo,    F128_ONE.hi)    == 1,  "cmptf2 +inf>1");
   sysio_assert(__cmptf2(F128_NEG_INF.lo, F128_NEG_INF.hi, F128_ONE.lo,    F128_ONE.hi)    == -1, "cmptf2 -inf<1");
   sysio_assert(__cmptf2(F128_NEG_ONE.lo, F128_NEG_ONE.hi, F128_ONE.lo,    F128_ONE.hi)    == -1, "cmptf2 -1<1");
   sysio_assert(__cmptf2(F128_ONE.lo,     F128_ONE.hi,     F128_NEG_ONE.lo,F128_NEG_ONE.hi)== 1,  "cmptf2 1>-1");
}

void test_f128_builtins::test_unordtf2() {
   sysio_assert(__unordtf2(F128_ONE.lo,     F128_ONE.hi,     F128_TWO.lo,     F128_TWO.hi)     == 0, "unordtf2 1,2 ordered");
   sysio_assert(__unordtf2(F128_ZERO.lo,    F128_ZERO.hi,    F128_ZERO.lo,    F128_ZERO.hi)    == 0, "unordtf2 0,0 ordered");
   sysio_assert(__unordtf2(F128_POS_INF.lo, F128_POS_INF.hi, F128_NEG_INF.lo, F128_NEG_INF.hi) == 0, "unordtf2 +inf,-inf ordered");
   sysio_assert(__unordtf2(F128_QNAN.lo,    F128_QNAN.hi,    F128_ONE.lo,     F128_ONE.hi)     != 0, "unordtf2 NaN,1 unordered");
   sysio_assert(__unordtf2(F128_ONE.lo,     F128_ONE.hi,     F128_QNAN.lo,    F128_QNAN.hi)    != 0, "unordtf2 1,NaN unordered");
   sysio_assert(__unordtf2(F128_QNAN.lo,    F128_QNAN.hi,    F128_QNAN.lo,    F128_QNAN.hi)    != 0, "unordtf2 NaN,NaN unordered");
}

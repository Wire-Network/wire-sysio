/**
 * Host-side golden-value tests for all 58 softfloat f32/f64 host functions.
 *
 * The existing WAST tests (test_softfloat_wasts.hpp, 21k lines) run through
 * the WASM runtime and verify behavior at the WASM level. These host-side tests
 * complement them by asserting exact bit-pattern outputs directly, running
 * without WASM overhead, and catching host implementation changes that WAST
 * tests might miss.
 *
 * Uses the builtin_harness pattern (fake apply_context) because all softfloat
 * methods are const and never dereference the context pointer.
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
// IEEE 754 bit-pattern constants
// ============================================================================

// --- float32 ---
constexpr uint32_t F32_POS_ZERO =  0x00000000u;
constexpr uint32_t F32_NEG_ZERO =  0x80000000u;
constexpr uint32_t F32_ONE      =  0x3F800000u; // 1.0f
constexpr uint32_t F32_NEG_ONE  =  0xBF800000u; // -1.0f
constexpr uint32_t F32_TWO      =  0x40000000u; // 2.0f
constexpr uint32_t F32_NEG_TWO  =  0xC0000000u; // -2.0f
constexpr uint32_t F32_THREE    =  0x40400000u; // 3.0f
constexpr uint32_t F32_FOUR     =  0x40800000u; // 4.0f
constexpr uint32_t F32_HALF     =  0x3F000000u; // 0.5f
constexpr uint32_t F32_SIX      =  0x40C00000u; // 6.0f
constexpr uint32_t F32_ONE_PT_5 =  0x3FC00000u; // 1.5f
constexpr uint32_t F32_TWO_PT_5 =  0x40200000u; // 2.5f
constexpr uint32_t F32_THREE_PT_5 = 0x40600000u; // 3.5f
constexpr uint32_t F32_NEG_ONE_PT_5 = 0xBFC00000u; // -1.5f
constexpr uint32_t F32_POS_INF  =  0x7F800000u;
constexpr uint32_t F32_NEG_INF  =  0xFF800000u;
constexpr uint32_t F32_QNAN     =  0x7FC00000u;

// --- float64 ---
constexpr uint64_t F64_POS_ZERO =  0x0000000000000000ULL;
constexpr uint64_t F64_NEG_ZERO =  0x8000000000000000ULL;
constexpr uint64_t F64_ONE      =  0x3FF0000000000000ULL;
constexpr uint64_t F64_NEG_ONE  =  0xBFF0000000000000ULL;
constexpr uint64_t F64_TWO      =  0x4000000000000000ULL;
constexpr uint64_t F64_NEG_TWO  =  0xC000000000000000ULL;
constexpr uint64_t F64_THREE    =  0x4008000000000000ULL;
constexpr uint64_t F64_FOUR     =  0x4010000000000000ULL;
constexpr uint64_t F64_HALF     =  0x3FE0000000000000ULL;
constexpr uint64_t F64_SIX      =  0x4018000000000000ULL;
constexpr uint64_t F64_ONE_PT_5 =  0x3FF8000000000000ULL;
constexpr uint64_t F64_TWO_PT_5 =  0x4004000000000000ULL;
constexpr uint64_t F64_THREE_PT_5 = 0x400C000000000000ULL;
constexpr uint64_t F64_NEG_ONE_PT_5 = 0xBFF8000000000000ULL;
constexpr uint64_t F64_POS_INF  =  0x7FF0000000000000ULL;
constexpr uint64_t F64_NEG_INF  =  0xFFF0000000000000ULL;
constexpr uint64_t F64_QNAN     =  0x7FF8000000000000ULL;

// ============================================================================
// Helpers - type-pun float/double to/from bit patterns
// ============================================================================

uint32_t fb(float f)    { uint32_t b; std::memcpy(&b, &f, 4); return b; }
uint64_t db(double d)   { uint64_t b; std::memcpy(&b, &d, 8); return b; }
float    bf(uint32_t b) { float f;    std::memcpy(&f, &b, 4); return f; }
double   bd(uint64_t b) { double d;   std::memcpy(&d, &b, 8); return d; }

bool is_f32_nan(uint32_t b) { return (b & 0x7F800000u) == 0x7F800000u && (b & 0x007FFFFFu) != 0; }
bool is_f64_nan(uint64_t b) { return (b & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL && (b & 0x000FFFFFFFFFFFFFULL) != 0; }

// ============================================================================
// Minimal test harness
// ============================================================================

struct sf_harness {
   alignas(apply_context) char storage[sizeof(apply_context)]{};
   interface iface{reinterpret_cast<apply_context&>(storage)};
};

} // anonymous namespace

// ============================================================================
//  F32 Arithmetic: _sysio_f32_add, _sysio_f32_sub, _sysio_f32_mul, _sysio_f32_div
// ============================================================================

BOOST_AUTO_TEST_SUITE(softfloat_golden_tests)

BOOST_AUTO_TEST_CASE(f32_add_golden) {
   sf_harness h;
   auto add = [&](float a, float b) { return fb(h.iface._sysio_f32_add(a, b)); };
   // Basic
   BOOST_CHECK_EQUAL(add(bf(F32_ONE), bf(F32_TWO)), F32_THREE);
   BOOST_CHECK_EQUAL(add(bf(F32_ONE), bf(F32_NEG_ONE)), F32_POS_ZERO);
   BOOST_CHECK_EQUAL(add(bf(F32_HALF), bf(F32_HALF)), F32_ONE);
   BOOST_CHECK_EQUAL(add(bf(F32_POS_ZERO), bf(F32_POS_ZERO)), F32_POS_ZERO);
   // Inf
   BOOST_CHECK_EQUAL(add(bf(F32_POS_INF), bf(F32_ONE)), F32_POS_INF);
   BOOST_CHECK(is_f32_nan(add(bf(F32_POS_INF), bf(F32_NEG_INF))));
   // NaN propagation
   BOOST_CHECK(is_f32_nan(add(bf(F32_QNAN), bf(F32_ONE))));
}

BOOST_AUTO_TEST_CASE(f32_sub_golden) {
   sf_harness h;
   auto sub = [&](float a, float b) { return fb(h.iface._sysio_f32_sub(a, b)); };
   BOOST_CHECK_EQUAL(sub(bf(F32_THREE), bf(F32_TWO)), F32_ONE);
   BOOST_CHECK_EQUAL(sub(bf(F32_ONE), bf(F32_TWO)), F32_NEG_ONE);
   BOOST_CHECK_EQUAL(sub(bf(F32_ONE), bf(F32_ONE)), F32_POS_ZERO);
   BOOST_CHECK(is_f32_nan(sub(bf(F32_POS_INF), bf(F32_POS_INF))));
}

BOOST_AUTO_TEST_CASE(f32_mul_golden) {
   sf_harness h;
   auto mul = [&](float a, float b) { return fb(h.iface._sysio_f32_mul(a, b)); };
   BOOST_CHECK_EQUAL(mul(bf(F32_TWO), bf(F32_THREE)), F32_SIX);
   BOOST_CHECK_EQUAL(mul(bf(F32_TWO), bf(F32_HALF)), F32_ONE);
   BOOST_CHECK_EQUAL(mul(bf(F32_NEG_ONE), bf(F32_NEG_ONE)), F32_ONE);
   BOOST_CHECK_EQUAL(mul(bf(F32_TWO), bf(F32_POS_ZERO)), F32_POS_ZERO);
   BOOST_CHECK(is_f32_nan(mul(bf(F32_POS_INF), bf(F32_POS_ZERO))));
}

BOOST_AUTO_TEST_CASE(f32_div_golden) {
   sf_harness h;
   auto div = [&](float a, float b) { return fb(h.iface._sysio_f32_div(a, b)); };
   BOOST_CHECK_EQUAL(div(bf(F32_SIX), bf(F32_TWO)), F32_THREE);
   BOOST_CHECK_EQUAL(div(bf(F32_ONE), bf(F32_TWO)), F32_HALF);
   BOOST_CHECK_EQUAL(div(bf(F32_ONE), bf(F32_ONE)), F32_ONE);
   BOOST_CHECK_EQUAL(div(bf(F32_ONE), bf(F32_POS_ZERO)), F32_POS_INF);
   BOOST_CHECK_EQUAL(div(bf(F32_NEG_ONE), bf(F32_POS_ZERO)), F32_NEG_INF);
   BOOST_CHECK(is_f32_nan(div(bf(F32_POS_ZERO), bf(F32_POS_ZERO))));
}

// ============================================================================
//  F32 Unary: _sysio_f32_sqrt, _sysio_f32_abs, _sysio_f32_neg
// ============================================================================

BOOST_AUTO_TEST_CASE(f32_sqrt_golden) {
   sf_harness h;
   auto sq = [&](float a) { return fb(h.iface._sysio_f32_sqrt(a)); };
   BOOST_CHECK_EQUAL(sq(bf(F32_FOUR)), F32_TWO);
   BOOST_CHECK_EQUAL(sq(bf(F32_ONE)), F32_ONE);
   BOOST_CHECK_EQUAL(sq(bf(F32_POS_ZERO)), F32_POS_ZERO);
   BOOST_CHECK_EQUAL(sq(bf(F32_POS_INF)), F32_POS_INF);
   // sqrt(negative) = NaN
   BOOST_CHECK(is_f32_nan(sq(bf(F32_NEG_ONE))));
   BOOST_CHECK(is_f32_nan(sq(bf(F32_QNAN))));
}

BOOST_AUTO_TEST_CASE(f32_abs_golden) {
   sf_harness h;
   auto abs = [&](float a) { return fb(h.iface._sysio_f32_abs(a)); };
   BOOST_CHECK_EQUAL(abs(bf(F32_ONE)), F32_ONE);
   BOOST_CHECK_EQUAL(abs(bf(F32_NEG_ONE)), F32_ONE);
   BOOST_CHECK_EQUAL(abs(bf(F32_POS_ZERO)), F32_POS_ZERO);
   BOOST_CHECK_EQUAL(abs(bf(F32_NEG_ZERO)), F32_POS_ZERO);
   BOOST_CHECK_EQUAL(abs(bf(F32_NEG_INF)), F32_POS_INF);
}

BOOST_AUTO_TEST_CASE(f32_neg_golden) {
   sf_harness h;
   auto neg = [&](float a) { return fb(h.iface._sysio_f32_neg(a)); };
   BOOST_CHECK_EQUAL(neg(bf(F32_ONE)), F32_NEG_ONE);
   BOOST_CHECK_EQUAL(neg(bf(F32_NEG_ONE)), F32_ONE);
   BOOST_CHECK_EQUAL(neg(bf(F32_POS_ZERO)), F32_NEG_ZERO);
   BOOST_CHECK_EQUAL(neg(bf(F32_NEG_ZERO)), F32_POS_ZERO);
   BOOST_CHECK_EQUAL(neg(bf(F32_POS_INF)), F32_NEG_INF);
}

// ============================================================================
//  F32 Min/Max/Copysign
// ============================================================================

BOOST_AUTO_TEST_CASE(f32_min_golden) {
   sf_harness h;
   auto mn = [&](float a, float b) { return fb(h.iface._sysio_f32_min(a, b)); };
   BOOST_CHECK_EQUAL(mn(bf(F32_ONE), bf(F32_TWO)), F32_ONE);
   BOOST_CHECK_EQUAL(mn(bf(F32_TWO), bf(F32_ONE)), F32_ONE);
   BOOST_CHECK_EQUAL(mn(bf(F32_NEG_ONE), bf(F32_ONE)), F32_NEG_ONE);
   // min(+0, -0) = -0 per IEEE 754-2019
   BOOST_CHECK_EQUAL(mn(bf(F32_POS_ZERO), bf(F32_NEG_ZERO)), F32_NEG_ZERO);
   BOOST_CHECK_EQUAL(mn(bf(F32_NEG_ZERO), bf(F32_POS_ZERO)), F32_NEG_ZERO);
   // NaN propagation
   BOOST_CHECK(is_f32_nan(mn(bf(F32_QNAN), bf(F32_ONE))));
   BOOST_CHECK(is_f32_nan(mn(bf(F32_ONE), bf(F32_QNAN))));
}

BOOST_AUTO_TEST_CASE(f32_max_golden) {
   sf_harness h;
   auto mx = [&](float a, float b) { return fb(h.iface._sysio_f32_max(a, b)); };
   BOOST_CHECK_EQUAL(mx(bf(F32_ONE), bf(F32_TWO)), F32_TWO);
   BOOST_CHECK_EQUAL(mx(bf(F32_TWO), bf(F32_ONE)), F32_TWO);
   BOOST_CHECK_EQUAL(mx(bf(F32_NEG_ONE), bf(F32_ONE)), F32_ONE);
   // max(+0, -0) = +0
   BOOST_CHECK_EQUAL(mx(bf(F32_POS_ZERO), bf(F32_NEG_ZERO)), F32_POS_ZERO);
   BOOST_CHECK_EQUAL(mx(bf(F32_NEG_ZERO), bf(F32_POS_ZERO)), F32_POS_ZERO);
   BOOST_CHECK(is_f32_nan(mx(bf(F32_QNAN), bf(F32_ONE))));
}

BOOST_AUTO_TEST_CASE(f32_copysign_golden) {
   sf_harness h;
   auto cs = [&](float a, float b) { return fb(h.iface._sysio_f32_copysign(a, b)); };
   // Takes magnitude of a, sign of b
   BOOST_CHECK_EQUAL(cs(bf(F32_ONE), bf(F32_NEG_TWO)), F32_NEG_ONE);
   BOOST_CHECK_EQUAL(cs(bf(F32_NEG_ONE), bf(F32_TWO)), F32_ONE);
   BOOST_CHECK_EQUAL(cs(bf(F32_ONE), bf(F32_TWO)), F32_ONE);
   BOOST_CHECK_EQUAL(cs(bf(F32_NEG_ONE), bf(F32_NEG_TWO)), F32_NEG_ONE);
}

// ============================================================================
//  F32 Rounding: ceil, floor, trunc, nearest
// ============================================================================

BOOST_AUTO_TEST_CASE(f32_ceil_golden) {
   sf_harness h;
   auto ceil = [&](float a) { return fb(h.iface._sysio_f32_ceil(a)); };
   BOOST_CHECK_EQUAL(ceil(bf(F32_ONE_PT_5)), F32_TWO);
   BOOST_CHECK_EQUAL(ceil(bf(F32_NEG_ONE_PT_5)), F32_NEG_ONE);
   BOOST_CHECK_EQUAL(ceil(bf(F32_ONE)), F32_ONE);
   BOOST_CHECK_EQUAL(ceil(bf(F32_HALF)), F32_ONE);
   BOOST_CHECK_EQUAL(ceil(bf(F32_POS_ZERO)), F32_POS_ZERO);
   BOOST_CHECK_EQUAL(ceil(bf(F32_NEG_ZERO)), F32_NEG_ZERO);
   BOOST_CHECK_EQUAL(ceil(bf(F32_POS_INF)), F32_POS_INF);
}

BOOST_AUTO_TEST_CASE(f32_floor_golden) {
   sf_harness h;
   auto floor = [&](float a) { return fb(h.iface._sysio_f32_floor(a)); };
   BOOST_CHECK_EQUAL(floor(bf(F32_ONE_PT_5)), F32_ONE);
   BOOST_CHECK_EQUAL(floor(bf(F32_NEG_ONE_PT_5)), F32_NEG_TWO);
   BOOST_CHECK_EQUAL(floor(bf(F32_ONE)), F32_ONE);
   BOOST_CHECK_EQUAL(floor(bf(F32_HALF)), F32_POS_ZERO);
   BOOST_CHECK_EQUAL(floor(bf(F32_POS_ZERO)), F32_POS_ZERO);
   BOOST_CHECK_EQUAL(floor(bf(F32_POS_INF)), F32_POS_INF);
}

BOOST_AUTO_TEST_CASE(f32_trunc_golden) {
   sf_harness h;
   auto trunc = [&](float a) { return fb(h.iface._sysio_f32_trunc(a)); };
   BOOST_CHECK_EQUAL(trunc(bf(F32_ONE_PT_5)), F32_ONE);
   BOOST_CHECK_EQUAL(trunc(bf(F32_NEG_ONE_PT_5)), F32_NEG_ONE);
   BOOST_CHECK_EQUAL(trunc(bf(F32_ONE)), F32_ONE);
   BOOST_CHECK_EQUAL(trunc(bf(F32_POS_ZERO)), F32_POS_ZERO);
   BOOST_CHECK_EQUAL(trunc(bf(F32_POS_INF)), F32_POS_INF);
}

BOOST_AUTO_TEST_CASE(f32_nearest_golden) {
   sf_harness h;
   auto near = [&](float a) { return fb(h.iface._sysio_f32_nearest(a)); };
   // Banker's rounding: round half to even
   BOOST_CHECK_EQUAL(near(bf(F32_ONE_PT_5)), F32_TWO);      // 1.5 → 2 (even)
   BOOST_CHECK_EQUAL(near(bf(F32_TWO_PT_5)), F32_TWO);      // 2.5 → 2 (even)
   BOOST_CHECK_EQUAL(near(bf(F32_THREE_PT_5)), F32_FOUR);   // 3.5 → 4 (even)
   BOOST_CHECK_EQUAL(near(bf(F32_ONE)), F32_ONE);
   BOOST_CHECK_EQUAL(near(bf(F32_POS_ZERO)), F32_POS_ZERO);
   BOOST_CHECK_EQUAL(near(bf(F32_NEG_ZERO)), F32_NEG_ZERO);
   BOOST_CHECK_EQUAL(near(bf(F32_POS_INF)), F32_POS_INF);
}

// ============================================================================
//  F32 Comparison: eq, ne, lt, le, gt, ge
// ============================================================================

BOOST_AUTO_TEST_CASE(f32_comparison_golden) {
   sf_harness h;
   auto& i = h.iface;
   float one = bf(F32_ONE), two = bf(F32_TWO), nan = bf(F32_QNAN);
   float pz = bf(F32_POS_ZERO), nz = bf(F32_NEG_ZERO);

   // eq
   BOOST_CHECK(i._sysio_f32_eq(one, one));
   BOOST_CHECK(!i._sysio_f32_eq(one, two));
   BOOST_CHECK(i._sysio_f32_eq(pz, nz));  // +0 == -0
   BOOST_CHECK(!i._sysio_f32_eq(nan, one));
   BOOST_CHECK(!i._sysio_f32_eq(nan, nan));

   // ne
   BOOST_CHECK(!i._sysio_f32_ne(one, one));
   BOOST_CHECK(i._sysio_f32_ne(one, two));
   BOOST_CHECK(i._sysio_f32_ne(nan, nan)); // NaN != NaN

   // lt
   BOOST_CHECK(i._sysio_f32_lt(one, two));
   BOOST_CHECK(!i._sysio_f32_lt(two, one));
   BOOST_CHECK(!i._sysio_f32_lt(one, one));
   BOOST_CHECK(!i._sysio_f32_lt(nan, one));

   // le
   BOOST_CHECK(i._sysio_f32_le(one, two));
   BOOST_CHECK(i._sysio_f32_le(one, one));
   BOOST_CHECK(!i._sysio_f32_le(two, one));
   BOOST_CHECK(!i._sysio_f32_le(nan, one));

   // gt
   BOOST_CHECK(i._sysio_f32_gt(two, one));
   BOOST_CHECK(!i._sysio_f32_gt(one, two));
   BOOST_CHECK(!i._sysio_f32_gt(one, one));
   BOOST_CHECK(!i._sysio_f32_gt(nan, one)); // NaN → false

   // ge
   BOOST_CHECK(i._sysio_f32_ge(two, one));
   BOOST_CHECK(i._sysio_f32_ge(one, one));
   BOOST_CHECK(!i._sysio_f32_ge(one, two));
   BOOST_CHECK(!i._sysio_f32_ge(nan, one)); // NaN → false
}

// ============================================================================
//  F64 Arithmetic: _sysio_f64_add, _sysio_f64_sub, _sysio_f64_mul, _sysio_f64_div
// ============================================================================

BOOST_AUTO_TEST_CASE(f64_add_golden) {
   sf_harness h;
   auto add = [&](double a, double b) { return db(h.iface._sysio_f64_add(a, b)); };
   BOOST_CHECK_EQUAL(add(bd(F64_ONE), bd(F64_TWO)), F64_THREE);
   BOOST_CHECK_EQUAL(add(bd(F64_ONE), bd(F64_NEG_ONE)), F64_POS_ZERO);
   BOOST_CHECK_EQUAL(add(bd(F64_HALF), bd(F64_HALF)), F64_ONE);
   BOOST_CHECK_EQUAL(add(bd(F64_POS_INF), bd(F64_ONE)), F64_POS_INF);
   BOOST_CHECK(is_f64_nan(add(bd(F64_POS_INF), bd(F64_NEG_INF))));
   BOOST_CHECK(is_f64_nan(add(bd(F64_QNAN), bd(F64_ONE))));
}

BOOST_AUTO_TEST_CASE(f64_sub_golden) {
   sf_harness h;
   auto sub = [&](double a, double b) { return db(h.iface._sysio_f64_sub(a, b)); };
   BOOST_CHECK_EQUAL(sub(bd(F64_THREE), bd(F64_TWO)), F64_ONE);
   BOOST_CHECK_EQUAL(sub(bd(F64_ONE), bd(F64_TWO)), F64_NEG_ONE);
   BOOST_CHECK_EQUAL(sub(bd(F64_ONE), bd(F64_ONE)), F64_POS_ZERO);
   BOOST_CHECK(is_f64_nan(sub(bd(F64_POS_INF), bd(F64_POS_INF))));
}

BOOST_AUTO_TEST_CASE(f64_mul_golden) {
   sf_harness h;
   auto mul = [&](double a, double b) { return db(h.iface._sysio_f64_mul(a, b)); };
   BOOST_CHECK_EQUAL(mul(bd(F64_TWO), bd(F64_THREE)), F64_SIX);
   BOOST_CHECK_EQUAL(mul(bd(F64_TWO), bd(F64_HALF)), F64_ONE);
   BOOST_CHECK_EQUAL(mul(bd(F64_NEG_ONE), bd(F64_NEG_ONE)), F64_ONE);
   BOOST_CHECK_EQUAL(mul(bd(F64_TWO), bd(F64_POS_ZERO)), F64_POS_ZERO);
   BOOST_CHECK(is_f64_nan(mul(bd(F64_POS_INF), bd(F64_POS_ZERO))));
}

BOOST_AUTO_TEST_CASE(f64_div_golden) {
   sf_harness h;
   auto div = [&](double a, double b) { return db(h.iface._sysio_f64_div(a, b)); };
   BOOST_CHECK_EQUAL(div(bd(F64_SIX), bd(F64_TWO)), F64_THREE);
   BOOST_CHECK_EQUAL(div(bd(F64_ONE), bd(F64_TWO)), F64_HALF);
   BOOST_CHECK_EQUAL(div(bd(F64_ONE), bd(F64_ONE)), F64_ONE);
   BOOST_CHECK_EQUAL(div(bd(F64_ONE), bd(F64_POS_ZERO)), F64_POS_INF);
   BOOST_CHECK_EQUAL(div(bd(F64_NEG_ONE), bd(F64_POS_ZERO)), F64_NEG_INF);
   BOOST_CHECK(is_f64_nan(div(bd(F64_POS_ZERO), bd(F64_POS_ZERO))));
}

// ============================================================================
//  F64 Unary: sqrt, abs, neg
// ============================================================================

BOOST_AUTO_TEST_CASE(f64_sqrt_golden) {
   sf_harness h;
   auto sq = [&](double a) { return db(h.iface._sysio_f64_sqrt(a)); };
   BOOST_CHECK_EQUAL(sq(bd(F64_FOUR)), F64_TWO);
   BOOST_CHECK_EQUAL(sq(bd(F64_ONE)), F64_ONE);
   BOOST_CHECK_EQUAL(sq(bd(F64_POS_ZERO)), F64_POS_ZERO);
   BOOST_CHECK_EQUAL(sq(bd(F64_POS_INF)), F64_POS_INF);
   BOOST_CHECK(is_f64_nan(sq(bd(F64_NEG_ONE))));
}

BOOST_AUTO_TEST_CASE(f64_abs_golden) {
   sf_harness h;
   auto abs = [&](double a) { return db(h.iface._sysio_f64_abs(a)); };
   BOOST_CHECK_EQUAL(abs(bd(F64_ONE)), F64_ONE);
   BOOST_CHECK_EQUAL(abs(bd(F64_NEG_ONE)), F64_ONE);
   BOOST_CHECK_EQUAL(abs(bd(F64_POS_ZERO)), F64_POS_ZERO);
   BOOST_CHECK_EQUAL(abs(bd(F64_NEG_ZERO)), F64_POS_ZERO);
   BOOST_CHECK_EQUAL(abs(bd(F64_NEG_INF)), F64_POS_INF);
}

BOOST_AUTO_TEST_CASE(f64_neg_golden) {
   sf_harness h;
   auto neg = [&](double a) { return db(h.iface._sysio_f64_neg(a)); };
   BOOST_CHECK_EQUAL(neg(bd(F64_ONE)), F64_NEG_ONE);
   BOOST_CHECK_EQUAL(neg(bd(F64_NEG_ONE)), F64_ONE);
   BOOST_CHECK_EQUAL(neg(bd(F64_POS_ZERO)), F64_NEG_ZERO);
   BOOST_CHECK_EQUAL(neg(bd(F64_NEG_ZERO)), F64_POS_ZERO);
   BOOST_CHECK_EQUAL(neg(bd(F64_POS_INF)), F64_NEG_INF);
}

// ============================================================================
//  F64 Min/Max/Copysign
// ============================================================================

BOOST_AUTO_TEST_CASE(f64_min_golden) {
   sf_harness h;
   auto mn = [&](double a, double b) { return db(h.iface._sysio_f64_min(a, b)); };
   BOOST_CHECK_EQUAL(mn(bd(F64_ONE), bd(F64_TWO)), F64_ONE);
   BOOST_CHECK_EQUAL(mn(bd(F64_TWO), bd(F64_ONE)), F64_ONE);
   BOOST_CHECK_EQUAL(mn(bd(F64_NEG_ONE), bd(F64_ONE)), F64_NEG_ONE);
   BOOST_CHECK_EQUAL(mn(bd(F64_POS_ZERO), bd(F64_NEG_ZERO)), F64_NEG_ZERO);
   BOOST_CHECK_EQUAL(mn(bd(F64_NEG_ZERO), bd(F64_POS_ZERO)), F64_NEG_ZERO);
   BOOST_CHECK(is_f64_nan(mn(bd(F64_QNAN), bd(F64_ONE))));
   BOOST_CHECK(is_f64_nan(mn(bd(F64_ONE), bd(F64_QNAN))));
}

BOOST_AUTO_TEST_CASE(f64_max_golden) {
   sf_harness h;
   auto mx = [&](double a, double b) { return db(h.iface._sysio_f64_max(a, b)); };
   BOOST_CHECK_EQUAL(mx(bd(F64_ONE), bd(F64_TWO)), F64_TWO);
   BOOST_CHECK_EQUAL(mx(bd(F64_TWO), bd(F64_ONE)), F64_TWO);
   BOOST_CHECK_EQUAL(mx(bd(F64_NEG_ONE), bd(F64_ONE)), F64_ONE);
   BOOST_CHECK_EQUAL(mx(bd(F64_POS_ZERO), bd(F64_NEG_ZERO)), F64_POS_ZERO);
   BOOST_CHECK_EQUAL(mx(bd(F64_NEG_ZERO), bd(F64_POS_ZERO)), F64_POS_ZERO);
   BOOST_CHECK(is_f64_nan(mx(bd(F64_QNAN), bd(F64_ONE))));
}

BOOST_AUTO_TEST_CASE(f64_copysign_golden) {
   sf_harness h;
   auto cs = [&](double a, double b) { return db(h.iface._sysio_f64_copysign(a, b)); };
   BOOST_CHECK_EQUAL(cs(bd(F64_ONE), bd(F64_NEG_TWO)), F64_NEG_ONE);
   BOOST_CHECK_EQUAL(cs(bd(F64_NEG_ONE), bd(F64_TWO)), F64_ONE);
   BOOST_CHECK_EQUAL(cs(bd(F64_ONE), bd(F64_TWO)), F64_ONE);
   BOOST_CHECK_EQUAL(cs(bd(F64_NEG_ONE), bd(F64_NEG_TWO)), F64_NEG_ONE);
}

// ============================================================================
//  F64 Rounding: ceil, floor, trunc, nearest
// ============================================================================

BOOST_AUTO_TEST_CASE(f64_ceil_golden) {
   sf_harness h;
   auto ceil = [&](double a) { return db(h.iface._sysio_f64_ceil(a)); };
   BOOST_CHECK_EQUAL(ceil(bd(F64_ONE_PT_5)), F64_TWO);
   BOOST_CHECK_EQUAL(ceil(bd(F64_NEG_ONE_PT_5)), F64_NEG_ONE);
   BOOST_CHECK_EQUAL(ceil(bd(F64_ONE)), F64_ONE);
   BOOST_CHECK_EQUAL(ceil(bd(F64_HALF)), F64_ONE);
   BOOST_CHECK_EQUAL(ceil(bd(F64_POS_ZERO)), F64_POS_ZERO);
   BOOST_CHECK_EQUAL(ceil(bd(F64_NEG_ZERO)), F64_NEG_ZERO);
   BOOST_CHECK_EQUAL(ceil(bd(F64_POS_INF)), F64_POS_INF);
}

BOOST_AUTO_TEST_CASE(f64_floor_golden) {
   sf_harness h;
   auto floor = [&](double a) { return db(h.iface._sysio_f64_floor(a)); };
   BOOST_CHECK_EQUAL(floor(bd(F64_ONE_PT_5)), F64_ONE);
   BOOST_CHECK_EQUAL(floor(bd(F64_NEG_ONE_PT_5)), F64_NEG_TWO);
   BOOST_CHECK_EQUAL(floor(bd(F64_ONE)), F64_ONE);
   BOOST_CHECK_EQUAL(floor(bd(F64_HALF)), F64_POS_ZERO);
   BOOST_CHECK_EQUAL(floor(bd(F64_POS_ZERO)), F64_POS_ZERO);
   BOOST_CHECK_EQUAL(floor(bd(F64_POS_INF)), F64_POS_INF);
}

BOOST_AUTO_TEST_CASE(f64_trunc_golden) {
   sf_harness h;
   auto trunc = [&](double a) { return db(h.iface._sysio_f64_trunc(a)); };
   BOOST_CHECK_EQUAL(trunc(bd(F64_ONE_PT_5)), F64_ONE);
   BOOST_CHECK_EQUAL(trunc(bd(F64_NEG_ONE_PT_5)), F64_NEG_ONE);
   BOOST_CHECK_EQUAL(trunc(bd(F64_ONE)), F64_ONE);
   BOOST_CHECK_EQUAL(trunc(bd(F64_POS_ZERO)), F64_POS_ZERO);
   BOOST_CHECK_EQUAL(trunc(bd(F64_POS_INF)), F64_POS_INF);
}

BOOST_AUTO_TEST_CASE(f64_nearest_golden) {
   sf_harness h;
   auto near = [&](double a) { return db(h.iface._sysio_f64_nearest(a)); };
   BOOST_CHECK_EQUAL(near(bd(F64_ONE_PT_5)), F64_TWO);      // 1.5 → 2 (even)
   BOOST_CHECK_EQUAL(near(bd(F64_TWO_PT_5)), F64_TWO);      // 2.5 → 2 (even)
   BOOST_CHECK_EQUAL(near(bd(F64_THREE_PT_5)), F64_FOUR);   // 3.5 → 4 (even)
   BOOST_CHECK_EQUAL(near(bd(F64_ONE)), F64_ONE);
   BOOST_CHECK_EQUAL(near(bd(F64_POS_ZERO)), F64_POS_ZERO);
   BOOST_CHECK_EQUAL(near(bd(F64_NEG_ZERO)), F64_NEG_ZERO);
   BOOST_CHECK_EQUAL(near(bd(F64_POS_INF)), F64_POS_INF);
}

// ============================================================================
//  F64 Comparison: eq, ne, lt, le, gt, ge
// ============================================================================

BOOST_AUTO_TEST_CASE(f64_comparison_golden) {
   sf_harness h;
   auto& i = h.iface;
   double one = bd(F64_ONE), two = bd(F64_TWO), nan = bd(F64_QNAN);
   double pz = bd(F64_POS_ZERO), nz = bd(F64_NEG_ZERO);

   BOOST_CHECK(i._sysio_f64_eq(one, one));
   BOOST_CHECK(!i._sysio_f64_eq(one, two));
   BOOST_CHECK(i._sysio_f64_eq(pz, nz));
   BOOST_CHECK(!i._sysio_f64_eq(nan, one));
   BOOST_CHECK(!i._sysio_f64_eq(nan, nan));

   BOOST_CHECK(!i._sysio_f64_ne(one, one));
   BOOST_CHECK(i._sysio_f64_ne(one, two));
   BOOST_CHECK(i._sysio_f64_ne(nan, nan));

   BOOST_CHECK(i._sysio_f64_lt(one, two));
   BOOST_CHECK(!i._sysio_f64_lt(two, one));
   BOOST_CHECK(!i._sysio_f64_lt(one, one));
   BOOST_CHECK(!i._sysio_f64_lt(nan, one));

   BOOST_CHECK(i._sysio_f64_le(one, two));
   BOOST_CHECK(i._sysio_f64_le(one, one));
   BOOST_CHECK(!i._sysio_f64_le(two, one));
   BOOST_CHECK(!i._sysio_f64_le(nan, one));

   BOOST_CHECK(i._sysio_f64_gt(two, one));
   BOOST_CHECK(!i._sysio_f64_gt(one, two));
   BOOST_CHECK(!i._sysio_f64_gt(one, one));
   BOOST_CHECK(!i._sysio_f64_gt(nan, one));

   BOOST_CHECK(i._sysio_f64_ge(two, one));
   BOOST_CHECK(i._sysio_f64_ge(one, one));
   BOOST_CHECK(!i._sysio_f64_ge(one, two));
   BOOST_CHECK(!i._sysio_f64_ge(nan, one));
}

// ============================================================================
//  Conversions: f32 ↔ f64
// ============================================================================

BOOST_AUTO_TEST_CASE(f32_promote_golden) {
   sf_harness h;
   auto promo = [&](float a) { return db(h.iface._sysio_f32_promote(a)); };
   BOOST_CHECK_EQUAL(promo(bf(F32_ONE)), F64_ONE);
   BOOST_CHECK_EQUAL(promo(bf(F32_NEG_ONE)), F64_NEG_ONE);
   BOOST_CHECK_EQUAL(promo(bf(F32_POS_ZERO)), F64_POS_ZERO);
   BOOST_CHECK_EQUAL(promo(bf(F32_NEG_ZERO)), F64_NEG_ZERO);
   BOOST_CHECK_EQUAL(promo(bf(F32_TWO)), F64_TWO);
   BOOST_CHECK_EQUAL(promo(bf(F32_POS_INF)), F64_POS_INF);
   BOOST_CHECK(is_f64_nan(promo(bf(F32_QNAN))));
}

BOOST_AUTO_TEST_CASE(f64_demote_golden) {
   sf_harness h;
   auto demo = [&](double a) { return fb(h.iface._sysio_f64_demote(a)); };
   BOOST_CHECK_EQUAL(demo(bd(F64_ONE)), F32_ONE);
   BOOST_CHECK_EQUAL(demo(bd(F64_NEG_ONE)), F32_NEG_ONE);
   BOOST_CHECK_EQUAL(demo(bd(F64_POS_ZERO)), F32_POS_ZERO);
   BOOST_CHECK_EQUAL(demo(bd(F64_NEG_ZERO)), F32_NEG_ZERO);
   BOOST_CHECK_EQUAL(demo(bd(F64_TWO)), F32_TWO);
   BOOST_CHECK_EQUAL(demo(bd(F64_POS_INF)), F32_POS_INF);
   BOOST_CHECK(is_f32_nan(demo(bd(F64_QNAN))));
}

// Round-trip: f32 → f64 → f32 preserves all f32 values exactly
BOOST_AUTO_TEST_CASE(f32_f64_roundtrip) {
   sf_harness h;
   auto check = [&](uint32_t bits) {
      float f = bf(bits);
      BOOST_CHECK_EQUAL(fb(h.iface._sysio_f64_demote(h.iface._sysio_f32_promote(f))), bits);
   };
   check(F32_ONE);
   check(F32_NEG_ONE);
   check(F32_POS_ZERO);
   check(F32_HALF);
   check(F32_THREE);
}

// ============================================================================
//  F32 → Integer conversions (with overflow traps)
// ============================================================================

BOOST_AUTO_TEST_CASE(f32_trunc_i32s_golden) {
   sf_harness h;
   auto& i = h.iface;
   BOOST_CHECK_EQUAL(i._sysio_f32_trunc_i32s(bf(F32_ONE)), 1);
   BOOST_CHECK_EQUAL(i._sysio_f32_trunc_i32s(bf(F32_NEG_ONE)), -1);
   BOOST_CHECK_EQUAL(i._sysio_f32_trunc_i32s(bf(F32_ONE_PT_5)), 1);
   BOOST_CHECK_EQUAL(i._sysio_f32_trunc_i32s(bf(F32_NEG_ONE_PT_5)), -1);
   BOOST_CHECK_EQUAL(i._sysio_f32_trunc_i32s(bf(F32_POS_ZERO)), 0);
   // Overflow traps
   BOOST_CHECK_THROW(i._sysio_f32_trunc_i32s(bf(F32_POS_INF)), wasm_execution_error);
   BOOST_CHECK_THROW(i._sysio_f32_trunc_i32s(bf(F32_QNAN)), wasm_execution_error);
}

BOOST_AUTO_TEST_CASE(f32_trunc_i32u_golden) {
   sf_harness h;
   auto& i = h.iface;
   BOOST_CHECK_EQUAL(i._sysio_f32_trunc_i32u(bf(F32_ONE)), 1u);
   BOOST_CHECK_EQUAL(i._sysio_f32_trunc_i32u(bf(F32_POS_ZERO)), 0u);
   BOOST_CHECK_EQUAL(i._sysio_f32_trunc_i32u(bf(F32_ONE_PT_5)), 1u);
   // Negative traps
   BOOST_CHECK_THROW(i._sysio_f32_trunc_i32u(bf(F32_NEG_ONE)), wasm_execution_error);
   BOOST_CHECK_THROW(i._sysio_f32_trunc_i32u(bf(F32_POS_INF)), wasm_execution_error);
   BOOST_CHECK_THROW(i._sysio_f32_trunc_i32u(bf(F32_QNAN)), wasm_execution_error);
}

BOOST_AUTO_TEST_CASE(f32_trunc_i64s_golden) {
   sf_harness h;
   auto& i = h.iface;
   BOOST_CHECK_EQUAL(i._sysio_f32_trunc_i64s(bf(F32_ONE)), 1LL);
   BOOST_CHECK_EQUAL(i._sysio_f32_trunc_i64s(bf(F32_NEG_ONE)), -1LL);
   BOOST_CHECK_EQUAL(i._sysio_f32_trunc_i64s(bf(F32_POS_ZERO)), 0LL);
   BOOST_CHECK_THROW(i._sysio_f32_trunc_i64s(bf(F32_POS_INF)), wasm_execution_error);
   BOOST_CHECK_THROW(i._sysio_f32_trunc_i64s(bf(F32_QNAN)), wasm_execution_error);
}

BOOST_AUTO_TEST_CASE(f32_trunc_i64u_golden) {
   sf_harness h;
   auto& i = h.iface;
   BOOST_CHECK_EQUAL(i._sysio_f32_trunc_i64u(bf(F32_ONE)), 1ULL);
   BOOST_CHECK_EQUAL(i._sysio_f32_trunc_i64u(bf(F32_POS_ZERO)), 0ULL);
   BOOST_CHECK_THROW(i._sysio_f32_trunc_i64u(bf(F32_NEG_ONE)), wasm_execution_error);
   BOOST_CHECK_THROW(i._sysio_f32_trunc_i64u(bf(F32_POS_INF)), wasm_execution_error);
}

// ============================================================================
//  F64 → Integer conversions (with overflow traps)
// ============================================================================

BOOST_AUTO_TEST_CASE(f64_trunc_i32s_golden) {
   sf_harness h;
   auto& i = h.iface;
   BOOST_CHECK_EQUAL(i._sysio_f64_trunc_i32s(bd(F64_ONE)), 1);
   BOOST_CHECK_EQUAL(i._sysio_f64_trunc_i32s(bd(F64_NEG_ONE)), -1);
   BOOST_CHECK_EQUAL(i._sysio_f64_trunc_i32s(bd(F64_ONE_PT_5)), 1);
   BOOST_CHECK_EQUAL(i._sysio_f64_trunc_i32s(bd(F64_POS_ZERO)), 0);
   BOOST_CHECK_THROW(i._sysio_f64_trunc_i32s(bd(F64_POS_INF)), wasm_execution_error);
   BOOST_CHECK_THROW(i._sysio_f64_trunc_i32s(bd(F64_QNAN)), wasm_execution_error);
}

BOOST_AUTO_TEST_CASE(f64_trunc_i32u_golden) {
   sf_harness h;
   auto& i = h.iface;
   BOOST_CHECK_EQUAL(i._sysio_f64_trunc_i32u(bd(F64_ONE)), 1u);
   BOOST_CHECK_EQUAL(i._sysio_f64_trunc_i32u(bd(F64_POS_ZERO)), 0u);
   BOOST_CHECK_THROW(i._sysio_f64_trunc_i32u(bd(F64_NEG_ONE)), wasm_execution_error);
   BOOST_CHECK_THROW(i._sysio_f64_trunc_i32u(bd(F64_POS_INF)), wasm_execution_error);
}

BOOST_AUTO_TEST_CASE(f64_trunc_i64s_golden) {
   sf_harness h;
   auto& i = h.iface;
   BOOST_CHECK_EQUAL(i._sysio_f64_trunc_i64s(bd(F64_ONE)), 1LL);
   BOOST_CHECK_EQUAL(i._sysio_f64_trunc_i64s(bd(F64_NEG_ONE)), -1LL);
   BOOST_CHECK_EQUAL(i._sysio_f64_trunc_i64s(bd(F64_POS_ZERO)), 0LL);
   BOOST_CHECK_THROW(i._sysio_f64_trunc_i64s(bd(F64_POS_INF)), wasm_execution_error);
}

BOOST_AUTO_TEST_CASE(f64_trunc_i64u_golden) {
   sf_harness h;
   auto& i = h.iface;
   BOOST_CHECK_EQUAL(i._sysio_f64_trunc_i64u(bd(F64_ONE)), 1ULL);
   BOOST_CHECK_EQUAL(i._sysio_f64_trunc_i64u(bd(F64_POS_ZERO)), 0ULL);
   BOOST_CHECK_THROW(i._sysio_f64_trunc_i64u(bd(F64_NEG_ONE)), wasm_execution_error);
   BOOST_CHECK_THROW(i._sysio_f64_trunc_i64u(bd(F64_POS_INF)), wasm_execution_error);
}

// ============================================================================
//  Integer → F32 conversions
// ============================================================================

BOOST_AUTO_TEST_CASE(i32_to_f32_golden) {
   sf_harness h;
   auto& i = h.iface;
   BOOST_CHECK_EQUAL(fb(i._sysio_i32_to_f32(0)), F32_POS_ZERO);
   BOOST_CHECK_EQUAL(fb(i._sysio_i32_to_f32(1)), F32_ONE);
   BOOST_CHECK_EQUAL(fb(i._sysio_i32_to_f32(-1)), F32_NEG_ONE);
   BOOST_CHECK_EQUAL(fb(i._sysio_i32_to_f32(2)), F32_TWO);
}

BOOST_AUTO_TEST_CASE(ui32_to_f32_golden) {
   sf_harness h;
   auto& i = h.iface;
   BOOST_CHECK_EQUAL(fb(i._sysio_ui32_to_f32(0)), F32_POS_ZERO);
   BOOST_CHECK_EQUAL(fb(i._sysio_ui32_to_f32(1)), F32_ONE);
   BOOST_CHECK_EQUAL(fb(i._sysio_ui32_to_f32(2)), F32_TWO);
}

BOOST_AUTO_TEST_CASE(i64_to_f32_golden) {
   sf_harness h;
   auto& i = h.iface;
   BOOST_CHECK_EQUAL(fb(i._sysio_i64_to_f32(0)), F32_POS_ZERO);
   BOOST_CHECK_EQUAL(fb(i._sysio_i64_to_f32(1)), F32_ONE);
   BOOST_CHECK_EQUAL(fb(i._sysio_i64_to_f32(-1)), F32_NEG_ONE);
}

BOOST_AUTO_TEST_CASE(ui64_to_f32_golden) {
   sf_harness h;
   auto& i = h.iface;
   BOOST_CHECK_EQUAL(fb(i._sysio_ui64_to_f32(0)), F32_POS_ZERO);
   BOOST_CHECK_EQUAL(fb(i._sysio_ui64_to_f32(1)), F32_ONE);
   BOOST_CHECK_EQUAL(fb(i._sysio_ui64_to_f32(2)), F32_TWO);
}

// ============================================================================
//  Integer → F64 conversions
// ============================================================================

BOOST_AUTO_TEST_CASE(i32_to_f64_golden) {
   sf_harness h;
   auto& i = h.iface;
   BOOST_CHECK_EQUAL(db(i._sysio_i32_to_f64(0)), F64_POS_ZERO);
   BOOST_CHECK_EQUAL(db(i._sysio_i32_to_f64(1)), F64_ONE);
   BOOST_CHECK_EQUAL(db(i._sysio_i32_to_f64(-1)), F64_NEG_ONE);
   BOOST_CHECK_EQUAL(db(i._sysio_i32_to_f64(2)), F64_TWO);
}

BOOST_AUTO_TEST_CASE(ui32_to_f64_golden) {
   sf_harness h;
   auto& i = h.iface;
   BOOST_CHECK_EQUAL(db(i._sysio_ui32_to_f64(0)), F64_POS_ZERO);
   BOOST_CHECK_EQUAL(db(i._sysio_ui32_to_f64(1)), F64_ONE);
   BOOST_CHECK_EQUAL(db(i._sysio_ui32_to_f64(2)), F64_TWO);
}

BOOST_AUTO_TEST_CASE(i64_to_f64_golden) {
   sf_harness h;
   auto& i = h.iface;
   BOOST_CHECK_EQUAL(db(i._sysio_i64_to_f64(0)), F64_POS_ZERO);
   BOOST_CHECK_EQUAL(db(i._sysio_i64_to_f64(1)), F64_ONE);
   BOOST_CHECK_EQUAL(db(i._sysio_i64_to_f64(-1)), F64_NEG_ONE);
}

BOOST_AUTO_TEST_CASE(ui64_to_f64_golden) {
   sf_harness h;
   auto& i = h.iface;
   BOOST_CHECK_EQUAL(db(i._sysio_ui64_to_f64(0)), F64_POS_ZERO);
   BOOST_CHECK_EQUAL(db(i._sysio_ui64_to_f64(1)), F64_ONE);
   BOOST_CHECK_EQUAL(db(i._sysio_ui64_to_f64(2)), F64_TWO);
}

// ============================================================================
//  Cross-conversion round-trips: verify int → float → int preserves value
// ============================================================================

BOOST_AUTO_TEST_CASE(int_float_roundtrip) {
   sf_harness h;
   auto& i = h.iface;
   // i32 → f64 → i32 (f64 has 53 mantissa bits, lossless for all i32)
   auto check_i32 = [&](int32_t v) {
      BOOST_CHECK_EQUAL(i._sysio_f64_trunc_i32s(i._sysio_i32_to_f64(v)), v);
   };
   check_i32(0);
   check_i32(1);
   check_i32(-1);
   check_i32(std::numeric_limits<int32_t>::max());
   check_i32(std::numeric_limits<int32_t>::min());

   // u32 → f64 → u32 (lossless for all u32)
   auto check_u32 = [&](uint32_t v) {
      BOOST_CHECK_EQUAL(i._sysio_f64_trunc_i32u(i._sysio_ui32_to_f64(v)), v);
   };
   check_u32(0);
   check_u32(1);
   check_u32(std::numeric_limits<uint32_t>::max());
}

BOOST_AUTO_TEST_SUITE_END()

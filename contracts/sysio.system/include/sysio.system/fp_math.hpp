#pragma once

#include <cstdint>

/// Q32.32 fixed-point math used by the emissions contract to derive a
/// per-epoch decay factor from a target annual decay rate, on chain. Designed
/// for the small-magnitude inputs that arise in emissions (target survival
/// ratio in [0.01, 1.0], exponent epoch_secs / year_secs in [~2e-6, ~0.08]).
/// All operations are deterministic integer arithmetic on __int128.
///
/// Precision: per-op Q32 epsilon is ~2.3e-10; pow_frac compounds across mul,
/// ln (range reduction is exact, Taylor truncation is below epsilon for
/// |u| <= 0.5 with 35 terms), and exp (back-squaring multiplies epsilon by
/// the halving depth). Worst-case relative error on a single pow_frac call
/// is ~2e-9. Cumulative error over a year of 6-min epochs (~88k epochs) is
/// ~2e-4, well within treasury accounting tolerance.
namespace sysiosystem::fp_math {

/// Backing integer type for Q32.32 values. Real number x is stored as
/// (int64_t)(x * 2^32); __int128 is used for intermediate products and
/// dividend pre-shifts so we have headroom against overflow.
using fp_t = __int128;

inline constexpr int  FRAC_BITS = 32;
inline constexpr fp_t ONE = static_cast<fp_t>(1) << FRAC_BITS;

/// ln(2) in Q32.32. round(0.6931471805599453 * 2^32) = 2977044472.
inline constexpr fp_t LN2 = 2977044472;

/// Taylor-series term counts. Sized to cover Q32.32 precision over the
/// post-range-reduction input domain: |u| <= 0.5 for ln, |y| <= 1.0 for exp.
inline constexpr int LN_TAYLOR_TERMS  = 35;
inline constexpr int EXP_TAYLOR_TERMS = 20;

/// Multiply two Q32.32 values: (a*b) in Q32.32.
inline constexpr fp_t mul(fp_t a, fp_t b) {
   return (a * b) >> FRAC_BITS;
}

/// Divide two Q32.32 values: (a/b) in Q32.32.
/// Pre-shift left by FRAC_BITS so the integer division yields a Q32.32 result.
inline constexpr fp_t div(fp_t a, fp_t b) {
   return (a << FRAC_BITS) / b;
}

/// Natural log of x for x in (0, ONE]; returns a non-positive Q32.32 value.
/// Strategy: range-reduce by repeatedly doubling until x is in [0.5, 1.0],
/// then evaluate the standard Taylor series ln(1-u) = -sum_{n>=1} u^n / n
/// with |u| <= 0.5 (LN_TAYLOR_TERMS covers Q32.32 precision).
constexpr fp_t ln(fp_t x) {
   if (x >= ONE) return 0;
   int k = 0;
   while (x < ONE / 2) {
      x *= 2;
      ++k;
   }
   const fp_t u = ONE - x;
   fp_t result = 0;
   fp_t term = u;
   for (int n = 1; n <= LN_TAYLOR_TERMS; ++n) {
      const fp_t delta = term / n;
      if (delta == 0) break;          // remaining terms truncate to zero in Q32.32
      result -= delta;
      term = mul(term, u);
   }
   return result - static_cast<fp_t>(k) * LN2;
}

/// exp(y) for y in (-large, 0]; returns a positive Q32.32 value in (0, ONE].
/// Strategy: range-reduce by halving y until |y| <= 1, evaluate Taylor
/// (EXP_TAYLOR_TERMS covers |y| <= 1 to Q32.32 precision), then square the
/// result back k times to undo the halving (exp(y) = (exp(y/2^k))^(2^k)).
constexpr fp_t exp_neg(fp_t y) {
   if (y == 0) return ONE;
   int k = 0;
   while (y < -ONE) {
      y /= 2;
      ++k;
   }
   fp_t result = ONE;
   fp_t term = ONE;
   for (int n = 1; n <= EXP_TAYLOR_TERMS; ++n) {
      term = mul(term, y) / n;
      if (term == 0) break;           // remaining terms truncate to zero in Q32.32
      result += term;
   }
   for (int i = 0; i < k; ++i) {
      result = mul(result, result);
   }
   return result;
}

/// base^p for base in (0, ONE], p in [0, large). Returns Q32.32 in (0, ONE].
/// Computed as exp(p * ln(base)). base >= ONE short-circuits to ONE; p == 0
/// short-circuits to ONE.
constexpr fp_t pow_frac(fp_t base, fp_t p) {
   if (base >= ONE || p == 0) return ONE;
   const fp_t ln_base = ln(base);
   const fp_t y = mul(ln_base, p);
   return exp_neg(y);
}

} // namespace sysiosystem::fp_math

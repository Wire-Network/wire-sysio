#pragma once
/**
 * @file twap.hpp
 * @brief Cumulative-price accumulators for time-weighted average prices.
 *
 * A pool keeps, for each of its two sides, the running sum of
 * `price * elapsed` over every interval during which its balances were
 * unchanged: `price` is the side's spot price in Q64.64, `elapsed` the
 * interval's length in microseconds. A consumer snapshots the accumulator
 * `r0` at time `t0` and reads `r` at a later `t`; `(r - r0) / (t - t0)` is the
 * time-weighted average price over the window. Because each interval is
 * weighted by its duration, a trade that moves the spot price inside one
 * block contributes nothing until time has passed at the moved price, which
 * is what makes the average expensive to manipulate.
 *
 * The math is integer-only and self-contained (no contract intrinsics), so it
 * is unit-testable on the host and deterministic on-chain. Accumulators are
 * 256 bits wide: a Q64.64 price of int64 balances is below 2^126, and any
 * elapsed time fits in 64 bits, so a single step is below 2^190 and the sum
 * cannot wrap within the life of any chain. Consumers that store the
 * accumulator in a narrower type must subtract modulo that width.
 */

#include <cstdint>

namespace sysio::opp::twap {

/// Spelled `uint128_t` so the CDT ABI generator, which maps type names by
/// their spelling, emits the `uint128` builtin for the accumulator limbs.
using uint128_t = unsigned __int128;
using u128      = uint128_t;

/// Fractional bits of a price. Q64.64 holds every ratio of two int64 balances
/// with a resolution of 2^-64.
inline constexpr int  PRICE_FRACTION_BITS = 64;
inline constexpr u128 PRICE_ONE           = static_cast<u128>(1) << PRICE_FRACTION_BITS;

/// Mask selecting the low 64 bits of a u128.
inline constexpr u128 LOW_64_MASK = ~static_cast<uint64_t>(0);

/// A 256-bit unsigned cumulative price, as two little-endian 128-bit limbs.
struct cumulative_price {
   uint128_t lo = 0;
   uint128_t hi = 0;
};

/// Q64.64 spot price of one unit of the `denominator` side expressed in units
/// of the `numerator` side, rounded down. Zero when the denominator side is
/// empty (there is no price to record).
inline u128 price_fp(uint64_t numerator, uint64_t denominator) {
   if (denominator == 0) return 0;
   return (static_cast<u128>(numerator) << PRICE_FRACTION_BITS) / denominator;
}

/// `acc += price * elapsed`, computed in 256 bits.
inline void accumulate(cumulative_price& acc, u128 price, uint64_t elapsed) {
   // price * elapsed as (hi_part << 64) + lo_part, each partial product below 2^128.
   const u128 lo_part = (price & LOW_64_MASK) * elapsed;
   const u128 hi_part = (price >> 64) * elapsed;
   const u128 add_lo  = lo_part + (hi_part << 64);
   u128       add_hi  = hi_part >> 64;
   if (add_lo < lo_part) ++add_hi;            // carry out of the low limb of the product
   acc.lo += add_lo;
   acc.hi += add_hi + (acc.lo < add_lo ? 1 : 0);   // carry out of the low limb of the sum
}

/// `later - earlier` modulo 2^256.
inline cumulative_price difference(const cumulative_price& later, const cumulative_price& earlier) {
   cumulative_price d;
   d.lo = later.lo - earlier.lo;
   d.hi = later.hi - earlier.hi - (later.lo < earlier.lo ? 1 : 0);
   return d;
}

/// `delta / elapsed` as a Q64.64 price: the time-weighted average over a window
/// whose accumulator moved by `delta` in `elapsed` microseconds. A genuine
/// delta always yields a quotient that fits, since it is a mean of prices
/// each below 2^126. Returns 0 for a zero window or a delta no window of that
/// length could have produced.
inline u128 average_price(const cumulative_price& delta, uint64_t elapsed) {
   if (elapsed == 0 || delta.hi >= elapsed) return 0;
   // Long division of the 256-bit delta by a 64-bit divisor, one 64-bit digit
   // at a time; every partial dividend stays below 2^128.
   u128       cur = (delta.hi << 64) | (delta.lo >> 64);
   const u128 q1  = cur / elapsed;
   cur = ((cur % elapsed) << 64) | (delta.lo & LOW_64_MASK);
   const u128 q0  = cur / elapsed;
   return (q1 << 64) | q0;
}

} // namespace sysio::opp::twap

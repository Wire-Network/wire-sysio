/**
 * @file twap_tests.cpp
 * @brief Host-side unit tests for the cumulative-price accumulator kernel
 *        (`sysio::opp::twap`, contracts/sysio.opp.common/.../twap.hpp).
 *
 * The kernel is pure integer C++ (no contract intrinsics), so it is exercised
 * directly on the host against boost 256-bit arithmetic. Coverage:
 *   - `price_fp` is the floored Q64.64 ratio, zero on an empty denominator.
 *   - `accumulate` equals the exact 256-bit sum, including every carry path.
 *   - `difference` subtracts modulo 2^256, including a borrow across limbs.
 *   - `average_price` recovers the exact time-weighted mean of a step sequence
 *     and reports 0 for a zero window or an impossible delta.
 */

#include <boost/test/unit_test.hpp>
#include <sysio.opp.common/twap.hpp>
#include "twap_wide.hpp"

#include <cstdint>
#include <random>
#include <vector>

using namespace sysio::opp::twap;
using namespace twap_testing;

namespace {

constexpr uint64_t I64_MAX = uint64_t(INT64_MAX);
constexpr u128 U128_MAX = ~u128(0);

} // namespace

BOOST_AUTO_TEST_SUITE(twap_tests)

BOOST_AUTO_TEST_CASE(price_fp_is_the_floored_ratio) {
   BOOST_CHECK(price_fp(1, 1) == PRICE_ONE);
   BOOST_CHECK(price_fp(3, 2) == PRICE_ONE + PRICE_ONE / 2);
   BOOST_CHECK(price_fp(1, 3) == PRICE_ONE / 3);                       // floored
   BOOST_CHECK(price_fp(I64_MAX, 1) == (u128(I64_MAX) << 64));         // the largest price of int64 balances
   BOOST_CHECK(price_fp(1, I64_MAX) == PRICE_ONE / I64_MAX);
   BOOST_CHECK(price_fp(5, 0) == 0);
   BOOST_CHECK(price_fp(0, 5) == 0);
}

BOOST_AUTO_TEST_CASE(accumulate_matches_exact_256_bit_arithmetic) {
   std::mt19937_64 rng(0x5457'4150'5445'5354ULL);
   cumulative_price acc;
   uint256_t expected = 0;
   for (int i = 0; i < 2000; ++i) {
      // prices across the whole Q64.64 range of int64 balances, elapsed across 64 bits
      const u128     price   = price_fp(rng() % (I64_MAX + 1), 1 + rng() % I64_MAX);
      const uint64_t elapsed = (i % 3 == 0) ? rng() : rng() % 1'000'000'000;
      accumulate(acc, price, elapsed);
      expected += wide(price) * elapsed;
      BOOST_REQUIRE(wide(acc) == expected);
   }
}

BOOST_AUTO_TEST_CASE(accumulate_carries_across_the_limb) {
   // The product itself spills into the high limb.
   {
      cumulative_price acc;
      accumulate(acc, u128(I64_MAX) << 64, UINT64_MAX);
      BOOST_CHECK(wide(acc) == wide(u128(I64_MAX) << 64) * UINT64_MAX);
      BOOST_CHECK(acc.hi != 0);
   }
   // The sum spills into the high limb while the product does not.
   {
      cumulative_price acc{U128_MAX, 0};
      accumulate(acc, 1, 1);
      BOOST_CHECK(acc.lo == 0);
      BOOST_CHECK(acc.hi == 1);
   }
   // Both carries at once.
   {
      cumulative_price acc{U128_MAX, 7};
      const u128 price = (u128(1) << 127) | 1;
      accumulate(acc, price, 3);
      BOOST_CHECK(wide(acc) == (wide(U128_MAX) | (uint256_t(7) << 128)) + wide(price) * 3);
   }
}

BOOST_AUTO_TEST_CASE(difference_borrows_across_the_limb) {
   const cumulative_price later{5, 3};
   const cumulative_price earlier{U128_MAX, 1};
   const cumulative_price d = difference(later, earlier);
   BOOST_CHECK(wide(d) == wide(later) - wide(earlier));
   BOOST_CHECK(d.lo == 6);
   BOOST_CHECK(d.hi == 1);
   // Identical accumulators differ by zero.
   const cumulative_price z = difference(later, later);
   BOOST_CHECK(z.lo == 0 && z.hi == 0);
}

BOOST_AUTO_TEST_CASE(average_price_recovers_the_time_weighted_mean) {
   std::mt19937_64 rng(0x4156'4552'4147'45ULL);
   for (int round = 0; round < 200; ++round) {
      cumulative_price start, end;
      uint64_t  total = 0;
      uint256_t sum   = 0;
      const int steps = 1 + rng() % 8;
      for (int s = 0; s < steps; ++s) {
         const u128     price   = price_fp(1 + rng() % I64_MAX, 1 + rng() % I64_MAX);
         const uint64_t elapsed = 1 + rng() % 4'000'000'000'000ULL;   // up to ~46 days of microseconds
         accumulate(end, price, elapsed);
         total += elapsed;
         sum   += wide(price) * elapsed;
      }
      BOOST_REQUIRE(average_price(difference(end, start), total) == narrow(sum / total));
   }
   // A constant price over any window averages to exactly itself.
   {
      cumulative_price acc;
      const u128 price = price_fp(I64_MAX, 1);
      accumulate(acc, price, UINT64_MAX);
      BOOST_CHECK(average_price(acc, UINT64_MAX) == price);
   }
}

BOOST_AUTO_TEST_CASE(average_price_rejects_degenerate_windows) {
   cumulative_price acc;
   accumulate(acc, PRICE_ONE, 10);
   BOOST_CHECK(average_price(acc, 0) == 0);
   // A delta whose high limb reaches the window length cannot be a mean of valid prices.
   const cumulative_price impossible{0, 10};
   BOOST_CHECK(average_price(impossible, 10) == 0);
   const cumulative_price possible{0, 9};
   BOOST_CHECK(average_price(possible, 10) != 0);
}

BOOST_AUTO_TEST_SUITE_END()

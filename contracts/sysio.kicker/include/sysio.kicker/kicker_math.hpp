#pragma once
/**
 * @file kicker_math.hpp
 * @brief Integer-only simple accrual, floored twice in favour of the treasury.
 */
#include <sysio.opp.common/twap.hpp>
#include <cstdint>
#include <optional>

namespace sysio::kicker_math {
using u128 = unsigned __int128;
/// Governance rates are basis points; one Julian year is exactly 365.25 days.
inline constexpr uint64_t bps_scale = 10'000;
inline constexpr uint64_t year_sec = 31'557'600;
inline constexpr uint64_t micros_per_sec = 1'000'000;
inline constexpr uint64_t accrual_divisor = bps_scale * year_sec * micros_per_sec;
/// Asset amounts are bounded by the protocol, even when intermediate values are wider.
inline constexpr uint64_t max_amount = (uint64_t{1} << 62) - 1;
/// Default minimum unpaid accrual interval: one hour.
inline constexpr uint32_t default_min_interval_sec = 3600;
/// Default per-token annual simple rate: 2%.
inline constexpr uint16_t default_rate_bps = 200;
/// Default payment granularity: one nine-decimal WIRE.
inline constexpr uint64_t default_min_gift = 1'000'000'000;
/// UTC-sized day used for the optional per-pool spend ceiling.
inline constexpr uint64_t day_sec = 86'400;

/** Compute floor(S * bps * elapsed_us / (10000 * Julian_year_us)).
 * Dividing S*bps first as quotient and remainder keeps both products below
 * 2^128 for every asset, accepted rate and uint64 elapsed time. No catch-up
 * cap or unchecked S*bps*elapsed product is needed.
 */
inline u128 interest(uint64_t supply, uint16_t rate_bps, uint64_t elapsed_us) {
   const u128 annual = static_cast<u128>(supply) * rate_bps;
   return (annual / accrual_divisor) * elapsed_us +
          (annual % accrual_divisor) * elapsed_us / accrual_divisor;
}

/** Price an interest amount at the live Q64.64 pool ratio.
 * Two 128-bit limbs hold the full product, including intervals so long that
 * interest itself exceeds uint64. A result outside the asset range is a
 * non-payable gift, never a wrapped or clipped transfer.
 */
inline std::optional<uint64_t> gift(uint64_t supply, uint16_t rate_bps, uint64_t elapsed_us,
                                   uint64_t pool_wire, uint64_t pool_shadow) {
   const u128 amount = interest(supply, rate_bps, elapsed_us);
   const u128 price = opp::twap::price_fp(pool_wire, pool_shadow);
   opp::twap::cumulative_price product{}, high{};
   opp::twap::accumulate(product, price, static_cast<uint64_t>(amount));
   opp::twap::accumulate(high, price, static_cast<uint64_t>(amount >> opp::twap::PRICE_FRACTION_BITS));
   const u128 shifted = high.lo << opp::twap::PRICE_FRACTION_BITS;
   const u128 old_lo = product.lo;
   product.lo += shifted;
   product.hi += (high.hi << opp::twap::PRICE_FRACTION_BITS) +
                 (high.lo >> opp::twap::PRICE_FRACTION_BITS) + (product.lo < old_lo ? 1 : 0);
   const u128 priced = product.lo >> opp::twap::PRICE_FRACTION_BITS;
   if (product.hi != 0 || priced > max_amount) return std::nullopt;
   return static_cast<uint64_t>(priced);
}
} // namespace sysio::kicker_math

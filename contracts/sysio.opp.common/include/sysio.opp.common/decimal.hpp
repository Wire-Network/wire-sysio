#pragma once
/**
 * @file decimal.hpp
 * @brief Decimal scale of an asset precision, for contracts that size amounts in whole tokens.
 */

#include <sysio/check.hpp>

#include <cstdint>

namespace sysio::opp {

/// Largest decimal precision an amount may carry: 10^18 is the last power of ten inside int64.
inline constexpr uint8_t MAX_AMOUNT_PRECISION = 18;

/// 10^decimals: the subunits in one whole token of that precision.
inline int64_t precision_from_decimals(uint8_t decimals) {
   check(decimals <= MAX_AMOUNT_PRECISION, "precision should be <= 18");
   int64_t p10 = 1;
   for (uint8_t i = 0; i < decimals; ++i) p10 *= 10;
   return p10;
}

} // namespace sysio::opp

#pragma once
/**
 * @file twap_wide.hpp
 * @brief Conversions between the twap kernel's 128/256-bit types and boost
 *        256-bit integers, shared by the host kernel tests and the sysio.swap
 *        contract tests so each can check the on-chain arithmetic against an
 *        exact reference.
 */

#include <boost/multiprecision/cpp_int.hpp>
#include <sysio.opp.common/twap.hpp>

#include <cstdint>

namespace twap_testing {

using boost::multiprecision::uint256_t;
using sysio::opp::twap::cumulative_price;
using sysio::opp::twap::u128;

inline uint256_t wide(u128 v) { return (uint256_t(uint64_t(v >> 64)) << 64) | uint64_t(v); }
inline uint256_t wide(const cumulative_price& c) { return (wide(c.hi) << 128) | wide(c.lo); }
inline u128 narrow(const uint256_t& v) { return (u128(uint64_t(v >> 64)) << 64) | uint64_t(v); }
inline cumulative_price to_cumulative(const uint256_t& v) {
   return cumulative_price{ narrow(v), narrow(v >> 128) };
}

} // namespace twap_testing

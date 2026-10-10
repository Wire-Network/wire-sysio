#pragma once
/** @file kicker_test_reference.hpp
 * @brief Independent literal arithmetic oracle shared by host and chain tests.
 */
#include <boost/multiprecision/cpp_int.hpp>
#include <cstdint>
namespace kicker_test_reference {
using wide = boost::multiprecision::uint256_t;
/// Independent literal divisor and fixed-point valuation.
inline uint64_t reference(uint64_t supply, uint16_t rate, uint64_t elapsed, uint64_t wire, uint64_t shadow) {
   const wide accrued = wide(supply) * rate * elapsed / (wide(10'000) * 31'557'600 * 1'000'000);
   return static_cast<uint64_t>((accrued * ((wide(wire) << 64) / shadow)) >> 64);
}
}

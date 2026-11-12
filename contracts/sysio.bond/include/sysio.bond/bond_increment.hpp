#pragma once

#include <cstdint>
#include <optional>

/// Bond increments shared by CDT contracts and the native underwriting planner.
namespace sysio::bond_increment {

/// Contributions move in hundredths of a token.
inline constexpr uint8_t decimals = 2;
/// Decimal arithmetic base.
inline constexpr uint64_t decimal_base = 10;
/// Largest supported token precision.
inline constexpr uint8_t max_token_decimals = 18;

/// Return the increment in base units, or nullopt for unsupported token precision.
constexpr std::optional<uint64_t> for_decimals(uint8_t token_decimals) {
   if (token_decimals < decimals || token_decimals > max_token_decimals) return std::nullopt;
   uint64_t increment = 1;
   for (uint8_t d = decimals; d < token_decimals; ++d) increment *= decimal_base;
   return increment;
}

} // namespace sysio::bond_increment

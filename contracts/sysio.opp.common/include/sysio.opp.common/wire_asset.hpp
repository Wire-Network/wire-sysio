#pragma once
/**
 * @file wire_asset.hpp
 * @brief Canonical identity of the depot-native WIRE asset.
 *
 * Contracts use the same WIRE symbol for token custody and the same slug for
 * the depot chain and its native token. Keeping those values here prevents a
 * symbol, precision, or registry-code change from drifting across contracts.
 */

#include <sysio/asset.hpp>
#include <sysio.opp.common/slug_name.hpp>

namespace sysio::opp::wire {

using sysio::slug_name_literals::operator""_s;

/// Native WIRE token symbol and its system-wide nine-decimal precision.
inline constexpr sysio::symbol asset_symbol{"WIRE", 9};

/// Registry slug identifying the native WIRE token.
inline constexpr sysio::slug_name token_code = "WIRE"_s;

/// Registry slug identifying the WIRE depot chain.
inline constexpr sysio::slug_name chain_code = "WIRE"_s;

/// True when a registry pair identifies the depot-native WIRE asset.
[[nodiscard]] inline constexpr bool is_native_asset(sysio::slug_name chain,
                                                     sysio::slug_name token) noexcept {
   return chain == chain_code && token == token_code;
}

} // namespace sysio::opp::wire

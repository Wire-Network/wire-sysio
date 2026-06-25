#pragma once
/**
 * @file outpost_epoch_lookup.hpp
 * @brief Host-side helpers for OPP outpost/epoch secondary-index lookups.
 */

#include <cstdint>
#include <format>
#include <string>

#include <fc/int128.hpp>

namespace sysio::batch_operator_detail {

/// Secondary-index name shared by msgch delivered and outbound envelope tables.
inline constexpr auto byoutepoch_index_name = "byoutepoch";

/// Host-side mirror of `sysio::opp::outpost_epoch_key` for read-only API lookups.
///
/// Contract code stores `(chain_code, epoch_index)` in the `byoutepoch` secondary
/// index as `chain_code << 32 | epoch_index`. The key must stay 128-bit because
/// `slug_name` chain codes can use bits above bit 31; a 64-bit intermediate would
/// collapse many outposts onto the epoch number.
inline fc::uint128 outpost_epoch_key(uint64_t chain_code, uint32_t epoch_index) {
   return (static_cast<fc::uint128>(chain_code) << 32) | static_cast<fc::uint128>(epoch_index);
}

/// Builds the JSON exact-match bound expected by chain_plugin's BE secondary-key codec.
inline std::string byoutepoch_find_bound(uint64_t chain_code, uint32_t epoch_index) {
   return std::format("{{\"{}\":\"{}\"}}", byoutepoch_index_name,
                      fc::to_string(outpost_epoch_key(chain_code, epoch_index)));
}

} // namespace sysio::batch_operator_detail

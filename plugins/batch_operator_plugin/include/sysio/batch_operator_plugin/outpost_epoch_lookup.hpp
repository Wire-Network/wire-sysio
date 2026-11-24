#pragma once
/**
 * @file outpost_epoch_lookup.hpp
 * @brief Host-side helpers for OPP outpost/epoch secondary-index lookups.
 */

#include <cstdint>
#include <format>
#include <string>

#include <fc/int128.hpp>
#include <fc/variant.hpp>
#include <sysio/chain/name.hpp>

#include <sysio/batch_operator_plugin/table_range.hpp>

namespace sysio::batch_operator_detail {

/// Secondary-index name shared by msgch delivered and outbound envelope tables.
inline constexpr auto byoutepoch_index_name = "byoutepoch";

/// Field of a `sysio.msgch::envelopes` row naming the batch operator that delivered it.
inline constexpr auto envelope_batch_op_field = "batch_op_name";

/// Host-side mirror of `sysio::opp::outpost_epoch_key` for read-only API lookups.
///
/// Contract code stores `(chain_code, epoch_index)` in the `byoutepoch` secondary
/// index as `chain_code << 32 | epoch_index`. The key must stay 128-bit because
/// `slug_name` chain codes can use bits above bit 31; a 64-bit intermediate would
/// collapse many outposts onto the epoch number.
inline fc::uint128 outpost_epoch_key(uint64_t chain_code, uint32_t epoch_index) {
   return (static_cast<fc::uint128>(chain_code) << 32) | static_cast<fc::uint128>(epoch_index);
}

/// The JSON bound of `byoutepoch` key `key` that chain_plugin's BE secondary-key codec expects. The 128-bit key is
/// written as a decimal string, so the bound stays lossless past 2^64.
inline std::string byoutepoch_bound(fc::uint128 key) {
   return std::format("{{\"{}\":\"{}\"}}", byoutepoch_index_name, fc::to_string(key));
}

/// Builds the JSON exact-match bound expected by chain_plugin's BE secondary-key codec.
inline std::string byoutepoch_find_bound(uint64_t chain_code, uint32_t epoch_index) {
   return byoutepoch_bound(outpost_epoch_key(chain_code, epoch_index));
}

/// The `byoutepoch` range of every row of `(chain_code, epoch_index)`: to the next key, exclusive.
inline index_range byoutepoch_range(uint64_t chain_code, uint32_t epoch_index) {
   const fc::uint128 key = outpost_epoch_key(chain_code, epoch_index);
   return {.lower_bound = byoutepoch_bound(key), .upper_bound = byoutepoch_bound(key + 1)};
}

/// Whether `rows`, `sysio.msgch::envelopes` rows of one chain epoch, hold a delivery by `batch_op`.
inline bool delivered_by(const fc::variants& rows, chain::name batch_op) {
   for (const fc::variant& row : rows) {
      if (chain::name(row[envelope_batch_op_field].as_string()) == batch_op) return true;
   }
   return false;
}

} // namespace sysio::batch_operator_detail

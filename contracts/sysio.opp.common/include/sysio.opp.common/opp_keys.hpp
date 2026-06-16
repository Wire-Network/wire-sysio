#pragma once
/**
 * @file opp_keys.hpp
 * @brief Shared composite-key helpers for OPP contract tables.
 *
 * Centralizes the secondary-index keys that combine an outpost chain code with an
 * epoch index, so every contract (sysio.msgch / sysio.chalg / sysio.epoch) derives
 * and looks up the same key the same way.
 */

#include <cstdint>

namespace sysio::opp {

/// Order-preserving composite key for an `(outpost chain_code, epoch_index)` pair.
///
/// `chain_code` is a `slug_name` value. Per `slug_name.hpp` those values live in
/// `[0, 2^48)` and pack most-significant-symbol-first, so the *distinguishing* bits of
/// a short code sit in the high half (the first symbol occupies bits 42-47). Packing a
/// 48-bit chain code with a 32-bit epoch therefore needs up to 80 bits and MUST use a
/// 128-bit key.
///
/// The earlier `(static_cast<uint64_t>(chain_code) << 32) | epoch_index` truncated
/// chain_code's bits 32-63, collapsing distinct outposts onto the same key: every
/// 1-2 character code mapped to `epoch_index` alone, and many 3-character codes shared a
/// key. That silently cross-wired per-(outpost, epoch) buckets in any multi-outpost
/// deployment. The 128-bit key below is lossless for the full slug range.
///
/// Layout: `chain_code` in bits 32-79, `epoch_index` in bits 0-31. Sorts by chain_code
/// then epoch_index — the same ordering the index relied on, minus the truncation.
inline uint128_t outpost_epoch_key(uint64_t chain_code, uint32_t epoch_index) {
   return (static_cast<uint128_t>(chain_code) << 32) | static_cast<uint128_t>(epoch_index);
}

} // namespace sysio::opp

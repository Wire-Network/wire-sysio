#pragma once

#include <cstdint>

/// Producer-rank constants shared by the sysio.system contract and the host code that reads its `rank_score` keys and
/// installs its score configuration. Include only headers that both CDT and the host build provide.
///
/// The composite is `M x (seniority + snapshot credit + seniority base)`: the collateral multiple M in hundredths of
/// the minimum bond times the time term in whole hours. It orders producers exactly as stake share x the time term
/// would, since every producer divides by the same total stake; skipping that division spares a rescore of every
/// producer whenever any bond changes.
namespace sysiosystem::producer_rank {

/// Fixed-point scale of the collateral and snapshot factors: basis points. A factor at `score_scale` is "100%"; a
/// collateral ratio of exactly the configured minimum bond is also `score_scale`.
inline constexpr uint64_t score_scale = 10'000;

/// Seniority counts in whole hours: the score floors `producer_info::service_sec` to a multiple of this.
inline constexpr uint64_t seniority_quantum_sec = 3'600;

/// Default `snapshot_weight`: full snapshot credit is worth three days of seniority.
inline constexpr uint32_t default_snapshot_weight_sec = 259'200;

/// Default `miss_penalty_sec`: an unserved round costs seven days of seniority.
inline constexpr uint32_t default_miss_penalty_sec = 604'800;

/// Default `seniority_base_sec`: seven days, added to every producer's time term so a producer with no seniority still
/// scores, ordered by stake.
inline constexpr uint32_t default_seniority_base_sec = 604'800;

/// The composite's collateral unit: hundredths of the minimum bond, so 1x the minimum is 100.
inline constexpr uint64_t collateral_multiple_scale = 100;

/// Basis points of collateral ratio per composite collateral unit.
inline constexpr uint64_t ratio_per_multiple_unit = score_scale / collateral_multiple_scale;

/// Bits the packed `rank_score` key gives the producer tier: the two high bits.
inline constexpr unsigned tier_bits = 2;

/// Bits the key gives the registration order: the low bits, about 4.2 million registrations.
inline constexpr unsigned registration_bits = 22;

/// Bits the key gives the composite score, hundredths of the minimum bond times whole hours. 2^40 units hold about
/// 1.25 million years at 1x the minimum and about 125 years at 10,000x; past that the composite saturates and equal
/// composites fall to registration order.
inline constexpr unsigned composite_bits = 64 - tier_bits - registration_bits;

/// Position of the tier above the composite and the registration order.
inline constexpr unsigned tier_shift = composite_bits + registration_bits;

/// Largest representable composite, and the saturation point of every score term.
inline constexpr uint64_t composite_max = (uint64_t{1} << composite_bits) - 1;

/// Largest representable registration order. An unassigned order (0) and any order past this encode as this value,
/// so they sort after every assigned one.
inline constexpr uint64_t registration_max = (uint64_t{1} << registration_bits) - 1;

} // namespace sysiosystem::producer_rank

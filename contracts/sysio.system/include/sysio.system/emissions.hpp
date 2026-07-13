#pragma once

#include <sysio.system/fp_math.hpp>

#include <sysio/asset.hpp>
#include <sysio/kv_global.hpp>
#include <sysio/kv_table.hpp>
#include <sysio/name.hpp>
#include <sysio/symbol.hpp>
#include <sysio/system.hpp>
#include <sysio/time.hpp>

#include <algorithm>
#include <cstdint>

// Core emissions tables owned by sysio.system. This header is intentionally
// free of OPP / protobuf dependencies so that downstream contracts (notably
// sysio.roa) can include it to check emitcfg_t::exists() before invoking
// addnodeowner. The opreg / token / epoch cross-contract reads used by
// payepoch and viewepoch live in emissions.cpp (implementation detail,
// not public API).

namespace sysiosystem::emissions {

// ---------------------------------------------------------------------------
// Per-tier maximum number of registered node owners. Single source of truth
// for both sysio.roa::activateroa (which seeds tier-pool allocations) and
// sysio.system::addnodeowner (which rejects registrations beyond the tier
// caps). Hardcoded rather than configurable because the tier sizing is part
// of the network's economic constants -- governance does not adjust it.
// ---------------------------------------------------------------------------

inline constexpr uint32_t T1_MAX_NODE_OWNERS = 21;
inline constexpr uint32_t T2_MAX_NODE_OWNERS = 84;
inline constexpr uint32_t T3_MAX_NODE_OWNERS = 1000;

// ---------------------------------------------------------------------------
// Emission configuration (set via setemitcfg action)
// ---------------------------------------------------------------------------

struct [[sysio::table("emitcfg"), sysio::contract("sysio.system")]] emission_config {
   // Node-owner allocations (WIRE subunits, 9-decimal)
   int64_t   t1_allocation;
   int64_t   t2_allocation;
   int64_t   t3_allocation;
   uint32_t  t1_duration;            // vesting seconds
   uint32_t  t2_duration;
   uint32_t  t3_duration;
   int64_t   min_claimable;          // minimum claim threshold (subunits)

   // T5 treasury
   int64_t   t5_distributable;       // total distributable amount
   int64_t   t5_floor;               // treasury floor
   // Epoch length is canonical on sysio.epoch::epochcfg::epoch_duration_sec;
   // payepoch / viewepoch read it cross-contract. Keeping a duplicate here
   // would let the two values drift and silently break producer-pay math.
   //
   // Decay and emission caps are expressed in *annual* terms; per-epoch
   // values are derived inside compute_epoch_emission from the canonical
   // epoch_duration_sec, so the wall-clock emission curve does not change
   // when governance retunes the epoch frequency.
   uint16_t  target_annual_decay_bps; // surviving fraction per year, 1..10000
   int64_t   annual_initial_emission; // E_0 expressed per year
   int64_t   annual_max_emission;     // per-year ceiling
   int64_t   annual_min_emission;     // per-year floor

   // Category splits (basis points). compute + capex + governance must
   // sum to <= 10000; the remainder is the implicit capital reserve drained
   // lazily by sysio.dclaim::onreward via sysio.system::fundclaim, not at
   // payepoch. Sum == 10000 means no implicit reserve (capital draws eat
   // future periods' headroom).
   uint16_t  compute_bps;
   uint16_t  capex_bps;
   uint16_t  governance_bps;

   // Compute sub-split (basis points, must sum to 10000)
   uint16_t  producer_bps;
   uint16_t  batch_op_bps;

   // Producer config
   uint32_t  standby_end_rank;       // last standby rank (default 28)

   // Audit-log retention. Caps the unbounded `epochlog` table at this many
   // rows; payepoch prunes head-first after each insert. One row per epoch
   // (~64 bytes), so 8640 ~= 30 days at 6-min epoch cadence.
   uint32_t  epoch_log_retention_count;

   // How many epochs accumulate before `payepoch` actually fires. 1 = pay
   // every epoch (matches the original emissions behavior). Recommended
   // production value: 100, which at a 6-min epoch is ~10h between pays
   // and at a 1-min epoch is ~1h40m. Higher values reduce the per-tick
   // inline-transfer count proportionally; the period-aggregate emission
   // and per-recipient share-by-rounds math stay equivalent to summing
   // the per-epoch results. Must be > 0; setemitcfg rejects zero.
   uint16_t  pay_cadence_epochs;

   SYSLIB_SERIALIZE(emission_config,
      (t1_allocation)(t2_allocation)(t3_allocation)
      (t1_duration)(t2_duration)(t3_duration)(min_claimable)
      (t5_distributable)(t5_floor)
      (target_annual_decay_bps)
      (annual_initial_emission)(annual_max_emission)(annual_min_emission)
      (compute_bps)(capex_bps)(governance_bps)
      (producer_bps)(batch_op_bps)
      (standby_end_rank)(epoch_log_retention_count)
      (pay_cadence_epochs))
};

using emitcfg_t = sysio::kv::global<"emitcfg"_n, emission_config>;

// ---------------------------------------------------------------------------
// Emission state (node-owner distribution start time)
// ---------------------------------------------------------------------------

struct [[sysio::table("emissionmngr"), sysio::contract("sysio.system")]] emission_state {
   sysio::time_point_sec    node_rewards_start;

   SYSLIB_SERIALIZE(emission_state, (node_rewards_start))
};

using emissionstate_t = sysio::kv::global<"emissionmngr"_n, emission_state>;

// Per-tier registration counts maintained by addnodeowner. Checked against
// emission_config::tN_max_count before each registration. Held in its own
// singleton (rather than emission_state) so addnodeowner does not require
// setinittime to have run first.
struct [[sysio::table("nodecount"), sysio::contract("sysio.system")]] node_count_state {
   uint32_t  t1_count = 0;
   uint32_t  t2_count = 0;
   uint32_t  t3_count = 0;

   SYSLIB_SERIALIZE(node_count_state, (t1_count)(t2_count)(t3_count))
};

using nodecountstate_t = sysio::kv::global<"nodecount"_n, node_count_state>;

// ---------------------------------------------------------------------------
// Node-owner distribution table (per-account vesting row)
// ---------------------------------------------------------------------------

struct nodedist_key {
   uint64_t account_name;
   SYSLIB_SERIALIZE(nodedist_key, (account_name))
};

struct [[sysio::table("nodedist"), sysio::contract("sysio.system")]] node_owner_distribution {
   sysio::name          account_name;
   sysio::asset         total_allocation;
   sysio::asset         claimed;
   uint32_t             total_duration;

   SYSLIB_SERIALIZE(node_owner_distribution,
      (account_name)(total_allocation)(claimed)(total_duration))
};

using nodedist_t = sysio::kv::table<"nodedist"_n, nodedist_key, node_owner_distribution>;

struct node_claim_result {
   sysio::asset  total_allocation;
   sysio::asset  claimed;
   sysio::asset  claimable;
   bool          can_claim;

   SYSLIB_SERIALIZE(node_claim_result,
      (total_allocation)(claimed)(claimable)(can_claim))
};

// ---------------------------------------------------------------------------
// T5 treasury emissions
// ---------------------------------------------------------------------------

struct [[sysio::table("t5state"), sysio::contract("sysio.system")]] t5_state {
   sysio::time_point_sec  start_time;
   uint64_t               epoch_count         = 0;
   // last_epoch_index tracks the most recently-distributed sysio.epoch index.
   // Each payepoch call advances this field from N to N+1 and validates the
   // incoming epoch_index strictly exceeds it (idempotency guard).
   uint32_t               last_epoch_index    = 0;
   sysio::time_point_sec  last_epoch_time;
   int64_t                last_epoch_emission = 0;
   int64_t                total_distributed   = 0;

   // Pay-cadence accumulator. Each non-pay epoch (advance() with
   // is_pay_epoch=false) increments pending_emission_amount by that
   // epoch's per-epoch share via accrueepoch. The pay-epoch reads
   // pending + this-epoch's share as period_emission, distributes it,
   // and resets pending_emission_amount to 0 / period_start_epoch to
   // last_epoch_index+1. With pay_cadence_epochs=1 these two fields
   // are written and immediately reset on every advance, so the
   // legacy per-epoch behavior is unchanged.
   int64_t                pending_emission_amount = 0;
   uint32_t               period_start_epoch      = 0;
   // Per-batch-op-group active-epoch counter, indexed by group number.
   // accrueepoch increments batch_group_epochs[current_batch_op_group]
   // each non-pay epoch; payepoch divides batch_pool proportionally
   // and clears the vector. Sized lazily to current_batch_op_group+1
   // on first use; pre-pay-cadence chains see length 0.
   std::vector<uint32_t>  batch_group_epochs;

   // Cumulative shortfall (WIRE subunits) between requested and actually-
   // funded capital draws. fundclaim caps each request at the remaining
   // pool; any unfunded delta is added here so under-sized pools are
   // visible without breaking the OPP-handler never-throw contract.
   int64_t                capital_shortfall_total = 0;

   SYSLIB_SERIALIZE(t5_state,
      (start_time)(epoch_count)(last_epoch_index)
      (last_epoch_time)(last_epoch_emission)(total_distributed)
      (pending_emission_amount)(period_start_epoch)(batch_group_epochs)
      (capital_shortfall_total))
};

using t5state_t = sysio::kv::global<"t5state"_n, t5_state>;

// ---------------------------------------------------------------------------
// Per-epoch derivations from annual config + canonical epoch_duration_sec.
// Both the gate (sysio.epoch::check_emissions_ready) and the success path
// (sysio.system::payepoch / viewepoch) call these so derived per-epoch
// values cannot drift between the two contracts.
// ---------------------------------------------------------------------------

inline constexpr int64_t SECONDS_PER_YEAR = 365LL * 24 * 60 * 60;

/// Linearly scale a per-year amount to a per-epoch amount.
/// per_epoch = annual * epoch_secs / SECONDS_PER_YEAR.
inline int64_t scale_annual_to_epoch(int64_t annual, uint32_t epoch_duration_sec) {
   return static_cast<int64_t>(
      (static_cast<__int128>(annual) * epoch_duration_sec) / SECONDS_PER_YEAR);
}

/// Per-epoch decay factor in Q32.32, derived from target annual survival
/// ratio and epoch length: factor = (target_bps/10000)^(epoch_secs/year_secs).
/// target_bps == 10000 (no decay) short-circuits to fp_math::ONE.
inline fp_math::fp_t compute_per_epoch_decay(uint16_t target_annual_decay_bps,
                                              uint32_t epoch_duration_sec) {
   const fp_math::fp_t base =
      (static_cast<fp_math::fp_t>(target_annual_decay_bps) << fp_math::FRAC_BITS) / 10000;
   const fp_math::fp_t exponent = fp_math::div(
      static_cast<fp_math::fp_t>(epoch_duration_sec) << fp_math::FRAC_BITS,
      static_cast<fp_math::fp_t>(SECONDS_PER_YEAR) << fp_math::FRAC_BITS);
   return fp_math::pow_frac(base, exponent);
}

// ---------------------------------------------------------------------------
// Pure emission formula. Shared between sysio.system::payepoch (success path)
// and sysio.epoch's readiness gate (precompute path). One source of truth so
// the gate-decided amount cannot drift from what payepoch would have computed.
// Returns 0 when the treasury is at or below the floor (gate sees this as
// EMISSIONS_BLOCK_REASON_TREASURY_EXHAUSTED).
//
// epoch_duration_sec must be the canonical value from sysio.epoch::epochcfg.
// ---------------------------------------------------------------------------

inline int64_t compute_epoch_emission(const emission_config& cfg,
                                      uint32_t epoch_duration_sec,
                                      int64_t prev_emission,
                                      int64_t total_distributed) {
   const int64_t remaining = cfg.t5_distributable - cfg.t5_floor - total_distributed;
   if (remaining <= 0) return 0;

   const fp_math::fp_t factor =
      compute_per_epoch_decay(cfg.target_annual_decay_bps, epoch_duration_sec);
   __int128 product = static_cast<__int128>(prev_emission) * factor;
   int64_t emission = static_cast<int64_t>(product / fp_math::ONE);

   const int64_t per_epoch_max =
      scale_annual_to_epoch(cfg.annual_max_emission, epoch_duration_sec);
   const int64_t per_epoch_min =
      scale_annual_to_epoch(cfg.annual_min_emission, epoch_duration_sec);
   if (emission > per_epoch_max) emission = per_epoch_max;
   if (emission < per_epoch_min) emission = per_epoch_min;

   if (emission > remaining) emission = remaining;

   return emission;
}

// ---------------------------------------------------------------------------
// Pay-cadence accumulator arithmetic. accrueepoch runs as an inline action
// from sysio.epoch::advance and must never throw, so the pending accumulator
// saturates at the asset ceiling instead of wrapping. The readiness gate
// (sysio.epoch::check_emissions_ready) precomputes the same period total and
// payepoch asserts equality between the two, so all of them MUST share this
// one helper: an unsaturated gate sum could exceed any holdable balance once
// the accumulator clamps, blocking epoch advancement permanently.
// ---------------------------------------------------------------------------

/// Saturating pay-period accumulation: pending + per_epoch, capped at
/// sysio::asset::max_amount (2^62 - 1). Expects 0 <= pending <= max_amount and
/// per_epoch >= 0 (both enforced upstream).
inline int64_t saturating_accrue(int64_t pending, int64_t per_epoch) {
   const int64_t room = sysio::asset::max_amount - pending;
   return pending + (per_epoch <= room ? per_epoch : room);
}

/// Largest per-epoch emission the curve can produce under cfg at the given
/// epoch duration: the scaled initial emission (first-epoch path, capped only
/// by the remaining treasury) or the scaled annual ceiling (the decay-path
/// clamp in compute_epoch_emission), whichever is larger.
inline int64_t per_epoch_emission_ceiling(const emission_config& cfg, uint32_t epoch_duration_sec) {
   return std::max(scale_annual_to_epoch(cfg.annual_initial_emission, epoch_duration_sec),
                   scale_annual_to_epoch(cfg.annual_max_emission, epoch_duration_sec));
}

/// True when the worst-case pay-period accumulation stays inside the asset
/// range: whatever is already pending plus a full pay cadence of ceiling-rate
/// epochs must not exceed sysio::asset::max_amount. Enforced at both config
/// boundaries that feed the accumulator -- sysio.system::setemitcfg (annual
/// emission values, pay cadence) and sysio.epoch::setconfig (epoch duration,
/// which scales the per-epoch ceiling linearly) -- so no accepted
/// configuration can drive saturating_accrue to its clamp mid-period.
inline bool period_accrual_fits_asset_range(const emission_config& cfg,
                                            uint32_t epoch_duration_sec,
                                            int64_t pending_emission_amount) {
   const __int128 worst_case =
      static_cast<__int128>(pending_emission_amount) +
      static_cast<__int128>(per_epoch_emission_ceiling(cfg, epoch_duration_sec)) * cfg.pay_cadence_epochs;
   return worst_case <= static_cast<__int128>(sysio::asset::max_amount);
}

// ---------------------------------------------------------------------------
// Per-epoch audit log (unpruned)
// ---------------------------------------------------------------------------

struct epochlog_key {
   // Primary key mirrors sysio.epoch::current_epoch_index (same value as
   // t5_state::last_epoch_index at write time). Aligns the audit log with
   // sysio.epoch's own sense of "epoch N" for forensics.
   uint64_t sysio_epoch_index;
   SYSLIB_SERIALIZE(epochlog_key, (sysio_epoch_index))
};

struct [[sysio::table("epochlog"), sysio::contract("sysio.system")]] epoch_log {
   uint32_t               sysio_epoch_index = 0;  // mirrors t5_state.last_epoch_index / sysio.epoch::current_epoch_index
   uint64_t               epoch_count       = 0;  // internal counter of payepoch invocations
   sysio::time_point_sec  timestamp;
   // Total budget for the period (curve output). compute + capex + governance
   // are paid out of this at payepoch; the implicit capital reserve
   // (= total_emission - compute - capex - governance) is drained lazily
   // by fundclaim across the same period and never appears here.
   int64_t                total_emission    = 0;
   int64_t                compute_amount    = 0;
   int64_t                capex_amount      = 0;
   int64_t                governance_amount = 0;
   // Swap-fee rewards (sysio.reserv rewards_bucket) actually distributed to
   // producers + batch operators this period, ON TOP of the emission above.
   // Sourced from collected swap fees, not the T5 treasury, so it is NOT
   // included in total_emission / total_distributed.
   int64_t                fee_distributed   = 0;

   SYSLIB_SERIALIZE(epoch_log,
      (sysio_epoch_index)(epoch_count)(timestamp)(total_emission)
      (compute_amount)(capex_amount)(governance_amount)(fee_distributed))
};

using epochlog_t = sysio::kv::table<"epochlog"_n, epochlog_key, epoch_log>;

// ---------------------------------------------------------------------------
// Read-only return types (used by viewepoch / viewnodedist)
// ---------------------------------------------------------------------------

struct epoch_info_result {
   uint64_t               epoch_count         = 0;
   uint32_t               last_epoch_index    = 0;
   sysio::time_point_sec  last_epoch_time;
   int64_t                last_epoch_emission = 0;
   int64_t                total_distributed   = 0;
   int64_t                treasury_remaining  = 0;
   int64_t                next_emission_est   = 0;
   uint32_t               seconds_until_next  = 0;

   SYSLIB_SERIALIZE(epoch_info_result,
      (epoch_count)(last_epoch_index)(last_epoch_time)(last_epoch_emission)
      (total_distributed)(treasury_remaining)(next_emission_est)(seconds_until_next))
};

} // namespace sysiosystem::emissions

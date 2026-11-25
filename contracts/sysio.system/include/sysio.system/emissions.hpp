#pragma once

#include <sysio.system/fp_math.hpp>
#include <sysio.opp.common/safe_ops.hpp>

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
// free of protobuf dependencies (its one sysio.opp.common include, safe_ops.hpp,
// is dependency-free) so that downstream contracts (notably sysio.roa) can
// include it to check emitcfg_t::exists() before invoking addnodeowner. The
// opreg / token / epoch cross-contract reads used by payepoch and viewepoch
// live in emissions.cpp (implementation detail, not public API).

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

/// Longest pay period, in economic epochs: bounds how long earned pay waits for its payout.
inline constexpr uint16_t MAX_PAY_CADENCE_EPOCHS = 10;

/// `batchduty` rows one settlement step examines. Bounds every step -- inside `payepoch`, inside
/// `settleduty` and inside `rcrdduty` -- however many operators served in the settling period; a
/// larger roster settles across several economic epochs.
inline constexpr uint32_t MAX_DUTY_SETTLEMENT_ROWS = 50;

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
   // The economic epoch length is canonical on sysio.epoch::economycfg::epoch_duration_sec;
   // accrueepoch / payepoch / viewepoch read it cross-contract. Keeping a duplicate here
   // would let the two values drift and silently break producer-pay math.
   //
   // Decay and emission caps are expressed in *annual* terms; per-epoch
   // values are derived inside compute_epoch_emission from the canonical
   // economic epoch_duration_sec, so the wall-clock emission curve does not change
   // when governance retunes the economic epoch length.
   uint16_t  target_annual_decay_bps; // surviving fraction per year, 1..10000
   int64_t   annual_initial_emission; // E_0 expressed per year
   int64_t   annual_max_emission;     // per-year ceiling
   int64_t   annual_min_emission;     // per-year floor

   // Category splits (basis points). compute + capex + governance must
   // sum to <= 10000; any unallocated remainder stays in the treasury.
   // LIQ yield draws no treasury bonus.
   uint16_t  compute_bps;
   uint16_t  capex_bps;
   uint16_t  governance_bps;

   // Compute sub-split (basis points, must sum to 10000)
   uint16_t  producer_bps;
   uint16_t  batch_op_bps;

   // Producer config
   uint32_t  standby_end_rank;       // last standby rank (default 28)
   // Share of the producer pool reserved for the standby retainer (basis points, <= 10000). The
   // rest funds the per-block rate active producers are paid at. Each standby POSITION
   // (22..standby_end_rank) holds a fixed, linearly decaying share of this slice; a vacant
   // position's share stays in the treasury rather than flowing to the standbys present.
   uint16_t  standby_bps;

   // Audit-log retention. Caps the unbounded `epochlog` table at this many
   // rows; payepoch prunes head-first after each insert. There is one row per
   // payment period (one per epoch when pay_cadence_epochs == 1).
   uint32_t  epoch_log_retention_count;

   // How many economic epochs accumulate before `payepoch` fires. 1 = pay every
   // economic epoch. Must be in [1, MAX_PAY_CADENCE_EPOCHS].
   uint16_t  pay_cadence_epochs;

   SYSLIB_SERIALIZE(emission_config,
      (t1_allocation)(t2_allocation)(t3_allocation)
      (t1_duration)(t2_duration)(t3_duration)(min_claimable)
      (t5_distributable)(t5_floor)
      (target_annual_decay_bps)
      (annual_initial_emission)(annual_max_emission)(annual_min_emission)
      (compute_bps)(capex_bps)(governance_bps)
      (producer_bps)(batch_op_bps)
      (standby_end_rank)(standby_bps)(epoch_log_retention_count)
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
// Claimable epoch pay (producers, standbys, batch operators)
// ---------------------------------------------------------------------------

// payepoch runs as an inline action from sysio.epoch::tickeconomy and must never abort. It therefore
// does NOT push `sysio.token::transfer` to recipients: a transfer notifies `to`, and a recipient
// whose on_notify handler asserts (or burns CPU) would abort the tick and stall the economic clock
// chain-wide. Pay is credited here instead and pulled later by `claimpay`, which carries the
// claimant's own authority -- so a hostile recipient blocks only its own payout.
//
// The two CATEGORY BUCKETS (`sysio.ops`, `sysio.gov`) are the deliberate exception and are still
// pushed -- see the transfer site in payepoch. They hold no code today, so no handler exists to
// abort `tickeconomy`, and any contract governance later deploys there must not assert on a
// transfer notify. Being `sysio.*` they can neither sign a claim (`sysio.roa` zeroes net/cpu for
// that prefix) nor host a contract to emit one inline, so crediting them would strand the pay
// permanently. This table's recipients are therefore the accounts that CAN claim.
//
// Rows do not expire: this is earned pay, held until claimed.
//
// The recipient set is bounded only CONCURRENTLY (the schedulable rows one payepoch walk reaches,
// capped at max_rank_walk_rows, plus the batchduty rows one settlement step reaches) — not across this table's lifetime.
// It is NOT `standby_end_rank`: no-forfeiture pays carried blocks well below the standby band. Producers and batch
// operators churn, and every departed account that never calls `claimpay` leaves a row billed to
// the sysio RAM pool forever, so system-funded claim storage grows with historical participants
// rather than with the live set. That is a known, accepted cost here: expiring earned pay is an
// economic decision, not a RAM one. Returned collateral, banked operator yield and DClaim
// rewards follow the same no-expiry policy.
//
/// WIRE-339 policy: earned pay never expires, including after a recipient stops participating.
/// Rows deliberately omit expiry metadata and indexes; balances remain reserved until claimed.

struct payclaim_key {
   uint64_t account_name;
   SYSLIB_SERIALIZE(payclaim_key, (account_name))
};

/// Earned WIRE held indefinitely for the recipient to claim.
struct [[sysio::table("payclaims"), sysio::contract("sysio.system")]] pay_claim {
   sysio::name account_name;
   uint64_t    balance = 0;   ///< Atomic WIRE units owed, not yet claimed.

   SYSLIB_SERIALIZE(pay_claim, (account_name)(balance))
};

using payclaims_t = sysio::kv::table<"payclaims"_n, payclaim_key, pay_claim>;

// Running total of every outstanding `payclaims` balance.
//
// The WIRE backing unclaimed rows sits in sysio's token balance but is already owed, so every
// gate that spends against that balance must reserve it -- otherwise the treasury double-commits
// and a later `claimpay` fails on overdraw, stranding earned pay. Treasury withdrawals
// and `sysio.epoch`'s emissions readiness gate reserve this total.
//
// Held as a maintained counter rather than recomputed by scanning `payclaims`, because the
// emissions gate reads it on EVERY economic epoch and an O(rows) scan there would grow with the standby set. Kept
// in its own singleton rather than folded into `t5_state` so the two evolve independently (the
// same reason `node_count_state` is separate from `emission_state`).
struct [[sysio::table("payclaimtot"), sysio::contract("sysio.system")]] pay_claim_total {
   uint64_t outstanding = 0;

   SYSLIB_SERIALIZE(pay_claim_total, (outstanding))
};

using payclaimtot_t = sysio::kv::global<"payclaimtot"_n, pay_claim_total>;

// ---------------------------------------------------------------------------
// T5 treasury emissions
// ---------------------------------------------------------------------------

/// T5 emission state. Every "epoch" here is an ECONOMIC epoch (sysio.epoch::economystate), never
/// the consensus epoch.
///
/// The ABI is generated from the declarations while the wasm serializes in SYSLIB_SERIALIZE order,
/// so the two lists below must stay in the same order.
struct [[sysio::table("t5state"), sysio::contract("sysio.system")]] t5_state {
   /// The economic clock's start time (sysio.epoch::economycfg), copied at initt5.
   sysio::time_point_sec  start_time;
   uint64_t               epoch_count         = 0;   ///< payepoch invocations
   /// The most recently accrued economic epoch. accrueepoch requires each new index to exceed it.
   uint32_t               last_epoch_index    = 0;
   sysio::time_point_sec  last_epoch_time;
   /// Most recent positive per-epoch share: the base the next share decays from. A zero-share epoch leaves it
   /// unchanged. Near the end of the budget it may itself be a share clamped to the remainder.
   int64_t                last_epoch_emission = 0;
   /// Length of the economic epoch `last_epoch_emission` accrued for. The next share rescales the base from it to
   /// the length of the epoch that share accrues for, so a duration change keeps the emission per second.
   uint32_t               last_epoch_duration_sec = 0;
   int64_t                total_distributed   = 0;

   /// Pay-cadence accumulator. accrueepoch adds each economic epoch's share; the pay epoch
   /// distributes the total and resets it, and period_start_epoch to the next economic epoch.
   /// The pending amount is already owed, so net_headroom counts it against the T5 budget.
   int64_t                pending_emission_amount = 0;
   uint32_t               period_start_epoch      = 0;

   /// Block slots the open pay period is entitled to: each accruing tick adds the slots of the economic
   /// epoch it closes, from the previous boundary to its own, so the count spans the boundaries of the
   /// blocks the period pays even across a duration change. Reset with `pending_emission_amount`.
   uint64_t               pending_nominal_slots = 0;

   /// Batch emission of a paid period not yet credited to its operators: the settling period's
   /// pool less what its settlement has credited so far. Still inside the T5 budget (it is not
   /// yet distributed) but already committed, so net_headroom and the readiness gate's spendable
   /// balance both exclude it. Zeroed when the settlement completes, releasing any remainder.
   int64_t                unsettled_batch_emission = 0;

   SYSLIB_SERIALIZE(t5_state,
      (start_time)(epoch_count)(last_epoch_index)
      (last_epoch_time)(last_epoch_emission)(last_epoch_duration_sec)(total_distributed)
      (pending_emission_amount)(period_start_epoch)
      (pending_nominal_slots)(unsettled_batch_emission))
};

using t5state_t = sysio::kv::global<"t5state"_n, t5_state>;

/// sysio's WIRE `balance` less what is already owed out of it -- pay credited by payepoch and not yet
/// pulled, and the batch emission still settling -- floored at 0. The emissions readiness gate and
/// `claimnodedis` both spend only this amount, so neither can draw on WIRE the other has committed.
inline int64_t spendable_wire(int64_t balance, const pay_claim_total& claims, const t5_state& state) {
   const __int128 spendable = static_cast<__int128>(balance) - static_cast<__int128>(claims.outstanding)
                            - static_cast<__int128>(state.unsettled_batch_emission);
   return spendable > 0 ? static_cast<int64_t>(spendable) : 0;
}

// ---------------------------------------------------------------------------
// Batch-operator duty. Each consensus epoch close credits one duty unit to every member of the
// group that was on duty (rcrdduty); each payepoch closes the open pay period and splits its batch
// pool pro rata to those units. Settlement walks `batchduty` in bounded steps, so the number of operators that served in
// a period bounds no single transaction.
// ---------------------------------------------------------------------------

struct batch_duty_key {
   uint64_t account_name;
   SYSLIB_SERIALIZE(batch_duty_key, (account_name))
};

/// `batchduty`'s `byperiod` key: rows ordered by period, then account.
inline uint128_t duty_period_key(uint32_t period, uint64_t account) {
   return (static_cast<uint128_t>(period) << 64) | static_cast<uint128_t>(account);
}

/// One batch operator's unpaid duty. The row exists only while it holds unpaid units; settlement
/// erases it, and a row of a period that closed without a settlement is pruned by `settleduty`.
struct [[sysio::table("batchduty"), sysio::contract("sysio.system")]] batch_duty {
   sysio::name account_name;
   uint32_t    period = 0;   ///< pay period the units belong to (duty_state::open_period when credited)
   uint32_t    units  = 0;   ///< consensus epochs served on duty in that period

   uint128_t by_period() const { return duty_period_key(period, account_name.value); }

   SYSLIB_SERIALIZE(batch_duty, (account_name)(period)(units))
};

using batchduty_t = sysio::kv::table<"batchduty"_n, batch_duty_key, batch_duty,
   sysio::kv::index<"byperiod"_n, sysio::const_mem_fun<batch_duty, uint128_t, &batch_duty::by_period>>>;

/// Duty accounting: the open pay period and, while one is being paid out, the settling period.
/// A period number is only an accounting label, independent of both clocks.
struct [[sysio::table("dutystate"), sysio::contract("sysio.system")]] duty_state {
   uint32_t open_period          = 1;   ///< pay period new duty accrues to
   uint64_t open_units           = 0;   ///< duty units accrued to the open period
   uint32_t settling_period      = 0;   ///< closed period being paid out; 0 when none is
   int64_t  settling_pool        = 0;   ///< batch emission of the settling period
   uint64_t settling_units       = 0;   ///< duty units of the settling period
   int64_t  settling_paid        = 0;   ///< credited to the settling period's operators so far
   uint32_t settling_epoch_index = 0;   ///< economic epoch that paid it (its epochlog row)
   uint64_t settle_cursor        = 0;   ///< account the next settlement step starts at, within the settling period

   bool settling() const { return settling_period != 0; }

   SYSLIB_SERIALIZE(duty_state,
      (open_period)(open_units)(settling_period)(settling_pool)(settling_units)(settling_paid)
      (settling_epoch_index)(settle_cursor))
};

using dutystate_t = sysio::kv::global<"dutystate"_n, duty_state>;

// ---------------------------------------------------------------------------
// Per-economic-epoch derivations from annual config + an economic epoch's length.
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
// T5 budget headroom. The readiness gate, viewepoch and setemitcfg's post-init guard all read the budget through these
// two helpers.
// ---------------------------------------------------------------------------

/// T5 budget left before the floor: t5_distributable - t5_floor - total_distributed.
///
/// Gross of the open pay period's accrual. The readiness gate decides TREASURY_EXHAUSTED on it; no accrual is sized
/// against it.
inline int64_t gross_headroom(const emission_config& cfg, const t5_state& state) {
   return cfg.t5_distributable - cfg.t5_floor - state.total_distributed;
}

/// Gross headroom less the emission the open pay period has accrued and the batch emission still
/// settling: the budget still free to commit.
///
/// Accruals are sized against it, which keeps
/// total_distributed + pending_emission_amount + unsettled_batch_emission <= t5_distributable - t5_floor.
inline int64_t net_headroom(const emission_config& cfg, const t5_state& state) {
   return gross_headroom(cfg, state) - state.pending_emission_amount - state.unsettled_batch_emission;
}

/// Whether the next accrual is one of the first pay period's scheduled epochs, which accrue the initial emission: no
/// payout has run, and the last epoch accrued, if any, was not yet that period's pay epoch. A first payout held or
/// deferred past its pay epoch therefore decays from then on, as it would have after paying.
inline bool in_initial_emission_window(const emission_config& cfg, const t5_state& state) {
   if (state.epoch_count != 0) return false;
   if (state.last_epoch_index == 0) return true;
   return uint64_t{state.last_epoch_index} + 1 < uint64_t{state.period_start_epoch} + cfg.pay_cadence_epochs;
}

/// The per-epoch share the next accrual commits, capped at the net headroom.
///
/// The first pay period's scheduled epochs (in_initial_emission_window) accrue the scaled initial emission; every
/// later accrual decays the last positive share, rescaled from the length of the epoch it accrued for to this one's,
/// and clamps it to the per-epoch min and max. Both the share and the decay follow the epoch's length, so the emission
/// per second does not move when the duration changes. The share is 0 once the open period has committed the rest of
/// the budget, or when the curve itself is 0. Shared by sysio.epoch's readiness gate, which accrues it, and viewepoch,
/// which reports it, so the estimate cannot drift from the accrual.
///
/// epoch_duration_sec is the length of the economic epoch the share accrues for: the one the tick closes
/// (sysio.epoch::epoch::closing_epoch_duration_sec).
inline int64_t compute_epoch_emission(const emission_config& cfg,
                                      uint32_t epoch_duration_sec,
                                      const t5_state& state) {
   const int64_t headroom = net_headroom(cfg, state);
   if (headroom <= 0) return 0;

   int64_t emission = 0;
   if (in_initial_emission_window(cfg, state)) {
      emission = scale_annual_to_epoch(cfg.annual_initial_emission, epoch_duration_sec);
   } else {
      // Guards the division: a zero length takes the base unscaled.
      const __int128 base = state.last_epoch_duration_sec == 0
         ? static_cast<__int128>(state.last_epoch_emission)
         : static_cast<__int128>(state.last_epoch_emission) * epoch_duration_sec / state.last_epoch_duration_sec;
      const fp_math::fp_t factor =
         compute_per_epoch_decay(cfg.target_annual_decay_bps, epoch_duration_sec);
      __int128 decayed = base * factor / fp_math::ONE;

      const int64_t per_epoch_max =
         scale_annual_to_epoch(cfg.annual_max_emission, epoch_duration_sec);
      const int64_t per_epoch_min =
         scale_annual_to_epoch(cfg.annual_min_emission, epoch_duration_sec);
      if (decayed > per_epoch_max) decayed = per_epoch_max;
      if (decayed < per_epoch_min) decayed = per_epoch_min;
      emission = static_cast<int64_t>(decayed);
   }

   return std::min(emission, headroom);
}

// ---------------------------------------------------------------------------
// Pay-cadence accumulator arithmetic. accrueepoch runs as an inline action
// from sysio.epoch::tickeconomy and must never throw, so the pending accumulator
// saturates at the asset ceiling instead of wrapping. The readiness gate
// (sysio.epoch::check_emissions_ready) precomputes the same period total and
// payepoch asserts equality between the two, so all of them MUST share this
// one helper: an unsaturated gate sum could exceed any holdable balance once
// the accumulator clamps, blocking every payout permanently.
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
/// economic epochs must not exceed sysio::asset::max_amount. Enforced at both config
/// boundaries that feed the accumulator -- sysio.system::setemitcfg (annual
/// emission values, pay cadence) and sysio.epoch::seteconomy (economic epoch duration,
/// which scales the per-epoch ceiling linearly) -- so no accepted
/// configuration can drive saturating_accrue to its clamp mid-period. Both pass the
/// longest epoch a tick can still close (sysio.epoch::epoch::longest_accruing_epoch_sec).
inline bool period_accrual_fits_asset_range(const emission_config& cfg,
                                            uint32_t epoch_duration_sec,
                                            int64_t pending_emission_amount) {
   const __int128 worst_case =
      static_cast<__int128>(pending_emission_amount) +
      static_cast<__int128>(per_epoch_emission_ceiling(cfg, epoch_duration_sec)) * cfg.pay_cadence_epochs;
   return worst_case <= static_cast<__int128>(sysio::asset::max_amount);
}

// ---------------------------------------------------------------------------
// Per-payout audit log (pruned to emission_config::epoch_log_retention_count)
// ---------------------------------------------------------------------------

struct epochlog_key {
   /// The economic epoch whose payepoch wrote the row (t5_state::last_epoch_index at write time).
   uint64_t economic_epoch_index;
   SYSLIB_SERIALIZE(epochlog_key, (economic_epoch_index))
};

struct [[sysio::table("epochlog"), sysio::contract("sysio.system")]] epoch_log {
   uint32_t               economic_epoch_index = 0;  ///< the paying economic epoch
   uint64_t               epoch_count          = 0;  ///< payepoch invocations, this one included
   sysio::time_point_sec  timestamp;
   // Total budget for the period (curve output). compute + capex + governance
   // are paid out of this at payepoch; the unallocated remainder stays in
   // the treasury and is not a separate payout category.
   int64_t                total_emission    = 0;
   int64_t                compute_amount    = 0;
   int64_t                capex_amount      = 0;
   int64_t                governance_amount = 0;
   /// The period's batch-operator pool, split by duty units over the settlement that follows.
   int64_t                batch_emission          = 0;
   /// Batch emission the settlement left in the treasury: a period with no duty, members no
   /// longer ACTIVE, and integer-division remainders. Final once `batch_settled` is true.
   int64_t                batch_emission_retained = 0;
   bool                   batch_settled           = false;

   SYSLIB_SERIALIZE(epoch_log,
      (economic_epoch_index)(epoch_count)(timestamp)(total_emission)
      (compute_amount)(capex_amount)(governance_amount)
      (batch_emission)(batch_emission_retained)(batch_settled))
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
   /// Net headroom, floored at 0: the budget not yet paid out or accrued to the open pay period.
   int64_t                treasury_remaining  = 0;
   /// The share the readiness gate would accrue next, so never above treasury_remaining.
   int64_t                next_emission_est   = 0;
   uint32_t               seconds_until_next  = 0;

   SYSLIB_SERIALIZE(epoch_info_result,
      (epoch_count)(last_epoch_index)(last_epoch_time)(last_epoch_emission)
      (total_distributed)(treasury_remaining)(next_emission_est)(seconds_until_next))
};

} // namespace sysiosystem::emissions

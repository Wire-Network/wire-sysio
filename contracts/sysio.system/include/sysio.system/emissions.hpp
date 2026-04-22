#pragma once

#include <sysio/asset.hpp>
#include <sysio/kv_global.hpp>
#include <sysio/kv_table.hpp>
#include <sysio/name.hpp>
#include <sysio/symbol.hpp>
#include <sysio/system.hpp>
#include <sysio/time.hpp>

#include <cstdint>

// Core emissions tables owned by sysio.system. This header is intentionally
// free of OPP / protobuf dependencies so that downstream contracts (notably
// sysio.roa) can include it to check emitcfg_t::exists() before invoking
// addnodeowner. The opreg / epoch read-only mirrors used by processepoch
// live in emissions.cpp (implementation detail, not public API).

namespace sysiosystem::emissions {

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
   uint32_t  epoch_duration_secs;    // epoch length
   int64_t   decay_numerator;        // decay formula numerator
   int64_t   decay_denominator;      // decay formula denominator
   int64_t   epoch_initial_emission; // E_0
   int64_t   epoch_max_emission;     // ceiling
   int64_t   epoch_min_emission;     // floor

   // Category splits (basis points, must sum to 10000)
   uint16_t  compute_bps;
   uint16_t  capital_bps;
   uint16_t  capex_bps;
   uint16_t  governance_bps;

   // Compute sub-split (basis points, must sum to 10000)
   uint16_t  producer_bps;
   uint16_t  batch_op_bps;

   // Producer config
   uint32_t  standby_end_rank;       // last standby rank (default 28)

   SYSLIB_SERIALIZE(emission_config,
      (t1_allocation)(t2_allocation)(t3_allocation)
      (t1_duration)(t2_duration)(t3_duration)(min_claimable)
      (t5_distributable)(t5_floor)(epoch_duration_secs)
      (decay_numerator)(decay_denominator)
      (epoch_initial_emission)(epoch_max_emission)(epoch_min_emission)
      (compute_bps)(capital_bps)(capex_bps)(governance_bps)
      (producer_bps)(batch_op_bps)
      (standby_end_rank))
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
   // Each processepoch call distributes exactly one epoch by advancing this
   // field from N to N+1; it doubles as an idempotency guard.
   uint32_t               last_epoch_index    = 0;
   sysio::time_point_sec  last_epoch_time;
   int64_t                last_epoch_emission = 0;
   int64_t                total_distributed   = 0;

   SYSLIB_SERIALIZE(t5_state,
      (start_time)(epoch_count)(last_epoch_index)
      (last_epoch_time)(last_epoch_emission)(total_distributed))
};

using t5state_t = sysio::kv::global<"t5state"_n, t5_state>;

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
   uint64_t               epoch_count       = 0;  // internal counter of processepoch invocations
   sysio::time_point_sec  timestamp;
   int64_t                total_emission    = 0;
   int64_t                compute_amount    = 0;
   int64_t                capital_amount    = 0;
   int64_t                capex_amount      = 0;
   int64_t                governance_amount = 0;

   SYSLIB_SERIALIZE(epoch_log,
      (sysio_epoch_index)(epoch_count)(timestamp)(total_emission)
      (compute_amount)(capital_amount)(capex_amount)(governance_amount))
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

#pragma once

#include <cstdint>
#include <sysio/asset.hpp>
#include <sysio/singleton.hpp>
#include <sysio/symbol.hpp>
#include <sysio/multi_index.hpp>
#include <sysio/name.hpp>

namespace sysiosystem::emissions {

// ---------------------------------------------------------------------------
// Emission configuration (set via setemitcfg action)
// ---------------------------------------------------------------------------

struct [[sysio::table("emitcfg"), sysio::contract("sysio.system")]] emission_config {
   // Node owner allocations (WIRE subunits, 9-decimal)
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

typedef sysio::singleton<"emitcfg"_n, emission_config> emitcfg_t;

// ---------------------------------------------------------------------------
// Emission state (node owner distribution start time)
// ---------------------------------------------------------------------------

struct [[sysio::table("emissionmngr"), sysio::contract("sysio.system")]] emission_state {
   sysio::time_point_sec    node_rewards_start;

   SYSLIB_SERIALIZE(emission_state, (node_rewards_start))
};

typedef sysio::singleton<"emissionmngr"_n, emission_state> emissionstate_t;

// ---------------------------------------------------------------------------
// Node Owner Distribution table
// ---------------------------------------------------------------------------

struct [[sysio::table("nodedist"), sysio::contract("sysio.system")]] node_owner_distribution {
   sysio::name          account_name;
   sysio::asset         total_allocation;
   sysio::asset         claimed;
   uint32_t             total_duration;

   uint64_t primary_key() const { return account_name.value; }
};

typedef sysio::multi_index<"nodedist"_n, node_owner_distribution> nodedist_t;

struct node_claim_result {
   sysio::asset  total_allocation;
   sysio::asset  claimed;
   sysio::asset  claimable;
   bool          can_claim;

   SYSLIB_SERIALIZE(node_claim_result,
      (total_allocation)(claimed)(claimable)(can_claim))
};

// ---------------------------------------------------------------------------
// T5 Treasury Emissions
// ---------------------------------------------------------------------------

struct [[sysio::table("t5state"), sysio::contract("sysio.system")]] t5_state {
   sysio::time_point_sec  start_time;
   uint64_t               epoch_count        = 0;
   sysio::time_point_sec  last_epoch_time;
   int64_t                last_epoch_emission = 0;
   int64_t                total_distributed   = 0;

   SYSLIB_SERIALIZE(t5_state, (start_time)(epoch_count)(last_epoch_time)
                    (last_epoch_emission)(total_distributed))
};

typedef sysio::singleton<"t5state"_n, t5_state> t5state_t;

// Per-epoch audit log (unpruned)
struct [[sysio::table("epochlog"), sysio::contract("sysio.system")]] epoch_log {
   uint64_t               epoch_num;
   sysio::time_point_sec  timestamp;
   int64_t                total_emission    = 0;
   int64_t                compute_amount    = 0;
   int64_t                capital_amount    = 0;
   int64_t                capex_amount      = 0;
   int64_t                governance_amount = 0;

   uint64_t primary_key() const { return epoch_num; }

   SYSLIB_SERIALIZE(epoch_log, (epoch_num)(timestamp)(total_emission)
                    (compute_amount)(capital_amount)(capex_amount)(governance_amount))
};

typedef sysio::multi_index<"epochlog"_n, epoch_log> epochlog_t;

// ---------------------------------------------------------------------------
// Read-only return types
// ---------------------------------------------------------------------------

struct epoch_info_result {
   uint64_t               epoch_count        = 0;
   sysio::time_point_sec  last_epoch_time;
   int64_t                last_epoch_emission = 0;
   int64_t                total_distributed   = 0;
   int64_t                treasury_remaining  = 0;
   int64_t                next_emission_est   = 0;
   uint32_t               seconds_until_next  = 0;

   SYSLIB_SERIALIZE(epoch_info_result, (epoch_count)(last_epoch_time)(last_epoch_emission)
                    (total_distributed)(treasury_remaining)(next_emission_est)(seconds_until_next))
};

}

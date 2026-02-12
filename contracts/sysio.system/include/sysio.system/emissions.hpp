#pragma once

#include <cstdint>
#include <sysio/asset.hpp>
#include <sysio/singleton.hpp>
#include <sysio/symbol.hpp>
#include <sysio/multi_index.hpp>
#include <sysio/name.hpp>

namespace sysiosystem::emissions {

/**
 * Singleton table to manage emissions variables.
 */
struct [[sysio::table, sysio::contract("sysio.system")]] emission_state {
   sysio::time_point_sec    node_rewards_start;

   SYSLIB_SERIALIZE(emission_state, (node_rewards_start))
};

typedef sysio::singleton<"emissionmngr"_n, emission_state> emissionstate_t;

/**
 * The Node Owner Distribution table tracks all claimed / unclaimed $WIRE of Node Owners.
 *
 * Each Node Owner is allocated a certain amount of $WIRE based on their Tier.
 * Each Tier 1 - 3 has a different distribution schedule, where starting time of distribution
 * is a static value set on boot of Wire Network.
 */
struct [[sysio::table, sysio::contract("sysio.system")]] node_owner_distribution
{
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
      (total_allocation)
      (claimed)
      (claimable)
      (can_claim)
   )
};

// ---------------------------------------------------------------------------
// T5 Treasury Emissions
// ---------------------------------------------------------------------------

/**
 * Singleton tracking the T5 treasury epoch-based distribution state.
 */
struct [[sysio::table, sysio::contract("sysio.system")]] t5_state {
   sysio::time_point_sec  start_time;
   uint64_t               epoch_count        = 0;
   sysio::time_point_sec  last_epoch_time;
   int64_t                last_epoch_emission = 0;
   int64_t                total_distributed   = 0;

   SYSLIB_SERIALIZE(t5_state, (start_time)(epoch_count)(last_epoch_time)
                    (last_epoch_emission)(total_distributed))
};

typedef sysio::singleton<"t5state"_n, t5_state> t5state_t;

/**
 * Per-epoch audit log (FR-17 / NFR-4).
 */
struct [[sysio::table, sysio::contract("sysio.system")]] epoch_log {
   uint64_t               epoch_num;
   sysio::time_point_sec  timestamp;
   int64_t                total_emission   = 0;
   int64_t                compute_amount   = 0;
   int64_t                capital_amount   = 0;
   int64_t                capex_amount     = 0;
   int64_t                governance_amount = 0;

   uint64_t primary_key() const { return epoch_num; }

   SYSLIB_SERIALIZE(epoch_log, (epoch_num)(timestamp)(total_emission)
                    (compute_amount)(capital_amount)(capex_amount)(governance_amount))
};

typedef sysio::multi_index<"epochlog"_n, epoch_log> epochlog_t;

/**
 * Return type for the viewepoch read-only action.
 */
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

/**
 * Return type for the viewemitcfg read-only action.
 */
struct emission_config_result {
   // T1-T3 node owner allocations
   sysio::asset  t1_allocation;
   sysio::asset  t2_allocation;
   sysio::asset  t3_allocation;
   uint32_t      t1_duration;
   uint32_t      t2_duration;
   uint32_t      t3_duration;
   sysio::asset  min_claimable;

   // T5 treasury
   int64_t       t5_distributable;
   int64_t       t5_floor;
   uint32_t      epoch_duration_secs;
   int64_t       decay_numerator;
   int64_t       decay_denominator;
   int64_t       epoch_initial_emission;
   int64_t       epoch_max_emission;
   int64_t       epoch_min_emission;

   // Category splits (basis points)
   uint16_t      compute_bps;
   uint16_t      capital_bps;
   uint16_t      capex_bps;
   uint16_t      governance_bps;

   // Compute sub-split (basis points)
   uint16_t      producer_bps;
   uint16_t      batch_op_bps;

   // Producer config
   uint32_t      active_producer_count;
   uint32_t      standby_start_rank;
   uint32_t      standby_end_rank;
   uint32_t      blocks_per_round;

   SYSLIB_SERIALIZE(emission_config_result,
      (t1_allocation)(t2_allocation)(t3_allocation)
      (t1_duration)(t2_duration)(t3_duration)(min_claimable)
      (t5_distributable)(t5_floor)(epoch_duration_secs)
      (decay_numerator)(decay_denominator)
      (epoch_initial_emission)(epoch_max_emission)(epoch_min_emission)
      (compute_bps)(capital_bps)(capex_bps)(governance_bps)
      (producer_bps)(batch_op_bps)
      (active_producer_count)(standby_start_rank)(standby_end_rank)(blocks_per_round))
};

}

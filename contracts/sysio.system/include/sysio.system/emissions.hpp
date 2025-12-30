#pragma once

#include <cstdint>
#include <sysio/asset.hpp>
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

}

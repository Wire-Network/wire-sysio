#pragma once

#include <cstdint>
#include <sysio/asset.hpp>
#include <sysio/symbol.hpp>
#include <sysio/multi_index.hpp>
#include <sysio/name.hpp>
#include <sysio.system/uint256.hpp>

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

/**
 * Manages the liq staking meta data for each chain.
 *
 * @param chain            The name of the chain this row is related to.
 * @param total_shares     Total number of shares of all staked users on this chain.
 * @param index            Index multiplier used to determine how much each share is worth at any given time.
 */
struct [[sysio::table, sysio::contract("sysio.system")]] stake_manager
{
   sysio::name       chain;
   wns::uint256_t    total_shares;
   wns::uint256_t    index;

   uint64_t primary_key() const { return chain.value; }
};

typedef sysio::multi_index<"stakemanager"_n, stake_manager> stakemanager_t;

/**
 * TODO: Revisit shares / principal data types.
 *    account_name might actually need to be 'external address', to support historical warrant purchases as account_name isn't known at that time.
 *    Would need to look up against sysio.auth 'links' table.
 *
 * Scoped to chain name: eth, sol, etc..
 *
 * The liq_stake table is tracking all users liquid staked shares per chain, along with their principal.
 */
struct [[sysio::table, sysio::contract("sysio.system")]] liq_stake
{
   sysio::name       account_name; // TODO: Might need to be external_address
   wns::uint256_t    shares;
   wns::uint256_t    principal;

   uint64_t primary_key() const { return account_name.value; }
};

typedef sysio::multi_index<"liqstake"_n, liq_stake> liqstake_t;

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

// wns::uint256_t shareToToken(wns::uint256_t shares);

// wns::uint256_t tokenToShare(wns::uint256_t liqAmt);

}

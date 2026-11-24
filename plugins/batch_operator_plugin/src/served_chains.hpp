#pragma once
/**
 * @file served_chains.hpp
 * @brief Which outpost chains one batch operator relays, from its `sysio.opreg::servchains` memberships and its seats
 *        in each chain's schedule window, and the build of the outpost clients it relays through. Pure functions over
 *        decoded rows, so the plugin's tests drive them without a chain.
 */

#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <fc/exception/exception.hpp>
#include <fc/io/json.hpp>
#include <fc/slug_name.hpp>
#include <fc/variant.hpp>
#include <fc/variant_object.hpp>
#include <magic_enum/magic_enum.hpp>
#include <sysio.opreg/opreg_constants.hpp>
#include <sysio/batch_operator_plugin/table_range.hpp>
#include <sysio/chain/name.hpp>
#include <sysio/opp/types/types.pb.h>
#include <sysio/outpost_client/outpost_client.hpp>

#include "chain_epochs.hpp"

namespace sysio::batch_operator_detail {

using opreg_names::chain_service_status;

/// One active outpost on `sysio.chains`.
struct outpost_descriptor {
   uint64_t                     id         = 0;   ///< the chain's `sysio.chains` code
   sysio::opp::types::ChainKind chain_kind = sysio::opp::types::CHAIN_KIND_UNKNOWN;
   uint32_t                     chain_id   = 0;
   /// Remote contract identities from the row's nested `outpost` struct. EVM rows carry both; an SVM row carries
   /// `opp_addr` alone, because the single outpost program serves both directions. Either may be empty while the
   /// remote contract is not yet deployed, in which case no client is built for the chain.
   std::string opp_addr;
   std::string opp_inbound_addr;
};

/// This operator's membership status on each chain it has a `servchains` row for, by chain code.
using chain_memberships = std::map<uint64_t, chain_service_status>;

/// A `servchains` status as the ABI renders it: the member's name, or its value when the ABI does not name it.
/// Nullopt for a value this build does not know.
inline std::optional<chain_service_status> decode_service_status(const fc::variant& status) {
   using underlying = std::underlying_type_t<chain_service_status>;
   if (status.is_string()) return magic_enum::enum_cast<chain_service_status>(status.get_string());
   if (!status.is_int64() && !status.is_uint64()) return std::nullopt;
   if (status.is_int64() && status.as_int64() < 0) return std::nullopt;
   const uint64_t value = status.as_uint64();
   if (value > std::numeric_limits<underlying>::max()) return std::nullopt;
   return magic_enum::enum_cast<chain_service_status>(static_cast<underlying>(value));
}

/// The JSON bound of `byaccount` key `account_value` that chain_plugin's secondary-key codec expects.
inline std::string byaccount_bound(uint64_t account_value) {
   return fc::json::to_string(fc::mutable_variant_object()(opreg_names::index_byaccount, account_value),
                              fc::json::yield_function_t{});
}

/// The `byaccount` range of every `servchains` row of `account`, one per chain it has a membership on: to the next
/// account's key, exclusive, or to the end of the index for the largest name.
inline index_range membership_range(chain::name account) {
   const uint64_t value = account.to_uint64_t();
   index_range    range{.lower_bound = byaccount_bound(value)};
   if (value != std::numeric_limits<uint64_t>::max()) range.upper_bound = byaccount_bound(value + 1);
   return range;
}

/// `operator_account`'s memberships from `servchains` values read by `byaccount`. A row of another account, or one
/// whose status this build does not know, is left out. Throws when a row does not decode.
inline chain_memberships decode_memberships(const fc::variants& rows, chain::name operator_account) {
   chain_memberships memberships;
   for (const fc::variant& row : rows) {
      const fc::variant_object& membership = row.get_object();
      if (chain::name(membership[opreg_names::field::account].as_string()) != operator_account) continue;
      const std::optional<chain_service_status> status =
         decode_service_status(membership[opreg_names::field::status]);
      if (!status) continue;
      memberships.emplace(membership[opreg_names::field::chain_code].as<fc::slug_name>().value, *status);
   }
   return memberships;
}

/// Whether this operator relays a chain it holds `status` on: SERVING, or LEFT while it still holds a seat in the
/// chain's window, which it still delivers. An EVICTED member's deliveries are refused, so it does not relay.
inline bool relays_chain(chain_service_status status, bool seated) {
   switch (status) {
      case chain_service_status::SERVING: return true;
      case chain_service_status::LEFT:    return seated;
      case chain_service_status::EVICTED: return false;
   }
   return false;
}

/// The chains this operator relays: each membership `relays_chain` accepts, seated per that chain's view (a chain
/// without one seats nobody).
inline std::set<uint64_t> relayed_chains(const chain_memberships& memberships, const chain_epochs& views) {
   std::set<uint64_t> relayed;
   for (const auto& [chain_code, status] : memberships) {
      const auto view   = views.find(chain_code);
      const bool seated = view != views.end() && view->second.election.my_group != GROUP_NONE;
      if (relays_chain(status, seated)) relayed.insert(chain_code);
   }
   return relayed;
}

/// The outposts in `active` on a `relayed` chain, in `active`'s order.
inline std::vector<outpost_descriptor> relayed_outposts(const std::vector<outpost_descriptor>& active,
                                                        const std::set<uint64_t>& relayed) {
   std::vector<outpost_descriptor> outposts;
   for (const outpost_descriptor& op : active) {
      if (relayed.contains(op.id)) outposts.push_back(op);
   }
   return outposts;
}

/// A chain this node cannot serve, and why.
struct unserviceable_chain {
   std::string code;
   std::string why;
};

/// An outpost client, or why there is none.
struct client_build {
   std::shared_ptr<sysio::outpost_client> client;
   /// Why this node cannot serve the chain at all; empty when `client` is set, or when the row only lacks its remote
   /// contract address or is awaiting Ethereum chain-ID discovery.
   std::string                            unserviceable;
};

/// Build, with `wrap(op, client)`, what `built` lacks for each chain in `chains`, each client made by `make(op)`.
/// Returns the chains this node cannot serve; a chain whose client is still pending (no remote address yet, Ethereum
/// discovery) is skipped until a later refresh.
template <typename Key, typename Built, typename Make, typename Wrap>
std::vector<unserviceable_chain> build_missing(std::map<Key, Built>& built,
                                               const std::vector<outpost_descriptor>& chains, Make&& make,
                                               Wrap&& wrap) {
   std::vector<unserviceable_chain> unserviceable;
   for (const outpost_descriptor& op : chains) {
      const Key key{op.id};
      if (built.contains(key)) continue;
      client_build made = make(op);
      if (!made.unserviceable.empty()) {
         unserviceable.push_back({fc::slug_name{op.id}.to_string(), std::move(made.unserviceable)});
         continue;
      }
      if (!made.client) continue;
      built.emplace(key, wrap(op, made.client));
   }
   return unserviceable;
}

} // namespace sysio::batch_operator_detail

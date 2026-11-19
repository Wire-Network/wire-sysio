#pragma once

#include <sysio/net_plugin/protocol.hpp>

#include <fc/mutex.hpp>
#include <fc/thread_utils.hpp>

#include <boost/multi_index_container.hpp>

#include <tuple>

namespace sysio {

/// Key of a gossiped bp entry: its producer, server endpoint and outbound ip address. The expiration and signature
/// are no part of it, so an entry signed anew, or with a later expiration, has the key of the entry it replaces.
using gossip_bp_key_t = std::tuple<const name&, const std::string&, const std::string&>;

/// @return the key of peer, whose peer info is unpacked
inline gossip_bp_key_t gossip_bp_key(const gossip_bp_peers_message::signed_bp_peer& peer) {
   return gossip_bp_key_t(peer.producer_name, peer.server_endpoint(), peer.outbound_ip_address());
}

struct gossip_bp_index_t {
   /// Gossiped bp entries, unique by gossip_bp_key()
   using gossip_bps_index_container_t = boost::multi_index_container<
      gossip_bp_peers_message::signed_bp_peer,
      indexed_by<
         ordered_unique<
            tag<struct by_producer>,
            composite_key< gossip_bp_peers_message::signed_bp_peer,
               member<gossip_bp_peers_message::bp_peer, name, &gossip_bp_peers_message::bp_peer::producer_name>,
               const_mem_fun<gossip_bp_peers_message::signed_bp_peer, const std::string&, &gossip_bp_peers_message::signed_bp_peer::server_endpoint>,
               const_mem_fun<gossip_bp_peers_message::signed_bp_peer, const std::string&, &gossip_bp_peers_message::signed_bp_peer::outbound_ip_address>
            >
         >,
         ordered_non_unique<
            tag< struct by_expiry >,
            const_mem_fun<gossip_bp_peers_message::signed_bp_peer, block_timestamp_type, &gossip_bp_peers_message::signed_bp_peer::expiration>
         >
      >
      >;
   using by_producer_iterator = gossip_bps_index_container_t::index<by_producer>::type::iterator;

   alignas(hardware_destructive_interference_sz)
   mutable fc::mutex                mtx;
   gossip_bps_index_container_t     index GUARDED_BY(mtx);
};


} // namespace sysio

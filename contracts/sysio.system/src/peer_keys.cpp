#include <sysio.system/sysio.system.hpp>
#include <sysio.system/producer_score.hpp>
#include <sysio.system/peer_keys.hpp>

#include <sysio/sysio.hpp>
#include <cassert>
#include <cstdint>

namespace sysiosystem {

void peer_keys::regpeerkey(const name& proposer_finalizer_name, const public_key& key) {
   require_auth(proposer_finalizer_name);
   peer_keys_table pkt(get_self());
   check(!std::holds_alternative<sysio::webauthn_public_key>(key), "webauthn keys not allowed in regpeerkey action");

   auto pk = peerkey_key{proposer_finalizer_name.value};
   if( !pkt.contains(pk) ) {
      pkt.emplace(proposer_finalizer_name, pk, peer_key{
         .account = proposer_finalizer_name,
         .data = peer_key::v0_data{key},
      });
   } else {
      auto row = pkt.get(pk);
      const auto& prev_key = row.get_public_key();
      check(!prev_key || *prev_key != key, "Provided key is the same as currently stored one");
      pkt.modify(same_payer, pk, [&](auto& row) {
         row.set_public_key(key);
      });
   }
}

void peer_keys::delpeerkey(const name& proposer_finalizer_name, const public_key& key) {
   require_auth(proposer_finalizer_name);
   peer_keys_table pkt(get_self());

   auto pk = peerkey_key{proposer_finalizer_name.value};
   check(pkt.contains(pk), "Key not present for name: " + proposer_finalizer_name.to_string());
   auto row = pkt.get(pk);
   const auto& prev_key = row.get_public_key();
   check(prev_key && *prev_key == key, "Current key does not match the provided one");
   pkt.erase(pk);
}

peer_keys::getpeerkeys_res_t peer_keys::getpeerkeys() {
   peer_keys_table  pkt(get_self());
   producers_table  producers(get_self());
   constexpr size_t max_return = 50;
   constexpr uint32_t max_rank = 30;

   getpeerkeys_res_t resp;
   resp.reserve(max_return);

   auto add_peer = [&](const producer_info& p) {
      auto pk = peerkey_key{p.owner.value};
      if (!pkt.contains(pk))
         resp.push_back(peerkeys_t{p.owner, {}});
      else
         resp.push_back(peerkeys_t{p.owner, pkt.get(pk).get_public_key()});
   };

   auto idx = producers.get_index<"prodrank"_n>();

   // `rank` is POSITION among ELIGIBLE producers, so this counts matches rather than taking the
   // first `max_rank` index entries. Taking the first N would let unbonded registrants -- which
   // occupy index slots but can never be scheduled -- crowd real producers out of peer discovery.
   // The demoted tier sorts last and is never eligible, so it also bounds the walk over what is
   // a permissionless, unbounded table.
   //
   // Peer discovery walks `is_eligible_operator`, NOT `is_schedulable`: it must not require a
   // finalizer key. A producer scheduled through `setprods` -- the bootstrap window, and every
   // harness that publishes schedules directly -- produces blocks before it registers one, and a
   // block producer that `getpeerkeys` hides is a block producer the BP gossip mesh cannot reach.
   uint32_t position = 0;
   for (auto i = idx.cbegin(); i != idx.cend() && resp.size() < max_return; ++i) {
      if (producer_rank::tier_of(i->rank_score) == producer_tier::demoted)
         break;
      if (!producer_rank::is_eligible_operator(*i))
         continue;
      if (++position > max_rank)
         break;
      add_peer(*i);
   }

   return resp;
}

} // namespace sysiosystem

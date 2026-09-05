#include <sysio.system/sysio.system.hpp>
#include <sysio.system/producer_score.hpp>
#include <sysio.system/peer_keys.hpp>

#include <sysio/sysio.hpp>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <vector>

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

   // Names already in the response, so a producer seeded from the schedule is not repeated by the
   // rank walk. Bounded by max_return, so the linear scan is trivial.
   std::vector<name> added;
   added.reserve(max_return);

   auto already_added = [&](const name& owner) {
      return std::find(added.begin(), added.end(), owner) != added.end();
   };

   // Keyed by NAME, not by a producers row: a producer scheduled through `setprods` during the
   // bootstrap window may have no `producers` row at all, and its peer key still has to be
   // discoverable. An absent peerkeys row yields an empty key rather than an omission -- the
   // consumer needs to know the producer EXISTS.
   auto add_peer = [&](const name& owner) {
      auto pk = peerkey_key{owner.value};
      if (!pkt.contains(pk))
         resp.push_back(peerkeys_t{owner, {}});
      else
         resp.push_back(peerkeys_t{owner, pkt.get(pk).get_public_key()});
      added.push_back(owner);
   };

   // SEED with the live schedule before ranking anything. `peer_keys_db_t::update_peer_keys`
   // returns early only on an EMPTY response, so a non-empty one ERASES every producer it omits --
   // omitting a producer that is currently producing blocks evicts it from the BP peer map and
   // cuts it out of the gossip mesh. Rank alone does not identify those producers: a demoted one
   // retained by the `min_schedule_size` floor still holds its slot and still produces (its next
   // block is what clears the demotion), yet it sorts into the tier this walk stops at. The
   // schedule is the authority on who is producing; rank only orders the candidates behind them.
   for (const auto& scheduled : sysio::get_active_producers()) {
      if (resp.size() >= max_return) break;
      if (!already_added(scheduled)) add_peer(scheduled);
   }

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
      if (already_added(i->owner))
         continue;
      add_peer(i->owner);
   }

   return resp;
}

} // namespace sysiosystem

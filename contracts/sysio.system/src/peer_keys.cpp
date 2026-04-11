#include <sysio.system/sysio.system.hpp>
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

   for (auto i = idx.cbegin(); i != idx.cend() && resp.size() < max_return; ++i) {
      if (i->rank > max_rank)
         break;
      add_peer(*i);
   }

   return resp;
}

} // namespace sysiosystem

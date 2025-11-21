#pragma once

#include <sysio/contract.hpp>
#include <sysio/crypto.hpp>
#include <sysio/name.hpp>

#include <string>
#include <optional>

namespace sysiosystem {

using sysio::name;
using sysio::public_key;

// -------------------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------------------
struct [[sysio::table("peerkeys"), sysio::contract("sysio.system")]] peer_key {
   struct v0_data {
      std::optional<public_key> pubkey; // peer key for network message authentication
      SYSLIB_SERIALIZE(v0_data, (pubkey))
   };

   name                  account;
   std::variant<v0_data> data;

   uint64_t primary_key() const { return account.value; }

   void                                    set_public_key(const public_key& key) { data = v0_data{key}; }
   const std::optional<sysio::public_key>& get_public_key() const {
      return std::visit([](auto& v) -> const std::optional<sysio::public_key>& { return v.pubkey; }, data);
   }
   void update_row() {}
   void init_row(name n) { *this = peer_key{n, v0_data{}}; }
};

// -------------------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------------------
typedef sysio::multi_index<"peerkeys"_n, peer_key> peer_keys_table;

// -------------------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------------------
struct [[sysio::contract("sysio.system")]] peer_keys : public sysio::contract {
   
   peer_keys(name s, name code, sysio::datastream<const char*> ds)
      : sysio::contract(s, code, ds) {}

   struct peerkeys_t {
      name                      producer_name;
      std::optional<public_key> peer_key;

      SYSLIB_SERIALIZE(peerkeys_t, (producer_name)(peer_key))
   };

   using getpeerkeys_res_t = std::vector<peerkeys_t>;

   /**
    * Action to register a public key for a proposer or finalizer name.
    * This key will be used to validate a network peer's identity.
    * A proposer or finalizer can only have have one public key registered at a time.
    * If a key is already registered for `proposer_finalizer_name`, and `regpeerkey` is
    * called with a different key, the new key replaces the previous one in `peer_keys_table`
    */
   [[sysio::action]]
   void regpeerkey(const name& proposer_finalizer_name, const public_key& key);

   /**
    * Action to delete a public key for a proposer or finalizer name.
    *
    * An existing public key for a given account can be changed by calling `regpeerkey` again.
    */
   [[sysio::action]]
   void delpeerkey(const name& proposer_finalizer_name, const public_key& key);

   /**
    * Returns a list of up to 50 top producers (active *and* non-active, in votes rank order), 
    * along with their peer public key if it was registered via the regpeerkey action.
    */
   [[sysio::action]]
   getpeerkeys_res_t getpeerkeys();

};

} // namespace sysiosystem
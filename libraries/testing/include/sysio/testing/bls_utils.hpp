#pragma once

#include <fc/crypto/bls_private_key.hpp>
#include <fc/crypto/bls_public_key.hpp>
#include <sysio/chain/name.hpp>

namespace sysio::testing {

using bls_key_input_data_t = std::variant<std::string, fc::crypto::bls::private_key, sysio::chain::name>;
using namespace fc::crypto;

inline std::pair<fc::crypto::bls::private_key, fc::crypto::signature_provider_ptr> get_bls_private_key(

   bls_key_input_data_t key_input_data) {
   fc::crypto::bls::private_key privkey;
   std::string key_name;
   if (std::holds_alternative<fc::crypto::bls::private_key>(key_input_data)) {
      privkey = std::get<fc::crypto::bls::private_key>(key_input_data);
   } else if (std::holds_alternative<std::string>(key_input_data)) {
      auto& privkey_str = std::get<std::string>(key_input_data);
      privkey = fc::crypto::bls::private_key(privkey_str);
   } else {
      auto& key_input_name = std::get<sysio::chain::name>(key_input_data);
      key_name = key_input_name.to_string();
      auto secret = fc::sha256::hash(key_name);
      std::vector<uint8_t> seed(secret.data_size());
      memcpy(seed.data(), secret.data(), secret.data_size());
      privkey = fc::crypto::bls::private_key(seed);
   }
   if (key_name.empty())
      key_name = privkey.to_string();
   return std::make_pair(privkey, std::make_shared<fc::crypto::signature_provider_t>(
                            fc::crypto::chain_kind_wire,
                            fc::crypto::chain_key_type_wire_bls,
                            key_name,
                            fc::crypto::public_key{fc::crypto::bls::public_key_shim{privkey.get_public_key().serialize()}},
                            fc::crypto::private_key{fc::crypto::bls::private_key_shim{privkey.get_secret()}},
                            [privkey](const fc::sha256& digest) {
                               return fc::crypto::signature{bls::signature_shim{privkey.sign(digest.to_uint8_array()).serialize()}};
                            }
                         ));
}

inline fc::crypto::signature_provider_ptr get_bls_key_sig_provider(bls_key_input_data_t key_input_data) {
   return get_bls_private_key(key_input_data).second;
}

inline std::tuple<fc::crypto::bls::private_key,
                  fc::crypto::bls::public_key,
                  fc::crypto::bls::signature,
                  fc::crypto::signature_provider_ptr> get_bls_key(bls_key_input_data_t key_input_data) {
   const auto [private_key,sig_prov] = get_bls_private_key(key_input_data);
   return {
      private_key,
      private_key.get_public_key(),
      private_key.proof_of_possession(),
      sig_prov};
}

}
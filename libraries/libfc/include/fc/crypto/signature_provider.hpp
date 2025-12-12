#pragma once
#include <fc/crypto/public_key.hpp>

namespace fc::crypto {
using signature_provider_id_t  = std::variant<std::string, fc::crypto::public_key>;
using signature_provider_sign_fn = std::function<fc::crypto::signature(fc::sha256)>;

/**
 * `signature_provider_entry` constructed provider
 */
struct signature_provider_t  {

   /** The chain/key type */
   fc::crypto::chain_kind_t target_chain;

   /** The chain/key type */
   fc::crypto::chain_key_type_t key_type;

   /** The alias or name assigned to identify this key pair */
   std::string key_name;

   /** The public key component of this key pair */
   fc::crypto::public_key public_key;

   signature_provider_sign_fn sign;
};

using signature_provider_ptr = std::shared_ptr<signature_provider_t>;

}
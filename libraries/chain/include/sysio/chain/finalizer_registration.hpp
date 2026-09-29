#pragma once

#include <sysio/finalizer_registration.hpp>
#include <fc/crypto/bls_private_key.hpp>
#include <fc/crypto/bls_public_key.hpp>
#include <fc/crypto/bls_signature.hpp>

namespace sysio::chain {

/** Create the versioned regfinkey proof while preserving the standard BLS PoP security domain. */
inline std::string make_finalizer_registration_proof(uint64_t account, const fc::crypto::bls::private_key& key) {
   const auto msg = finalizer_registration::message(account, key.get_public_key().serialize());
   const auto signature = key.sign_raw({reinterpret_cast<const uint8_t*>(msg.data()), msg.size()});
   return std::string(finalizer_registration::proof_prefix) + key.proof_of_possession().to_string() + ":" +
          signature.to_string();
}

} // namespace sysio::chain

#include <fc/crypto/bls_utils.hpp>

namespace fc::crypto::bls {

bool verify(const bls::public_key& pubkey,
            std::span<const uint8_t> message,
            const bls::signature& signature) {
   return bls12_381::verify(pubkey.jacobian_montgomery_le(), message, signature.jacobian_montgomery_le());
};

} // fc::crypto::bls
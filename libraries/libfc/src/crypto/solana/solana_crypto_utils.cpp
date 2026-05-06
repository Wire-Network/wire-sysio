#include <fc/crypto/solana/solana_crypto_utils.hpp>

#include <fc/crypto/base58.hpp>
#include <fc/exception/exception.hpp>

#include <cstring>

namespace fc::crypto::solana {

std::string to_base58(const solana_signature& sig) {
   return fc::to_base58(reinterpret_cast<const char*>(sig.data()), sig.size(),
                        fc::yield_function_t{});
}

solana_signature signature_from_base58(const std::string& str) {
   auto bytes = fc::from_base58(str);
   FC_ASSERT(bytes.size() == std::tuple_size_v<solana_signature>,
             "Invalid Solana signature length: expected {}, got {}",
             std::tuple_size_v<solana_signature>, bytes.size());
   solana_signature result{};
   std::memcpy(result.data(), bytes.data(), result.size());
   return result;
}

solana_public_key from_fc_public_key(const fc::crypto::public_key& pk) {
   FC_ASSERT(pk.contains<fc::crypto::ed::public_key_shim>(),
             "Public key must be ED25519 type for Solana");
   return pk.get<fc::crypto::ed::public_key_shim>();
}

solana_signature from_ed_signature(const fc::crypto::ed::signature_shim& sig) {
   // Signature_shim layout: [0..31] embedded pubkey, [32..95] ed25519 sig.
   solana_signature result{};
   std::memcpy(result.data(), sig._data.data() + crypto_sign_PUBLICKEYBYTES, result.size());
   return result;
}

} // namespace fc::crypto::solana

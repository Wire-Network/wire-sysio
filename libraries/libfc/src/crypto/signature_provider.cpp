#include <fc/crypto/chain_types_reflect.hpp>
#include <fc/crypto/key_serdes.hpp>
#include <fc/crypto/signature_provider.hpp>

namespace fc::crypto {

private_key from_native_string_to_private_key(chain_key_type_t key_type, const std::string& private_key_str) {
   switch (key_type) {
   case chain_key_type_wire:
      return from_native_string_to_private_key<chain_key_type_wire>(private_key_str);
   case chain_key_type_wire_bls:
      return from_native_string_to_private_key<chain_key_type_wire_bls>(private_key_str);
   case chain_key_type_ethereum:
      return from_native_string_to_private_key<chain_key_type_ethereum>(private_key_str);
   case chain_key_type_solana:
      return from_native_string_to_private_key<chain_key_type_solana>(private_key_str);
   default:
      FC_THROW_EXCEPTION(fc::unsupported_exception,
                         "No native private-key string form implemented for chain key type: {}",
                         chain_key_type_reflector::to_fc_string(key_type));
   }
}

public_key from_native_string_to_public_key(chain_key_type_t key_type, const std::string& public_key_str) {
   switch (key_type) {
   case chain_key_type_wire:
      return from_native_string_to_public_key<chain_key_type_wire>(public_key_str);
   case chain_key_type_wire_bls:
      return from_native_string_to_public_key<chain_key_type_wire_bls>(public_key_str);
   case chain_key_type_ethereum:
      return from_native_string_to_public_key<chain_key_type_ethereum>(public_key_str);
   case chain_key_type_solana:
      return from_native_string_to_public_key<chain_key_type_solana>(public_key_str);
   default:
      FC_THROW_EXCEPTION(fc::unsupported_exception,
                         "No native public-key string form implemented for chain key type: {}",
                         chain_key_type_reflector::to_fc_string(key_type));
   }
}

sign_fn make_local_sign_fn(const private_key& key) {
   return [key](const sha256& digest) { return key.sign(digest); };
}

std::string to_signature_provider_spec(const std::string& key_name, fc::crypto::chain_kind_t target_chain,
                                       fc::crypto::chain_key_type_t key_type, const std::string& public_key_text,
                                       const std::string& private_key_provider_spec) {
   using namespace fc::crypto;
   return std::format("{},{},{},{},{}", key_name, chain_kind_reflector::to_fc_string(target_chain),
                      chain_key_type_reflector::to_fc_string(key_type), public_key_text, private_key_provider_spec);
}

}
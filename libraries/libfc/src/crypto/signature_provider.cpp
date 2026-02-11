#include <fc/crypto/chain_types_reflect.hpp>
#include <fc/crypto/signature_provider.hpp>

namespace fc::crypto {

std::string to_signature_provider_spec(const std::string& key_name, fc::crypto::chain_kind_t target_chain,
                                       fc::crypto::chain_key_type_t key_type, const std::string& public_key_text,
                                       const std::string& private_key_provider_spec) {
   using namespace fc::crypto;
   return std::format("{},{},{},{},{}", key_name, chain_kind_reflector::to_fc_string(target_chain),
                      chain_key_type_reflector::to_fc_string(key_type), public_key_text, private_key_provider_spec);
}

}
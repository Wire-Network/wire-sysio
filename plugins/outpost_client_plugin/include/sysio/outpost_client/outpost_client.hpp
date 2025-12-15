//
// Created by jglanz on 12/4/25.
//

#pragma once
#include <memory>
#include <fc/crypto/blake2.hpp>
#include <fc/crypto/sha256.hpp>
#include <fc/crypto/signature_provider.hpp>
#include <sysio/signature_provider_manager_plugin/signature_provider_manager_plugin.hpp>

namespace sysio::outpost_client {

using payload_default_t = std::variant<std::string, fc::bytes>;

template<
fc::crypto::chain_kind_t TargetChain,
typename MessageType,
typename DigestType = fc::sha256,
typename PayloadType = payload_default_t
>
class outpost_client
{
protected:
   fc::crypto::signature_provider_ptr  _signing_provider;

public:
   using action_payload_type = PayloadType;

   constexpr static auto target_chain = TargetChain;

   explicit outpost_client(std::shared_ptr<fc::crypto::signature_provider_t> signing_provider) : _signing_provider(signing_provider) {}

   explicit outpost_client(const fc::crypto::signature_provider_id_t& sig_provider_query) :
   _signing_provider(sysio::get_signature_provider(sig_provider_query)) {}

   virtual ~outpost_client() = default;
   // virtual fc::transaction_t sign_and_send(PayloadType payload) = 0;

protected:
   virtual MessageType send_message(MessageType message) = 0;

   virtual MessageType create_message(PayloadType payload) = 0;

   virtual fc::crypto::signature sign(PayloadType payload) = 0;

   fc::bytes serialize_payload_default(const PayloadType& payload) {
      static_assert(std::is_same_v<payload_default_t, PayloadType>,
                    "default serializer is only available when ActionPayloadType == action_payload_default_t");

      if (std::holds_alternative<std::string>(payload)) {
         auto payload_str = std::get<std::string>(payload);
         return std::vector<char>(payload_str.begin(), payload_str.end());
      }

      return std::get<fc::bytes>(payload);
   }

};
}
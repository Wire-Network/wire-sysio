#pragma once

#include <sysio/chain/types.hpp>
#include <sysio/chain/block_header.hpp>
#include <fc/crypto/bls_public_key.hpp>
#include <fc/crypto/bls_signature.hpp>


namespace sysio::chain {

   inline fc::logger vote_logger{"vote"};

   using bls_public_key          = fc::crypto::bls::public_key;
   using bls_signature           = fc::crypto::bls::signature;

   struct vote_message {
      block_id_type       block_id;
      bool                strong{false};
      bls_public_key      finalizer_key;
      bls_signature       sig;

      auto operator<=>(const vote_message&) const = default;
      bool operator==(const vote_message&) const = default;
   };

   using vote_message_ptr = std::shared_ptr<vote_message>;

   constexpr auto format_as(const vote_message& vm) {
      return fmt::format("vote_message{{block: #{}:{}.., {}, finalizer_key: {}..}}",
                         block_header::num_from_id(vm.block_id), vm.block_id.str().substr(8, 16),
                         vm.strong ? "strong" : "weak",
                         vm.finalizer_key.to_string().substr(0, 10));
   }

} // namespace sysio::chain

FC_REFLECT(sysio::chain::vote_message, (block_id)(strong)(finalizer_key)(sig));

// SPDX-License-Identifier: MIT
#pragma once

#include <fc/crypto/sha256.hpp>
#include <fc/io/json.hpp>
#include <sysio/outpost_client/external_chain_json_rpc_client.hpp>

namespace sysio::outpost_client::solana {

class external_solana_chain_json_rpc_client
   : public sysio::outpost_client::external_chain_json_rpc_client<fc::crypto::chain_kind_solana> {
   using base = sysio::outpost_client::external_chain_json_rpc_client<fc::crypto::chain_kind_solana>;

public:
   using base::invoke_read;
   using base::invoke_write;

   external_solana_chain_json_rpc_client(std::shared_ptr<sysio::signature_provider> signing_provider,
                                         std::string                                endpoint)
      : base(std::move(signing_provider), std::move(endpoint)) {}

   external_solana_chain_json_rpc_client(const signature_provider_id_t& sig_provider_query, std::string endpoint)
      : base(sig_provider_query, std::move(endpoint)) {}

protected:
   fc::sha256 make_digest(const envelope_t& env) const override {
      // Solana transactions are signed over the raw message bytes; we hash the
      // serialized envelope to produce a deterministic digest for the signature provider.
      auto body = fc::json::to_string(env);
      return fc::sha256::hash(body);
   }
};

} // namespace sysio::outpost_client::solana

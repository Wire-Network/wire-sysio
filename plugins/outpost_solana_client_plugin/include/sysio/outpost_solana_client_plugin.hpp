#pragma once

#include <sysio/outpost_client_plugin.hpp>
#include <sysio/chain_plugin/chain_plugin.hpp>
#include <sysio/signature_provider_manager_plugin/signature_provider_manager_plugin.hpp>

#include <fc/network/solana/solana_client.hpp>
#include <fc/network/solana/solana_idl.hpp>

namespace sysio {
using namespace fc::network::solana;

struct solana_client_entry_t {
   std::string                        id;
   std::string                        url;
   fc::crypto::signature_provider_ptr signature_provider;
   solana_client_ptr                  client;
};

using solana_client_entry_ptr = std::shared_ptr<solana_client_entry_t>;

/// Typed program client for the Solana OPP outpost program.
/// Mirrors the Ethereum opp_contract_client / opp_inbound_contract_client pattern.
struct opp_solana_outpost_client : fc::network::solana::solana_program_client {
   solana_program_tx_fn<std::string, std::vector<uint8_t>> epoch_in;
   solana_program_tx_fn<std::string>                       emit_outbound_envelope;

   opp_solana_outpost_client(const solana_client_ptr& client,
                             const fc::network::solana::solana_public_key& program_id,
                             const std::vector<fc::network::solana::idl::program>& idls = {})
      : solana_program_client(client, program_id, idls)
      , epoch_in(create_tx<std::string, std::vector<uint8_t>>(get_idl("epoch_in")))
      , emit_outbound_envelope(create_tx<std::string>(get_idl("emit_outbound_envelope"))) {}
};

class outpost_solana_client_plugin : public appbase::plugin<outpost_solana_client_plugin> {
public:
   APPBASE_PLUGIN_REQUIRES((outpost_client_plugin)(signature_provider_manager_plugin))
   outpost_solana_client_plugin();
   virtual ~outpost_solana_client_plugin() = default;

   virtual void set_program_options(options_description& cli, options_description& cfg) override;

   virtual void plugin_initialize(const variables_map& options);

   virtual void plugin_startup();

   virtual void plugin_shutdown();

   std::vector<solana_client_entry_ptr> get_clients();
   solana_client_entry_ptr get_client(const std::string& id);
   const std::vector<std::pair<std::filesystem::path, std::vector<fc::network::solana::idl::program>>>& get_idl_files();

private:
   std::unique_ptr<class outpost_solana_client_plugin_impl> my;
};

} // namespace sysio

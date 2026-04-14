#pragma once

#include <sysio/outpost_client_plugin.hpp>
#include <sysio/chain_plugin/chain_plugin.hpp>
#include <fc/network/ethereum/ethereum_abi.hpp>
#include <fc/network/ethereum/ethereum_client.hpp>

namespace sysio {
using namespace fc::crypto::ethereum;
using namespace fc::network::ethereum;

struct ethereum_client_entry_t {
   std::string                        id;
   std::string                        url;
   fc::crypto::signature_provider_ptr signature_provider;
   ethereum_client_ptr                client;
};

using ethereum_client_entry_ptr = std::shared_ptr<ethereum_client_entry_t>;

/// Typed contract client for OPP.sol
struct opp_contract_client : ethereum_contract_client {
   ethereum_contract_tx_fn<fc::variant> emit_outbound_envelope;
   ethereum_contract_tx_fn<fc::variant> finalize_epoch;

   opp_contract_client(const ethereum_client_ptr& client,
                       const address_compat_type& contract_address,
                       const std::vector<fc::network::ethereum::abi::contract>& contracts)
      : ethereum_contract_client(client, contract_address, contracts)
      , emit_outbound_envelope(create_tx<fc::variant>(get_abi("emitOutboundEnvelope")))
      , finalize_epoch(create_tx<fc::variant>(get_abi("finalizeEpoch"))) {}
};

/// Typed contract client for OPPInbound.sol
struct opp_inbound_contract_client : ethereum_contract_client {
   ethereum_contract_tx_fn<fc::variant, std::string> epoch_in;
   ethereum_contract_call_fn<fc::variant> next_epoch_index;

   opp_inbound_contract_client(const ethereum_client_ptr& client,
                               const address_compat_type& contract_address,
                               const std::vector<fc::network::ethereum::abi::contract>& contracts)
      : ethereum_contract_client(client, contract_address, contracts)
      , epoch_in(create_tx<fc::variant, std::string>(get_abi("epochIn")))
      , next_epoch_index(create_call<fc::variant>(get_abi("nextEpochIndex"))) {}
};

class outpost_ethereum_client_plugin : public appbase::plugin<outpost_ethereum_client_plugin> {
public:
   APPBASE_PLUGIN_REQUIRES((outpost_client_plugin)(signature_provider_manager_plugin))
   outpost_ethereum_client_plugin();
   virtual ~outpost_ethereum_client_plugin() = default;

   virtual void set_program_options(options_description& cli, options_description& cfg) override;

   virtual void plugin_initialize(const variables_map& options);

   virtual void plugin_startup();

   virtual void plugin_shutdown();

   std::vector<ethereum_client_entry_ptr> get_clients();
   ethereum_client_entry_ptr get_client(const std::string& id);
   const std::vector<std::pair<std::filesystem::path, std::vector<fc::network::ethereum::abi::contract>>>& get_abi_files();
private:
   std::unique_ptr<class outpost_ethereum_client_plugin_impl> my;
};


} // namespace sysio

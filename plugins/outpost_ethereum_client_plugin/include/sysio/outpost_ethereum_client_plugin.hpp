#pragma once

#include <sysio/outpost_client_plugin.hpp>
#include <sysio/outpost_client/outpost_client.hpp>
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

/// Typed contract client for OPP.sol. State-changing calls go through
/// `create_tx_and_confirm` — OPP writes are consensus-critical and must
/// not silently drop (see epoch-859 stall RCA); the confirmed factory
/// awaits `eth_getTransactionReceipt` + N blocks before returning.
struct opp_contract_client : ethereum_contract_client {
   ethereum_contract_tx_fn<fc::variant> emit_outbound_envelope;
   ethereum_contract_tx_fn<fc::variant> finalize_epoch;
   /// View: latest outbound envelope's raw bytes + epoch — overwritten
   /// on every `emitOutboundEnvelope`. Read by the WIRE batch operator
   /// to relay the envelope back to WIRE.
   ethereum_contract_call_fn<fc::variant> get_latest_outbound_envelope;

   opp_contract_client(const ethereum_client_ptr& client,
                       const address_compat_type& contract_address,
                       const std::vector<fc::network::ethereum::abi::contract>& contracts)
      : ethereum_contract_client(client, contract_address, contracts)
      , emit_outbound_envelope(create_tx_and_confirm<fc::variant>(get_abi("emitOutboundEnvelope")))
      , finalize_epoch(create_tx_and_confirm<fc::variant>(get_abi("finalizeEpoch")))
      , get_latest_outbound_envelope(create_call<fc::variant>(get_abi("getLatestOutboundEnvelope"))) {}
};

/// Typed contract client for OPPInbound.sol. Same confirmed-default
/// policy as `opp_contract_client` for write paths.
struct opp_inbound_contract_client : ethereum_contract_client {
   ethereum_contract_tx_fn<fc::variant, std::string> epoch_in;
   ethereum_contract_call_fn<fc::variant> next_epoch_index;

   opp_inbound_contract_client(const ethereum_client_ptr& client,
                               const address_compat_type& contract_address,
                               const std::vector<fc::network::ethereum::abi::contract>& contracts)
      : ethereum_contract_client(client, contract_address, contracts)
      , epoch_in(create_tx_and_confirm<fc::variant, std::string>(get_abi("epochIn")))
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

   /**
    * @brief Build an `outpost_client` concrete for an ETH outpost.
    *
    * Resolves the shared chain-connection entry by id, flattens the plugin's
    * loaded ABI set, and constructs an `outpost_ethereum_client` bound to the
    * given OPP / OPPInbound contract addresses.
    *
    * @param eth_client_id     Id passed to `--outpost-ethereum-client`.
    * @param outpost_id        Outpost id from `sysio.epoch::outposts`.
    * @param chain_id          Numeric chain id from the outpost row (e.g. 31337, 1).
    * @param opp_addr          Hex address of the `OPP.sol` contract.
    * @param opp_inbound_addr  Hex address of the `OPPInbound.sol` contract.
    * @throws fc::exception if the client id is unknown or addresses are empty.
    */
   std::shared_ptr<outpost_client> create_outpost_client(const std::string& eth_client_id,
                                                       uint64_t           outpost_id,
                                                       uint32_t           chain_id,
                                                       const std::string& opp_addr,
                                                       const std::string& opp_inbound_addr);

private:
   std::unique_ptr<class outpost_ethereum_client_plugin_impl> my;
};


} // namespace sysio

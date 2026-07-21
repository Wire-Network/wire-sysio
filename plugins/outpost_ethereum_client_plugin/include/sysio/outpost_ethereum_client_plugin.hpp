#pragma once

#include <sysio/outpost_client_plugin.hpp>
#include <sysio/outpost_client/outpost_client.hpp>
#include <sysio/chain_plugin/chain_plugin.hpp>
#include <fc/network/ethereum/ethereum_abi.hpp>
#include <fc/network/ethereum/ethereum_client.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace sysio {
using namespace fc::crypto::ethereum;
using namespace fc::network::ethereum;

struct ethereum_client_entry_t {
   /** Configuration identifier used to select this connection. */
   std::string                        id;
   /** Provider used by the policy-enforcing client at the final signing boundary. */
   fc::crypto::signature_provider_ptr signature_provider;
   /** Ethereum JSON-RPC client with its immutable local transaction policy. */
   ethereum_client_ptr                client;
   /** Authoritative EVM chain id from the client's policy. */
   uint32_t                           chain_id;
};

using ethereum_client_entry_ptr = std::shared_ptr<ethereum_client_entry_t>;
using ethereum_transaction_policy_map = std::map<std::string, ethereum_transaction_policy>;

/** Load and strictly validate a versioned per-client Ethereum transaction-policy file. */
ethereum_transaction_policy_map
load_ethereum_transaction_policy_file(const std::filesystem::path& policy_file);

/// Typed contract client for OPP.sol. State-changing calls go through
/// `create_tx_and_confirm` — OPP writes are consensus-critical and must
/// not silently drop (see epoch-859 stall RCA); the confirmed factory
/// awaits `eth_getTransactionReceipt` + N blocks before returning.
struct opp_contract_client : ethereum_contract_client {
   /// Recovery-only write matching `emitOutboundEnvelope(uint32)` on the
   /// Ethereum outpost. No in-tree steady-state caller invokes this wrapper:
   /// normal operation emits during inbound consensus. It remains available
   /// for explicit operator recovery tooling that must advance a stalled
   /// outpost with the expected WIRE epoch.
   ethereum_contract_tx_fn<fc::variant, uint32_t> emit_outbound_envelope;
   /// View: latest outbound envelope's raw bytes + epoch — overwritten
   /// on every `emitOutboundEnvelope`. Read by the WIRE batch operator
   /// to relay the envelope back to WIRE.
   ethereum_contract_call_fn<fc::variant> get_latest_outbound_envelope;

   opp_contract_client(const ethereum_client_ptr& client,
                       const address_compat_type& contract_address,
                       const std::vector<fc::network::ethereum::abi::contract>& contracts)
      : ethereum_contract_client(client, contract_address, contracts)
      , emit_outbound_envelope(create_tx_and_confirm<fc::variant, uint32_t>(get_abi("emitOutboundEnvelope")))
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

/// Typed contract client for OperatorRegistry.sol. Carries the actions
/// plugins reach for outside the OPP envelope path — today `commit`
/// (underwriter UIC relay); future deposit / withdraw / slash actions
/// land here as additional `ethereum_contract_tx_fn` members.
///
/// State-changing actions use `create_tx_and_confirm` so the call
/// returns only after on-chain inclusion + confirmations — the caller
/// uses the return as a "this leg landed" signal before recording the
/// action locally.
struct operator_registry_contract_client : ethereum_contract_client {
   /// `commit(bytes uicBytes)` — relays a signed `UnderwriteIntentCommit`
   /// from an underwriter into the OperatorRegistry as opaque bytes. The
   /// hardhat-generated ABI passes the parameter as a hex-encoded string
   /// (per `ethereum_abi::encode_dynamic_data` for `dt::bytes`).
   ethereum_contract_tx_fn<fc::variant, std::string> commit;

   operator_registry_contract_client(const ethereum_client_ptr& client,
                                     const address_compat_type& contract_address,
                                     const std::vector<fc::network::ethereum::abi::contract>& contracts)
      : ethereum_contract_client(client, contract_address, contracts)
      , commit(create_tx_and_confirm<fc::variant, std::string>(get_abi("commit"))) {}
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

   /// Return the single configured client whose policy chain id equals `chain_id`, or nullptr when
   /// none — or more than one — match. Ambiguous same-chain clients fail closed because this lookup
   /// has no separate endpoint-selection signal.
   ethereum_client_entry_ptr get_client_by_chain_id(uint64_t chain_id);

   const std::vector<std::pair<std::filesystem::path, std::vector<fc::network::ethereum::abi::contract>>>& get_abi_files();

   /**
    * @brief Build an `outpost_client` concrete for an ETH outpost.
    *
    * Resolves the shared chain-connection entry by id, flattens the plugin's
    * loaded ABI set, and constructs an `outpost_ethereum_client` bound to the
    * given OPP / OPPInbound / OperatorRegistry contract addresses.
    *
    * All three contract addresses are independently optional — pass an
    * empty string for any the caller doesn't need. The SPI virtuals that
    * require an unprovisioned wrapper assert at call time with a clear
    * diagnostic; the SPI shape itself stays uniform regardless. Per
    * `outpost-client-spi.md`, address configuration is a per-caller
    * concern (batch operator wires OPP + OPPInbound; underwriter wires
    * OperatorRegistry; both share the same SPI surface).
    *
    * @param eth_client_id     Id passed to `--outpost-ethereum-client`.
    * @param chain_code        Outpost id from `sysio.epoch::outposts`.
    * @param chain_id          Numeric chain id from the outpost row (e.g. 31337, 1).
    * @param opp_addr          Hex address of the `OPP.sol` contract, or empty.
    * @param opp_inbound_addr  Hex address of the `OPPInbound.sol` contract, or empty.
    * @param operator_registry_addr  Hex address of the `OperatorRegistry.sol`
    *                                contract, or empty.
    * @throws fc::exception if the client id is unknown.
    */
   std::shared_ptr<outpost_client> create_outpost_client(const std::string& eth_client_id,
                                                       uint64_t           chain_code,
                                                       uint32_t           chain_id,
                                                       const std::string& opp_addr,
                                                       const std::string& opp_inbound_addr,
                                                       const std::string& operator_registry_addr = "");

private:
   std::unique_ptr<class outpost_ethereum_client_plugin_impl> my;
};


} // namespace sysio

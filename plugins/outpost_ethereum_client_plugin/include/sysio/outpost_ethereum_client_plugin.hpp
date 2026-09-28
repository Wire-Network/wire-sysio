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
   fc::crypto::signature_provider_ptr signature_provider;
   ethereum_client_ptr                client;
   /// Authoritative numeric EVM chain id. Lets the
   /// batch operator auto-select the client for an outpost row by matching the
   /// row's `external_chain_id`, so multiple EVM outposts never share one
   /// remote endpoint.
   uint32_t                           chain_id;
};

using ethereum_client_entry_ptr = std::shared_ptr<ethereum_client_entry_t>;

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

/// EIP-7825's per-transaction gas cap (2^24) — the most any single `epochIn`
/// can be funded with, whatever the client's policy allows.
inline constexpr uint64_t EIP_7825_TX_GAS_CAP = 16'777'216;

/**
 * @brief Gas an `epochIn` transaction is funded with: the client's policy
 *        ceiling, bounded by EIP-7825's cap.
 *
 * Delivery is funded to a FLOOR rather than to the node's estimate because the
 * estimate is systematically wrong for this call. `OPPInbound.epochIn` stops
 * dispatching on a `gasleft()` watchdog and records where it stopped instead
 * of reverting, so `eth_estimateGas` converges on the least gas at which the
 * call SUCCEEDS — the consensus tip plus ONE attestation. Funded to that
 * figure every call would spill after one attestation and an envelope would
 * cost one transaction per attestation. Funded to the ceiling, one call
 * carries as much dispatch as the chain allows and the unused remainder is
 * refunded.
 *
 * @param policy The client's local expenditure policy.
 * @return The floor, in gas.
 */
inline uint64_t delivery_gas_limit_floor(const ethereum_transaction_policy& policy) {
   const fc::uint256 cap{EIP_7825_TX_GAS_CAP};
   return policy.max_gas_limit < cap ? policy.max_gas_limit.convert_to<uint64_t>() : EIP_7825_TX_GAS_CAP;
}

/**
 * @brief Confirmation options for `epochIn`: the defaults, funded to
 *        `delivery_gas_limit_floor`.
 * @param client The client whose policy sets the floor; null (an ABI-only
 *        construction) leaves the floor at zero.
 * @return The options the `epochIn` wrapper is built with.
 */
inline ethereum_confirm_options delivery_confirm_options(const ethereum_client_ptr& client) {
   ethereum_confirm_options options = ethereum_confirm_option_defaults;
   if (client) options.gas_limit_floor = delivery_gas_limit_floor(client->transaction_policy());
   return options;
}

/// Typed contract client for OPPInbound.sol. Same confirmed-default
/// policy as `opp_contract_client` for the write path.
struct opp_inbound_contract_client : ethereum_contract_client {
   /// `epochIn(uint32 epochIndex, bytes envelopeData)` — the WHOLE envelope in
   /// ONE call, addressed to its epoch. The call that reaches consensus
   /// dispatches as many attestations as its gas allows and records where it
   /// stopped; a continuation is the SAME call with the same arguments, which
   /// the contract resumes from its cursor. Funded to the policy ceiling — see
   /// `delivery_gas_limit_floor`. `envelopeData` rides as a hex-encoded string
   /// because the libfc ABI encoder takes `dt::bytes` that way (see
   /// `ethereum_abi::encode_dynamic_data`).
   ///
   /// `ethereum_contract_tx_fn` binds every argument as a non-const lvalue
   /// reference, so callers must materialize named locals for both.
   ethereum_contract_tx_fn<fc::variant, uint32_t, std::string> epoch_in;
   /// `nextEpochIndex()` view — the epoch the outpost is currently accepting.
   ethereum_contract_call_fn<fc::variant> next_epoch_index;
   /// `dispatchSpill(uint32 epochIndex)` view — where a tipped epoch's dispatch
   /// stands: `(tipped, dispatched, complete, finalized)`. Block tag rides
   /// first per `ethereum_contract_call_fn`. Returns the raw `eth_call` hex —
   /// `create_call<fc::variant>` does not auto-decode, so the caller pushes it
   /// back through `contract_decode_data` against this ABI entry.
   ethereum_contract_call_fn<fc::variant, uint32_t> dispatch_spill;
   /// `epochDeliveries(uint32 epochIndex, address operator_)` view — the digest
   /// `operator_` delivered for the epoch, or zero when it has not delivered.
   ethereum_contract_call_fn<fc::variant, uint32_t, std::string> epoch_deliveries;
   /// `pendingEpochHash()` view — the digest consensus settled on for the epoch
   /// the outpost is processing.
   ethereum_contract_call_fn<fc::variant> pending_epoch_hash;
   /// `attestationHandlers(uint16 attestationType)` view — the outpost's own
   /// inbound routing table: the `IOPPReceiver` registered for one attestation
   /// type, `address(0)` when none is and `ATTESTATION_BLACKHOLE` when governance
   /// dropped the type. The relay discovers the liq syndication pool through it,
   /// since the pool registers itself for `DESYNDICATE_LIQ`, so neither a depot
   /// row nor operator config has to name the pool. Returns the raw `eth_call`
   /// hex, one left-padded address word.
   ethereum_contract_call_fn<fc::variant, uint16_t> attestation_handlers;

   opp_inbound_contract_client(const ethereum_client_ptr& client,
                               const address_compat_type& contract_address,
                               const std::vector<fc::network::ethereum::abi::contract>& contracts)
      : ethereum_contract_client(client, contract_address, contracts)
      , epoch_in(create_tx_and_confirm<fc::variant, uint32_t, std::string>(
           get_abi("epochIn"), delivery_confirm_options(client)))
      , next_epoch_index(create_call<fc::variant>(get_abi("nextEpochIndex")))
      , dispatch_spill(create_call<fc::variant, uint32_t>(get_abi("dispatchSpill")))
      , epoch_deliveries(create_call<fc::variant, uint32_t, std::string>(get_abi("epochDeliveries")))
      , pending_epoch_hash(create_call<fc::variant>(get_abi("pendingEpochHash")))
      , attestation_handlers(create_call<fc::variant, uint16_t>(get_abi("attestationHandlers"))) {}
};

/// Typed contract client for wire-ethereum's `SyndicationPool.sol`, the liq
/// syndication surface (Wire-Network/wire-ethereum#207). The relay reaches it for
/// one crank, `realizeYield()`: the pool folds the liqETH yield it accrued since
/// its last report into its principal and reports the delta as one `LIQ_YIELD`
/// attestation, the Ethereum counterpart of liqsol-core's `report_liq_yield`.
/// The call is access-restricted on chain (`yield_operator`), so the relay's
/// signer must hold that role on the outpost's AccessManager.
struct syndication_pool_contract_client : ethereum_contract_client {
   /// `realizeYield()` — confirmed like every other OPP write. The pool refuses
   /// at estimate time, before any gas is spent, with `WIRE_NoYield()` or
   /// `WIRE_YieldBelowDeadband(uint64,uint64)` when there is nothing to report
   /// and with `WIRE_PoolUnderbacked(uint64,uint64)` when its balance fell below
   /// the principal; the relay reads those as outcomes of the crank
   /// (`classify_realize_yield_revert`), not as failures of it.
   ethereum_contract_tx_fn<fc::variant> realize_yield;

   syndication_pool_contract_client(const ethereum_client_ptr& client,
                                    const address_compat_type& contract_address,
                                    const std::vector<fc::network::ethereum::abi::contract>& contracts)
      : ethereum_contract_client(client, contract_address, contracts)
      , realize_yield(create_tx_and_confirm<fc::variant>(get_abi("realizeYield"))) {}
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
   /// `commit(bytes uicBytes)` — submits the original canonical
   /// `UnderwriteIntentCommit` bytes. OperatorRegistry binds their signed EVM
   /// caller and claimed ACTIVE roster identity before queuing the unchanged
   /// bytes. The hardhat-generated ABI passes the parameter as a hex-encoded
   /// string (per `ethereum_abi::encode_dynamic_data` for `dt::bytes`).
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
   virtual ~outpost_ethereum_client_plugin();

   virtual void set_program_options(options_description& cli, options_description& cfg) override;

   virtual void plugin_initialize(const variables_map& options);

   virtual void plugin_startup();

   virtual void plugin_shutdown();

   std::vector<ethereum_client_entry_ptr> get_clients();
   /// Return the configured client registered under `id`, or nullptr when there
   /// is none. For an outpost the id is the chain's `sysio.chains` code, so a
   /// null result means that chain has no endpoint configured on this node and
   /// the caller must fail closed rather than fall back to another client.
   ethereum_client_entry_ptr get_client(const std::string& id);

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
    * @param eth_client_id     Id from the file configuration or CLI client option.
    * @param chain_code        Outpost id from `sysio.chains::chains`.
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

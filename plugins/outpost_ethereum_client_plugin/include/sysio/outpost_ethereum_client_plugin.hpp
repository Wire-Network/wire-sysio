#pragma once

#include <algorithm>
#include <functional>
#include <optional>

#include <sysio/outpost_client_plugin.hpp>
#include <sysio/outpost_client/outpost_client.hpp>
#include <sysio/chain_plugin/chain_plugin.hpp>
#include <fc/int256.hpp>
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
 * @brief The most gas any `epochIn` may be funded with under the policy's
 *        static limit: `max_gas_limit`, bounded by EIP-7825's cap.
 * @param policy The client's local expenditure policy.
 * @return The ceiling, in gas.
 */
inline uint64_t delivery_gas_ceiling(const ethereum_transaction_policy& policy) {
   const fc::uint256 cap{EIP_7825_TX_GAS_CAP};
   return policy.max_gas_limit < cap ? policy.max_gas_limit.convert_to<uint64_t>() : EIP_7825_TX_GAS_CAP;
}

/**
 * @brief The most gas any `epochIn` may be funded with at a given fee: the
 *        static ceiling, further bounded by what `max_total_native_cost`
 *        pays for at `max_fee_per_gas`.
 *
 * The policy refuses a transaction whose `gas_limit * max_fee_per_gas`
 * exceeds `max_total_native_cost`, so a limit the static ceiling admits can
 * still be a rejection at the fee the call is sent at. Sizing to this
 * ceiling turns that late rejection into a smaller budget the contract
 * spills across continuations instead.
 *
 * @param policy          The client's local expenditure policy.
 * @param max_fee_per_gas The fee the call will be sent at.
 * @return The ceiling, in gas; the static ceiling when the fee is zero.
 */
inline uint64_t delivery_gas_ceiling(const ethereum_transaction_policy& policy, const fc::uint256& max_fee_per_gas) {
   const uint64_t static_ceiling = delivery_gas_ceiling(policy);
   if (max_fee_per_gas == 0) return static_ceiling;
   const fc::uint256 affordable = policy.max_total_native_cost / max_fee_per_gas;
   return affordable < fc::uint256{static_ceiling} ? affordable.convert_to<uint64_t>() : static_ceiling;
}

/// Gas an `epochIn` pays before its dispatch loop, independent of size: the
/// transaction base, the delivery record, and the consensus bookkeeping a tip
/// adds. A ~700-byte tip reaches the loop at ~320k (wire-ethereum
/// `OPPCapacity`, the under-funded probe); rounded up.
inline constexpr uint64_t DELIVERY_FIXED_GAS = 500'000;
/// Gas an `epochIn` pays per envelope byte: calldata (EIP-7623 floors a
/// calldata-heavy transaction at 40 gas a byte), the linear decode (~50 gas
/// a byte, measured at the cap) and the dispatch walk; rounded up.
inline constexpr uint64_t DELIVERY_GAS_PER_BYTE = 120;
/// Gas allowed per attestation still to dispatch. A remit is tens of
/// thousands; a routine rotation seating seven unseen operators is ~1.15M.
/// An attestation that costs more spills, and the next call escalates.
inline constexpr uint64_t ATTESTATION_GAS_ALLOWANCE = 1'500'000;
/// Gas left for the outbound emit the finishing call attempts:
/// `OPPInbound.EmitAttemptGasFloor`, below which the contract defers it.
inline constexpr uint64_t EMIT_GAS_ALLOWANCE = 4'000'000;

/// The least static ceiling the relay accepts: what a full-cap envelope costs
/// to deliver and tip, dispatch one attestation at its allowance, and emit.
/// Under less, the largest envelope the platform allows cannot complete on
/// this client, so the relay refuses to be built on it rather than discover
/// that one epoch at a time.
inline constexpr uint64_t DELIVERY_MINIMUM_GAS_CEILING = DELIVERY_FIXED_GAS +
                                                        OPP_MAX_ENVELOPE_BYTES * DELIVERY_GAS_PER_BYTE +
                                                        ATTESTATION_GAS_ALLOWANCE + EMIT_GAS_ALLOWANCE;

/// What one `epochIn` has to carry, from which its gas budget is sized.
struct delivery_gas_request {
   /// Encoded envelope size.
   uint64_t envelope_bytes = 0;
   /// Attestations the outpost has not dispatched yet, or `std::nullopt`
   /// when the envelope could not be read: the budget is then the ceiling.
   std::optional<uint32_t> remaining_attestations;
   /// Calls already sent for this epoch in this tick that spilled: each one
   /// doubles the budget, so a run of attestations dearer than the allowance
   /// costs at most a logarithmic number of extra calls.
   uint32_t escalations = 0;
   /// `delivery_gas_ceiling`.
   uint64_t ceiling = 0;
};

/**
 * @brief Gas one `epochIn` is funded with.
 *
 * Sized to what the call has left to do rather than to the node's estimate or
 * to the ceiling. The estimate is systematically wrong for this call:
 * `OPPInbound.epochIn` stops dispatching on a `gasleft()` watchdog and records
 * where it stopped, so `eth_estimateGas` converges on the least gas at which
 * the call SUCCEEDS — the tip plus one attestation — and funding to it would
 * cost one transaction per attestation. The ceiling is wrong the other way:
 * a node reserves `gas_limit * max_fee_per_gas` of the signer's balance
 * whatever the call uses, so a small envelope funded to the cap is refused for
 * balance at exactly the fee levels where delivery matters most.
 *
 * The budget is the fixed and per-byte cost, an allowance per attestation
 * still to dispatch, and the emit; doubled per spill already met this tick;
 * never above the ceiling. The contract spills what does not fit and the
 * next call carries on, so an under-estimate costs a continuation, never the
 * epoch. An envelope that cannot be read is funded to the ceiling.
 *
 * @param request What the call has to carry.
 * @return The budget, in gas.
 */
inline uint64_t delivery_gas_budget(const delivery_gas_request& request) {
   if (!request.remaining_attestations) return request.ceiling;
   uint64_t budget = DELIVERY_FIXED_GAS + request.envelope_bytes * DELIVERY_GAS_PER_BYTE +
                     uint64_t{*request.remaining_attestations} * ATTESTATION_GAS_ALLOWANCE +
                     EMIT_GAS_ALLOWANCE;
   for (uint32_t i = 0; i < request.escalations && budget < request.ceiling; ++i) budget *= 2;
   return std::min(budget, request.ceiling);
}

/**
 * @brief Confirmation options for one `epochIn`: the defaults, funded to
 *        exactly `gas_budget`.
 *
 * The cap is the limit the transaction is sent with and the gas its
 * pre-flight estimate runs under: a call the budget cannot carry is refused
 * by the node before it is signed, and a call that can is sent with the
 * budget, never with a buffered estimate.
 *
 * @param gas_budget The budget from `delivery_gas_budget`.
 * @return The options the call is sent with.
 */
inline ethereum_confirm_options delivery_confirm_options(uint64_t gas_budget) {
   ethereum_confirm_options options = ethereum_confirm_option_defaults;
   options.gas_limit_floor = gas_budget;
   options.gas_limit_cap   = gas_budget;
   return options;
}

/// `eth_getTransactionReceipt` field names the relay reads.
namespace ethereum_receipt_field {
inline constexpr auto block_number = "blockNumber";
inline constexpr auto logs         = "logs";
} // namespace ethereum_receipt_field

/// What one `epochIn` left on chain, read from its receipt once it confirmed.
struct epoch_in_receipt {
   /// The transaction's hash, for logs.
   std::string  tx_hash;
   /// The block that holds the transaction. Every read that decides the
   /// relay's next move is pinned to it, so a backend lagging behind the block
   /// that just confirmed cannot report the state from before the call.
   fc::uint256  block_number;
   /// The receipt's `logs`, as the node returned them: what the call did.
   fc::variants logs;
};

/**
 * @brief Read an `epoch_in_receipt` out of an `eth_getTransactionReceipt` result.
 * @param tx_hash The confirmed transaction's hash.
 * @param receipt The receipt object as `ethereum_client::wait_for_receipt` returned it.
 * @return The block number and logs the relay acts on.
 * @throws fc::exception when the receipt is not an object or carries no block number.
 */
inline epoch_in_receipt parse_epoch_in_receipt(std::string tx_hash, const fc::variant& receipt) {
   FC_ASSERT(receipt.is_object(), "transaction receipt for {} is not an object", tx_hash);
   const auto& object = receipt.get_object();
   FC_ASSERT(object.contains(ethereum_receipt_field::block_number),
             "transaction receipt for {} carries no blockNumber", tx_hash);
   epoch_in_receipt parsed{.tx_hash      = std::move(tx_hash),
                           .block_number = fc::to_uint256(object[ethereum_receipt_field::block_number]),
                           .logs         = {}};
   if (object.contains(ethereum_receipt_field::logs) && object[ethereum_receipt_field::logs].is_array()) {
      parsed.logs = object[ethereum_receipt_field::logs].get_array();
   }
   return parsed;
}

/// ABI entry names of `OPPInbound.sol` the typed wrapper binds.
namespace opp_inbound_abi_name {
inline constexpr auto epoch_in                     = "epochIn";
inline constexpr auto next_epoch_index             = "nextEpochIndex";
inline constexpr auto dispatch_spill               = "dispatchSpill";
inline constexpr auto epoch_deliveries             = "epochDeliveries";
inline constexpr auto pending_epoch_hash           = "pendingEpochHash";
inline constexpr auto pending_consensus_for_digest = "pendingConsensusForDigest";
inline constexpr auto attestation_handlers         = "attestationHandlers";
} // namespace opp_inbound_abi_name

/// Typed contract client for OPPInbound.sol. Same confirmed-default
/// policy as `opp_contract_client` for the write path.
struct opp_inbound_contract_client : ethereum_contract_client {
   /// `epochIn(uint32 epochIndex, bytes envelopeData)` — the WHOLE envelope in
   /// ONE call, addressed to its epoch. The call that reaches consensus
   /// dispatches as many attestations as its gas allows and records where it
   /// stopped; a continuation is the SAME call with the same arguments, which
   /// the contract resumes from its cursor. Funded to exactly `gas_budget`
   /// (see `delivery_gas_budget`), which is why this is not a plain
   /// `ethereum_contract_tx_fn`: that binds its options once at construction,
   /// and the budget differs per call. Returns the confirmed receipt rather
   /// than the hash alone, because the relay's next move is decided from the
   /// block the call landed in. `envelopeData` rides as a hex-encoded string
   /// because the libfc ABI encoder takes `dt::bytes` that way (see
   /// `ethereum_abi::encode_dynamic_data`).
   ///
   /// The ABI arguments are bound as non-const lvalue references, so callers
   /// must materialize named locals for both.
   std::function<epoch_in_receipt(uint32_t& epoch_index, std::string& envelope_hex, uint64_t gas_budget)> epoch_in;
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
   /// `pendingConsensusForDigest(bytes32 digest)` view — how close the current
   /// epoch is to tipping on `digest`: `(nextEpoch, agreeing, groupSize,
   /// currentEpochStartedAtTs, epochDurationSec_)`. The relay reads it before a
   /// consensus retry, so a re-delivery is sent only when the contract's own
   /// path-2 predicate would fire. The digest rides as `0x`-hex.
   ethereum_contract_call_fn<fc::variant, std::string> pending_consensus_for_digest;
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
      , epoch_in([this](uint32_t& epoch_index, std::string& envelope_hex, uint64_t gas_budget) -> epoch_in_receipt {
           const auto& abi     = get_abi(opp_inbound_abi_name::epoch_in);
           const auto  options = delivery_confirm_options(gas_budget);
           contract_invoke_data_items params = {epoch_index, envelope_hex};
           auto tx = this->client->create_default_tx(this->contract_address, abi, params, options.gas_limit_floor,
                                                     options.gas_limit_cap);
           const auto tx_hash = this->client->execute_contract_tx_fn(tx, abi, params).as_string();
           return parse_epoch_in_receipt(tx_hash, this->client->wait_for_receipt(tx_hash, options));
        })
      , next_epoch_index(create_call<fc::variant>(get_abi(opp_inbound_abi_name::next_epoch_index)))
      , dispatch_spill(create_call<fc::variant, uint32_t>(get_abi(opp_inbound_abi_name::dispatch_spill)))
      , epoch_deliveries(
           create_call<fc::variant, uint32_t, std::string>(get_abi(opp_inbound_abi_name::epoch_deliveries)))
      , pending_epoch_hash(create_call<fc::variant>(get_abi(opp_inbound_abi_name::pending_epoch_hash)))
      , pending_consensus_for_digest(
           create_call<fc::variant, std::string>(get_abi(opp_inbound_abi_name::pending_consensus_for_digest)))
      , attestation_handlers(create_call<fc::variant, uint16_t>(get_abi(opp_inbound_abi_name::attestation_handlers))) {}
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
                                                       const std::string& opp_inbound_addr);

private:
   std::unique_ptr<class outpost_ethereum_client_plugin_impl> my;
};

} // namespace sysio

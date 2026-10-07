#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <fc/network/ethereum/ethereum_abi.hpp>
#include <fc/network/ethereum/ethereum_client.hpp>

#include <sysio/outpost_client/outpost_client.hpp>
#include <sysio/outpost_ethereum_client_plugin.hpp>

namespace sysio {

namespace outpost_ethereum_client_detail {

/// Decoded `OPPInbound.dispatchSpill(uint32)` view result — where a tipped
/// epoch's processing stands. All-false/zero for an epoch that has not tipped,
/// which is also what an unreadable response decodes to.
struct dispatch_spill {
   /// Consensus settled the epoch's digest and dispatch began.
   bool     tipped     = false;
   /// Attestations routed so far — also where the next continuation resumes.
   uint16_t dispatched = 0;
   /// Every attestation has been routed.
   bool     complete   = false;
   /// The outbound envelope has been emitted and `nextEpochIndex` advanced.
   bool     finalized  = false;
};

/// What the relay should do about the epoch it is holding, given the outpost's
/// view of that epoch.
enum class delivery_action {
   /// The epoch has not tipped: deliver the envelope — a first delivery, or
   /// the idempotent same-digest re-delivery a path-2 consensus retry is.
   deliver,
   /// The epoch tipped on THIS relay's digest and is not finished: re-supply
   /// the envelope so the outpost dispatches the next stretch.
   continue_dispatch,
   /// The epoch tipped on a digest this relay did not deliver. Only a relay
   /// whose recorded delivery matches the settled digest may continue it, so
   /// there is nothing for this one to send.
   wait_for_deliverer,
   /// The outpost has already finalized this epoch: nothing to send.
   already_finalized
};

/// Compare two EVM addresses for identity, tolerating an absent `0x` prefix
/// and EIP-55 checksum casing on either side.
bool same_evm_address(std::string_view lhs, std::string_view rhs);

/// Decide the relay's next move for `epoch_index`.
///
/// Pure and side-effect free so the table is unit-testable without an EVM
/// node; the caller performs whichever RPC the verdict names.
///
/// @param next_epoch_index      the outpost's `nextEpochIndex()`.
/// @param epoch_index           WIRE epoch being delivered.
/// @param spill                 the outpost's `dispatchSpill(epoch_index)`.
/// @param own_delivery_settled  whether this relay's recorded delivery digest
///                              equals the settled `pendingEpochHash`.
delivery_action decide_delivery(uint32_t              next_epoch_index,
                                uint32_t              epoch_index,
                                const dispatch_spill& spill,
                                bool                  own_delivery_settled);

/// Why `SyndicationPool.realizeYield()` refused, read from the node's revert bytes.
enum class realize_yield_refusal {
   /// `WIRE_NoYield()`: the pool balance equals the principal.
   no_yield,
   /// `WIRE_YieldBelowDeadband(uint64 delta, uint64 deadband)`: accrued, but under
   /// the contract's reporting floor.
   below_deadband,
   /// `WIRE_PoolUnderbacked(uint64 balanceDepot, uint64 principal)`: the balance
   /// fell below the principal, a loss the `LIQYield` carrier cannot express.
   underbacked,
   /// `EnforcedPause()`: OpenZeppelin `Pausable`'s refusal. `realizeYield()` is
   /// `whenNotPaused`, so a pool its panic role has frozen refuses every crank
   /// until it is unpaused -- an expected state, not a failed crank.
   paused
};

/// Classify `realizeYield()`'s revert bytes. `std::nullopt` for anything that is
/// not one of the pool's own refusals, exactly shaped (the selector alone, or the
/// selector plus two words): a role error or a foreign implementation must not
/// pass as a quiet no-op. `EnforcedPause()` is one of the pool's refusals: it
/// comes only from `SyndicationPool`'s own pause (`whenNotPaused`), since the
/// OPP endpoint has no pause of its own.
///
/// @param revert_data  `json_rpc_error::data`, the node's revert bytes as `0x`-hex.
std::optional<realize_yield_refusal> classify_realize_yield_revert(std::string_view revert_data);

/// The address in one raw `eth_call` word, as `0x`-hex, or `std::nullopt` when
/// the hex is not exactly one word of hex digits with a zero 12-byte pad.
std::optional<std::string> address_from_word(std::string_view raw_hex);

/// How many attestations `envelope_bytes` carries across its messages, or
/// `std::nullopt` when the bytes do not decode as an OPP envelope. The relay
/// sizes a delivery's gas budget from it; an unreadable envelope is funded to
/// the ceiling and left to the contract to judge.
std::optional<uint32_t> count_envelope_attestations(const std::vector<char>& envelope_bytes);

/// True when `handler_address` names a contract the outpost routes to: neither
/// `address(0)` (nothing registered) nor `ATTESTATION_BLACKHOLE` (governance
/// dropped the type).
bool is_routable_handler(std::string_view handler_address);

} // namespace outpost_ethereum_client_detail

/**
 * @brief Ethereum concrete `outpost_client`.
 *
 * Composes the plugin-owned `ethereum_client_entry_t` (shared chain connection
 * + signature provider) with per-outpost OPP contract metadata (the
 * `OPP.sol` and `OPPInbound.sol` addresses) to provide the chain-agnostic SPI
 * to `outpost_opp_job`.
 *
 * Constructed by `outpost_ethereum_client_plugin::create_outpost_client` —
 * `batch_operator_plugin` never builds one directly; it just calls the factory.
 */
class outpost_ethereum_client : public outpost_client {
public:
   outpost_ethereum_client(ethereum_client_entry_ptr                                entry,
                           std::string                                              opp_addr,
                           std::string                                              opp_inbound_addr,
                           std::vector<fc::network::ethereum::abi::contract>        abis,
                           uint64_t                                                 chain_code,
                           uint32_t                                                 chain_id);

   // ── outpost_client SPI ───────────────────────────────────────────────
   sysio::opp::types::ChainKind chain_kind() const override;
   uint64_t                     chain_code() const override { return _outpost_id; }
   uint32_t                     chain_id()   const override { return _chain_id; }
   std::vector<uint8_t>         authenticated_caller_address() const override;
   // to_string() inherits the base-class default: "{chain_code}:{ChainKind}:{chain_id}".

   std::string deliver_outbound_envelope(uint32_t                 epoch_index,
                                         const std::vector<char>& envelope_bytes,
                                         fc::microseconds         deadline) override;

   std::vector<char> read_inbound_envelope(uint32_t         epoch_index,
                                           fc::microseconds deadline) override;

   /// The Ethereum crank: `SyndicationPool.realizeYield()` on the pool the
   /// outpost registers as its `DESYNDICATE_LIQ` handler. Idle, at debug level,
   /// while the ABI set carries no `realizeYield` (a deployment that predates
   /// the pool) or no handler is registered; quiet when the pool has nothing to
   /// report. Any other revert and any transport failure propagate.
   void crank_outpost(uint32_t epoch_index, fc::microseconds deadline) override;

   // Expose for inspection / tests
   const ethereum_client_entry_ptr& entry()                       const { return _entry; }
   const std::string&               opp_address()                 const { return _opp_addr; }
   const std::string&               opp_inbound_address()         const { return _opp_inbound_addr; }
   /// This relay's own signer address in `0x`-hex — the identity the outpost
   /// records deliveries under. Derived once at construction; the continuation
   /// check compares the digest recorded under it against the settled one.
   const std::string&               signer_address_hex()          const { return _signer_address_hex; }
   /// The liq syndication pool the crank drives, `0x`-hex, or empty until the
   /// outpost's `DESYNDICATE_LIQ` handler has been discovered.
   const std::string&               syndication_pool_address()    const { return _syndication_pool_addr; }
   /// Bind the pool wrapper the crank drives to `address`, replacing whatever was
   /// bound. Discovery goes through this; tests bind a wrapper whose
   /// `realize_yield` is a stub.
   void bind_syndication_pool(std::string address, std::shared_ptr<syndication_pool_contract_client> client);

private:
   /// The `DESYNDICATE_LIQ` handler the outpost routes to, read from
   /// `OPPInbound.attestationHandlers` at `latest` (the routing table is
   /// configuration, not delivered content), or `std::nullopt` when none is
   /// registered or the word did not decode.
   ///
   /// @throws fc::exception on transport failure.
   std::optional<std::string> discover_syndication_pool();

   /// Read `OPPInbound.nextEpochIndex()` at `latest`.
   /// @throws fc::exception on transport failure, deadline expiry, or an
   ///         unparsable response.
   uint32_t read_next_epoch_index();

   /// Read `OPPInbound.dispatchSpill(epoch_index)` at `latest` and decode it.
   ///
   /// `latest`, deliberately, where `read_inbound_envelope` reads at
   /// `finalized`: the cursor is this outpost's own bookkeeping about work
   /// this relay is doing right now, and the worst a reorg can cost is a
   /// re-sent transaction the contract already treats as an idempotent no-op.
   /// Finality lags head by roughly 64 blocks, so a continuation gated on it
   /// would wait that out between every stretch of dispatch.
   ///
   /// @throws fc::exception on transport failure, deadline expiry, or an
   ///         unparsable response.
   outpost_ethereum_client_detail::dispatch_spill read_dispatch_spill(uint32_t epoch_index);

   /// Whether the digest this relay delivered for `epoch_index` is the one
   /// consensus settled on (`epochDeliveries(epoch, self) == pendingEpochHash`).
   /// False when this relay has not delivered, or delivered a minority digest.
   ///
   /// @throws fc::exception on transport failure, deadline expiry, or an
   ///         unparsable response.
   bool own_delivery_settled(uint32_t epoch_index);

   ethereum_client_entry_ptr                              _entry;
   std::string                                            _opp_addr;
   std::string                                            _opp_inbound_addr;

   std::shared_ptr<opp_contract_client>                   _opp_client;
   std::shared_ptr<opp_inbound_contract_client>           _opp_inbound_client;
     // nullable
   /// The plugin's loaded ABI set, kept for the wrapper bound after construction.
   std::vector<fc::network::ethereum::abi::contract>      _abis;
   /// See `syndication_pool_address()` / `bind_syndication_pool`.
   std::string                                            _syndication_pool_addr;
   std::shared_ptr<syndication_pool_contract_client>      _syndication_pool_client;  // nullable
   /// Cached `0x`-hex signer address — see `signer_address_hex()`.
   std::string                                            _signer_address_hex;
   uint64_t                                               _outpost_id;
   uint32_t                                               _chain_id;
};

using outpost_ethereum_client_ptr = std::shared_ptr<outpost_ethereum_client>;

} // namespace sysio

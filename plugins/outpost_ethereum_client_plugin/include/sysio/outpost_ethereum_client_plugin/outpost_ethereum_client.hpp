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

/// Per-`epochIn` chunk payload limit on Ethereum.
///
/// MIRROR DUTY — compiled into BOTH sides and never configured: this value must
/// equal `MAX_CHUNK_BYTES` in wire-ethereum's `contracts/outpost/OPPCommon.sol`
/// (re-declared as the public constant `OPPInbound.MAX_CHUNK_BYTES` so the
/// hardhat drift-guard suite can pin it). Deliveries carrying more than this in
/// a single transaction were demonstrated to break the Ethereum envelope
/// exchange, so `deliver_outbound_envelope` splits an envelope into
/// `ceil(size / ETHEREUM_MAX_CHUNK_BYTES)` transactions: every non-final chunk
/// is EXACTLY this many bytes and the final chunk carries the remainder.
///
/// A multiple of 32 so the contract's staging-cell writes stay word-aligned.
/// Against `OPP_MAX_ENVELOPE_BYTES` this yields at most
/// `ceil(32'768 / 8'192) == 4` chunks — and there is no terminal call: the
/// contract finalizes inline on the chunk that completes the envelope. This is
/// the Ethereum analogue of `SOLANA_MAX_CHUNK_BYTES` (672).
inline constexpr size_t ETHEREUM_MAX_CHUNK_BYTES = 8'192;

namespace outpost_ethereum_client_detail {

/// Number of `epochIn` transactions one envelope of `total_bytes` costs.
/// Ceil-division, matching `OPPInbound`'s own `totalChunks` validation — the
/// contract rejects any delivery whose declared `totalChunks` differs.
constexpr uint16_t chunk_count_for(size_t total_bytes) {
   return static_cast<uint16_t>((total_bytes + ETHEREUM_MAX_CHUNK_BYTES - 1) /
                                ETHEREUM_MAX_CHUNK_BYTES);
}

/// Decoded `OPPInbound.envelopeChunkState(address)` view result — the
/// owner-bound staging header this operator has (or has not) left on chain.
///
/// A never-staged / fully-reclaimed header decodes as all-zero, which the
/// contract spells `totalChunks == SLOT_EMPTY (0)`; `decide_chunk_resume`
/// treats that as "nothing to adopt".
struct envelope_chunk_state {
   /// WIRE epoch the staged header belongs to.
   uint32_t    epoch_index     = 0;
   /// Staging owner, as the ABI decoder returns it (lower-case `0x`-hex), or
   /// empty when the read failed / no header exists.
   std::string owner;
   /// Declared chunk count of the staged envelope; 0 == empty header.
   uint16_t    total_chunks    = 0;
   /// Chunks actually STORED on chain. The final chunk is never stored, so
   /// this never reaches `total_chunks` for a well-formed header.
   uint16_t    received_chunks = 0;
   /// Declared total envelope size of the staged envelope.
   uint32_t    total_bytes     = 0;
   /// Sum of the stored cells' lengths — diagnostics only.
   uint64_t    stored_bytes    = 0;
};

/// What the relay should do about the staging header it just read.
enum class chunk_resume_action {
   /// Nothing adoptable on chain — send every chunk from index 0.
   start_fresh,
   /// Our own header for this exact epoch and shape — continue from its
   /// high-water mark.
   resume,
   /// Our own CURRENT-epoch header with a different shape (a superseded
   /// envelope for the same epoch) — `discardEnvelopeChunks()` first, then
   /// send from index 0.
   discard_and_restart,
   /// Our own header for a DIFFERENT epoch — read `nextEpochIndex()` to find
   /// out whether the epoch under delivery has already been consumed; if it
   /// has, the delivery is skipped entirely (it would only buy late-no-op gas).
   confirm_epoch_advanced
};

/// `decide_chunk_resume`'s verdict.
struct chunk_resume_decision {
   chunk_resume_action action      = chunk_resume_action::start_fresh;
   /// First chunk index to send. Meaningful for `resume`; 0 for every other
   /// action.
   uint16_t            start_chunk = 0;
};

/// Compare two EVM addresses for identity, tolerating an absent `0x` prefix
/// and EIP-55 checksum casing on either side.
bool same_evm_address(std::string_view lhs, std::string_view rhs);

/// True when `revert_data` is exactly `OPP_ChunkBufferMissing(owner_address)`.
///
/// `discardEnvelopeChunks()` raises that error, and only that error, when it
/// finds nothing of ours to clear — the one revert meaning the staging header
/// is already in the state the caller wanted. Every other revert (a wrong
/// implementation behind the proxy, a reentrancy guard, an unrecognised
/// selector) must not be mistaken for it.
///
/// @param revert_data    `json_rpc_error::data`, the node's revert bytes as
///                       `0x`-hex; empty or truncated data returns false.
/// @param owner_address  the signer this relay expects the contract to name.
bool is_chunk_buffer_missing_revert(std::string_view revert_data, std::string_view owner_address);

/// Decide how to resume (or abandon) a chunked delivery given the on-chain
/// staging header.
///
/// Pure and side-effect free so the decision table is unit-testable without an
/// EVM node; the caller performs whichever RPC the returned action names.
///
/// @param staged        header read back from `envelopeChunkState(self)`.
/// @param self_address  this relay's own signer address.
/// @param epoch_index   WIRE epoch being delivered.
/// @param total_chunks  chunk count of the envelope being delivered.
/// @param total_bytes   size of the envelope being delivered.
chunk_resume_decision decide_chunk_resume(const envelope_chunk_state& staged,
                                          std::string_view            self_address,
                                          uint32_t                    epoch_index,
                                          uint16_t                    total_chunks,
                                          uint32_t                    total_bytes);

/// Why `SyndicationPool.realizeYield()` refused, read from the node's revert bytes.
enum class realize_yield_refusal {
   /// `WIRE_NoYield()`: the pool balance equals the principal.
   no_yield,
   /// `WIRE_YieldBelowDeadband(uint64 delta, uint64 deadband)`: accrued, but under
   /// the contract's reporting floor.
   below_deadband,
   /// `WIRE_PoolUnderbacked(uint64 balanceDepot, uint64 principal)`: the balance
   /// fell below the principal, a loss the `LIQYield` carrier cannot express.
   underbacked
};

/// Classify `realizeYield()`'s revert bytes. `std::nullopt` for anything that is
/// not one of the pool's own three refusals, exactly shaped (the selector alone,
/// or the selector plus two words): a role error, a paused endpoint or a foreign
/// implementation must not pass as a quiet no-op.
///
/// @param revert_data  `json_rpc_error::data`, the node's revert bytes as `0x`-hex.
std::optional<realize_yield_refusal> classify_realize_yield_revert(std::string_view revert_data);

/// The address in one raw `eth_call` word, as `0x`-hex, or `std::nullopt` when
/// the hex is not exactly one word of hex digits with a zero 12-byte pad.
std::optional<std::string> address_from_word(std::string_view raw_hex);

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
                           std::string                                              operator_registry_addr,
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

   std::string uw_commit(uint64_t                 uw_request_id,
                         const std::vector<char>& uic_bytes,
                         fc::microseconds         deadline) override;

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
   const std::string&               operator_registry_address()   const { return _operator_registry_addr; }
   /// This relay's own signer address in `0x`-hex — the identity every staging
   /// header is bound to. Derived once at construction; the chunk resume path
   /// compares it against `envelopeChunkState`'s `owner` on every multi-chunk
   /// delivery, so it is cached rather than re-derived per tick.
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

   /// Read `OPPInbound.envelopeChunkState(self)` at `latest` and decode it.
   ///
   /// `latest` is correct here (unlike `read_inbound_envelope`, which reads at
   /// `finalized`): this is our OWN staging high-water mark, not delivered
   /// content WIRE consensus commits against. Reading it at `finalized` would
   /// re-send chunks already staged in unfinalized blocks every tick.
   ///
   /// A malformed / unreadable response yields an all-zero state, which
   /// degrades to `start_fresh` — the contract absorbs the replayed chunks as
   /// idempotent no-ops.
   ///
   /// @throws fc::exception on transport failure or deadline expiry.
   outpost_ethereum_client_detail::envelope_chunk_state read_envelope_chunk_state();

   /// First chunk index to send for this delivery, or `std::nullopt` when the
   /// epoch has already advanced past `epoch_index` and the delivery must be
   /// skipped outright.
   ///
   /// Multi-chunk deliveries only — a single-chunk envelope stages nothing, so
   /// the dominant case pays no extra RPC.
   ///
   /// @throws fc::exception on transport failure or deadline expiry.
   std::optional<uint16_t> resume_chunk_index(uint32_t       epoch_index,
                                              uint16_t       total_chunks,
                                              uint32_t       total_bytes,
                                              fc::time_point deadline_abs);

   ethereum_client_entry_ptr                              _entry;
   std::string                                            _opp_addr;
   std::string                                            _opp_inbound_addr;
   std::string                                            _operator_registry_addr;
   std::shared_ptr<opp_contract_client>                   _opp_client;
   std::shared_ptr<opp_inbound_contract_client>           _opp_inbound_client;
   std::shared_ptr<operator_registry_contract_client>     _operator_registry_client;  // nullable
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

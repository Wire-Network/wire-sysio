#pragma once

#include <memory>
#include <string>
#include <vector>

#include <fc/network/solana/solana_client.hpp>
#include <fc/network/solana/solana_idl.hpp>

#include <sysio/outpost_client/outpost_client.hpp>
#include <sysio/outpost_solana_client_plugin.hpp>

namespace sysio {

/// Hard cap on the assembled OPP envelope. Mirrors the Solana program's
/// `MAX_ENVELOPE_BYTES` (`programs/opp-outpost/src/state/envelope_chunks.rs`).
/// 64 KiB is the e2e-supported max across WIRE / Ethereum / Solana — the
/// binding constraint is Solana's 256 KB BPF heap divided by the ~3.3×
/// envelope-size peak heap usage during the finalising chunk's
/// `Envelope::decode` + `keccak::hash` + assembled-buffer + clone. Kept in
/// sync by hand because there's no shared C++/Rust constant header.
inline constexpr size_t SOLANA_MAX_ENVELOPE_BYTES = 65'536;

/// Per-`epoch_in` chunk payload limit. Mirrors `MAX_CHUNK_BYTES` on the
/// Solana side. Solana's tx-packet MTU is 1 232 B raw. Tx overhead at the
/// current 12-account / 1-ix shape is ~492 B. The FINAL chunk tx also
/// carries one ComputeBudget `request_heap_frame(256_000)` pre-ix (~40 B
/// for the ComputeBudget program key + ix wrapper) so the consensus-reach
/// finalize path gets a 256 KiB BPF heap budget instead of the 32 KiB
/// default that OOM'd at epoch 13. Budget:
///     492 (overhead) + 40 (last-chunk pre-ix) + chunk_size ≤ 1232
///     → chunk_size ≤ 700; rounded down to 672 for a comfortable
///     32-byte safety margin against future ABI / varint slop.
/// Was 704 (no pre-ixs) and 768 before that (when `EpochIn` had 7
/// accounts pre-Task 54).
inline constexpr size_t SOLANA_MAX_CHUNK_BYTES = 672;

/**
 * @brief Solana concrete `outpost_client`.
 *
 * Composes the plugin-owned `solana_client_entry_t` (shared chain connection +
 * signature provider) with the outpost program id + IDL to implement the
 * chain-agnostic SPI.
 *
 * The `deliver_outbound_envelope` implementation preserves the two-step
 * Solana pattern: call `epoch_in` to stage the incoming envelope, then
 * `emit_outbound_envelope` so the outpost emits any queued outgoing ones —
 * the return value is the signature of the second call (the one that signals
 * "work done for this epoch").
 *
 * Constructed by `outpost_solana_client_plugin::create_outpost_client` —
 * `batch_operator_plugin` never builds one directly.
 */
class outpost_solana_client : public outpost_client {
public:
   outpost_solana_client(solana_client_entry_ptr                             entry,
                         fc::network::solana::solana_public_key              program_id,
                         std::vector<fc::network::solana::idl::program>      program_idls,
                         uint64_t                                            outpost_id,
                         uint32_t                                            chain_id);

   // ── outpost_client SPI ───────────────────────────────────────────────
   sysio::opp::types::ChainKind chain_kind() const override;
   uint64_t                     outpost_id() const override { return _outpost_id; }
   uint32_t                     chain_id()   const override { return _chain_id; }
   // to_string() inherits the base-class default: "{outpost_id}:{ChainKind}:{chain_id}".

   std::string deliver_outbound_envelope(uint32_t                 epoch_index,
                                         const std::vector<char>& envelope_bytes,
                                         fc::microseconds         deadline) override;

   std::vector<char> read_inbound_envelope(uint32_t         epoch_index,
                                           fc::microseconds deadline) override;

   // Expose for inspection / tests
   const solana_client_entry_ptr&                entry()                 const { return _entry; }
   const fc::network::solana::solana_public_key& program_id()            const { return _program_id; }

private:
   solana_client_entry_ptr                       _entry;
   fc::network::solana::solana_public_key        _program_id;
   std::shared_ptr<opp_solana_outpost_client>    _program_client;
   uint64_t                                      _outpost_id;
   uint32_t                                      _chain_id;
};

using outpost_solana_client_ptr = std::shared_ptr<outpost_solana_client>;

namespace outpost_solana_client_detail {

/// Decode an inbound envelope and return the deduplicated set of
/// 32-byte Solana pubkeys that the on-chain `epoch_in` handler will
/// need to address in its CPI lamport transfers:
///
///   * `OPERATOR_ACTION(WITHDRAW_REMIT)` → operator's SOL wallet
///     (`op_address.address`) for the vault → operator transfer.
///   * `DEPOSIT_REVERT`                   → depositor's SOL wallet
///     (`depositor.address`)              for the vault → depositor refund.
///
/// `OPERATOR_ACTION(SLASH)` routes vault → Reserve PDA, which is
/// already a declared account on the `epoch_in` IDL, so SLASH targets
/// are NOT in the returned vector.
///
/// Malformed attestations (wrong chain kind, wrong address length,
/// proto decode failure) are skipped silently — the on-chain handler
/// log+skips them the same way per `feedback_opp_handlers_never_throw.md`.
/// A whole-envelope decode failure returns an empty vector + a warning
/// log; the on-chain handlers will then log+skip every remit/revert in
/// the envelope, the depot retains the authoritative state, and the
/// next envelope can re-attempt.
///
/// Exposed in this header (rather than the .cpp's anonymous namespace)
/// so the plugin's unit tests can exercise the decoder against a
/// synthesised Envelope without spinning up a full Solana client.
std::vector<fc::network::solana::solana_public_key>
extract_inbound_recipient_pubkeys(const std::vector<char>& envelope_bytes);

} // namespace outpost_solana_client_detail

} // namespace sysio

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
/// Solana side. Chosen to fit a single `epoch_in` chunk transaction inside
/// Solana's 1 232-byte tx packet MTU after header/signature/account-meta
/// overhead. Bumped down from 768 → 704 when `EpochIn` grew from 7 → 10
/// accounts (added `outbound_message_buffer`, `outbound_envelopes`,
/// `latest_outbound_envelope` for the inline-emit-on-finalize path); the
/// extra 99 raw bytes of account keys + indices ate into the previous
/// margin and pushed 768-byte chunks past MTU.
inline constexpr size_t SOLANA_MAX_CHUNK_BYTES = 704;

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

} // namespace sysio

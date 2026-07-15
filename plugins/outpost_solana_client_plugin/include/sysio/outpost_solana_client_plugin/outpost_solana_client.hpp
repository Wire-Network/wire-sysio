#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <fc/network/solana/solana_client.hpp>
#include <fc/network/solana/solana_idl.hpp>

#include <sysio/outpost_client/outpost_client.hpp>
#include <sysio/outpost_solana_client_plugin.hpp>

namespace sysio {

/// Hard cap on the assembled OPP envelope. Mirrors the Solana program's
/// `MAX_ENVELOPE_BYTES` (`programs/opp-outpost/src/state/envelope_chunks.rs`).
/// The shared C++ boundary lives on `outpost_client` because Ethereum inbound
/// reads must reject the same envelope size before hex decoding.
inline constexpr size_t SOLANA_MAX_ENVELOPE_BYTES = OPP_MAX_ENVELOPE_BYTES;

/// Per-`epoch_in` chunk payload limit. Mirrors `MAX_CHUNK_BYTES` on the
/// Solana side. Solana's tx-packet MTU is 1 232 B raw. Tx overhead at the
/// current full data-chunk shape is fixture-measured and must stay <= 1 232 B.
/// Finalization happens in a separate zero-data terminal call, so data chunks
/// do not reserve packet space for dynamic effect accounts. If the static
/// `epoch_in` account list changes, the SEC-94 fixture/parity test must be
/// regenerated and will catch any packet overflow.
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
                         uint64_t                                            chain_code,
                         uint32_t                                            chain_id);

   // ── outpost_client SPI ───────────────────────────────────────────────
   sysio::opp::types::ChainKind chain_kind() const override;
   uint64_t                     chain_code() const override { return _outpost_id; }
   uint32_t                     chain_id()   const override { return _chain_id; }
   // to_string() inherits the base-class default: "{chain_code}:{ChainKind}:{chain_id}".

   std::string deliver_outbound_envelope(uint32_t                 epoch_index,
                                         const std::vector<char>& envelope_bytes,
                                         fc::microseconds         deadline) override;

   std::vector<char> read_inbound_envelope(uint32_t         epoch_index,
                                           fc::microseconds deadline) override;

   std::string uw_commit(uint64_t                 uw_request_id,
                         const std::vector<char>& uic_bytes,
                         fc::microseconds         deadline) override;

   // Expose for inspection / tests
   const solana_client_entry_ptr&                entry()                 const { return _entry; }
   const fc::network::solana::solana_public_key& program_id()            const { return _program_id; }

private:
   struct reserve_terminal_info {
      fc::network::solana::solana_public_key creator;
      fc::network::solana::solana_public_key custody_mint;
      uint8_t                                custody_decimals = 0;
   };

   /// Resolve the terminal-finalization facts for a per-reserve PDA:
   /// `creator` from the `Reserve` account, custody (mint / decimals) from
   /// the `OutpostConfig` maps keyed by `token_code`. The clean-room outpost
   /// program resolves custody from `config.token_addresses_by_code` at
   /// dispatch time (`Reserve` carries no custody fields), so the relay
   /// mirrors that lookup to stay account-consistent with the on-chain
   /// handlers.
   std::optional<reserve_terminal_info>
   reserve_info_for_codes(uint64_t token_code, uint64_t reserve_code);

   solana_client_entry_ptr                       _entry;
   fc::network::solana::solana_public_key        _program_id;
   std::shared_ptr<opp_solana_outpost_client>    _program_client;
   uint64_t                                      _outpost_id;
   uint32_t                                      _chain_id;

};

using outpost_solana_client_ptr = std::shared_ptr<outpost_solana_client>;

namespace outpost_solana_client_detail {

/// Custody binding for a `token_code`, resolved from the outpost's
/// `OutpostConfig` maps. The clean-room program pins custody on the config
/// (`token_addresses_by_code` / `precision_by_token_code`) instead of
/// denormalizing it onto each `Reserve` account.
struct token_custody_info {
   /// SPL mint for the token, or the all-zero system-program key when the
   /// token is native lamports (the on-chain zero-marker convention).
   fc::network::solana::solana_public_key mint;
   /// Chain-native decimals for the token.
   uint8_t decimals = 0;
};

/// Resolve `token_code`'s custody binding from a decoded `OutpostConfig`
/// account object. BOTH entries are required — same contract as
/// wire-ethereum's `ReserveManager` (`WIRE_TokenPrecisionUnset`) and the
/// program's own `PrecisionUnconfigured` / `TokenCodeNotConfigured` gates:
/// a missing address or precision entry throws instead of silently
/// defaulting. Native custody is expressed by an EXPLICIT zero-mint entry.
token_custody_info resolve_token_custody(const fc::variant_object& outpost_config,
                                         uint64_t token_code);

/// Append `key` to `metas`, or merge its writable flag into the existing
/// entry when an earlier terminal effect already required the same account.
void record_terminal_account(std::vector<fc::network::solana::account_meta>& metas,
                             const fc::network::solana::solana_public_key& key,
                             bool is_writable);

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

/// `(token_code, reserve_code)` pair for a Reserve PDA derivation. Used
/// by the SWAP_REMIT remaining-accounts path: the cranker walks inbound
/// SWAP_REMIT attestations, collects every (token_code, reserve_code)
/// pair, and the caller derives + appends the corresponding Reserve
/// PDA(s) past the IDL's declared accounts on the zero-data terminal
/// `epoch_in` submission. Without this the on-chain `handle_swap_remit`
/// can't `find_remaining_account` the Reserve PDA and logs the unpaid remit
/// instead of paying the recipient.
struct reserve_pda_seeds {
   uint64_t token_code;
   uint64_t reserve_code;
};

/// Walk every Reserve-PDA-consuming attestation in `envelope_bytes` —
/// `SWAP_REMIT`, `SWAP_REVERT`, `RESERVE_READY`, and
/// `RESERVE_CREATE_CANCELLED` — and collect the (token_code, reserve_code)
/// pair for each (deduped).
/// Caller derives the Reserve PDA via Anchor's `find_program_address`
/// with the `[RESERVE_SEED, &token_code.to_le_bytes(),
/// &reserve_code.to_le_bytes()]` seed list against the program id.
/// RESERVE_READY is single-shot (queued once at `matchreserve`); a
/// missing PDA strands the outpost-local reserve in PENDING forever, so
/// the lifecycle types are first-class here, not best-effort.
std::vector<reserve_pda_seeds>
extract_inbound_swap_remit_reserve_seeds(const std::vector<char>& envelope_bytes);

/// Walk every `RESERVE_CREATE_CANCELLED` attestation in `envelope_bytes`
/// and collect each unique reserve PDA seed pair. The terminal manifest
/// uses this narrower extractor after the Reserve PDA has been declared so
/// it can append branch-specific refund/vault accounts from pinned reserve
/// metadata.
std::vector<reserve_pda_seeds>
extract_inbound_reserve_create_cancelled_seeds(const std::vector<char>& envelope_bytes);

/// Tuple of (token_code, reserve_code, recipient) pulled from every
/// inbound SWAP_REMIT attestation in the envelope. The relay uses this
/// to derive the per-attestation reserve_vault PDA + recipient ATA
/// (`get_associated_token_address(recipient, mint)`) when the target
/// `token_code` resolves to an SPL mint via `OutpostConfig.token_addresses_by_code`.
///
/// Native-SOL targets harmlessly produce the same struct — the relay
/// just skips the ATA / mint derivation when the mint lookup returns
/// the native marker. The on-chain `handle_swap_remit` native branch
/// doesn't reference any of the extra accounts, so they're inert.
struct swap_remit_spl_target {
   uint64_t                                token_code;
   uint64_t                                reserve_code;
   fc::network::solana::solana_public_key  recipient;
};

/// Walk every `SWAP_REMIT` attestation in `envelope_bytes` and collect
/// the SPL-relevant tuple. Caller is responsible for resolving each
/// `token_code` to a mint pubkey (cached from `OutpostConfig`) before
/// deriving the recipient ATA + including it in `remaining_accounts`.
std::vector<swap_remit_spl_target>
extract_inbound_swap_remit_spl_targets(const std::vector<char>& envelope_bytes);

/// Walk every `SWAP_REVERT` attestation in `envelope_bytes` and collect the
/// SPL-relevant tuple. The `recipient` field carries the depositor pubkey,
/// because the SPL revert branch refunds into the depositor's ATA.
std::vector<swap_remit_spl_target>
extract_inbound_swap_revert_spl_targets(const std::vector<char>& envelope_bytes);

} // namespace outpost_solana_client_detail

} // namespace sysio

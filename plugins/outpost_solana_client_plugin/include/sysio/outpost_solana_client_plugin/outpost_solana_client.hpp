#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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

/// Must equal `MAX_CHUNK_BYTES` on the Solana side — non-final chunks must be
/// exactly this size. 668 keeps a full data chunk under Solana's 1 232 B packet
/// limit with the `epoch_in` tx overhead (static account list, header,
/// blockhash, signature, instruction discriminator + chunked args) included.
/// Any change to the `epoch_in` account list or argument tuple moves that
/// overhead; `epoch_in_full_data_chunk_fits_packet_limit` is what catches it.
inline constexpr size_t SOLANA_MAX_CHUNK_BYTES = 668;

/// Dynamic (`remaining_accounts`) budget for ONE `dispatch_attestations` call.
///
/// A legacy Solana transaction is capped at 1232 bytes and each account costs
/// 32 bytes of key plus 1 byte of index; after the instruction's own static
/// account list, the fee payer, the compute-budget pre-instruction, header,
/// blockhash and signature, ~16 dynamic accounts fit. This lives in the RELAY
/// because the relay builds the transaction -- the depot used to carry an
/// equivalent estimate and could not, since a WIRE consensus contract has no
/// business modelling another chain's packet limit.
///
/// Exported so `dispatch_attestations_full_manifest_fits_packet_limit` measures
/// THIS constant rather than a copy of it.
inline constexpr size_t MAX_TERMINAL_DYNAMIC_ACCOUNTS = 16;

/// Heap frame requested on every `dispatch_attestations` call, in bytes.
///
/// The frame rides with the work it protects: that call decodes the staged
/// envelope, dispatches its effects, and on the draining call encodes the
/// outbound emit -- all of which allocate well past Solana's 32 KiB default
/// heap. The compute-budget pre-instruction carrying it also costs packet
/// budget, which is why `dispatch_attestations_full_manifest_fits_packet_limit`
/// measures the transaction WITH it.
///
/// Exported so the packet-limit test (and the program-side test pinning the
/// same frame) measure THIS constant rather than a copy of it.
inline constexpr uint32_t SOLANA_DISPATCH_HEAP_FRAME_BYTES = 256'000;

/// Backstop on `dispatch_attestations` rounds per envelope. The drain loop
/// already exits on a drained cursor, on consensus not yet reached, and on a
/// cursor that fails to advance; this only bounds a pathological envelope so
/// the relay can never spin forever on one epoch.
///
/// Sized against the depot's worst case now that the depot no longer bounds the
/// envelope by Solana's packet limit: many LIQ effects can require separate
/// rounds under the account budget. Exhaustion is a liveness alarm, never a
/// silent success.
///
/// Exported so the plugin's round-exhaustion test sizes its fixture against
/// THIS constant rather than a copy of it.
inline constexpr uint32_t MAX_DISPATCH_ROUNDS = 128;

namespace outpost_solana_client_detail {

/// Assert that the loaded IDL's `LatestOutboundEnvelope` declaration has the
/// shape `read_inbound_envelope` relies on: the account exists (inline fields
/// or the Anchor IDL v2 `types`-section fallback), `epoch_index` is a u32, and
/// `data` is a length-prefixed `bytes` / `Vec<u8>` payload. Field ORDER is
/// deliberately unconstrained: the reader decodes the whole account through
/// libfc's IDL-driven `decode_account_data`, which follows the declared field
/// order at decode time, so BOTH the standalone `opp_outpost`
/// ({epoch_index, checksum, data, bump}) and the integrated `liqsol_core`
/// ({bump, epoch_index, checksum, data}) layouts are handled by a single build.
///
/// Called at construction for roles that read inbound envelopes so a
/// misshaped IDL fails at boot (`create_outpost_client`) instead of on the
/// first inbound poll, where the job loop would wlog and retry forever.
///
/// Exposed in this header (rather than the .cpp's anonymous namespace) so the
/// plugin's unit tests can exercise the pass and fail-loud paths against
/// synthesized IDLs.
///
/// @param program  the program's loaded Anchor IDL.
/// @throws fc::exception if the account or either field is absent, or a field
///         has a type the reader cannot faithfully interpret.
void assert_latest_envelope_shape(const fc::network::solana::idl::program& program);

/// Reduce the name-filtered IDL candidates to the ones whose declared
/// `address` (Anchor IDL v2 top-level / `metadata.address`) matches the
/// configured program id. Without this, WHICH same-named IDL version drives
/// account decoding is decided by `--solana-idl-file` order, and an IDL whose
/// field order disagrees with the deployed program silently misreads accounts
/// (the epoch=511 RCA class).
///
///   * any candidate matches           -> only the matching ones are returned.
///   * single candidate, no match      -> returned as-is (address-less stub
///     IDLs and dev fixtures stay usable; a declared-but-mismatched address
///     logs a warning).
///   * multiple candidates, no match   -> throws: the selection would be
///     order-dependent, which is exactly the misread risk.
///
/// @param program_idls  name-filtered candidate IDLs (order preserved).
/// @param program_id    the deployed outpost program id the client will bind.
/// @return the surviving candidates, order preserved.
/// @throws fc::exception when multiple candidates are loaded and none carries
///         a matching declared address.
std::vector<fc::network::solana::idl::program>
select_program_idls_matching(std::vector<fc::network::solana::idl::program> program_idls,
                             const fc::network::solana::solana_public_key&  program_id);

/// Raw payload bytes of a decoded Borsh `bytes` / `Vec<u8>` IDL field.
/// libfc's `decode_account_data` renders `bytes` as a base64 string variant
/// and `Vec<u8>` as an array-of-integers variant; both IDL spellings appear
/// across outpost program versions, so the reader accepts either.
///
/// @param field_value  the decoded field variant.
/// @return the payload bytes.
/// @throws fc::exception if the variant is neither shape or an array element
///         is out of byte range.
std::vector<char> borsh_payload_bytes(const fc::variant& field_value);

/// Decode an already-fetched `LatestOutboundEnvelope` account through the
/// outpost program client's loaded IDL and validate it end-to-end:
///
///   1. IDL-driven decode (`decode_account_info_data`) - verifies the 8-byte
///      Anchor discriminator and follows the IDL's declared field order, so
///      the same binary reads both known program layouts value-exactly.
///   2. `epoch_index` gate - 0 (never emitted) and stored != requested both
///      return empty. A stored epoch AHEAD of the request is warned about
///      (likely IDL-vs-deployment drift misreading the account, or an outpost
///      relaying for a stale WIRE view); a stored epoch behind the request is
///      normal emit-cadence lag and stays at debug.
///   3. `checksum` gate - when the IDL declares the 32-byte checksum field,
///      the payload's keccak256 must match it (both program versions write
///      `keccak256(encoded_envelope)`); a mismatch means the decode read the
///      wrong bytes for `data` (field-order drift) and is warned + rejected.
///   4. envelope-cap, protobuf-decode and inner-epoch checks, as before.
///
/// Any decode/extraction failure logs a warning (visible at default log
/// level - a permanently undecodable account must not be silent) and returns
/// empty so the poll loop keeps running.
///
/// Exposed in this header so the plugin's unit tests can drive the complete
/// post-fetch read path against synthesized accounts for BOTH program
/// layouts without a live RPC endpoint.
///
/// @param program_client  outpost program client carrying the loaded IDL.
/// @param account_data    raw fetched account bytes (incl. discriminator).
/// @param epoch_index     the WIRE epoch the caller expects to read.
/// @param log_label       client identity for log lines (`to_string()`).
/// @return the envelope's protobuf bytes, or empty when unavailable/invalid.
std::vector<char> decode_latest_envelope_account(opp_solana_outpost_client&  program_client,
                                                 const std::vector<uint8_t>& account_data,
                                                 uint32_t                    epoch_index,
                                                 const std::string&          log_label);

/// Assert that the loaded IDL's `EpochDeliveries` declaration carries the
/// fields the dispatch cursor read depends on: `consensus_reached` (bool) and
/// `dispatched_count` (u32), in either IDL field home (inline on the account
/// or in the Anchor IDL v2 `types` section). Field ORDER is unconstrained for
/// the same reason as `assert_latest_envelope_shape` -- the reader decodes
/// through the IDL.
///
/// Called at construction for the batch-operator role, beside the
/// `dispatch_attestations` instruction check. A PRESENT but drifted
/// `EpochDeliveries` (renamed or dropped cursor field) would otherwise leave
/// `consensus_reached` silently false forever: `drain_dispatch` would no-op
/// every tick behind a dlog, the epoch would never close, and nothing in the
/// logs would say why.
///
/// @param program  the program's loaded Anchor IDL.
/// @throws fc::exception if the account, either field, or either field's type
///         is absent or disagrees with what the cursor read decodes.
void assert_epoch_deliveries_shape(const fc::network::solana::idl::program& program);

/// Assert the loaded IDL declares `DistributionState` with the one field a
/// DESYNDICATE_LIQ manifest resolves from it: `liqsol_mint` (pubkey). Only the
/// integrated liqsol-core program declares the account, so the boot check runs
/// only when the IDL carries it; a drifted declaration would make a LIVE pool
/// unreadable and wedge every desyndication window on the same cursor.
void assert_distribution_state_shape(const fc::network::solana::idl::program& program);

/// One entry of a Token-2022 mint's `ExtraAccountMetaList`, exactly as the
/// validation account stores it (35 bytes: discriminator, 32-byte address
/// config, signer flag, writable flag).
///
/// `discriminator` selects how `address_config` resolves:
///   * `0`        — the config IS the pubkey.
///   * `1`        — a PDA of the HOOK program, seeds packed in the config.
///   * `>= 0x80`  — a PDA of the account at index `discriminator - 0x80`
///                  in the Execute account list, seeds packed in the config.
/// `2` (pubkey-from-account-data) is not resolved; see `resolve_hook_metas`.
struct extra_account_meta {
   uint8_t                    discriminator = 0;
   std::array<uint8_t, 32>    address_config{};
   bool                       is_signer     = false;
   bool                       is_writable   = false;
};

/// Read a Token-2022 MINT account's TLV and return its `TransferHook`
/// program id, or `nullopt` when the mint carries no hook (a legacy SPL mint,
/// a Token-2022 mint without the extension, or one whose hook is explicitly
/// disabled by an all-zero program id).
///
/// Throws on a MALFORMED TLV -- a truncated entry, or a `TransferHook` entry
/// too short to carry its authority and program id. Neither can be read as
/// "no hook" without risking a hook-free manifest for a hook mint.
///
/// A hook mint is the case SOL-396 exists for: `settle_desyndication` pays
/// through `spl_token_2022::onchain::invoke_transfer_checked`, which for such a
/// mint resolves the hook program, its validation PDA and every account that
/// PDA declares OUT OF `remaining_accounts`. Omitting them aborts the dispatch
/// window before the transfer and re-packs it from the unadvanced cursor.
std::optional<fc::network::solana::solana_public_key>
mint_transfer_hook_program(const std::vector<uint8_t>& mint_account_data);

/// Derive the `ExtraAccountMetaList` validation PDA for `mint` under
/// `hook_program`: seeds `["extra-account-metas", mint]`.
fc::network::solana::solana_public_key
derive_extra_account_metas_pda(const fc::network::solana::solana_public_key& hook_program,
                               const fc::network::solana::solana_public_key& mint);

/// Parse the validation account's TLV into its declared metas, in order.
/// Throws when the account carries no `Execute` entry, or on any malformed
/// entry: a partial parse yields a manifest short exactly the accounts the CPI
/// is about to demand, which is worse than failing here.
std::vector<extra_account_meta>
parse_extra_account_metas(const std::vector<uint8_t>& validation_account_data);

/// Resolve declared metas into concrete accounts against the Execute account
/// list, whose fixed prefix is `[source, mint, destination, authority,
/// validation_pda]` — the indices `AccountKey` seeds refer to. Resolved metas
/// append to that list in order, so later entries may reference earlier ones.
///
/// THROWS on a seed or discriminator form this resolver does not implement
/// (`InstructionData` beyond the Execute payload, `AccountData`, pubkey-from-
/// account-data). Failing loudly is deliberate: silently dropping a meta
/// produces exactly the wedged epoch this resolution exists to prevent, and it
/// would be invisible until an outpost stalled.
std::vector<fc::network::solana::account_meta>
resolve_hook_metas(const std::vector<extra_account_meta>&        metas,
                   const fc::network::solana::solana_public_key& hook_program,
                   const fc::network::solana::solana_public_key& source,
                   const fc::network::solana::solana_public_key& mint,
                   const fc::network::solana::solana_public_key& destination,
                   const fc::network::solana::solana_public_key& authority,
                   const fc::network::solana::solana_public_key& validation_pda);

/// The transfer-hook facts a Token-2022 custody mint carries, as read from
/// chain: the hook program and the metas its validation PDA declares.
struct mint_transfer_hook {
   fc::network::solana::solana_public_key program;
   std::vector<extra_account_meta>        declared;
};

/// Read one custody mint's transfer-hook configuration. `nullopt` means the
/// mint carries no hook -- the overwhelmingly common case (every legacy SPL
/// mint, and any Token-2022 mint without the extension), for which the
/// manifest is unchanged.
using transfer_hook_reader =
   std::function<std::optional<mint_transfer_hook>(
      const fc::network::solana::solana_public_key& custody_mint)>;

/// How far the outpost has settled one inbound epoch's attestations.
struct epoch_dispatch_progress {
   /// Whether consensus has tipped for the epoch. Until it has, a terminal
   /// call records the delivery and dispatches NOTHING -- the normal
   /// outcome for every operator except the one whose delivery reaches the
   /// threshold.
   bool     consensus_reached = false;
   /// The on-chain cursor: attestations already settled, in envelope
   /// dispatch order. The relay resumes from here.
   uint32_t dispatched_count  = 0;
};

/// The `dispatch_attestations` crank loop, factored over its RPC touchpoints
/// so the plugin's unit tests can drive the full state machine (packing,
/// `dispatch_limit` sizing, cursor resume, and the consensus / stall /
/// exhaustion exits) without a live Solana endpoint.
///
/// `per_attestation` carries one account-meta manifest per attestation of the
/// envelope, indexed by the SAME flat position the on-chain dispatch cursor
/// counts (`messages[*].payload.attestations[*]`, walked in order);
/// attestations needing no effect accounts hold an empty entry. Its size IS
/// the envelope's attestation total -- the denominator the cursor counts
/// toward.
///
/// Behaviour:
///   * Consensus not reached -> returns without sending; the normal path for
///     every operator but the one whose delivery tipped the threshold.
///   * `per_attestation.empty()` (a zero-attestation envelope) -> sends ONE
///     `dispatch_limit = 1` crank and returns. The program clamps the window
///     to the empty envelope and runs its completion block, closing the epoch
///     and emitting the riding outbound envelope. Without this crank the
///     epoch would never close: `epoch_in`'s terminal call deliberately stops
///     at recording, and `dispatch_attestations` is the only place
///     `next_epoch_index` advances. On an already-closed epoch the crank is a
///     benign on-chain no-op.
///   * Otherwise packs greedily from the cursor while the account UNION fits
///     `MAX_TERMINAL_DYNAMIC_ACCOUNTS` (always taking at least one so a
///     single oversized manifest still makes progress), sends, and re-reads
///     the cursor. A cursor that fails to advance means another caller is
///     draining this envelope -- exit; a drained cursor is done.
///   * Elogs and returns when `MAX_DISPATCH_ROUNDS` is exhausted (the log IS
///     the alarm; the next tick resumes from the on-chain cursor); throws
///     when the deadline callback throws.
///
/// @param epoch_index             the WIRE epoch being settled (log context).
/// @param per_attestation         per-attestation effect-account manifests.
/// @param throw_if_past_deadline  throws once the caller's deadline passes.
/// @param read_progress           reads `EpochDeliveries` (consensus + cursor).
/// @param send_dispatch           sends `dispatch_attestations(limit, accounts)`
///                                and returns the tx signature.
/// @param log_label               client identity for log lines.
/// @return the last dispatch signature this call sent (empty when none).
std::string drive_dispatch_rounds(
   uint32_t                                                           epoch_index,
   const std::vector<std::vector<fc::network::solana::account_meta>>& per_attestation,
   const std::function<void()>&                                       throw_if_past_deadline,
   const std::function<epoch_dispatch_progress()>&                    read_progress,
   const std::function<std::string(uint32_t, std::vector<fc::network::solana::account_meta>)>&
                                                                      send_dispatch,
   const std::string&                                                 log_label);

/// The syndicated liqSOL pool's facts a DESYNDICATE_LIQ manifest is derived
/// from, read off the program's `DistributionState` singleton: the pool's
/// liqSOL mint (Token-2022). The handler binds the mint from the same account,
/// never from the mutable `OutpostConfig` token map.
struct liq_pool_info {
   fc::network::solana::solana_public_key liqsol_mint;
};

/// Reads the `DistributionState` singleton. `nullopt` means the account is
/// absent or empty -- the program has no syndicated pool, and the handler
/// log-and-skips -- while a present but unreadable account throws so the relay
/// never submits a manifest that is guaranteed to abort.
using liq_pool_reader = std::function<std::optional<liq_pool_info>()>;

/// `GlobalState` -- the liqSOL outpost singleton the yield-report crank gates on.
namespace global_state {
   constexpr auto account_name     = "GlobalState";
   constexpr auto field_wire_state = "wire_state";
   /// The `WireState` variant in which the syndicated pool is outpost property
   /// and `report_liq_yield` is accepted.
   constexpr auto post_launch      = "PostLaunch";
   /// The emergency-stop flag (wire-solana `GlobalState.frozen`, set by
   /// `set_frozen`). While it is set the program refuses `report_liq_yield`
   /// and stores every `DESYNDICATE_LIQ` as a pending payout instead of paying it.
   constexpr auto field_frozen     = "frozen";
   /// Byte width of an Anchor account discriminator, which precedes the Borsh body.
   inline constexpr size_t anchor_discriminator_bytes = 8;
   /// `GlobalState::INIT_SPACE` before `frozen` was appended -- a byte-exact mirror
   /// of wire-solana's `GLOBAL_STATE_PRE_FREEZE_INIT_SPACE`. An account whose body is
   /// no longer than this predates the freeze and has not been grown by
   /// `migrate_global_state_liq_fields`; the program refuses every handler that reads
   /// the current layout on it.
   inline constexpr size_t pre_freeze_init_space = 131;
} // namespace global_state

/// The key libfc's IDL decoder renders an enum field's variant name under.
constexpr auto IDL_ENUM_VARIANT_KEY = "variant";

/// The instruction-account names of liqsol-core's `report_liq_yield`, exactly
/// as its `#[derive(Accounts)]` declares them (wire-solana
/// `report_liq_yield.rs`). The relay overrides every one by name: the ATAs and
/// the `UserRecord`s seeded on them are account-based derivations the IDL
/// resolver does not perform, and `token_program` is an Interface the
/// well-known table would resolve to legacy SPL Token instead of Token-2022.
namespace report_liq_yield_accounts {
   constexpr auto liqsol_mint              = "liqsol_mint";
   constexpr auto global_state             = "global_state";
   constexpr auto distribution_state       = "distribution_state";
   constexpr auto pool_authority           = "pool_authority";
   constexpr auto bucket_authority         = "bucket_authority";
   constexpr auto bucket_token_account     = "bucket_token_account";
   constexpr auto bucket_user_record       = "bucket_user_record";
   constexpr auto liqsol_pool_ata          = "liqsol_pool_ata";
   constexpr auto pool_user_record         = "pool_user_record";
   constexpr auto extra_account_meta_list  = "extra_account_meta_list";
   constexpr auto liqsol_core_program      = "liqsol_core_program";
   constexpr auto transfer_hook_program    = "transfer_hook_program";
   constexpr auto config                   = "config";
   constexpr auto outbound_message_buffer  = "outbound_message_buffer";
   constexpr auto token_program            = "token_program";
   constexpr auto associated_token_program = "associated_token_program";
   constexpr auto system_program           = "system_program";
} // namespace report_liq_yield_accounts

/// True iff a decoded `GlobalState` says the outpost's emergency stop is set.
/// `frozen` absent from the row means the loaded IDL declares no emergency stop
/// (a program that predates it), which is "not frozen"; `frozen` present but not
/// a bool is a misdecoded row and reads as frozen, so the crank never pays for a
/// tx the program could refuse.
bool liq_outpost_frozen(const fc::variant_object& global_state);

/// True iff a decoded `GlobalState` says `report_liq_yield` would be accepted:
/// the outpost is PostLaunch and not frozen (`liq_outpost_frozen`). Anything
/// else (another state, a missing or misshaped field, the emergency stop) reads
/// as "not due", so the crank never pays for a refused tx.
bool liq_yield_report_due(const fc::variant_object& global_state);

/// True iff the loaded IDL declares `GlobalState.frozen` but the on-chain account
/// is too short to carry it: `account_bytes` is no longer than the discriminator
/// plus `global_state::pre_freeze_init_space`. Such an account has not been grown
/// by `migrate_global_state_liq_fields`; the IDL-driven decode would run past its
/// end, and the program refuses the crank on it anyway, so the crank treats it as
/// "not due". An IDL without `frozen` never predates the freeze -- its program has
/// no emergency stop and its own layout is the current one.
///
/// @param program        the loaded outpost program IDL.
/// @param account_bytes  the `GlobalState` account's data length, discriminator included.
bool global_state_predates_freeze(const fc::network::solana::idl::program& program, size_t account_bytes);

/// The `report_liq_yield` account overrides: every named account of the
/// instruction except the signer, derived from the program id, the pool's
/// liqSOL mint, the mint's transfer-hook program and the relay's own config /
/// outbound-buffer PDAs. Exported so the plugin's tests can hold it against the
/// instruction's declaration.
fc::network::solana::account_overrides_t report_liq_yield_overrides(
   const fc::network::solana::solana_public_key& program_id,
   const fc::network::solana::solana_public_key& liqsol_mint,
   const fc::network::solana::solana_public_key& hook_program,
   const fc::network::solana::solana_public_key& config_pda,
   const fc::network::solana::solana_public_key& outbound_message_buffer_pda);

/// Assert the loaded IDL declares `GlobalState` with a `wire_state` whose enum
/// type carries the `PostLaunch` variant the yield crank gates on, and -- when it
/// declares the `frozen` emergency stop at all -- declares it as a bool. Boot-checked
/// only on a program that declares `report_liq_yield`.
void assert_global_state_shape(const fc::network::solana::idl::program& program);

} // namespace outpost_solana_client_detail

/**
 * @brief Solana concrete `outpost_client`.
 *
 * Composes the plugin-owned `solana_client_entry_t` (shared chain connection +
 * signature provider) with the outpost program id + IDL to implement the
 * chain-agnostic SPI.
 *
 * `deliver_outbound_envelope` stages chunks through `epoch_in`, then sends a
 * zero-data terminal `epoch_in` call. When that call reaches consensus the
 * program emits its queued outbound envelope inline; the return value is the
 * terminal call's signature.
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
                         uint32_t                                            chain_id,
                         solana_outpost_role                                 role);

   // ── outpost_client SPI ───────────────────────────────────────────────
   sysio::opp::types::ChainKind chain_kind() const override;
   uint64_t                     chain_code() const override { return _outpost_id; }
   uint32_t                     chain_id()   const override { return _chain_id; }
   std::vector<uint8_t>         authenticated_caller_address() const override;
   // to_string() inherits the base-class default: "{chain_code}:{ChainKind}:{chain_id}".

   std::string deliver_outbound_envelope(uint32_t                 epoch_index,
                                         const std::vector<char>& envelope_bytes,
                                         fc::microseconds         deadline) override;

   /// Settle every outstanding attestation for `epoch_index` by cranking
   /// `dispatch_attestations` until the on-chain cursor drains.
   ///
   /// Separate from delivery on purpose: an undrained cursor blocks every later
   /// epoch on this outpost, so recovery must not depend on the one relay whose
   /// delivery happened to fail. Order of operations: (1) decode-probes the
   /// envelope locally -- THROWING when it does not decode, because an
   /// undecodable envelope must never read as an empty one; (2) reads
   /// `EpochDeliveries` and returns a no-op when consensus has not tipped yet
   /// (the normal state for every operator but the one whose delivery reaches
   /// the threshold), skipping the per-attestation manifest work below on that
   /// common path; (3) builds the per-attestation effect-account manifests from
   /// the envelope; (4) drives `outpost_solana_client_detail::drive_dispatch_rounds`,
   /// which re-reads the cursor at the top of every round so a re-drive resumes
   /// where the program will actually settle, closes a zero-attestation epoch
   /// with a single clamped crank, and elogs + returns on round-budget
   /// exhaustion -- the log is the liveness alarm, and the next tick resumes
   /// from the cursor.
   ///
   /// @return the last `dispatch_attestations` signature this call sent, or
   ///         empty when consensus had not tipped or the cursor was already
   ///         drained.
   std::string drain_dispatch(uint32_t                 epoch_index,
                              const std::vector<char>& envelope_bytes,
                              fc::microseconds         deadline);

   std::vector<char> read_inbound_envelope(uint32_t         epoch_index,
                                           fc::microseconds deadline) override;

   // Expose for inspection / tests
   const solana_client_entry_ptr&                entry()                 const { return _entry; }
   const fc::network::solana::solana_public_key& program_id()            const { return _program_id; }

private:

   /// Read `EpochDeliveries` for `epoch_index`. A missing/empty account means
   /// nothing has been delivered yet, reported as zero progress rather than
   /// treated as an error.
   outpost_solana_client_detail::epoch_dispatch_progress
   read_epoch_dispatch_progress(uint32_t epoch_index);

   /// Send ONE `dispatch_attestations(epoch_index, dispatch_limit)` call,
   /// appending `extra_remaining_accounts` (the packed effect-account batch)
   /// past the IDL account list as Anchor `remaining_accounts`. Built on the
   /// program client's PUBLIC generic API (`get_idl` / `resolve_accounts` /
   /// `execute_tx_and_confirm`) so the dispatch surface lives entirely in
   /// this client and the shell header stays at its master shape.
   std::string send_dispatch_attestations(
      uint32_t                                       epoch_index,
      uint32_t                                       dispatch_limit,
      std::vector<fc::network::solana::account_meta> extra_remaining_accounts);

   /// Read one custody mint's transfer-hook configuration (SOL-396). `nullopt`
   /// when the mint carries no hook -- every mint in the system today, for
   /// which the manifest is unchanged. A CONFIGURED hook whose
   /// `ExtraAccountMetaList` cannot be read throws rather than degrading: a
   /// manifest silently short the hook's accounts wedges the epoch inside the
   /// transfer CPI with no diagnostic pointing at the cause.
   std::optional<outpost_solana_client_detail::mint_transfer_hook>
   mint_transfer_hook_for(const fc::network::solana::solana_public_key& custody_mint);

   /// Read the syndicated liqSOL pool's facts off `DistributionState` -- the
   /// account `handle_desyndicate_liq` binds the pool's mint from. Absent or
   /// empty degrades (the handler log-and-skips an uninitialized pool); present
   /// but unreadable THROWS, because a guessed mint would name accounts the
   /// program never asks for.
   std::optional<outpost_solana_client_detail::liq_pool_info> syndicated_liq_pool();

   /// The outpost's per-epoch crank: liqsol-core's permissionless
   /// `report_liq_yield`, submitted once per epoch from the outbound relay job
   /// after this operator's delivery lands. PostLaunch only (read off
   /// `GlobalState` first, so a refused tx is never paid for); a program without
   /// the instruction has no syndicated pool and cranks nothing.
   void crank_outpost(uint32_t epoch_index, fc::microseconds deadline) override;

   solana_client_entry_ptr                       _entry;
   fc::network::solana::solana_public_key        _program_id;
   std::shared_ptr<opp_solana_outpost_client>    _program_client;
   uint64_t                                      _outpost_id;
   uint32_t                                      _chain_id;
   /// The latest envelope this relay delivered, kept so `read_inbound_envelope`
   /// can drain its epoch's dispatch cursor without depot access -- consensus
   /// can tip via OTHER operators' deliveries between our ticks, and the drain
   /// manifest can only be built from the envelope bytes themselves. In-memory
   /// on purpose: after a restart the still-pending envelope is re-delivered by
   /// the next outbound tick (re-staging chunks and a duplicate terminal call
   /// are benign on-chain), which repopulates this memo.
   std::optional<std::pair<uint32_t, std::vector<char>>> _delivered_envelope;

};

using outpost_solana_client_ptr = std::shared_ptr<outpost_solana_client>;

namespace outpost_solana_client_detail {

/// Append `key` to `metas`, or merge its writable flag into the existing
/// entry when an earlier terminal effect already required the same account.
void record_terminal_account(std::vector<fc::network::solana::account_meta>& metas,
                             const fc::network::solana::solana_public_key& key,
                             bool is_writable);

/// The liqSOL pool's fixed PDAs (wire-solana `liqsol-core`), byte-exact mirrors
/// of the program's seed declarations: `GlobalState`
/// (`["outpost_global_state"]`), `DistributionState` (`["distribution_state"]`),
/// the pool authority (`["liqsol_pool"]`) and the bucket authority
/// (`["liqsol_bucket"]`). Exported so the manifest builder and its tests derive
/// through ONE implementation.
fc::network::solana::solana_public_key
derive_liqsol_global_state_pda(const fc::network::solana::solana_public_key& program_id);
fc::network::solana::solana_public_key
derive_liqsol_distribution_state_pda(const fc::network::solana::solana_public_key& program_id);
fc::network::solana::solana_public_key
derive_liqsol_pool_authority_pda(const fc::network::solana::solana_public_key& program_id);
fc::network::solana::solana_public_key
derive_liqsol_bucket_authority_pda(const fc::network::solana::solana_public_key& program_id);

/// Seed prefix of liqsol-core's `PendingPayout` PDA -- a byte-exact mirror of
/// wire-solana's `PENDING_PAYOUT_SEED` (`states/pending_payout.rs`). Exported so
/// the derivation and its tests spell the seed through ONE constant.
inline constexpr std::string_view PENDING_DESYNDICATION_SEED = "pending_desyndication";

/// The `PendingPayout` PDA a `DESYNDICATE_LIQ` is stored at when the outpost
/// cannot pay it inline (the emergency stop, or a legacy `UserRecord`): seeds
/// `["pending_desyndication", request_id.to_le_bytes()]`, byte-exact with
/// wire-solana's `PendingPayout::find_address` and `defer_desyndication`. A seed
/// drift derives a well-formed WRONG address; the handler then aborts the
/// dispatch window with `EffectAccountMissing` on every retry.
///
/// @param program_id  the liqsol-core program id.
/// @param request_id  the depot's `DesyndicateLIQ.request_id` (never 0 on a real
///                    desyndication; the manifest omits the PDA for 0).
fc::network::solana::solana_public_key
derive_pending_desyndication_pda(const fc::network::solana::solana_public_key& program_id,
                                 uint64_t                                      request_id);

/// The `UserRecord` PDA of one liqSOL token account: seeds
/// `["user_record", token_account.as_ref()]`.
fc::network::solana::solana_public_key
derive_liqsol_user_record_pda(const fc::network::solana::solana_public_key& program_id,
                              const fc::network::solana::solana_public_key& token_account);

/// Which family of effect accounts one inbound attestation needs. The relay
/// derives the concrete metas per shape; the on-chain handler resolves them
/// out of `remaining_accounts` by pubkey.
///
enum class effect_shape {
   /// DESYNDICATE_LIQ: the depot releases a user's syndicated liqSOL. The
   /// handler resolves the pool's two state singletons, the pool-authority-signed
   /// Token-2022 transfer's accounts (the pool and user ATAs with their
   /// `UserRecord`s, the bucket ATA, the mint, Token-2022) and the liqSOL
   /// transfer hook's accounts (the bucket authority, the mint's extra-metas PDA,
   /// the hook program, liqsol-core itself) out of `remaining_accounts`.
   desyndicate_liq,
};

/// One inbound attestation's effect-account requirement, keyed by its FLAT
/// position in the envelope's dispatch order (`messages[*].payload
/// .attestations[*]`, walked in order).
///
/// That index is the coordinate the resumable-dispatch cursor counts in: the
/// program settles `[dispatched_count, dispatched_count + dispatch_limit)`
/// over exactly this sequence, so the relay can size a terminal call's
/// `dispatch_limit` to the attestations whose accounts it is actually
/// carrying. Attestations needing no effect account produce no entry.
struct inbound_effect {
   size_t                                                attestation_index;
   effect_shape                                          shape;
   /// DESYNDICATE_LIQ recipient, paid into its liqSOL ATA.
   std::optional<fc::network::solana::solana_public_key> recipient;

   /// Set for `desyndicate_liq`: the depot's `DesyndicateLIQ.request_id`, which
   /// keys the `PendingPayout` PDA the handler stores the payout at when it
   /// cannot pay inline (`derive_pending_desyndication_pda`).
   std::optional<uint64_t>                               desyndication_request_id;
};

/// Walk `envelope_bytes` ONCE and return every attestation that needs effect
/// accounts, in dispatch order. This is the authoritative decode and the ONLY
/// one on the production path -- `drain_dispatch` builds its per-attestation
/// manifests from it, so the per-type dispatch lives in exactly one place.
///
/// One entry per account-needing attestation, NO cross-attestation dedup --
/// duplicate accounts derived from the entries merge later in
/// `record_terminal_account` when a batch's manifests union.
///
/// Two on-chain failure modes, deliberately different, and this walk mirrors
/// the first: a MALFORMED ATTESTATION (bad chain code, unparseable payload) is
/// skipped here and logged-and-skipped on-chain, because no retry can fix it
/// and wedging the epoch on it would be worse. A MISSING EFFECT ACCOUNT is the
/// opposite -- the program ABORTS the whole call, because that is caller-side
/// and retryable, and tolerating it would let a caller choose which effects
/// land. A whole-envelope decode failure returns empty + a warning.
std::vector<inbound_effect>
extract_inbound_effects(const std::vector<char>& envelope_bytes);

/// Total attestations in `envelope_bytes`, across every message, in dispatch
/// order. This is the denominator the on-chain cursor counts toward, so it
/// includes attestations that need no effect accounts. Returns 0 if the
/// envelope does not decode.
uint32_t count_inbound_attestations(const std::vector<char>& envelope_bytes);

/// Build LIQ settlement account manifests, indexed by the envelope's flat
/// attestation position. Probe the deadline before each effect and memoize the
/// pool and transfer-hook reads for this build. An absent pool carries only the
/// state singletons and pending-payout PDA; an RPC error propagates for retry.
///
/// The full manifest includes the Token-2022 transfer, share-accounting and
/// hook accounts. A nonzero request id always contributes its writable pending
/// payout PDA so frozen or refused payouts can be stored instead of lost.
///
/// @return one manifest per attestation, including empty entries for no-effect tags.
std::vector<std::vector<fc::network::solana::account_meta>> build_dispatch_manifests(
   const fc::network::solana::solana_public_key& program_id,
   const std::vector<inbound_effect>&            effects,
   uint32_t                                      total_attestations,
   const std::function<void()>&                  throw_if_past_deadline,
   const transfer_hook_reader&                   read_transfer_hook,
   const liq_pool_reader&                        read_liq_pool,
   const std::string&                            log_label);

} // namespace outpost_solana_client_detail

} // namespace sysio

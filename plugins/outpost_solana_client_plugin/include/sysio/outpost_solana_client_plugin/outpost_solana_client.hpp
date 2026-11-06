#pragma once

#include <array>
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
#include <fc/time.hpp>

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
/// account list, the fee payer, the two-instruction ComputeBudget preamble, header,
/// blockhash and signature, ~16 dynamic accounts fit. This lives in the RELAY
/// because the relay builds the transaction -- the depot used to carry an
/// equivalent estimate and could not, since a WIRE consensus contract has no
/// business modelling another chain's packet limit.
///
/// Exported so `dispatch_attestations_full_manifest_fits_packet_limit` measures
/// THIS constant rather than a copy of it.
inline constexpr size_t MAX_TERMINAL_DYNAMIC_ACCOUNTS = 16;

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

/// Round-trip a fixed header through the loaded IDL; fail boot if discovery's epoch/operator offsets drift.
/// @param program_client Client holding the loaded outpost IDL.
/// @throws fc::exception when either field fails to decode at the discovery offsets.
void assert_envelope_chunks_shape(opp_solana_outpost_client& program_client);

/// Round-trip both reap cursor offsets through the loaded IDL; layout drift fails boot.
/// @param program_client Client holding the loaded outpost IDL.
/// @throws fc::exception when a cursor is missing or its byte offset disagrees with the IDL.
void assert_envelope_chunk_gate_shapes(opp_solana_outpost_client& program_client);

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

/// The checksum of the outbound `EnvelopeLog` record for `epoch_index` in the fetched account bytes, decoded
/// through the loaded IDL: the keccak256 the program recorded when it emitted that epoch's envelope. Nullopt when
/// the log holds no record for the epoch. Throws when the bytes do not decode as an `EnvelopeLog`. Exposed for the
/// plugin's unit tests, which drive it against synthesized accounts.
std::optional<fc::sha256> decode_envelope_log_digest(opp_solana_outpost_client&  program_client,
                                                     const std::vector<uint8_t>& account_data,
                                                     uint32_t                    epoch_index);

/// The first `epoch_in` data chunk the uploader still has to send, read from its fetched `EnvelopeChunks` staging
/// buffer: the buffer's `received_chunks` when the buffer was staged for exactly this upload (same epoch, uploader,
/// `total_chunks` and `total_bytes`), else 0. The program treats a re-sent chunk below `received_chunks` as an
/// idempotent no-op, so starting at 0 never skips data -- but every no-op still costs a full send + confirm, and
/// restarting from 0 each tick capped an upload at what ONE delivery deadline can carry. Resuming here lets a large
/// envelope finish across ticks.
///
/// Returns 0 without decoding when nothing is staged: the account is absent, empty, or not yet owned by the program
/// (system-owned before its first chunk created it). Also returns 0 when the loaded IDL does not declare
/// `EnvelopeChunks` (the pre-resume behaviour), and for a buffer staged for a different upload shape -- that buffer
/// still rejects chunk 0 with the program's own `ChunkBufferShapeMismatch`, exactly as before the resume existed.
///
/// The result never exceeds the chunks the account's length can hold. This relies on the program sizing the account
/// to exactly `DATA_OFFSET + bytes received` and only ever growing it, so a `received_chunks` misread through a
/// drifted IDL cannot resume past what is staged; a program that pre-sized the buffer would void this bound.
///
/// The PDA seeds bind the epoch and the uploader, so a decoded `epoch_index` or `operator` that differs from them,
/// or a `received_chunks` the account's length cannot hold, can only mean the loaded IDL disagrees with the program.
/// Those cases log a warning (and resume from 0, or from the length cap) so the drift is visible rather than
/// silently disabling the resume. Exposed for the plugin's unit tests, which drive it against synthesized accounts.
///
/// @param program_client  outpost program client whose loaded IDL decodes the account and whose `program_id` must
///                        own it.
/// @param buffer          the fetched `EnvelopeChunks` account, or `std::nullopt` when it does not exist.
/// @param epoch_index     the WIRE epoch being uploaded.
/// @param total_chunks    data-chunk count of the envelope being uploaded.
/// @param total_bytes     byte length of the envelope being uploaded.
/// @param uploader        this relay's signer, the buffer's third PDA seed.
/// @return the chunk index to resume from, in `[0, total_chunks]`.
/// @throws fc::exception if a program-owned account does not decode as the `EnvelopeChunks` the loaded IDL declares,
///         or the decoded header lacks a field the resume reads.
uint16_t decode_first_unstaged_chunk(opp_solana_outpost_client&                              program_client,
                                     const std::optional<fc::network::solana::account_info>& buffer,
                                     uint32_t                                                epoch_index,
                                     uint16_t                                                total_chunks,
                                     uint32_t                                                total_bytes,
                                     const fc::network::solana::solana_public_key&           uploader);

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

/// Assert the loaded IDL declares the outbound `EnvelopeLog` the way `decode_envelope_log_digest` reads it: an
/// `envelopes` vector of records carrying `epoch_index` (u32) and `checksum` ([u8; 32]). Called at construction for
/// the underwriter role, so a drifted IDL fails at boot instead of on every pass.
///
/// @param program  the program's loaded Anchor IDL.
/// @throws fc::exception if the account, the vector, the record type or either field is absent or misdeclared.
void assert_envelope_log_shape(const fc::network::solana::idl::program& program);

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

/// Backstop on cleanup calls per sweep, in addition to the wall-clock budget; oldest buffers go first.
inline constexpr uint32_t MAX_ENVELOPE_CHUNK_REAPS_PER_SWEEP = 16;

/// One of this relay's own `EnvelopeChunks` staging buffers, as the
/// `getProgramAccounts` discovery read reports it.
struct envelope_chunk_buffer {
   fc::network::solana::solana_public_key address;
   /// The WIRE epoch the buffer staged, read from its header.
   uint32_t                               epoch_index = 0;
   /// Rent the close returns to the uploader.
   uint64_t                               lamports    = 0;
};

/// The two on-chain cursors `cleanup_envelope_chunks` gates on, read at confirmed or stronger.
struct envelope_chunk_reap_gates {
   /// `OutpostConfig.next_epoch_index`: advanced only by the draining
   /// `dispatch_attestations` round.
   uint32_t next_epoch_index          = 0;
   /// `LatestOutboundEnvelope.epoch_index`: advanced by the outbound emit that
   /// rides the same draining round.
   uint32_t latest_outbound_epoch     = 0;
};

/// Decode the `getMultipleAccounts` pair in request order: `OutpostConfig`, then `LatestOutboundEnvelope`.
/// Both headers must contain their discriminator and u32 cursor, and be owned by `program_client.program_id`.
/// Cursor offsets are validated against the loaded IDL at boot. Invalid input throws instead of supplying a cursor
/// that could skip delivery. The delivery check falls through to normal delivery on a throw; the sweep counts it toward
/// failure back-off, or toward a deadline stop if its deadline has passed.
/// @throws fc::exception for a wrong account count, or invalid account/field data (naming the affected account).
envelope_chunk_reap_gates decode_envelope_chunk_reap_gates(
   opp_solana_outpost_client& program_client,
   const std::vector<std::optional<fc::network::solana::account_info>>& infos);

/// The program's own eligibility predicate for reaping `epoch_index`'s buffer
/// (wire-solana `cleanup_envelope_chunks_handler`). The relay mirrors it so it
/// never pays for a close the program is guaranteed to refuse.
bool envelope_chunks_reapable(const envelope_chunk_reap_gates& gates, uint32_t epoch_index);

/// Whether the outpost has already settled `epoch_index`: its draining
/// `dispatch_attestations` round moved `next_epoch_index` past it. A delivery
/// for a settled epoch has nothing left to do -- the program answers every
/// `epoch_in` for it with a late no-op -- so `deliver_outbound_envelope`
/// reports it as already finalized instead of re-staging chunks.
///
/// @param gates        the program's cursors (only `next_epoch_index` is consulted).
/// @param epoch_index  the WIRE epoch being delivered.
bool outpost_epoch_settled(const envelope_chunk_reap_gates& gates, uint32_t epoch_index);

/// What one sweep did. The sweep is complete -- every buffer that can be
/// reaped today was reaped -- only when nothing failed and nothing was left
/// over by the per-sweep bound or the deadline; the caller re-sweeps otherwise.
struct envelope_chunk_reap_result {
   uint32_t reaped           = 0;
   /// Failed closes whose exact buffer is now absent; no rent is attributed to this sweep.
   uint32_t already_closed   = 0;
   /// Buffers whose epoch has not settled yet (normally the live epoch's own).
   uint32_t not_yet_eligible = 0;
   /// Eligible buffers left for a later sweep, by the per-sweep bound or by the
   /// deadline (including a close the deadline cut short, which may have landed).
   uint32_t deferred         = 0;
   uint32_t failed           = 0;
   uint64_t lamports_reaped  = 0;
   /// The deadline ended the walk; remaining buffers are in `deferred`.
   bool     deadline_reached = false;

   /// At most one attempted close is cut off: the expired deadline prevents all subsequent sends.
   std::optional<fc::network::solana::solana_public_key> cut_off_close;

   /// Whether every eligible buffer was handled without a failed close.
   bool complete() const { return failed == 0 && deferred == 0; }
};

/// How long the chunk-buffer sweep stands down after a failure, or repeated deadline stops.
/// Failures include a refused `getProgramAccounts` or an RPC outage. Without it a
/// persistent failure would re-run the discovery read and every close on each
/// relay tick.
inline constexpr fc::microseconds ENVELOPE_CHUNK_SWEEP_RETRY_BACKOFF = fc::minutes(5);

/// Gate reads, discovery, closes and confirmation share five seconds, capped by the caller's deadline.
inline constexpr fc::microseconds ENVELOPE_CHUNK_SWEEP_BUDGET = fc::seconds(5);

/// The optional settled check shares the sweep's short budget so a stalled read leaves delivery time.
inline constexpr fc::microseconds ENVELOPE_CHUNK_SETTLED_CHECK_BUDGET = ENVELOPE_CHUNK_SWEEP_BUDGET;

/// Three consecutive no-progress sweep attempts tolerate occasional tight ticks while bounding slow discovery reads.
inline constexpr uint32_t ENVELOPE_CHUNK_SWEEP_DEADLINE_STOP_LIMIT = 3;

/// Sweep at startup and after the cursor moves; only complete walks advance the watermark.
/// Failures and repeated deadline stops without progress back off; partial walks stay due.
class envelope_chunk_sweep_schedule {
public:
   /// True during failure or deadline-stop back-off for any sweep.
   /// Checked before any RPC, so a backing-off sweep costs nothing.
   bool backing_off(fc::time_point now) const;

   /// Whether the cursor moved and no back-off remains.
   bool due(uint32_t next_epoch_index, fc::time_point now) const;

   /// Record a complete, bound-limited, deadline-stopped, or failed walk at cursor `next_epoch_index`.
   /// Return true when deadline-stop back-off begins, so the caller can log with its client label.
   bool record(uint32_t next_epoch_index, const envelope_chunk_reap_result& result, fc::time_point now);

   /// Count the last cut-off close as already closed if successful discovery no longer lists its address.
   /// Consume the record once; no rent is attributed, and a still-present buffer is retried normally.
   uint32_t reconcile_cut_off_close(const std::vector<envelope_chunk_buffer>& buffers);

   /// Record a sweep that threw before reaching its end and before its
   /// deadline; resets the deadline-stop streak and backs every sweep off.
   void record_failure(fc::time_point now);

   /// Count a deadline stop without progress; repeated stops back off all sweeps.
   /// Return true when the limit trips and the streak resets for the next back-off period.
   bool record_deadline_stop(fc::time_point now);

   /// Break the streak after progress, failure, or a gate read that found the sweep not due.
   void reset_deadline_stops();

private:
   /// Only the previous completed discovery walk's cut-off close; constant memory even across failed reads.
   std::optional<fc::network::solana::solana_public_key> _cut_off_close;
   /// Consecutive attempted sweeps stopped by their deadline without progress.
   uint32_t _deadline_stops = 0;
   /// `next_epoch_index` as of the last complete sweep; empty until one completes.
   std::optional<uint32_t> _swept_through;
   /// Failure or repeated deadline-stop back-off for every sweep; min() until first armed.
   fc::time_point          _retry_after = fc::time_point::min();
};

/// The `getProgramAccounts` filters selecting `uploader`'s `EnvelopeChunks`
/// buffers: the account discriminator at offset 0 and the header's `operator`
/// field. Both are base58 `memcmp` filters.
///
/// @param discriminator the `EnvelopeChunks` discriminator the loaded IDL declares.
/// @param uploader      the relay's signer, written into `operator` when the buffer was created.
std::vector<fc::variant> envelope_chunk_buffer_filters(const std::array<uint8_t, 8>&                 discriminator,
                                                       const fc::network::solana::solana_public_key& uploader);

/// The `dataSlice` the discovery read requests: the discriminator and header
/// only, never the staged payload (up to `SOLANA_MAX_ENVELOPE_BYTES` per buffer).
fc::network::solana::data_slice envelope_chunk_buffer_header_slice();

/// Decode a `getProgramAccounts` result (the RPC's raw `[{pubkey, account}]`
/// array, data sliced to the header) into `uploader`'s buffers, oldest epoch
/// first.
///
/// An entry is kept only when its decoded `operator` is `uploader` AND its
/// address is the PDA the program derives from (epoch, uploader): a mismatch
/// can only be IDL drift, and a close sent for it would fail its seeds check.
/// Such an entry is logged and skipped rather than failing the whole read.
///
/// @throws fc::exception if the result is not the RPC's array shape or the
///         loaded IDL does not declare `EnvelopeChunks`.
std::vector<envelope_chunk_buffer> decode_envelope_chunk_buffers(opp_solana_outpost_client&                    program_client,
                                                                 const fc::variant&                            program_accounts,
                                                                 const fc::network::solana::solana_public_key& uploader);

/// The chunk-buffer reaper, factored over its RPC touchpoints so the unit tests
/// drive it without a Solana endpoint.
///
/// Walks `buffers` in supplied order; discovery supplies oldest epoch first. A buffer `envelope_chunks_reapable`
/// rejects is counted and skipped -- it becomes reapable once its epoch drains.
/// Each eligible buffer is closed by `send_cleanup(epoch_index)`; a send that
/// throws is logged with its error, counted, and the walk continues, because one
/// stuck buffer must not strand the rest. After `MAX_ENVELOPE_CHUNK_REAPS_PER_SWEEP`
/// closes the remaining eligible buffers are counted as deferred.
///
/// The deadline ends the walk without throwing: once `deadline_passed()` is true
/// before a close, that buffer and every remaining eligible one are deferred and
/// `deadline_reached` is set. A send that throws once the deadline has passed is
/// deferred the same way rather than counted as failed -- the deadline cut it
/// short, and it may have landed.
/// Failed closes are reconciled by the exact account's absence, never by a generic Anchor error:
/// other required accounts can raise the same error. Already-closed buffers consume the per-sweep bound.
///
/// @param gates            the program's two cursors, read before the walk.
/// @param buffers          this relay's buffers.
/// @param deadline_passed  true once the caller's deadline has passed.
/// @param send_cleanup     sends `cleanup_envelope_chunks(epoch_index)` and
///                         returns the tx signature.
/// @param log_label        client identity for log lines.
/// @param buffer_absent    exact-buffer read at processed commitment after an error. A null account
///                         counts as already closed, without recovered rent. A read error preserves the close failure.
///                         The caller must keep this read under the same scoped deadline.
envelope_chunk_reap_result drive_envelope_chunk_reap(
   const envelope_chunk_reap_gates&                     gates,
   std::vector<envelope_chunk_buffer>                   buffers,
   const std::function<bool()>&                         deadline_passed,
   const std::function<std::string(uint32_t)>&          send_cleanup,
   const std::string&                                   log_label,
   const std::function<bool(const envelope_chunk_buffer&)>& buffer_absent);

/// How one call to `sweep_envelope_chunks` ended.
enum class envelope_chunk_sweep_outcome {
   /// Failure or deadline-stop back-off has not expired; nothing was read.
   backing_off,
   /// The deadline had already passed; nothing was read.
   deadline_spent,
   /// The cursor has not moved since the last complete sweep; only the gates were read.
   not_due,
   /// The walk ran -- to completion, to the per-sweep bound, or to the deadline --
   /// and the schedule recorded its result.
   swept,
   /// A read threw at or after the deadline; repeated stops back off.
   cut_short,
   /// A read threw before the deadline. The schedule backs off.
   failed,
};

/// The calls `sweep_envelope_chunks` makes outside itself: the clock and the
/// RPC touchpoints. `outpost_solana_client` binds them to the wall clock
/// and its Solana client; the unit tests script them.
struct envelope_chunk_sweep_touchpoints {
   /// The current time; every deadline and back-off check reads it.
   std::function<fc::time_point()>                     now;
   /// Reads both cursors at `confirmed` or stronger (`read_envelope_chunk_reap_gates`).
   std::function<envelope_chunk_reap_gates()>          read_gates;
   /// The relay's own buffers (`read_own_envelope_chunk_buffers`).
   std::function<std::vector<envelope_chunk_buffer>()> read_buffers;
   /// Sends `cleanup_envelope_chunks(epoch_index)` and returns the tx signature.
   std::function<std::string(uint32_t)>                send_cleanup;
   /// Reads whether the exact failed-close buffer is absent at processed commitment, within the sweep deadline.
   std::function<bool(const envelope_chunk_buffer&)>   buffer_absent;
};

/// One chunk-buffer sweep, gated by `schedule`: the body of
/// `outpost_solana_client::reap_settled_envelope_chunks`, factored over its
/// touchpoints so the unit tests drive every exit without a Solana endpoint.
///
/// In order: a backing-off schedule or a spent deadline returns before any
/// read; the cursors are read (unless `gates` carries them); a sweep that is not
/// due returns; otherwise the discovery read runs and `drive_envelope_chunk_reap`
/// walks the buffers, and the schedule records the result. A read that throws
/// before the deadline backs the schedule off; one that throws at or after the
/// deadline records a deadline stop, backing off after consecutive no-progress attempts.
/// Deadline-stop back-off applies to all sweeps;
/// not-due gates and failures reset the streak; an already-spent deadline leaves it unchanged.
///
/// Never throws: every failure is logged with its error.
///
/// @param schedule      when sweeps run; updated with this sweep's outcome.
/// @param sweep_start   the time before applying the budget, caller deadline and outer scope clamps.
/// @param deadline_abs  the effective absolute deadline of this sweep.
/// @param gates         the cursors, when the caller has just read them at `confirmed`.
/// @param touchpoints   the clock and the RPC calls.
/// @param log_label     client identity for log lines.
envelope_chunk_sweep_outcome sweep_envelope_chunks(envelope_chunk_sweep_schedule&                  schedule,
                                                   fc::time_point                                  sweep_start,
                                                   fc::time_point                                  deadline_abs,
                                                   const std::optional<envelope_chunk_reap_gates>& gates,
                                                   const envelope_chunk_sweep_touchpoints&         touchpoints,
                                                   const std::string&                              log_label);

/// The settled-epoch check at the top of
/// `outpost_solana_client::deliver_outbound_envelope`. Reads the cursors; when
/// the outpost has already settled `epoch_index` (`outpost_epoch_settled`), logs
/// the skip, runs `sweep` with the cursors just read, and returns true -- the
/// caller sends nothing and reports the epoch as already finalized. Returns
/// false when the epoch is not settled, and when the read throws (logged): an
/// unread cursor means "not known settled", so the delivery goes ahead.
///
/// @param epoch_index  the WIRE epoch being delivered.
/// @param read_gates   reads both cursors at `confirmed`.
/// @param sweep        the chunk-buffer sweep, handed the cursors it may reuse.
/// @param log_label    client identity for log lines.
bool skip_settled_epoch_delivery(uint32_t                                                     epoch_index,
                                 const std::function<envelope_chunk_reap_gates()>&            read_gates,
                                 const std::function<void(const envelope_chunk_reap_gates&)>& sweep,
                                 const std::string&                                           log_label);

/// The tail of `outpost_solana_client::read_inbound_envelope`: read the
/// outbound envelope, and only when it comes back empty -- an idle tick, with no
/// depot delivery waiting on the result -- pass `deadline_abs` to the chunk-buffer sweep.
/// The sweep member applies its short budget once, after the read. A found envelope is returned without sweeping.
///
/// @param read_envelope  the `finalized` envelope read.
/// @param sweep          the chunk-buffer sweep, given its absolute deadline.
/// @param deadline_abs   the caller's absolute deadline.
/// @return the envelope bytes, or empty when the outpost has not emitted the epoch's envelope yet.
std::vector<char> read_envelope_then_sweep_when_idle(const std::function<std::vector<char>()>& read_envelope,
                                                     const std::function<void(fc::time_point)>& sweep,
                                                     fc::time_point                            deadline_abs);

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
   /// The separate operational pause also refuses `report_liq_yield`.
   constexpr auto field_paused     = "paused";
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

/// True unless the decoded `GlobalState` holds `frozen == false`.
/// A set, missing or non-bool `frozen` reads as
/// frozen, so the crank never pays for a transaction the program could refuse.
bool liq_outpost_frozen(const fc::variant_object& global_state);

/// True unless the decoded `GlobalState` holds `paused == false`.
bool liq_outpost_paused(const fc::variant_object& global_state);

/// True iff the decoded `GlobalState` is PostLaunch, unfrozen and unpaused.
/// Missing or misshaped gating fields read as "not due". Other on-chain
/// prerequisites are checked by the program when the crank is submitted.
bool liq_yield_report_due(const fc::variant_object& global_state);

/// Matches wire-solana's `executeReportLiqYield` and the Tools yield harness.
inline constexpr uint32_t REPORT_LIQ_YIELD_COMPUTE_UNITS = 800'000;

enum class liq_yield_report_state { no_account, frozen, paused, not_due, due };

/// The automatic crank's fetch/decode/decision/submission path. Absent or empty
/// accounts are idle; malformed data throws; only a due account reaches
/// `report_yield`, with its compute-budget pre-instruction. Callbacks let tests
/// exercise the same path without RPC or a live signer.
liq_yield_report_state drive_liq_yield_report(
   const std::function<std::optional<fc::network::solana::account_info>()>& read_global_state,
   const std::function<fc::variant(const std::vector<uint8_t>&)>& decode_global_state,
   const std::function<void(const std::vector<fc::network::solana::instruction>&)>& report_yield);

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
/// type carries the `PostLaunch` variant the yield crank gates on, and declares
/// `frozen` and `paused` as bools. Boot-checked only on a
/// program that declares `report_liq_yield`.
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
 * zero-data terminal `epoch_in` call. That call records delivery and may reach
 * consensus; the relay then attempts the separate resumable
 * `dispatch_attestations` drain, which applies effects and emits outbound on
 * its final round. The delivery return value remains the terminal call's
 * signature even if settlement must resume later. An envelope too large for
 * one delivery deadline resumes from the staged buffer on later ticks: a tick
 * that staged at least one chunk before its deadline ran out throws
 * `chain::outpost_delivery_incomplete_exception` (progress, retried next tick),
 * and only a tick that staged nothing surfaces the deadline as a failure.
 * A delivery for an epoch the outpost has already settled (a depot consensus
 * retry, possibly after the chunk-buffer sweep closed that epoch's buffer)
 * sends nothing and returns an empty signature, which the caller reads as
 * "outpost already finalized".
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

   /// The outbound `EnvelopeLog` PDA at `finalized`: the checksum the program recorded when it emitted that
   /// epoch's envelope.
   std::optional<fc::sha256> read_emitted_envelope_digest(uint32_t         epoch_index,
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

   /// The first data chunk of `epoch_index`'s envelope this relay still has to upload, from its own on-chain
   /// `EnvelopeChunks` buffer (`decode_first_unstaged_chunk`). The resume is an optimisation the delivery must never
   /// depend on, so any failure -- signer or PDA lookup, the RPC read, or decoding -- is logged and yields 0. The
   /// caller still owns the delivery deadline: an RPC that fails because the deadline passed also yields 0 here, so
   /// the caller checks the deadline after this returns.
   virtual uint16_t read_first_unstaged_chunk(uint32_t epoch_index, uint16_t total_chunks, uint32_t total_bytes);

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

protected:
   // Subclasses and tests can override these reads and the private read_first_unstaged_chunk
   // to exercise member orchestration without a live endpoint.

   /// The `finalized` read of the `latest_outbound_envelope` PDA behind
   /// `read_inbound_envelope`, after its pre-read drain: the envelope bytes for
   /// `epoch_index`, or empty when the PDA does not (yet) hold that epoch.
   virtual std::vector<char> read_latest_outbound_envelope(uint32_t epoch_index, fc::time_point deadline_abs);

   /// RPC boundary for the ordered gate accounts; forwards the exact list, commitment and header slice.
   virtual std::vector<std::optional<fc::network::solana::account_info>> read_chunk_reap_accounts(
      const std::vector<fc::network::solana::solana_public_key>& accounts,
      fc::network::solana::commitment_t commitment,
      const std::optional<fc::network::solana::data_slice>& slice);

   /// RPC boundary for discovery; forwards the exact program, filters, commitment and slice.
   virtual fc::variant read_chunk_program_accounts(
      const fc::network::solana::solana_public_key& program_id, const std::vector<fc::variant>& filters,
      fc::network::solana::commitment_t commitment, const std::optional<fc::network::solana::data_slice>& slice);

   /// Read `OutpostConfig.next_epoch_index` and `LatestOutboundEnvelope.epoch_index`
   /// together at `confirmed` with a header-only slice, matching cleanup preflight and confirmation.
   virtual outpost_solana_client_detail::envelope_chunk_reap_gates read_envelope_chunk_reap_gates();

   /// Every `EnvelopeChunks` buffer this relay's signer still owns, oldest epoch
   /// first (`getProgramAccounts`, header-only slice).
   virtual std::vector<outpost_solana_client_detail::envelope_chunk_buffer> read_own_envelope_chunk_buffers();

   /// Read the exact failed-close buffer at the caller's commitment, under the active sweep deadline.
   /// nullopt means absent or a malformed libfc reply; treating it as closed may delay recovery to discovery.
   /// Empty data still means the account is present; no rent is attributed for nullopt.
   virtual std::optional<fc::network::solana::account_info> read_envelope_chunk_buffer(
      const fc::network::solana::solana_public_key& address, fc::network::solana::commitment_t commitment);

   /// Return the rent of every chunk buffer this relay staged for an epoch that
   /// has since settled.
   ///
   /// `epoch_in` stages each delivery in a per-(epoch, signer) buffer the
   /// uploader pays rent for, and `dispatch_attestations` deliberately leaves it
   /// open (the crank is permissionless, so a close there would credit the
   /// caller rather than the uploader). Without this sweep every delivered
   /// epoch leaks that rent. Runs after a delivery's inline drain, when a
   /// delivery finds its epoch already settled, and on inbound-read ticks that
   /// found no envelope; every path uses `ENVELOPE_CHUNK_SWEEP_BUDGET`. It
   /// covers every older buffer too -- one left by a
   /// restart, a failed close, or an abandoned partial upload.
   ///
   /// Cost-bounded by `_chunk_sweep_schedule`: nothing is read during failure back-off,
   /// or deadline-stop back-off. Otherwise the cursors
   /// are read (unless the caller passes them), and discovery and closes run only when the schedule
   /// says a sweep is due. The body is `outpost_solana_client_detail::sweep_envelope_chunks`;
   /// this member binds it to the wall clock and this client's RPC calls, all
   /// under a `deadline_scope` capped to the caller and the five-second sweep budget.
   ///
   /// Best-effort: never throws. Failures are logged with their chain-side
   /// reason; the buffers stay on chain and a later sweep retries them. Running
   /// out of deadline -- before the sweep starts or partway through it -- is not
   /// a close failure; consecutive attempted sweeps stopped without progress back off on every path.
   ///
   /// Sweeps only run while this operator is elected and within its epoch window. A buffer may
   /// wait for its owner's next election; an operator that leaves permanently strands its last buffer.
   /// Discovery has no paging and the RPC response cap is 4 MiB (roughly 16k header-only buffers).
   ///
   /// @param deadline_abs  caller deadline, clamped to the active scope and five-second sweep budget.
   /// @param gates         the cursors, when the caller has just read them at `confirmed`.
   void reap_settled_envelope_chunks(
      fc::time_point                                                               deadline_abs,
      const std::optional<outpost_solana_client_detail::envelope_chunk_reap_gates>& gates = std::nullopt);

private:
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

   /// When `reap_settled_envelope_chunks` next runs: the last complete sweep's
   /// cursor and any back-off after a failed one.
   outpost_solana_client_detail::envelope_chunk_sweep_schedule _chunk_sweep_schedule;

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

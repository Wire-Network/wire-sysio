#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
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
/// envelope by Solana's packet limit: `MAX_ENVELOPE_BYTES` (64 KiB) admits on the
/// order of 500 small SPL swap-remits across distinct reserves, and at ~5
/// attestations per 16-account round that needs ~100 rounds. Exhausting this is
/// a liveness alarm, never a silent success.
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

/// Assert that the loaded IDL's `Reserve` declaration carries the three fields
/// the terminal manifest resolves from it: `creator` and `custody_mint`
/// (pubkeys) and `custody_decimals` (u8), in either IDL field home.
///
/// Called at construction for the batch-operator role, beside the
/// `EpochDeliveries` check. IDL drift is the realistic reason a live,
/// program-readable `Reserve` becomes unreadable to THIS relay, and that
/// failure is unrecoverable in flight: the manifest cannot be completed, the
/// dispatch window would abort on a missing effect account, and every later
/// window repacks from the same unadvanced cursor. Catching it at boot turns a
/// wedged epoch into a startup error while the IDL is still fixable.
///
/// @param program  the program's loaded Anchor IDL.
/// @throws fc::exception if the account or any of the three fields is absent,
///         or a field has a type the manifest builder cannot interpret.
void assert_reserve_shape(const fc::network::solana::idl::program& program);

/// Terminal-finalization facts for one per-`(token_code, reserve_code)`
/// Reserve PDA, read from the `Reserve` ACCOUNT itself.
///
/// Custody is pinned on the Reserve at creation time and is exactly what the
/// on-chain handlers branch on (`handle_swap_remit` / `handle_swap_revert` /
/// `handle_reserve_create_cancelled` all test `reserve.custody_mint` against
/// the native marker). Resolving it from the mutable
/// `OutpostConfig.token_addresses_by_code` instead would let an admin
/// re-pointing a token address between reserve creation and dispatch drive the
/// relay down the native branch while the program takes the SPL one -- the
/// manifest would then lack the vault/ATA accounts the handler requires and
/// the call would abort permanently, wedging the epoch.
struct reserve_terminal_info {
   /// Reserve creator -- the `RESERVE_CREATE_CANCELLED` refund target.
   fc::network::solana::solana_public_key creator;
   /// Custody mint, or the all-zero system-program key for native lamports
   /// (the program's `NATIVE_TOKEN_MARKER` convention).
   fc::network::solana::solana_public_key custody_mint;
   /// Chain-native decimals pinned at reserve creation.
   uint8_t                                custody_decimals = 0;
};

/// Extract the terminal-finalization facts from an already-decoded `Reserve`
/// account object. ALL THREE fields are required: `creator`, `custody_mint`
/// and `custody_decimals` are written together at reserve creation, so a
/// record missing any of them is not a reserve this relay can build an
/// account-consistent manifest for — guessing custody is precisely the
/// divergence that aborts the on-chain call.
///
/// Throwing here FAILS THE BUILD, by design — do not "restore" a degrade on
/// this path. Reaching this function means the account EXISTS and the program
/// therefore decodes it fine and takes its real branch, demanding the accounts
/// that branch needs. A manifest built by guessing the missing field is
/// guaranteed to abort on chain (`require_remaining_account` ->
/// EffectAccountMissing), and because `drive_dispatch_rounds` repacks every
/// window from the unadvanced on-chain cursor, that aborting attestation heads
/// every future window and the epoch never closes.
///
/// The benign degrade lives one level up and covers a DIFFERENT cause: an
/// ABSENT or EMPTY reserve never reaches this function, and
/// `reserve_info_for_codes` returns empty for it because the program skips an
/// uninitialized reserve. Only that cause may degrade.
///
/// Exposed in this header so the plugin's unit tests can drive custody
/// resolution — including a config-vs-reserve divergence — without RPC.
///
/// @param reserve  a decoded `Reserve` account object.
/// @throws fc::exception when a required field is absent or unparseable.
reserve_terminal_info reserve_info_from_account(const fc::variant_object& reserve);

/// Reads the `Reserve` account behind `(token_code, reserve_code)`.
///
/// Returns empty ONLY for a reserve the program itself will skip (absent or
/// uninitialized) — a benign per-attestation degrade. A reserve that exists
/// but cannot be read THROWS, because the program would take its real branch
/// and abort on the accounts a degraded manifest omits; that exception is
/// propagated by `build_dispatch_manifests` rather than absorbed.
using reserve_info_reader =
   std::function<std::optional<reserve_terminal_info>(uint64_t token_code, uint64_t reserve_code)>;

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

   std::string uw_commit(uint64_t                 uw_request_id,
                         const std::vector<char>& uic_bytes,
                         fc::microseconds         deadline) override;

   // Expose for inspection / tests
   const solana_client_entry_ptr&                entry()                 const { return _entry; }
   const fc::network::solana::solana_public_key& program_id()            const { return _program_id; }

private:
   /// Resolve the terminal-finalization facts for a per-reserve PDA -- creator
   /// AND custody (mint / decimals) -- from the ONE account the on-chain
   /// handlers themselves branch on: the `Reserve` PDA. One RPC read per
   /// distinct reserve; no `OutpostConfig` read on this path at all.
   ///
   /// The two failure modes are deliberately NOT treated alike:
   ///
   ///   * An ABSENT or EMPTY reserve returns empty (a warning is logged). The
   ///     program skips an uninitialized reserve, so a partial manifest is
   ///     harmless.
   ///   * A reserve that EXISTS but this relay cannot decode (or that is
   ///     missing creator/custody) THROWS, after logging the chain-side
   ///     reason. The program reads that account fine and demands the accounts
   ///     its real branch needs, so a degraded manifest would be guaranteed to
   ///     abort — permanently, since every later window repacks from the same
   ///     unadvanced cursor. Failing the tick leaves the cursor untouched.
   std::optional<outpost_solana_client_detail::reserve_terminal_info>
   reserve_info_for_codes(uint64_t token_code, uint64_t reserve_code);

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

/// `(token_code, reserve_code)` pair for a Reserve PDA derivation, carried on
/// every reserve-backed `inbound_effect`. The manifest builder derives the
/// Reserve PDA via Anchor's `find_program_address` with the `[RESERVE_SEED,
/// &token_code.to_le_bytes(), &reserve_code.to_le_bytes()]` seed list against
/// the program id.
struct reserve_pda_seeds {
   uint64_t token_code;
   uint64_t reserve_code;
};

/// Derive the per-`(token_code, reserve_code)` `Reserve` PDA: seeds
/// `["reserve", token_code.to_le_bytes(), reserve_code.to_le_bytes()]`.
/// Byte-exact mirror of the program's `#[account(seeds = ...)]` declaration —
/// a derivation that disagrees fails seeds validation on chain.
///
/// Exported so the manifest builder and its tests derive through ONE
/// implementation rather than re-spelling the seed list.
fc::network::solana::solana_public_key
derive_reserve_pda(const fc::network::solana::solana_public_key& program_id,
                   uint64_t token_code,
                   uint64_t reserve_code);

/// Derive the per-`(token_code, reserve_code)` `reserve_vault` PDA: seeds
/// `["reserve_vault", token_code.to_le_bytes(), reserve_code.to_le_bytes()]`.
/// The vault holds the reserve's SPL custody; native reserves settle straight
/// out of the Reserve PDA, so this account only rides an SPL manifest.
fc::network::solana::solana_public_key
derive_reserve_vault_pda(const fc::network::solana::solana_public_key& program_id,
                         uint64_t token_code,
                         uint64_t reserve_code);

/// Which family of effect accounts one inbound attestation needs. The relay
/// derives the concrete metas per shape; the on-chain handler resolves them
/// out of `remaining_accounts` by pubkey.
enum class effect_shape {
   /// Pays a native-SOL recipient directly (WITHDRAW_REMIT, DEPOSIT_REVERT).
   native_payee,
   /// SWAP_REMIT: Reserve PDA, plus vault + recipient ATA + token program
   /// when the reserve's custody mint is SPL rather than native.
   swap_remit,
   /// SWAP_REVERT: refunds the depositor, so it additionally needs the mint,
   /// the ATA program and the system program to create the ATA if absent.
   swap_revert,
   /// RESERVE_READY: Reserve PDA only.
   reserve_ready,
   /// RESERVE_CREATE_CANCELLED: refunds the reserve's creator.
   reserve_create_cancelled,
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
   /// Native payee / SWAP_REMIT recipient / SWAP_REVERT depositor.
   std::optional<fc::network::solana::solana_public_key> recipient;
   /// Set for every reserve-backed shape.
   std::optional<reserve_pda_seeds>                      reserve;
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

/// Build the per-attestation effect-account manifests `drive_dispatch_rounds`
/// packs its `dispatch_attestations` windows from, factored over its ONE RPC
/// touchpoint (`read_reserve_info`) so the plugin's unit tests can drive the
/// whole build -- custody branching, per-reserve read coalescing, degrade
/// paths and the deadline probe -- without a live Solana endpoint.
///
/// The result is indexed by the FLAT attestation position the on-chain cursor
/// counts, sized to `total_attestations`; attestations needing no effect
/// account keep an empty entry so the indices stay aligned.
///
/// Behaviour that matters:
///   * `read_reserve_info` is called at most ONCE per distinct
///     `(token_code, reserve_code)` -- the results are memoised for the build,
///     including the empty (degraded) ones, so a repeated reserve never
///     re-pays an RPC round-trip.
///   * Custody branching follows `reserve_terminal_info::custody_mint`, the
///     same field the on-chain handler branches on.
///   * `throw_if_past_deadline` runs once per effect, BEFORE its reserve read,
///     so an over-deadline build fails at the loop rather than deep in the RPC
///     layer with the work already lost.
///   * A degraded (empty) reserve read costs only THAT attestation its
///     custody-dependent accounts; every other attestation's manifest is still
///     built, so the envelope's healthy prefix still dispatches. A THROWING
///     read propagates untouched — it means a manifest the program would abort
///     on, and shipping one would wedge the epoch rather than delay it.
///
/// @param program_id            outpost program id, for PDA derivation.
/// @param effects               account-needing attestations, in dispatch order.
/// @param total_attestations    the envelope's attestation total (the cursor's
///                              denominator) -- the size of the result.
/// @param throw_if_past_deadline  throws once the caller's deadline passes.
/// @param read_reserve_info     reads one `Reserve` record (may degrade).
/// @param log_label             client identity for log lines.
/// @return one manifest per attestation, in dispatch order.
std::vector<std::vector<fc::network::solana::account_meta>> build_dispatch_manifests(
   const fc::network::solana::solana_public_key& program_id,
   const std::vector<inbound_effect>&            effects,
   uint32_t                                      total_attestations,
   const std::function<void()>&                  throw_if_past_deadline,
   const reserve_info_reader&                    read_reserve_info,
   const std::string&                            log_label);

} // namespace outpost_solana_client_detail

} // namespace sysio

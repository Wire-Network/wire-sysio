#include <sysio.msgch/sysio.msgch.hpp>
#include <sysio.epoch/sysio.epoch.hpp>
#include <sysio.authex/sysio.authex.hpp>
#include <sysio.chains/sysio.chains.hpp>
#include <sysio.chalg/sysio.chalg.hpp>     // dispute trigger + open-dispute gate (disputes table)
#include <sysio.opp.common/slug_name.hpp>
#include <sysio/opp/opp.pb.hpp>
#include <sysio/opp/attestations/attestations.pb.hpp>
#include <zpp_bits.h>
#include <algorithm>
#include <optional>

namespace sysio {

using opp::types::ChainKind;
using opp::types::WireKeyType;
using opp::types::MessageDirection;
using opp::types::MessageStatus;
using opp::types::EnvelopeStatus;
using opp::types::AttestationType;
using opp::types::AttestationStatus;

namespace {

constexpr auto     EPOCH_ACCOUNT   = "sysio.epoch"_n;
constexpr auto     OPREG_ACCOUNT   = "sysio.opreg"_n;
constexpr auto     UWRIT_ACCOUNT   = "sysio.uwrit"_n;
constexpr auto     CHALG_ACCOUNT   = "sysio.chalg"_n;
constexpr auto     AUTHEX_ACCOUNT  = "sysio.authex"_n;
constexpr auto     CHAINS_ACCOUNT  = "sysio.chains"_n;
constexpr auto     RESERV_ACCOUNT  = "sysio.reserv"_n;
constexpr auto     ROA_ACCOUNT     = "sysio.roa"_n;

// System-owned rows bill to the sysio RAM pool, not this contract account (privileged-contract
// model, as sysio.token uses): the account stays finite at code+abi size; growth draws from the pool.
constexpr name     ram_payer       = "sysio"_n;

/// WIRE chain numeric id used in `opp::Endpoints` rows on the audit log.
/// One end of every cross-chain envelope is always WIRE.
constexpr uint32_t WIRE_CHAIN_ID  = 1;

using sysio::slug_name_literals::operator""_s;

/// Codename of the Ethereum outpost — the sole source of node-owner NFT (ERC1155) deposits, which
/// occur on Ethereum mainnet only. This is the `ChainSpec.code` the launch and dev bootstrap configs
/// register for the EVM chain (`etc/config/dex/dex-config*.json`); it is stable across environments
/// (only `external_chain_id` differs: 1 on mainnet, 31337 on the anvil devnet). `dispatch_node_owner_reg`
/// binds inbound registrations to this exact outpost — see the WSA-005 note there.
constexpr sysio::slug_name NODE_OWNER_SRC_CHAIN = "ETHEREUM"_s;

/// Hard cap on the encoded outbound envelope size, mirroring the Solana
/// (`opp_outpost::MAX_ENVELOPE_BYTES`) and Ethereum (`OPP.MAX_ENVELOPE_BYTES`)
/// caps. 64 KiB is the e2e-supported maximum across WIRE / Ethereum / Solana,
/// bound by Solana's 256 KiB BPF heap divided by ~3.3× envelope-size peak heap
/// usage during the finalising chunk's `Envelope::decode + keccak::hash + clone`.
/// The `buildenv` packing loop uses this to decide how many READY attestations
/// to bundle into the current epoch's envelope; any that don't fit stay in the
/// `attestations` table with status READY for the next epoch's `buildenv` call.
constexpr size_t   MAX_ENVELOPE_BYTES         = 65'536;

/// Conservative per-attestation byte budget used by the `buildenv` packing
/// loop: protobuf tags + length prefixes + the attestation type/data-size
/// fields. Over-counts by a few bytes per attestation versus the actual
/// `zpp::bits` encoded size, which keeps the loop O(N) and always errs on the
/// side of leaving a gap. The trailing `packed.size()` check after final
/// serialisation is the hard backstop.
constexpr size_t   ATTESTATION_OVERHEAD_BYTES = 24;

/// Conservative envelope/message header budget for the packing loop —
/// covers the `Envelope` header fields, the wrapping `Message`, its header
/// + payload preamble, and a safety margin for `zpp::bits` length prefixes.
constexpr size_t   ENVELOPE_BASELINE_BYTES    = 512;

uint32_t current_epoch_index() {
   epoch::epochstate_t tbl(EPOCH_ACCOUNT);
   return tbl.exists() ? tbl.get().current_epoch_index : 0;
}

/// Mint the next attestation id from the `attseq` singleton.
///
/// Replaces a `std::max<uint64_t>(1, atts.available_primary_key())` call
/// at every `attestations_t` insertion site. The `attseq` singleton survives the
/// `buildenv` cleanup of `ATTESTATION_STATUS_PROCESSED` rows, so the
/// monotonic counter keeps advancing across phases even when the atts
/// table is drained. Without this, Phase N+1's inbound `SwapRequest`
/// inherits Phase 1's attestation_id and collides with the existing UWREQ
/// row in `sysio.uwrit` — `createuwreq`'s idempotency guard then
/// silently drops the new swap.
///
/// First call materialises the row at `next = 2` and returns `1`.
/// Subsequent calls return the current `next` and post-increment.
uint64_t mint_att_id(name self) {
   msgch::att_seq_t seq(self);
   msgch::att_seq_key pk{0};
   if (!seq.contains(pk)) {
      seq.emplace(ram_payer, pk, msgch::att_seq_entry{ .id = 0, .next = 2 });
      return 1;
   }
   auto row = seq.get(pk);
   uint64_t out = row.next;
   seq.modify(same_payer, pk, [&](auto& r) { r.next = out + 1; });
   return out;
}

uint32_t epoch_operators_per_group() {
   epoch::epochcfg_t tbl(EPOCH_ACCOUNT);
   return tbl.exists() ? tbl.get().operators_per_epoch : 7;
}

/// Insert a metadata row into `envelope_log` and, if the table has grown
/// past its derived cap, evict the oldest full epoch (one
/// `per_epoch_record_count`'s worth of rows) from the head.
///
/// Cap derivation:
///   active_outposts        = sysio.epoch.outposts.size()
///   per_epoch_record_count = active_outposts * 2     // 1 inbound + 1 outbound per outpost
///   cap                    = per_epoch * cfg.epoch_retention_envelope_log_count
///
/// `live_count` is computed in O(1) from id arithmetic
/// (`available_primary_key()` and `tbl.begin()->id`) — no full-table scan
/// and no per-endpoint walk. Eviction at most touches one full epoch's
/// rows per write, since the slice can only grow by one per insert.
void write_envelope_log(name self,
                        const sysio::opp::Endpoints& endpoints,
                        uint32_t                     epoch_index,
                        const checksum256&           checksum) {
   sysio::msgch::envelope_log_t tbl(self);
   const uint64_t new_id = std::max<uint64_t>(1, tbl.available_primary_key());
   tbl.emplace(ram_payer, sysio::msgch::id_key{new_id}, sysio::msgch::envelope_log_entry{
      .id          = new_id,
      .endpoints   = endpoints,
      .epoch_index = epoch_index,
      .checksum    = checksum,
      .emitted_at  = current_time_point(),
   });

   // Active-outpost count is sourced from sysio.chains::chains, filtering
   // out the depot self-row. Mirrors the predicate used by sysio.epoch's
   // `is_active_outpost`: a chain row is an active outpost iff
   // `row.active == true && row.is_depot == false`.
   sysio::chains::chains_t chains_tbl(CHAINS_ACCOUNT);
   uint32_t active_outposts = 0;
   for (auto it = chains_tbl.begin(); it != chains_tbl.end(); ++it) {
      if (it->active && !it->is_depot) ++active_outposts;
   }
   if (active_outposts == 0) return;                  // nothing to bound against

   epoch::epochcfg_t cfg_tbl(EPOCH_ACCOUNT);
   if (!cfg_tbl.exists()) return;                     // no config yet, no cap
   const auto cfg = cfg_tbl.get();
   const uint32_t per_epoch = active_outposts * 2;    // 1 inbound + 1 outbound
   const uint64_t cap =
      static_cast<uint64_t>(per_epoch) * cfg.epoch_retention_envelope_log_count;

   auto first_it = tbl.begin();
   if (first_it == tbl.end()) return;                 // defensive: just inserted
   const uint64_t oldest_id  = first_it.key().id;
   const uint64_t live_count = (new_id + 1) - oldest_id;
   if (live_count <= cap) return;

   uint32_t dropped = 0;
   for (auto it = tbl.begin();
        it != tbl.end() && dropped < per_epoch; ) {
      it = tbl.erase(std::move(it));
      ++dropped;
   }
}

/// Build a `sysio::public_key` variant from the raw bytes carried in
/// `op_address.address` plus the originating chain. Inverse of
/// opreg.cpp's `pubkey_to_bytes`. Returns an empty (default-constructed
/// K1) variant for malformed input or unsupported chain kinds — the
/// downstream `bypubkey` lookup then misses and the dispatch drops.
sysio::public_key public_key_from_op_address(ChainKind chain,
                                             const std::vector<char>& bytes) {
   sysio::public_key pk;
   switch (chain) {
      case ChainKind::CHAIN_KIND_WIRE: {       // K1 — variant index 0
         if (bytes.size() != 33) return pk;
         sysio::ecc_public_key arr;
         std::copy(bytes.begin(), bytes.end(), arr.begin());
         pk.emplace<0>(arr);
         return pk;
      }
      case ChainKind::CHAIN_KIND_EVM: {        // EM — variant index 3
         if (bytes.size() != 33) return pk;
         sysio::ecc_public_key arr;
         std::copy(bytes.begin(), bytes.end(), arr.begin());
         pk.emplace<3>(arr);
         return pk;
      }
      case ChainKind::CHAIN_KIND_SVM: {        // ED — variant index 4
         if (bytes.size() != 32) return pk;
         sysio::ed_public_key arr;
         std::copy(bytes.begin(), bytes.end(),
                   reinterpret_cast<char*>(arr.data()));
         pk.emplace<4>(arr);
         return pk;
      }
      default:
         return pk;
   }
}

/// Resolve `op_address` (chain-kind + raw pubkey bytes) to the operator's
/// WIRE account name via `sysio.authex::links`'s `bypubkey` index. Returns
/// `name{}` (zero) on miss — caller treats that as "operator not linked,
/// drop the attestation".
name resolve_account_from_op_address(const opp::types::ChainAddress& op_address) {
   sysio::public_key pk = public_key_from_op_address(op_address.kind,
                                                     op_address.address);
   auto digest = sysio::pubkey_to_checksum256(pk);
   sysio::authex::links_t links(AUTHEX_ACCOUNT);
   auto by_pubkey = links.get_index<"bypubkey"_n>();
   auto it = by_pubkey.find(digest);
   if (it == by_pubkey.end()) return name{};
   return it->username;
}

/// Inbound source-chain binding check (WSA-005).
///
/// A consensus-reached envelope is delivered for exactly ONE proven source outpost:
/// `proven_chain_code`, which `deliver` validated as an active, non-depot row on
/// `sysio.chains` before the envelope reached consensus. Every value-bearing attestation the
/// envelope carries ALSO embeds its own chain identifier in the decoded payload
/// (`OperatorAction.chain_code`, `UnderwriteIntentCommit.chain_code`, `StakingReward.chain_code`,
/// `ReserveAmount.chain_code`, `ReserveCreateCancel.chain_code`). That embedded identifier MUST
/// equal the proven source outpost: in the OPP model an attestation about chain X is always relayed
/// by outpost X (confirmed across every leg in `sysio.dispatch_tests`, and stated in the proto —
/// `ReserveCreate.external_amount.chain_code` is documented as "the outpost's own chain").
///
/// A mismatch means an envelope proven from outpost A is asking the depot to apply a value-bearing
/// effect on a different chain B. Batch-operator consensus proves only that A's operators agree on
/// the envelope bytes; it does NOT prove the depositor acted on B. Applying the effect anyway would
/// let a compromised or malicious A-operator quorum drive deposits, withdrawals, swaps, underwrite
/// commits, reserve mutations, or staking credits against an unrelated chain B's ledger — the
/// cross-chain provenance forgery WSA-005 describes.
///
/// Per `feedback_opp_handlers_never_throw`, dispatch must never abort the envelope on a single bad
/// attestation: a `check()` here would revert the whole `deliver` / `evalcons` transaction and stall
/// consensus chain-wide. Instead we log the rejection (greppable under `--contracts-console`) and the
/// caller drops the single offending attestation, leaving every well-formed attestation in the same
/// envelope free to dispatch.
///
/// `path` labels the dispatch path in the diagnostic. Returns true iff the binding holds.
[[nodiscard]] bool source_chain_binding_ok(uint64_t proven_chain_code,
                                           uint64_t payload_chain_code,
                                           const char* path) {
   if (payload_chain_code == proven_chain_code) return true;
   sysio::print("msgch::", path, ": DROP attestation -- payload chain_code=", payload_chain_code,
                " does not match the proven source outpost chain_code=", proven_chain_code,
                " (WSA-005 cross-chain provenance mismatch)\n");
   return false;
}

/// Decode an OperatorAction sub-message and dispatch to the appropriate
/// sysio.opreg action. Called from the inbound dispatch loop in `evalcons`.
///
/// Sub-type routing (post v6 data-model refactor — codenames everywhere):
///   * DEPOSIT_REQUEST     → opreg::depositinle(account, chain_code, token_code,
///                                              amount, actor_chain, actor_addr,
///                                              msg_id)
///   * WITHDRAW_REQUEST    → opreg::withdrawinle(account, chain_code, token_code,
///                                                amount)
///   * WITHDRAW_REMIT      → outbound-only (depot → outpost); silently dropped if seen inbound
///   * SLASH               → depot-internal; rejected if seen inbound. Slash decisions
///                            originate from sysio.chalg → opreg::slash and never re-enter
///                            the depot via OPP. A slash arriving inbound here is either an
///                            outpost replaying its own outbound (no-op), or a malformed
///                            attestation from a misbehaving operator (drop).
///   * UNKNOWN             → no-op
///
/// `chain_code` (the proven source outpost from `deliver`) is the escrow-holding chain the
/// inline `depositinle` / `withdrawinle` is dispatched against. The payload's own
/// `OperatorAction.chain_code` must equal it (WSA-005): the proto documents field 7 as "the
/// outpost holding the escrow", which for a DEPOSIT_REQUEST / WITHDRAW_REQUEST is exactly the
/// outpost that relayed this attestation. We dispatch using the proven `chain_code`, never the
/// payload's, and drop the attestation when they diverge.
///
/// `original_message_id` is the OPP message id of the attestation's parent
/// Message — opreg::depositinle uses it to populate DEPOSIT_REVERT correlation
/// when refunding an unaccepted deposit.
void dispatch_operator_action(name self, const std::vector<char>& data,
                              uint64_t chain_code,
                              const checksum256& original_message_id) {
   opp::attestations::OperatorAction oa;
   {
      auto in = zpp::bits::in{std::span{data.data(), data.size()}, zpp::bits::no_size{}};
      auto rc = in(oa);
      if (rc != zpp::bits::errc{}) return;   // malformed; skip silently
   }

   // WSA-005: bind the payload's escrow chain to the proven source outpost before crediting or
   // debiting any operator collateral. A consensus envelope proven from outpost A must not be able
   // to deposit/withdraw against a different chain B's operator ledger.
   if (!source_chain_binding_ok(chain_code, oa.chain_code, "dispatch_operator_action")) return;

   // Resolve the operator's WIRE account from `op_address` via authex's
   // bypubkey index. Outposts emit the full chain pubkey (33 bytes for
   // secp256k1, 32 for Ed25519); the depot's authex link table is the
   // single source of truth that maps it back to a WIRE name. On miss
   // (no authex link, malformed bytes, unsupported chain kind), drop —
   // the OperatorRegistry that originated this deposit will see no
   // corresponding state update on the depot side and can re-emit after
   // the operator completes their authex registration.
   name account = resolve_account_from_op_address(oa.op_address);
   if (account == name{}) return;

   using AT = opp::attestations::OperatorAction;
   // Dispatch against the PROVEN source chain (equal to `oa.chain_code`, enforced by the WSA-005
   // binding check above), never the payload's own copy. TokenAmount + ChainAddress get split into
   // (chain_code, token_code, amount) / (kind, address) on the inline-action tuples per the
   // no-proto-messages-in-actions rule.
   const sysio::slug_name chain_code_slug{chain_code};
   const sysio::slug_name token_code{oa.amount.token_code};
   const uint64_t raw_amount =
      static_cast<uint64_t>(static_cast<int64_t>(oa.amount.amount));
   switch (oa.action_type) {
      case AT::ACTION_TYPE_DEPOSIT_REQUEST: {
         // opreg::depositinle checks require_auth(get_self()=opreg). msgch
         // must therefore declare opreg's own permission on the inline action.
         // For the chain's inline-send auth check to accept this declaration,
         // opreg.active must trust msgch@sysio.code — wired at cluster
         // bootstrap via `updateauth` (see wire-tools-ts ClusterManager
         // alongside the analogous sysio↔authex grant). The test fixture
         // sets up the same delegation in `sysio.dispatch_tests.cpp`.
         action(
            permission_level{OPREG_ACCOUNT, "active"_n},
            OPREG_ACCOUNT, "depositinle"_n,
            std::make_tuple(account, chain_code_slug, token_code, raw_amount,
                            oa.op_address.kind, oa.op_address.address,
                            original_message_id)
         ).send();
         break;
      }
      case AT::ACTION_TYPE_WITHDRAW_REQUEST: {
         // Same delegation requirement as DEPOSIT_REQUEST — opreg.active must
         // trust msgch@sysio.code at the cluster level.
         action(
            permission_level{OPREG_ACCOUNT, "active"_n},
            OPREG_ACCOUNT, "withdrawinle"_n,
            std::make_tuple(account, chain_code_slug, token_code, raw_amount)
         ).send();
         break;
      }
      case AT::ACTION_TYPE_WITHDRAW_REMIT:  // outbound-only — never expected inbound
      case AT::ACTION_TYPE_SLASH:           // depot-internal; never accepted inbound
      case AT::ACTION_TYPE_UNKNOWN:
      default:
         break;
   }
}

/// Dispatch an UNDERWRITE_INTENT_COMMIT to sysio.uwrit::rcrdcommit.
///
/// The full UIC bytes are forwarded verbatim so the depot can reconstruct
/// the digest and verify the underwriter's signature at race resolution
/// time. We decode here to extract the routing scalars (uwreq id,
/// uw_account, chain_code, token_code, reserve_code — the latter triple
/// disambiguates same-chain swap legs and points at the precise reserve
/// covering this leg); the authoritative copy for verification is the
/// bytes themselves, stored on `commit_entry.{source,dest}_uic_bytes`.
///
/// Post v6: identity scalars on UIC are codenames (uint64). `chain_code` is the proven source
/// outpost from `deliver`; `uic.chain_code` is the leg this commit covers. WSA-005 requires the two
/// to be identical — each leg's underwrite commit is emitted on, and relayed by, that leg's own
/// outpost (a source-leg UIC rides the source outpost's envelope, a dest-leg UIC the dest outpost's;
/// see `sysio.dispatch_tests`). `rcrdcommit` routes the commit to its src/dst slot by the leg chain
/// code, so a forged `uic.chain_code` could misroute the commit onto the wrong leg; we bind it to the
/// proven outpost and drop on divergence.
void dispatch_underwrite_commit(name self, const std::vector<char>& data, uint64_t chain_code) {
   opp::attestations::UnderwriteIntentCommit uic;
   {
      auto in = zpp::bits::in{std::span{data.data(), data.size()}, zpp::bits::no_size{}};
      auto rc = in(uic);
      if (rc != zpp::bits::errc{}) return;
   }
   if (uic.uw_account.name.empty()) return;

   // WSA-005: the leg's chain (uic.chain_code) must be the proven delivering outpost before the
   // commit is recorded against a swap leg.
   if (!source_chain_binding_ok(chain_code, uic.chain_code, "dispatch_underwrite_commit")) return;

   // Route with the proven `chain_code` (equal to `uic.chain_code`, enforced above) so the leg slot
   // is keyed off provenance, not the payload's self-asserted chain.
   action(
      permission_level{self, "active"_n},
      UWRIT_ACCOUNT, "rcrdcommit"_n,
      std::make_tuple(uic.uw_request_id, name{uic.uw_account.name}, chain_code,
                      sysio::slug_name{chain_code},
                      sysio::slug_name{uic.token_code},
                      sysio::slug_name{uic.reserve_code},
                      data)
   ).send();
}

/// Dispatch a RESERVE_CREATE attestation to sysio.reserv::oncrtreserve.
/// Inserts a PENDING reserve row on the depot. Per
/// `feedback_opp_handlers_never_throw`, decode failures silently no-op.
/// The downstream `oncrtreserve` is itself a never-throw handler — duplicate
/// reserves are logged + dropped on the depot side.
void dispatch_reserve_create(name self, const std::vector<char>& data, uint64_t chain_code) {
   opp::attestations::ReserveCreate rc;
   {
      auto in = zpp::bits::in{std::span{data.data(), data.size()}, zpp::bits::no_size{}};
      auto err = in(rc);
      if (err != zpp::bits::errc{}) return;
   }

   // Reserve identity + the custodied amount travel together in
   // `external_amount` (a `ReserveAmount`): the amount is ALREADY in the
   // depot's canonical 9-decimal frame (the outpost converts chain-native
   // units at the boundary, exactly like the swap paths). A negative
   // TokenAmount clamps to 0 so `oncrtreserve`'s zero-amount guard cancels
   // the request back (refunding the creator) instead of wrapping.
   const auto&    ext        = rc.external_amount;

   // WSA-005: `external_amount.chain_code` is documented in the proto as "the outpost's own chain".
   // Bind it to the proven delivering outpost so an envelope proven from outpost A cannot register a
   // reserve whose external custody is claimed against a different chain B.
   if (!source_chain_binding_ok(chain_code, ext.chain_code, "dispatch_reserve_create")) return;

   const uint64_t ext_amount = ext.amount.amount > 0
                                  ? static_cast<uint64_t>(ext.amount.amount)
                                  : 0;

   action(
      permission_level{self, "active"_n},
      RESERV_ACCOUNT, "oncrtreserve"_n,
      std::make_tuple(sysio::slug_name{ext.chain_code},
                      sysio::slug_name{ext.amount.token_code},
                      sysio::slug_name{ext.reserve_code},
                      rc.name,
                      rc.description,
                      ext_amount,
                      rc.requested_wire_amount,
                      rc.connector_weight_bps,
                      rc.creator_addr.kind,
                      rc.creator_addr.address,
                      rc.is_private,
                      rc.creator_pub_key)
   ).send();
}

/// Dispatch a RESERVE_CREATE_CANCEL attestation to sysio.reserv::oncnclrsv.
/// The depot decides whether the creator won or lost the race against any
/// `matchreserve` call — see `oncnclrsv`. Per
/// `feedback_opp_handlers_never_throw`, decode failures silently no-op
/// and downstream race-loss is also a silent no-op on the reserv side.
void dispatch_reserve_create_cancel(name self, const std::vector<char>& data, uint64_t chain_code) {
   opp::attestations::ReserveCreateCancel cancel;
   {
      auto in = zpp::bits::in{std::span{data.data(), data.size()}, zpp::bits::no_size{}};
      auto err = in(cancel);
      if (err != zpp::bits::errc{}) return;
   }

   // WSA-005: the cancel targets the reserve on `cancel.chain_code`; bind it to the proven
   // delivering outpost so an envelope proven from outpost A cannot cancel a reserve on chain B.
   if (!source_chain_binding_ok(chain_code, cancel.chain_code, "dispatch_reserve_create_cancel")) return;

   action(
      permission_level{self, "active"_n},
      RESERV_ACCOUNT, "oncnclrsv"_n,
      std::make_tuple(sysio::slug_name{cancel.chain_code},
                      sysio::slug_name{cancel.token_code},
                      sysio::slug_name{cancel.reserve_code},
                      cancel.creator_addr.kind,
                      cancel.creator_addr.address)
   ).send();
}

/// Validate and parse `s` (from `WireAccount.name`) into a sysio::name. The name string
/// constructor aborts on an out-of-charset character, so we pre-validate here to keep the depot
/// dispatch non-throwing. Returns nullopt for an empty, over-long (>12), or out-of-charset name.
/// The tier-specific length rule (tier-1 = 2-6, tier 2/3 = 1-12) is enforced later by nodeownreg;
/// here we only guarantee the name is constructible and within the node-owner budget.
std::optional<name> parse_owner_name(const std::string& s) {
   if (s.empty() || s.size() > 12) return std::nullopt;
   for (char c : s) {
      // sysio base32 alphabet: '.', '1'-'5', 'a'-'z'. Anything else aborts in name's ctor.
      const bool ok = (c == '.') || (c >= '1' && c <= '5') || (c >= 'a' && c <= 'z');
      if (!ok) return std::nullopt;
   }
   return name{std::string_view{s}};
}

/// Build a sysio::public_key from a WireKeyType + the raw key bytes carried in
/// `ChainAddress.address` (NodeOwnerRegistration.address) -- the bytes have NO variant-index prefix;
/// `key_type` is the discriminant. Returns nullopt on a length mismatch or an unsupported key type
/// so the caller can soft-drop. K1/R1/EM are the 33-byte secp256k1 point (variant 0/1/3), ED is the
/// 32-byte ed25519 point (variant 4). WA (variable-length WebAuthn -- can't be length-validated
/// without risking a datastream abort) and BLS (a finalizer/consensus key, never an account
/// owner/active authority) are not valid node-owner account keys and are rejected.
std::optional<sysio::public_key> public_key_from_wire_key(WireKeyType kt, const std::vector<char>& b) {
   // Validate + copy the 33-byte compressed secp256k1 point shared by K1/R1/EM.
   auto as_ecc = [&]() -> std::optional<sysio::ecc_public_key> {
      if (b.size() != 33) return std::nullopt;
      sysio::ecc_public_key arr;
      std::copy(b.begin(), b.end(), arr.begin());
      return arr;
   };
   sysio::public_key pk;
   switch (kt) {
      case WireKeyType::WIRE_KEY_TYPE_K1: { auto a = as_ecc(); if (!a) return std::nullopt; pk.emplace<0>(*a); return pk; }
      case WireKeyType::WIRE_KEY_TYPE_R1: { auto a = as_ecc(); if (!a) return std::nullopt; pk.emplace<1>(*a); return pk; }
      case WireKeyType::WIRE_KEY_TYPE_EM: { auto a = as_ecc(); if (!a) return std::nullopt; pk.emplace<3>(*a); return pk; }
      case WireKeyType::WIRE_KEY_TYPE_ED: {
         if (b.size() != 32) return std::nullopt;
         sysio::ed_public_key arr;
         std::copy(b.begin(), b.end(), reinterpret_cast<char*>(arr.data()));
         pk.emplace<4>(arr);
         return pk;
      }
      default:  // WIRE_KEY_TYPE_UNKNOWN / WA / BLS
         return std::nullopt;
   }
}

/// Build an EM (secp256k1) public_key from a depositor's Ethereum public key carried as raw bytes:
/// either the 33-byte compressed point (0x02/0x03 prefix) or the 65-byte uncompressed point (0x04
/// prefix + X + Y). Returns nullopt on any other shape so the caller soft-drops. authex stores EM as
/// the 33-byte compressed form, so an uncompressed input is compressed here (prefix carries Y parity).
std::optional<sysio::public_key> em_pubkey_from_eth_bytes(const std::vector<char>& b) {
   sysio::ecc_public_key compressed{};  // std::array, 33 bytes
   if (b.size() == 33 && (static_cast<uint8_t>(b[0]) == 0x02 || static_cast<uint8_t>(b[0]) == 0x03)) {
      std::copy(b.begin(), b.end(), compressed.begin());
   } else if (b.size() == 65 && static_cast<uint8_t>(b[0]) == 0x04) {
      // Compressed prefix = 0x02 if Y is even, 0x03 if odd; Y is bytes [33,65), parity is its LSB.
      compressed[0] = static_cast<char>(0x02 | (static_cast<uint8_t>(b[64]) & 1));
      std::copy(b.begin() + 1, b.begin() + 33, compressed.begin() + 1);  // X coordinate
   } else {
      return std::nullopt;
   }
   sysio::public_key pk;
   pk.emplace<3>(compressed);  // variant index 3 = EM
   return pk;
}

/// Decode an inbound NodeOwnerRegistration attestation and drive the NFT node-owner claim: create
/// the vanity-named Wire account from the claim's wire_pub_key, then register the owner and record
/// the depositor's ETH link. Both steps are inline-sent to sysio.roa declaring {sysio.roa, active}
/// (accepted via the msgch@sysio.code delegation on sysio.roa.active), newnameduser first so its
/// newaccount runs depth-first and the account exists before nodeownreg executes.
///
/// Trust-OPP: a malformed envelope (undecodable proto, unparseable name, unusable key bytes,
/// out-of-range tier) is silently dropped here -- nothing is sent. A well-formed envelope whose
/// *claim* is bad (name wrong length for tier, account held by a different key, already registered)
/// is soft-failed inside nodeownreg, which records a REJECTED audit row. Neither path aborts the
/// dispatching transaction.
///
/// WSA-005 source binding: this path is value-bearing (it creates a Wire account, registers a node
/// owner, and records an ETH authex link, with the consensus-reached attestation as the sole proof).
/// `NodeOwnerRegistration` carries no chain code of its own -- the claim is an ERC1155 NFT deposit
/// identified only by `ChainAddress` (VM-family) fields -- so, unlike the other value-bearing payloads,
/// it cannot self-describe its source. But node-owner NFT deposits originate on exactly one outpost:
/// the Ethereum mainnet chain (`NODE_OWNER_SRC_CHAIN`). So bind to that outpost directly -- accept only
/// when the proven source `chain_code` IS the Ethereum outpost, and drop a registration relayed by any
/// other outpost. That covers both a non-EVM (SVM/WIRE) quorum AND a second, unrelated EVM outpost
/// (Polygon/Base/Arbitrum/...): a `CHAIN_KIND_EVM` family check would let those through, but they are
/// not the chain the NFT was deposited on. This is the exact-outpost binding the other paths get from
/// `source_chain_binding_ok`; the constant stands in for the chain code the claim does not carry.
/// (If multiple Ethereum-family outposts must each source node-owner registrations, the source has to
/// become a chain-code field on the proto -- a coordinated outpost change, tracked as a follow-up.)
void dispatch_node_owner_reg(const std::vector<char>& data, uint64_t chain_code) {
   if (!source_chain_binding_ok(chain_code, NODE_OWNER_SRC_CHAIN.value, "dispatch_node_owner_reg"))
      return;

   opp::attestations::NodeOwnerRegistration reg;
   {
      auto in = zpp::bits::in{std::span{data.data(), data.size()}, zpp::bits::no_size{}};
      if (in(reg) != zpp::bits::errc{}) return;  // malformed proto; drop
   }

   const uint32_t tier_raw = reg.tier;
   if (tier_raw < 1 || tier_raw > 3) return;      // unusable tier; drop
   const uint8_t tier = static_cast<uint8_t>(tier_raw);

   auto owner = parse_owner_name(reg.account.name);
   if (!owner) return;                            // unparseable account name; drop

   auto wire_pk = public_key_from_wire_key(reg.wire_pub_key.key_type, reg.wire_pub_key.key);
   if (!wire_pk) return;                          // unusable owner/active key; drop

   auto eth_pk = em_pubkey_from_eth_bytes(reg.actor_pub_key);
   if (!eth_pk) return;                           // unusable depositor ETH key; drop

   // 1) Create the account (idempotent; soft-skips a name that breaks the tier rule).
   action(permission_level{ROA_ACCOUNT, "active"_n}, ROA_ACCOUNT, "newnameduser"_n,
          std::make_tuple(*owner, *wire_pk, tier)).send();

   // 2) Register the owner + record the ETH link, with claim-payload soft-fail + audit recording.
   action(permission_level{ROA_ACCOUNT, "active"_n}, ROA_ACCOUNT, "nodeownreg"_n,
          std::make_tuple(*owner, tier, *eth_pk, *wire_pk)).send();
}

/// Per-attestation dispatch entry. Called from the inbound extraction loop
/// in `evalcons` after a consensus envelope has been unpacked. Dispatch is
/// best-effort — silently no-ops on unknown / out-of-scope types so the
/// inbound stream can keep flowing even when the depot hasn't yet wired up
/// every handler (e.g. RESERVE_BALANCE_SHEET / STAKING_REWARD route to
/// sysio.reserve, which lands in Task 5).
void dispatch_attestation(name self, uint64_t attestation_id,
                          AttestationType type,
                          const std::vector<char>& data,
                          uint64_t chain_code,
                          const checksum256& original_message_id) {
   switch (type) {
      case AttestationType::ATTESTATION_TYPE_OPERATOR_ACTION:
         dispatch_operator_action(self, data, chain_code, original_message_id);
         break;

      case AttestationType::ATTESTATION_TYPE_SWAP_REQUEST:
         action(
            permission_level{self, "active"_n},
            UWRIT_ACCOUNT, "createuwreq"_n,
            std::make_tuple(attestation_id, type, chain_code, data)
         ).send();
         break;

      case AttestationType::ATTESTATION_TYPE_UNDERWRITE_INTENT_COMMIT:
         dispatch_underwrite_commit(self, data, chain_code);
         break;

      case AttestationType::ATTESTATION_TYPE_SWAP_REMIT:
         // Depot → outpost outbound-only. No outpost ever echoes a
         // SwapRemit back (verified: ETH `_handleSwapRemit` and SOL
         // `handle_swap_remit` only pay the recipient + emit local
         // events; failure is SWAP_REJECTED). The old "reflected remit
         // = delivery ack → uwrit::release" dispatch here was dead code:
         // success is implicit absent SWAP_REJECTED, and underwriter
         // locks are released exclusively by the wall-clock challenge
         // window sweep (`sysio.uwrit::chklocks` at epoch advance). A
         // misbehaving outpost relaying one inbound is a benign no-op.
         break;

      // ATTESTATION_TYPE_SWAP_REJECTED was dispatched here → sysio.reserv::onreject.
      // REMOVED: outposts no longer echo a rejection. Every REMIT is depot-initiated
      // against a verified reserve ledger, so the destination outpost can always pay
      // (or log+skips locally on a misconfig) — there is no post-underwriting rejection
      // and no reserve-ledger reconciliation. The retired type (enum slot 60957) no
      // longer exists; any stray inbound falls through to the default drop below.

      case AttestationType::ATTESTATION_TYPE_STAKING_REWARD:
         // Per-staker staking reward -> sysio.dclaim claim ledger. The v6
         // staking-reward path does not deposit back to a reserve (the
         // external-pool credit and native -> WIRE conversion are
         // outpost-side), so the pre-v6 reserv::onreward leg is dropped and
         // reward_amount.amount is forwarded as the WIRE-denominated credit.
         {
            opp::attestations::StakingReward sr;
            auto in = zpp::bits::in{std::span{data.data(), data.size()}, zpp::bits::no_size{}};
            auto rc = in(sr);
            if (rc != zpp::bits::errc{}) break;
            // WSA-005: the reward-emitting chain (sr.chain_code) must be the proven delivering
            // outpost before any claim credit is recorded.
            if (!source_chain_binding_ok(chain_code, sr.chain_code, "staking_reward")) break;
            const uint64_t reward_raw =
               static_cast<uint64_t>(static_cast<int64_t>(sr.reward_amount.amount));
            // staker_wire_account.name is the raw account string (empty =>
            // staker not yet AuthX-linked, so sysio.dclaim parks by native
            // address). staker_native_address carries the chain (kind) and
            // the raw address bytes. Credit against the PROVEN chain_code
            // (equal to sr.chain_code, enforced above), never the payload's copy.
            action(
               permission_level{self, "active"_n},
               "sysio.dclaim"_n, "onreward"_n,
               std::make_tuple(chain_code,
                               sr.staker_wire_account.name,
                               sr.staker_native_address.kind,
                               sr.staker_native_address.address,
                               reward_raw,
                               sr.reward_epoch_index,
                               sr.external_epoch_ref,
                               sr.share_bps)
            ).send();
         }
         break;

      case AttestationType::ATTESTATION_TYPE_RESERVE_CREATE:
         // Outpost-initiated reserve creation. Insert a PENDING row on
         // `sysio.reserv` awaiting a depot-side `matchreserve` call. The
         // creator's outpost-side custody is locked on the originating
         // outpost; refund (on RESERVE_CREATE_CANCELLED) targets
         // `creator_addr`.
         dispatch_reserve_create(self, data, chain_code);
         break;

      case AttestationType::ATTESTATION_TYPE_RESERVE_CREATE_CANCEL:
         // Creator cancellation of a still-PENDING reserve. If the race
         // against `matchreserve` is lost the reserv contract no-ops; if
         // won it flips status to CANCELLED + queues a RESERVE_CREATE_CANCELLED
         // back to the originating outpost so the local custody is released.
         dispatch_reserve_create_cancel(self, data, chain_code);
         break;

      case AttestationType::ATTESTATION_TYPE_RESERVE_CREATE_CANCELLED:
      case AttestationType::ATTESTATION_TYPE_RESERVE_READY:
         // Depot → outpost outbound-only. Should never appear inbound at
         // the depot; if a misbehaving outpost relays one back it is a
         // benign no-op. Silently drop per `feedback_opp_handlers_never_throw`.
         break;

      case AttestationType::ATTESTATION_TYPE_RESERVE_BALANCE_SHEET:
         // Per-epoch sanity check from the outpost. The depot is the
         // ground truth; this is informational. Decode and emit a
         // diagnostic event but do not auto-mutate the reserve — drift
         // detection / alerting belongs to off-chain monitors that
         // tail the chain log. Falling through silently is also
         // acceptable today; the row is persisted in `attestations`
         // for post-hoc inspection.
         break;

      case AttestationType::ATTESTATION_TYPE_CHALLENGE_REQUEST:
      case AttestationType::ATTESTATION_TYPE_CHALLENGE_RESPONSE:
         // No depot-side handler. Envelope disputes are resolved on the WIRE side by evalcons
         // opening a sysio.chalg dispute vote, not by inbound challenge attestations.
         break;

      case AttestationType::ATTESTATION_TYPE_STAKE:
      case AttestationType::ATTESTATION_TYPE_UNSTAKE:
      case AttestationType::ATTESTATION_TYPE_STAKE_UPDATE:
      case AttestationType::ATTESTATION_TYPE_STAKE_RESULT:
         // Validator-staking lifecycle; depot-side handlers land in a later
         // task alongside liqEth / liqsol-token wiring.
         break;

      // Outbound-only types (depot emits these, never receives them inbound)
      // and deprecated pre-launch types are dropped silently. SLASH was
      // formerly its own attestation type; it now rides on OPERATOR_ACTION
      // with action_type=SLASH and is gated inside `dispatch_operator_action`.
      case AttestationType::ATTESTATION_TYPE_SWAP_REVERT:
      case AttestationType::ATTESTATION_TYPE_DEPOSIT_REVERT:
      case AttestationType::ATTESTATION_TYPE_OPERATORS:
      case AttestationType::ATTESTATION_TYPE_BATCH_OPERATOR_GROUPS:
      case AttestationType::ATTESTATION_TYPE_NODE_OWNER_REG:
         // NFT node-owner claim: create the account + register + record the ETH link. Self-contained
         // (decodes NodeOwnerRegistration, inline-sends sysio.roa); soft-drops a malformed envelope.
         dispatch_node_owner_reg(data, chain_code);
         break;

      case AttestationType::ATTESTATION_TYPE_PRETOKEN_PURCHASE:
      case AttestationType::ATTESTATION_TYPE_PRETOKEN_YIELD:
      case AttestationType::ATTESTATION_TYPE_WIRE_TOKEN_PURCHASE:
      case AttestationType::ATTESTATION_TYPE_ATTESTATION_PROCESSING_ERROR:
      case AttestationType::ATTESTATION_TYPE_UNSPECIFIED:
      default:
         break;
   }
}

/// Apply a consensus-reached inbound envelope for one (outpost, epoch): decode it, store and
/// dispatch its attestations, write the audit-log row, drain the working envelope rows, and record
/// per-outpost consensus with the winning checksum. Shared by `evalcons` (majority/unanimous path)
/// and `resolvedisp` (dispute-resolution path) so both routes process the winner identically.
///
/// `raw` is the winning envelope's bytes; `winning_checksum` is its sha256 — recorded on `outpcons`
/// so `sysio.epoch::advance` can classify each operator's delivery as canonical or slashable.
void apply_consensus(name self, uint64_t chain_code, uint32_t epoch_index,
                     const std::vector<char>& raw, const checksum256& winning_checksum) {
   const uint128_t composite = opp::outpost_epoch_key(chain_code, epoch_index);

   // Idempotency guard, keyed off the DURABLE per-outpost consensus row. `outpcons.epoch_index` is
   // written only here (apply_consensus) and never cleared, so `epoch_index == this epoch` reliably
   // means the winning envelope for this (outpost, epoch) was already decoded, dispatched, and logged.
   // chkcons keys its advance gate off this same signal.
   //
   // The guard matters because the consensus cleanup below clears `raw_data` from every delivery row.
   // Without it, a post-quorum re-fire of evalcons would re-group the now-empty winning row and abort
   // here on an empty-envelope decode -- turning a benign late delivery into a hard failure and, because
   // the whole deliver tx reverts, dropping the late operator's own envelope row that epoch::advance
   // needs to classify/slash. This no-op returns before any cleanup, so that row persists.
   {
      msgch::outpost_consensus_t opcons(self);
      auto opc_pk = msgch::outpost_consensus_key{chain_code};
      if (opcons.contains(opc_pk)) {
         auto opc = opcons.get(opc_pk);
         if (opc.epoch_index == epoch_index) {
            sysio::print_f("apply_consensus: chain_code=%llu epoch=%u already dispatched, "
                           "treating as benign no-op\n",
                           static_cast<unsigned long long>(chain_code), epoch_index);
            return;
         }
      }
   }

   const auto now     = current_time_point();
   const auto now_sec = static_cast<uint64_t>(now.sec_since_epoch());

   // Decode the protobuf Envelope (raw protobuf, no size prefix).
   opp::Envelope envelope;
   auto in = zpp::bits::in{std::span{raw.data(), raw.size()}, zpp::bits::no_size{}};
   auto decode_result = in(envelope);
   check(decode_result == zpp::bits::errc{}, "failed to decode inbound OPP Envelope");

   // The `messages` row is intentionally not written here. The raw envelope bytes have already served
   // their consensus purpose at this point: attestations are extracted + dispatched below, the durable
   // trail lives in the metadata-only `envelope_log`, and idempotency keys off the durable `outpcons`
   // row (see the guard above) -- never off a `messages` row. Emplacing a `message_entry` whose
   // `raw_payload` mirrors `raw` (up to MAX_ENVELOPE_BYTES) only to erase it again at the end of this
   // same action would burn KV serialisation, undo-log entries, and a peak-RAM blip on the payer for
   // zero net storage. The `messages` / `message_entry` schema is left in place; if a downstream
   // consumer of inbound message rows ever materialises, the emplace can be reintroduced (and the
   // matching erase left out).

   // Extract + dispatch each attestation in every message of the envelope. The proven source
   // outpost is `chain_code` (validated by `deliver`); the per-attestation dispatch binds every
   // value-bearing payload to it (WSA-005) -- by exact chain_code via `source_chain_binding_ok`
   // where the payload carries one, and (for NodeOwnerRegistration, which carries no chain code)
   // against the fixed Ethereum source outpost `NODE_OWNER_SRC_CHAIN` in `dispatch_node_owner_reg`.
   msgch::attestations_t atts(self);
   for (auto& msg : envelope.messages) {
      for (auto& entry : msg.payload.attestations) {
         uint64_t att_id = mint_att_id(self);
         atts.emplace(ram_payer, msgch::id_key{att_id}, msgch::attestation_entry{
            .id                  = att_id,
            .chain_code          = chain_code,
            .epoch_index         = epoch_index,
            .type                = entry.type,
            .status              = AttestationStatus::ATTESTATION_STATUS_PROCESSED,
            .data                = entry.data,
            .pending_timestamp   = 0,
            .ready_timestamp     = now_sec,
            .processed_timestamp = now_sec,
         });
         // Reconstruct the OPP message_id as a checksum256 for downstream correlation.
         checksum256 m_id;
         {
            auto& mid = msg.header.message_id;
            std::array<uint8_t, 32> rawid{};
            const size_t n = std::min<size_t>(mid.size(), 32);
            for (size_t i = 0; i < n; ++i) rawid[i] = static_cast<uint8_t>(mid[i]);
            m_id = checksum256{rawid};
         }
         dispatch_attestation(self, att_id, entry.type, entry.data,
                              chain_code, m_id);
      }
   }

   // Audit log + inline cleanup of working state.
   {
      const auto op_row = [&]() {
         sysio::chains::chains_t chains_tbl(CHAINS_ACCOUNT);
         return chains_tbl.get(sysio::chains::chain_key{sysio::slug_name{chain_code}});
      }();

      sysio::opp::Endpoints endpoints;
      endpoints.start.kind = op_row.kind;
      endpoints.start.id   = op_row.external_chain_id;
      endpoints.end.kind   = ChainKind::CHAIN_KIND_WIRE;
      endpoints.end.id     = WIRE_CHAIN_ID;
      write_envelope_log(self, endpoints, epoch_index, winning_checksum);

      // Drop heavy raw_data from each per-batch-op envelope row but KEEP the metadata tuple so
      // sysio.epoch::advance can still read per-op checksums + delivery for slash classification.
      msgch::envelopes_t envs(self);
      std::vector<uint64_t> ids_to_clear;
      auto modify_idx = envs.get_index<"byoutepoch"_n>();
      for (auto it = modify_idx.lower_bound(composite);
           it != modify_idx.end() && it->by_outpost_epoch() == composite; ++it) {
         ids_to_clear.push_back(it->id);
      }
      for (auto id : ids_to_clear) {
         envs.modify(same_payer, msgch::id_key{id}, [](auto& r) {
            r.raw_data.clear();
            r.raw_data.shrink_to_fit();
         });
      }
   }

   // Record per-outpost consensus + the winning checksum.
   msgch::outpost_consensus_t opcons(self);
   auto opc_pk = msgch::outpost_consensus_key{chain_code};
   if (!opcons.contains(opc_pk)) {
      opcons.emplace(ram_payer, opc_pk, msgch::outpost_consensus_entry{
         .chain_code        = chain_code,
         .epoch_index       = epoch_index,
         .consensus_reached = true,
         .winning_checksum  = winning_checksum,
      });
   } else {
      opcons.modify(same_payer, opc_pk, [&](auto& r) {
         r.epoch_index       = epoch_index;
         r.consensus_reached = true;
         r.winning_checksum  = winning_checksum;
      });
   }
}

/// Evaluate the dispute trigger and, if met, open a Tier-1 dispute vote via sysio.chalg. Trigger:
/// the epoch boundary has passed, 3+ distinct envelope versions exist, and no version holds a
/// majority of the operator group. A majority — even within a 3+-way split — resolves without a
/// vote, so it is not a trigger; a sub-3-way or pre-boundary split just waits for more deliveries.
void maybe_open_dispute(name self, uint64_t chain_code, uint32_t epoch_index,
                        uint32_t operators_per_group,
                        const std::vector<checksum256>& seen_checksums,
                        const std::vector<uint32_t>& checksum_counts,
                        const std::vector<std::vector<name>>& checksum_operators) {
   // OPP silent-return diagnostics: each branch below silently declines to open a
   // dispute. Logged (visible under --contracts-console) so "the dispute never
   // opened" is greppable instead of a black hole.
   if (seen_checksums.size() < 3) {
      sysio::print_f("msgch::maybe_open_dispute: no dispute for (chain=%llu, epoch=%u): %u distinct version(s), a vote needs >=3\n",
                     chain_code, epoch_index, (uint32_t)seen_checksums.size());
      return;
   }

   epoch::epochstate_t state_tbl(EPOCH_ACCOUNT);
   if (!state_tbl.exists() || current_time_point() < state_tbl.get().next_epoch_start) {
      sysio::print_f("msgch::maybe_open_dispute: no dispute for (chain=%llu, epoch=%u): epoch boundary not yet passed\n",
                     chain_code, epoch_index);
      return;
   }

   uint32_t max_count = 0;
   for (auto c : checksum_counts) {
      if (c > max_count) max_count = c;
   }
   if (max_count > operators_per_group / 2) {  // a majority exists -> no vote needed
      sysio::print_f("msgch::maybe_open_dispute: no dispute for (chain=%llu, epoch=%u): a version holds a majority (%u of group %u), resolved without a vote\n",
                     chain_code, epoch_index, max_count, operators_per_group);
      return;
   }

   std::vector<chalg::dispute_candidate> candidates;
   candidates.reserve(seen_checksums.size());
   for (size_t g = 0; g < seen_checksums.size(); ++g) {
      candidates.push_back(chalg::dispute_candidate{
         .checksum  = seen_checksums[g],
         .operators = checksum_operators[g],
      });
   }

   action(
      permission_level{self, "active"_n},
      CHALG_ACCOUNT,
      "opendispute"_n,
      std::make_tuple(chain_code, epoch_index, candidates)
   ).send();
}

} // anonymous namespace

// ---------------------------------------------------------------------------
//  bootstrap — trigger first advance at epoch 0
// ---------------------------------------------------------------------------
void msgch::bootstrap() {
   require_auth(get_self());
   uint32_t epoch = current_epoch_index();
   check(epoch == 0, "bootstrap can only be called at epoch 0");
   action(
      permission_level{get_self(), "active"_n},
      EPOCH_ACCOUNT,
      "advance"_n,
      std::make_tuple()
   ).send();
}

// ---------------------------------------------------------------------------
//  deliver — batch operator delivers inbound OPP data for a specific outpost
// ---------------------------------------------------------------------------
void msgch::deliver(name batch_op_name, uint64_t chain_code, std::vector<char> data) {
   is_batch_operator_active(batch_op_name);
   check(!data.empty(), "delivery data cannot be empty");

   // Verify outpost exists on the new `sysio.chains::chains` table.
   // `chain_code` is the originating chain's slug_name value (uint64) per
   // the v6 data-model refactor — the chain row's PK is `code.value`.
   // Reject deliveries from the depot self-row (`is_depot==true`) and
   // from inactive chains; both are protocol invariants.
   sysio::chains::chains_t chains_tbl(CHAINS_ACCOUNT);
   auto chain_pk = sysio::chains::chain_key{sysio::slug_name{chain_code}};
   check(chains_tbl.contains(chain_pk), "outpost not found in sysio.chains");
   auto op_row = chains_tbl.get(chain_pk);
   check(!op_row.is_depot, "deliver: chain_code refers to the depot self-row");
   check(op_row.active, "deliver: outpost is not active");

   // Decode envelope to validate epoch_index matches current WIRE epoch
   uint32_t epoch = current_epoch_index();
   {
      opp::Envelope env_check;
      auto in = zpp::bits::in{std::span{data.data(), data.size()}, zpp::bits::no_size{}};
      auto result = in(env_check);
      check(result == zpp::bits::errc{}, "failed to decode inbound envelope");
      uint32_t env_epoch = static_cast<uint32_t>(env_check.epoch_index);
      // Lambda overload: the formatted message (triple std::to_string concat) is built only on the
      // failure path, not on every successful delivery.
      check(env_epoch == epoch, [&] {
         return "envelope epoch_index mismatch: envelope=" + std::to_string(env_epoch) +
                " current=" + std::to_string(epoch);
      });
   }

   // Compute checksum trustlessly inside the contract
   checksum256 cs = sha256(data.data(), data.size());

   // Reject duplicate delivery from the same operator for the same outpost+epoch.
   // A revert (check) is deliberate, NOT a soft print-and-return: a reverted
   // transaction is never recorded in a block and bills no CPU/NET, whereas the
   // previous soft-return shape landed every duplicate as a recorded, billed
   // no-op. Distinct operators delivering matching content are NOT duplicates --
   // each inserts its own row below, which is exactly what evalcons counts
   // toward the per-checksum consensus tally.
   envelopes_t envs(get_self());
   auto oe_idx = envs.get_index<"byoutepoch"_n>();
   uint128_t composite = opp::outpost_epoch_key(chain_code, epoch);
   for (auto it = oe_idx.lower_bound(composite);
        it != oe_idx.end() && it->by_outpost_epoch() == composite; ++it) {
      check(it->batch_op_name != batch_op_name,
            "operator already delivered for this outpost+epoch");
   }

   // Store envelope
   uint64_t env_id = std::max<uint64_t>(1, envs.available_primary_key());

   envs.emplace(ram_payer, id_key{env_id}, envelope_entry{
      .id            = env_id,
      .chain_code    = chain_code,
      .epoch_index   = epoch,
      .batch_op_name = batch_op_name,
      // `chain_kind` is the VM family (ChainKind enum) of the originating
      // chain — preserved on the row so per-batch-op audit consumers don't
      // need a follow-up cross-contract read of `sysio.chains` to know
      // whether this was an EVM/SVM/WIRE delivery. The chain row's `kind`
      // is authoritative; this is just the cached projection.
      .chain_kind    = op_row.kind,
      .checksum      = cs,
      .raw_data      = data,
      .received_at   = current_time_point(),
   });

   // Evaluate consensus inline
   action(
      permission_level{get_self(), "active"_n},
      get_self(),
      "evalcons"_n,
      std::make_tuple(chain_code, epoch)
   ).send();
}

// ---------------------------------------------------------------------------
//  evalcons — evaluate consensus on inbound envelopes for outpost+epoch
// ---------------------------------------------------------------------------
void msgch::evalcons(uint64_t chain_code, uint32_t epoch_index) {
   require_auth(get_self());

   envelopes_t envs(get_self());
   auto oe_idx = envs.get_index<"byoutepoch"_n>();
   uint128_t composite = opp::outpost_epoch_key(chain_code, epoch_index);

   // If a dispute has been opened for this (outpost, epoch), the dispute-vote flow owns its
   // resolution and winner dispatch (via `resolvedisp`). evalcons is a no-op for this bucket so a
   // late delivery cannot re-open the dispute or re-dispatch the envelope.
   {
      chalg::disputes_t disputes(CHALG_ACCOUNT);
      auto d_idx = disputes.get_index<"byoutepoch"_n>();
      if (d_idx.find(composite) != d_idx.end()) return;
   }

   // Group envelopes by checksum, tracking the operators that delivered each version (CDT-compatible
   // parallel vectors). The per-version operator lists become the dispute candidates on a 3+-way
   // split.
   std::vector<checksum256>       seen_checksums;
   std::vector<uint32_t>          checksum_counts;
   std::vector<std::vector<char>> checksum_data;
   std::vector<std::vector<name>> checksum_operators;
   uint32_t total_deliveries = 0;

   for (auto it = oe_idx.lower_bound(composite);
        it != oe_idx.end() && it->by_outpost_epoch() == composite; ++it) {
      bool found = false;
      for (size_t g = 0; g < seen_checksums.size(); ++g) {
         if (seen_checksums[g] == it->checksum) {
            checksum_counts[g]++;
            checksum_operators[g].push_back(it->batch_op_name);
            found = true;
            break;
         }
      }
      if (!found) {
         seen_checksums.push_back(it->checksum);
         checksum_counts.push_back(1);
         checksum_data.push_back(it->raw_data);
         checksum_operators.push_back(std::vector<name>{it->batch_op_name});
      }
      total_deliveries++;
   }

   uint32_t operators_per_group = epoch_operators_per_group();

   // Consensus check
   bool consensus_reached = false;
   size_t consensus_group = 0;

   for (size_t g = 0; g < seen_checksums.size(); ++g) {
      // Option A: ALL operators delivered identical data
      if (checksum_counts[g] == operators_per_group &&
          total_deliveries == operators_per_group) {
         consensus_reached = true;
         consensus_group = g;
         break;
      }
      // Option B: Majority at epoch boundary (current time >= next_epoch_start)
      if (checksum_counts[g] > operators_per_group / 2) {
         epoch::epochstate_t state_tbl(EPOCH_ACCOUNT);
         if (state_tbl.exists()) {
            auto state = state_tbl.get();
            if (current_time_point() >= state.next_epoch_start) {
               consensus_reached = true;
               consensus_group = g;
               break;
            }
         }
      }
   }

   if (!consensus_reached) {
      // No automatic consensus. On a 3+-way no-majority split past the epoch boundary, open a
      // Tier-1 dispute vote; a smaller or pre-boundary split just waits for more deliveries.
      maybe_open_dispute(get_self(), chain_code, epoch_index, operators_per_group,
                         seen_checksums, checksum_counts, checksum_operators);
      return;
   }

   // Consensus reached: store + dispatch the winning envelope and record the per-outpost winner
   // (so sysio.epoch::advance can classify each delivery). advance itself is triggered by chkcons
   // once next_epoch_start has passed.
   apply_consensus(get_self(), chain_code, epoch_index,
                   checksum_data[consensus_group], seen_checksums[consensus_group]);
}

// ---------------------------------------------------------------------------
//  chkcons — check all-outpost consensus + time gate, trigger advance
// ---------------------------------------------------------------------------
void msgch::chkcons() {
   uint32_t epoch = current_epoch_index();

   // Open-dispute gate: if any OPP dispute for the current epoch is still OPEN, hold advancement.
   // Two overlapping holds protect a disputed epoch: `sysio.epoch::is_paused` (set by opendispute)
   // is the hard stop -- advance() itself throws while paused -- and this gate is the soft one that
   // returns BEFORE chkcons triggers advance, so it never hits that throw mid-dispute. chkdispute
   // clears both on resolution (resolvedisp + unpause).
   {
      chalg::disputes_t disputes(CHALG_ACCOUNT);
      auto ep_idx = disputes.get_index<"byepoch"_n>();
      for (auto it = ep_idx.lower_bound(epoch);
           it != ep_idx.end() && it->by_epoch() == epoch; ++it) {
         if (it->status == opp::types::DisputeStatus::DISPUTE_STATUS_OPEN) return;
      }
   }

   // Check all active outposts have consensus for the current epoch.
   // Outpost set is sourced from `sysio.chains::chains` filtered to
   // active && !is_depot per the v6 data-model refactor; outpost ids
   // in `outpcons` are slug_name values (chain_row::code.value).
   outpost_consensus_t opcons(get_self());
   sysio::chains::chains_t chains_tbl(CHAINS_ACCOUNT);
   bool all_consensus = true;
   uint32_t outpost_count = 0;

   for (auto it = chains_tbl.begin(); it != chains_tbl.end(); ++it) {
      if (!it->active || it->is_depot) continue;
      ++outpost_count;
      auto opc_pk = outpost_consensus_key{it->code.value};
      if (!opcons.contains(opc_pk)) {
         all_consensus = false;
         break;
      }
      auto opc = opcons.get(opc_pk);
      // Readiness is the durable `epoch_index == current epoch` signal that apply_consensus writes on
      // dispatch. We intentionally do NOT also gate on `consensus_reached`: advance() can return without
      // bumping the epoch (the emissions gate in sysio.epoch::advance returns gracefully), and that
      // commit is not rolled back, so a pre-emptive reset of consensus_reached would strand the epoch
      // with nothing to re-arm it -- apply_consensus does not re-fire once the delivery set is complete.
      // A stale row from a prior epoch fails this check; a fresh dispatch for the new epoch overwrites it.
      if (opc.epoch_index != epoch) {
         all_consensus = false;
         break;
      }
   }

   if (outpost_count == 0 || !all_consensus) return;

   // Check wall-clock: next_epoch_start must be in the past
   epoch::epochstate_t estate(EPOCH_ACCOUNT);
   if (!estate.exists()) return;
   auto state = estate.get();
   if (current_time_point() < state.next_epoch_start) return;

   // All conditions met. Trigger advance. Per-outpost consensus is intentionally NOT reset here:
   // advance() can legally return without advancing (the emissions gate), and that graceful return does
   // not roll back a reset done beforehand, which would permanently strand the epoch. Stale consensus is
   // invalidated instead by the `epoch_index == epoch` check above once advance() actually bumps the
   // epoch, and the next epoch's dispatch overwrites the row.
   action(
      permission_level{get_self(), "active"_n},
      EPOCH_ACCOUNT,
      "advance"_n,
      std::make_tuple()
   ).send();
}

// ---------------------------------------------------------------------------
//  resolvedisp — dispatch the winning envelope of a resolved dispute
// ---------------------------------------------------------------------------
void msgch::resolvedisp(uint64_t chain_code, uint32_t epoch_index, checksum256 winning_checksum) {
   require_auth(CHALG_ACCOUNT);

   // Locate the winning envelope's raw bytes among this (outpost, epoch)'s deliveries. The dispute
   // path never cleared raw_data (evalcons returned early before the consensus cleanup), so the
   // bytes are still on file. Copy them out before apply_consensus drains the rows.
   envelopes_t envs(get_self());
   auto oe_idx = envs.get_index<"byoutepoch"_n>();
   uint128_t composite = opp::outpost_epoch_key(chain_code, epoch_index);

   std::vector<char> raw;
   bool found = false;
   for (auto it = oe_idx.lower_bound(composite);
        it != oe_idx.end() && it->by_outpost_epoch() == composite; ++it) {
      if (it->checksum == winning_checksum) {
         raw   = it->raw_data;
         found = true;
         break;
      }
   }
   // Asserted-unreachable in normal operation: the winning checksum is a dispute candidate, every
   // candidate came from a delivered envelope, and the dispute path retains raw_data (above). If it
   // ever fired it reverts the chkdispute crank (the dispute stays OPEN, the epoch stays paused) -- a
   // safe stall, not state corruption.
   check(found, "resolvedisp: winning envelope not found for this outpost+epoch");

   apply_consensus(get_self(), chain_code, epoch_index, raw, winning_checksum);
}

// ---------------------------------------------------------------------------
//  queueout — queue outbound attestation for an outpost
// ---------------------------------------------------------------------------
void msgch::queueout(uint64_t chain_code,
                     opp::types::AttestationType attest_type,
                     std::vector<char> data) {
   // Authorization gate: only the depot's own system contracts may queue an outbound
   // attestation. queueout carries no ABI-level auth, so without this check ANY account could
   // call it directly and inject a forged attestation that buildenv() then packs into the
   // depot's group-signed outbound envelope — a forged SWAP_REMIT / WITHDRAW_REMIT / SLASH that
   // the outpost authenticates by the group signature and executes. The intended callers
   // (sysio.epoch / .opreg / .uwrit / .reserv) each send under their own {self, active} authority;
   // get_self() permits msgch's own inline use and governance.
   check(has_auth(EPOCH_ACCOUNT) || has_auth(OPREG_ACCOUNT) || has_auth(UWRIT_ACCOUNT) ||
         has_auth(RESERV_ACCOUNT) || has_auth(get_self()),
         "queueout: caller not authorized to queue outbound attestations");

   auto now_sec = static_cast<uint64_t>(current_time_point().sec_since_epoch());

   attestations_t atts(get_self());
   uint64_t att_id = mint_att_id(get_self());

   atts.emplace(ram_payer, id_key{att_id}, attestation_entry{
      .id                  = att_id,
      .chain_code          = chain_code,
      .epoch_index         = current_epoch_index(),
      .type                = attest_type,
      .status              = AttestationStatus::ATTESTATION_STATUS_READY,
      .data                = data,
      .pending_timestamp   = 0,
      .ready_timestamp     = now_sec,
      .processed_timestamp = 0,
   });
}

// ---------------------------------------------------------------------------
//  buildenv — build outbound envelope from READY attestations
//
//  Packs as many READY attestations as fit under MAX_ENVELOPE_BYTES into a
//  single outbound envelope; any that don't fit stay in the table with
//  status = READY and ride the next epoch's `buildenv` call. Mirrors the
//  Solana (`emit_outbound_inner`) and Ethereum (`emitOutboundEnvelope`)
//  packing-loop pattern — never drop, never refuse, always emit what fits.
// ---------------------------------------------------------------------------
void msgch::buildenv(uint64_t chain_code) {
   require_auth(EPOCH_ACCOUNT);

   uint32_t epoch = current_epoch_index();
   attestations_t atts(get_self());
   auto now_sec = static_cast<uint64_t>(current_time_point().sec_since_epoch());

   // ── Phase 1: collect candidate READY attestations for this outpost.
   //    Order is the secondary index's natural order, which is stable across
   //    epochs — preserves cross-epoch attestation ordering for the receiving
   //    chain.
   std::vector<opp::AttestationEntry> candidate_entries;
   std::vector<uint64_t>              candidate_ids;

   auto status_idx = atts.get_index<"bystatus"_n>();
   for (auto it = status_idx.lower_bound(
           static_cast<uint64_t>(AttestationStatus::ATTESTATION_STATUS_READY));
        it != status_idx.end() &&
        it->status == AttestationStatus::ATTESTATION_STATUS_READY; ++it) {
      if (it->chain_code != chain_code) continue;

      opp::AttestationEntry entry;
      entry.type = it->type;
      entry.data_size = zpp::bits::vuint32_t{static_cast<uint32_t>(it->data.size())};
      entry.data = it->data;
      candidate_entries.push_back(std::move(entry));
      candidate_ids.push_back(it->id);
   }

   if (candidate_entries.empty()) return;

   // Phase 2: estimator-based initial pick. Walk candidates in order, accumulating a conservative byte
   // estimate; stop once the next one would push the envelope over MAX_ENVELOPE_BYTES. The trim loop
   // below is the source of truth for the size invariant; this estimator just keeps the typical case to
   // a single serialise pass.
   size_t included_count  = 0;
   size_t estimated_bytes = ENVELOPE_BASELINE_BYTES;
   for (const auto& entry : candidate_entries) {
      const size_t entry_bytes = ATTESTATION_OVERHEAD_BYTES + entry.data.size();
      if (estimated_bytes + entry_bytes > MAX_ENVELOPE_BYTES) {
         break;
      }
      estimated_bytes += entry_bytes;
      ++included_count;
   }

   // First-attestation-too-big guard. The estimator picks zero only when the first candidate alone
   // overshoots the cap; the trim loop below would surface the same condition, but aborting upfront
   // avoids building anything in the doomed case. Never expected at protocol level because individual
   // attestations are bounded well below MAX_ENVELOPE_BYTES.
   check(included_count > 0,
         "sysio.msgch::buildenv: a single READY attestation exceeds "
         "MAX_ENVELOPE_BYTES -- cannot pack into an envelope");

   std::vector<opp::AttestationEntry> entries(
      std::make_move_iterator(candidate_entries.begin()),
      std::make_move_iterator(candidate_entries.begin() + included_count));
   std::vector<uint64_t> included_ids(
      candidate_ids.begin(),
      candidate_ids.begin() + included_count);

   // Serialise the OPP Envelope wrapping `src` into raw protobuf wire format (no size prefix). Pulled
   // into a lambda so the trim loop below can re-run it after popping an entry. `src` is copied (not
   // moved) because the trim loop may need to rebuild from a shorter prefix of `entries`.
   auto build_packed = [&](const std::vector<opp::AttestationEntry>& src) {
      opp::MessagePayload payload;
      payload.version = zpp::bits::vuint32_t{1};
      payload.attestations = src;

      opp::MessageHeader header;
      header.timestamp = zpp::bits::vuint64_t{now_sec};

      opp::Message msg;
      msg.header = std::move(header);
      msg.payload = std::move(payload);

      opp::Envelope env;
      env.epoch_index = zpp::bits::vuint32_t{epoch};
      env.epoch_timestamp = zpp::bits::vuint64_t{now_sec};
      env.messages.push_back(std::move(msg));

      std::vector<char> out_buf;
      auto out_stream = zpp::bits::out{out_buf, zpp::bits::no_size{}};
      (void)out_stream(env);
      return out_buf;
   };

   // Phase 3: trim loop. Serialise; if the result overshoots MAX_ENVELOPE_BYTES (the estimator
   // under-counted relative to the actual zpp::bits encoding for this candidate set), pop the last
   // entry and retry. This loop -- not the estimator -- is the sole authority on the size invariant. It
   // replaces the previous post-build hard abort, which left this action in a dead-letter loop on
   // estimator drift: the PROCESSED markings would roll back with the aborted tx, the same set would
   // re-pack against the same estimator next epoch, and the same check would trip again. Popped entries
   // stay READY and ride the next `buildenv` call. PROCESSED marking is deferred until after the loop
   // converges so a popped entry never needs a status revert.
   std::vector<char> packed = build_packed(entries);
   while (packed.size() > MAX_ENVELOPE_BYTES) {
      check(entries.size() > 1,
            "sysio.msgch::buildenv: a single READY attestation exceeds "
            "MAX_ENVELOPE_BYTES -- cannot pack into an envelope");
      entries.pop_back();
      included_ids.pop_back();
      packed = build_packed(entries);
   }

   // Mark the surviving attestations as PROCESSED. Remaining candidates (popped by the trim loop or
   // never picked by the estimator) stay READY for the next epoch's `buildenv` call.
   for (uint64_t aid : included_ids) {
      auto att_pk = id_key{aid};
      if (atts.contains(att_pk)) {
         atts.modify(same_payer, att_pk, [&](auto& a) {
            a.status              = AttestationStatus::ATTESTATION_STATUS_PROCESSED;
            a.processed_timestamp = now_sec;
         });
      }
   }

   // Store outbound envelope
   outenvelopes_t envelopes(get_self());
   uint64_t out_id = std::max<uint64_t>(1, envelopes.available_primary_key());

   envelopes.emplace(ram_payer, id_key{out_id}, outbound_envelope{
      .id            = out_id,
      .chain_code    = chain_code,
      .epoch_index   = epoch,
      .envelope_hash = sha256(packed.data(), packed.size()),
      .status        = EnvelopeStatus::ENVELOPE_STATUS_PENDING_DELIVERY,
      .raw_envelope  = packed,
   });

   // === AUDIT LOG + INLINE CLEANUP OF WORKING STATE ===
   //
   // Audit-log row mirrors the outbound emit (WIRE → outpost). Followed
   // by inline drains of the previous-epoch outenvelopes row (one-deep
   // retention; the batch op only ever reads the most-recent emit) and
   // the just-PROCESSED attestations for this outpost (their bytes are
   // now baked into `packed` above).
   {
      // Resolve the destination chain row on `sysio.chains` (PK = slug_name
      // value). Symmetric with the evalcons inbound endpoints projection
      // — `kind` → `ChainId.kind`, `external_chain_id` → `ChainId.id`.
      const auto op_row = [&]() {
         sysio::chains::chains_t chains_tbl(CHAINS_ACCOUNT);
         return chains_tbl.get(sysio::chains::chain_key{sysio::slug_name{chain_code}});
      }();

      sysio::opp::Endpoints endpoints;
      endpoints.start.kind = ChainKind::CHAIN_KIND_WIRE;
      endpoints.start.id   = WIRE_CHAIN_ID;
      endpoints.end.kind   = op_row.kind;
      endpoints.end.id     = op_row.external_chain_id;

      write_envelope_log(get_self(), endpoints, epoch,
                         sha256(packed.data(), packed.size()));

      // Drop previous outpost emits — keep only the row we just inserted.
      auto by_outpost = envelopes.get_index<"byoutpost"_n>();
      for (auto it = by_outpost.lower_bound(chain_code);
           it != by_outpost.end() && it->chain_code == chain_code; ) {
         if (it->id == out_id) { ++it; continue; }
         it = by_outpost.erase(std::move(it));
      }

      // Drop the attestations we just consumed (status PROCESSED rows
      // for this outpost). They've been bundled into `packed`; the
      // bytes are dead weight on chain.
      auto processed_idx = atts.get_index<"bystatus"_n>();
      for (auto it = processed_idx.lower_bound(
                        static_cast<uint64_t>(AttestationStatus::ATTESTATION_STATUS_PROCESSED));
           it != processed_idx.end() &&
           it->status == AttestationStatus::ATTESTATION_STATUS_PROCESSED; ) {
         if (it->chain_code != chain_code) { ++it; continue; }
         it = processed_idx.erase(std::move(it));
      }
   }
}


} // namespace sysio

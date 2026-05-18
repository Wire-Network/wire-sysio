#include <sysio.msgch/sysio.msgch.hpp>
#include <sysio.epoch/sysio.epoch.hpp>
#include <sysio.authex/sysio.authex.hpp>
#include <sysio/opp/opp.pb.hpp>
#include <sysio/opp/attestations/attestations.pb.hpp>
#include <zpp_bits.h>

namespace sysio {

using opp::types::ChainKind;
using opp::types::TokenKind;
using opp::types::MessageDirection;
using opp::types::MessageStatus;
using opp::types::EnvelopeStatus;
using opp::types::AttestationType;
using opp::types::AttestationStatus;

namespace {

constexpr auto     EPOCH_ACCOUNT  = "sysio.epoch"_n;
constexpr auto     OPREG_ACCOUNT  = "sysio.opreg"_n;
constexpr auto     UWRIT_ACCOUNT  = "sysio.uwrit"_n;
constexpr auto     CHALG_ACCOUNT  = "sysio.chalg"_n;
constexpr auto     AUTHEX_ACCOUNT = "sysio.authex"_n;

/// WIRE chain numeric id used in `opp::Endpoints` rows on the audit log.
/// One end of every cross-chain envelope is always WIRE.
constexpr uint32_t WIRE_CHAIN_ID  = 1;

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
   const uint64_t new_id = tbl.available_primary_key();
   tbl.emplace(self, sysio::msgch::id_key{new_id}, sysio::msgch::envelope_log_entry{
      .id          = new_id,
      .endpoints   = endpoints,
      .epoch_index = epoch_index,
      .checksum    = checksum,
      .emitted_at  = current_time_point(),
   });

   epoch::outposts_t outposts(EPOCH_ACCOUNT);
   uint32_t active_outposts = 0;
   for (auto it = outposts.begin(); it != outposts.end(); ++it) ++active_outposts;
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
      case ChainKind::CHAIN_KIND_ETHEREUM: {   // EM — variant index 3
         if (bytes.size() != 33) return pk;
         sysio::ecc_public_key arr;
         std::copy(bytes.begin(), bytes.end(), arr.begin());
         pk.emplace<3>(arr);
         return pk;
      }
      case ChainKind::CHAIN_KIND_SOLANA: {     // ED — variant index 4
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

/// Decode an OperatorAction sub-message and dispatch to the appropriate
/// sysio.opreg action. Called from the inbound dispatch loop in `evalcons`.
///
/// Sub-type routing:
///   * DEPOSIT_REQUEST     → opreg::depositinle(account, chain, amount, actor, msg_id)
///   * WITHDRAW_REQUEST    → opreg::withdrawinle(account, chain, amount)
///   * WITHDRAW_REMIT      → outbound-only (depot → outpost); silently dropped if seen inbound
///   * SLASH               → depot-internal; rejected if seen inbound. Slash decisions
///                            originate from sysio.chalg → opreg::slash and never re-enter
///                            the depot via OPP. A slash arriving inbound here is either an
///                            outpost replaying its own outbound (no-op), or a malformed
///                            attestation from a misbehaving operator (drop).
///   * UNKNOWN             → no-op
///
/// `original_message_id` is the OPP message id of the attestation's parent
/// Message — opreg::depositinle uses it to populate DEPOSIT_REVERT correlation
/// when refunding an unaccepted deposit.
void dispatch_operator_action(name self, const std::vector<char>& data,
                              ChainKind from_chain,
                              const checksum256& original_message_id) {
   opp::attestations::OperatorAction oa;
   {
      auto in = zpp::bits::in{std::span{data.data(), data.size()}, zpp::bits::no_size{}};
      auto rc = in(oa);
      if (rc != zpp::bits::errc{}) return;   // malformed; skip silently
   }

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
   // TokenAmount + ChainAddress get split into (kind, amount) / (kind, address)
   // on the inline-action tuples per the no-proto-messages-in-actions rule.
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
            std::make_tuple(account, from_chain,
                            oa.amount.kind, raw_amount,
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
            std::make_tuple(account, from_chain,
                            oa.amount.kind, raw_amount)
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
/// uw_account, token_kind — the latter discriminates same-chain
/// swap legs); the authoritative copy for verification is the bytes
/// themselves, stored on `commit_entry.{source,dest}_uic_bytes`.
void dispatch_underwrite_commit(name self, const std::vector<char>& data,
                                ChainKind from_chain, uint64_t outpost_id) {
   opp::attestations::UnderwriteIntentCommit uic;
   {
      auto in = zpp::bits::in{std::span{data.data(), data.size()}, zpp::bits::no_size{}};
      auto rc = in(uic);
      if (rc != zpp::bits::errc{}) return;
   }
   if (uic.uw_account.name.empty()) return;

   action(
      permission_level{self, "active"_n},
      UWRIT_ACCOUNT, "rcrdcommit"_n,
      std::make_tuple(uic.uw_request_id, name{uic.uw_account.name},
                      outpost_id, from_chain, uic.token_kind, data)
   ).send();
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
                          ChainKind from_chain, uint64_t outpost_id,
                          const checksum256& original_message_id) {
   switch (type) {
      case AttestationType::ATTESTATION_TYPE_OPERATOR_ACTION:
         dispatch_operator_action(self, data, from_chain, original_message_id);
         break;

      case AttestationType::ATTESTATION_TYPE_SWAP_REQUEST:
         action(
            permission_level{self, "active"_n},
            UWRIT_ACCOUNT, "createuwreq"_n,
            std::make_tuple(attestation_id, type, outpost_id, data)
         ).send();
         break;

      case AttestationType::ATTESTATION_TYPE_UNDERWRITE_INTENT_COMMIT:
         dispatch_underwrite_commit(self, data, from_chain, outpost_id);
         break;

      case AttestationType::ATTESTATION_TYPE_SWAP_REMIT:
         // Inbound SWAP_REMIT — the destination outpost reflected our
         // depot-emitted SwapRemit envelope back to us, which is the
         // delivery acknowledgement. Use it as the release trigger.
         //
         // Renamed from the old REMIT_CONFIRM dispatch (which was a
         // separate outpost-emitted confirm message — removed; the depot
         // is the ground truth and every SwapRemit is depot-authorized,
         // so success is implicit absent SWAP_REJECTED).
         {
            opp::attestations::SwapRemit remit;
            auto in = zpp::bits::in{std::span{data.data(), data.size()}, zpp::bits::no_size{}};
            auto rc = in(remit);
            if (rc != zpp::bits::errc{}) break;
            // The original_message_id field encodes the uwreq's 64-bit id in
            // its low 8 bytes; treat the rest as zero-padding from the
            // depot-side encoder. Future task: a dedicated uw_request_id
            // field on SwapRemit would remove this dependency.
            uint64_t uwreq_id = 0;
            const auto& bytes = remit.original_message_id;
            if (bytes.size() >= 8) {
               for (size_t i = 0; i < 8; ++i) {
                  uwreq_id |= static_cast<uint64_t>(static_cast<uint8_t>(bytes[i])) << (i * 8);
               }
            }
            if (uwreq_id != 0) {
               action(
                  permission_level{self, "active"_n},
                  UWRIT_ACCOUNT, "release"_n,
                  std::make_tuple(uwreq_id)
               ).send();
            }
         }
         break;

      case AttestationType::ATTESTATION_TYPE_SWAP_REJECTED:
         // Destination outpost couldn't pay a SwapRemit; reconcile the
         // depot's view of the reserve so it matches the outpost's
         // (still-holding-the-amount) balance. Flatten the SwapRejected
         // proto message into primitive params on the inline action — the
         // ABI never sees a proto-message-typed parameter per the
         // no-proto-messages-in-actions rule.
         {
            opp::attestations::SwapRejected rejected;
            auto in = zpp::bits::in{std::span{data.data(), data.size()}, zpp::bits::no_size{}};
            auto rc = in(rejected);
            if (rc != zpp::bits::errc{}) break;
            checksum256 original_id;
            // SwapRejected.original_swap_remit_id is a proto `bytes` field —
            // OPP message ids are always 32 bytes (keccak/sha digests per the
            // platform spec). Anything shorter implies a malformed
            // attestation; drop it rather than silently truncate.
            const auto& id_bytes = rejected.original_swap_remit_id;
            if (id_bytes.size() == 32) {
               std::array<uint8_t, 32> arr{};
               std::copy(id_bytes.begin(), id_bytes.end(),
                         reinterpret_cast<char*>(arr.data()));
               original_id = checksum256(arr);
            } else {
               break;   // malformed; drop
            }
            const uint64_t unremitted_raw =
               static_cast<uint64_t>(static_cast<int64_t>(rejected.unremitted_amount.amount));
            action(
               permission_level{self, "active"_n},
               "sysio.reserv"_n, "onreject"_n,
               std::make_tuple(original_id,
                                rejected.recipient.kind,
                                rejected.recipient.address,
                                rejected.unremitted_amount.kind,
                                unremitted_raw,
                                rejected.reason)
            ).send();
         }
         break;

      case AttestationType::ATTESTATION_TYPE_STAKING_REWARD:
         // STAKING_REWARD routes two ways:
         //   1. the aggregate native amount credits the outpost-side reserve
         //      (sysio.reserv::onreward) so swapquote stays solvent;
         //   2. the per-staker body is dispatched to sysio.cap::onreward,
         //      which prices native -> WIRE off that reserve and credits the
         //      staker's claim ledger (pending_claims if AuthX-linked, else
         //      parked in unmapped_tokens by native address until they link).
         // Both proto messages are flattened into primitive params per the
         // no-proto-messages-in-actions rule.
         {
            opp::attestations::StakingReward sr;
            auto in = zpp::bits::in{std::span{data.data(), data.size()}, zpp::bits::no_size{}};
            auto rc = in(sr);
            if (rc != zpp::bits::errc{}) break;
            const uint64_t reward_raw =
               static_cast<uint64_t>(static_cast<int64_t>(sr.reward_amount.amount));
            // 1. Outpost-side reserve credit (aggregate, native-denominated).
            action(
               permission_level{self, "active"_n},
               "sysio.reserv"_n, "onreward"_n,
               std::make_tuple(from_chain, sr.reward_amount.kind, reward_raw)
            ).send();
            // 2. Per-staker WIRE-side claim-ledger credit. staker_wire_account
            //    is passed as the raw string (empty => not yet AuthX-linked,
            //    so sysio.cap parks by native address); staker_native_address
            //    .address is a proto `bytes` field, directly compatible with
            //    the action's std::vector<char> parameter.
            action(
               permission_level{self, "active"_n},
               "sysio.cap"_n, "onreward"_n,
               std::make_tuple(sr.outpost_id,
                                sr.staker_wire_account.name,
                                from_chain,
                                sr.staker_native_address.address,
                                sr.reward_amount.kind,
                                reward_raw,
                                sr.reward_epoch_index,
                                sr.external_epoch_ref,
                                sr.share_bps)
            ).send();
         }
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
         // Routed to sysio.chalg in Task 6 (the manual-msig flow has a
         // different entry point — `initchal` / `submitres` — than the
         // dispatch shape here).
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
      case AttestationType::ATTESTATION_TYPE_PRETOKEN_PURCHASE:
      case AttestationType::ATTESTATION_TYPE_PRETOKEN_YIELD:
      case AttestationType::ATTESTATION_TYPE_WIRE_TOKEN_PURCHASE:
      case AttestationType::ATTESTATION_TYPE_NODE_OWNER_REG:
      case AttestationType::ATTESTATION_TYPE_ATTESTATION_PROCESSING_ERROR:
      case AttestationType::ATTESTATION_TYPE_UNSPECIFIED:
      default:
         break;
   }
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
void msgch::deliver(name batch_op_name, uint64_t outpost_id, std::vector<char> data) {
   is_batch_operator_active(batch_op_name);
   check(!data.empty(), "delivery data cannot be empty");

   // Verify outpost exists
   epoch::outposts_t outposts(EPOCH_ACCOUNT);
   auto outpost_pk = epoch::outpost_key{outpost_id};
   check(outposts.contains(outpost_pk), "outpost not found");
   auto op_row = outposts.get(outpost_pk);

   // Decode envelope to validate epoch_index matches current WIRE epoch
   uint32_t epoch = current_epoch_index();
   {
      opp::Envelope env_check;
      auto in = zpp::bits::in{std::span{data.data(), data.size()}, zpp::bits::no_size{}};
      auto result = in(env_check);
      check(result == zpp::bits::errc{}, "failed to decode inbound envelope");
      uint32_t env_epoch = static_cast<uint32_t>(env_check.epoch_index);
      check(env_epoch == epoch,
         "envelope epoch_index mismatch: envelope=" + std::to_string(env_epoch) +
         " current=" + std::to_string(epoch));
   }

   // Compute checksum trustlessly inside the contract
   checksum256 cs = sha256(data.data(), data.size());

   // Prevent duplicate delivery from same operator for same outpost+epoch
   envelopes_t envs(get_self());
   auto oe_idx = envs.get_index<"byoutepoch"_n>();
   uint64_t composite = (static_cast<uint64_t>(outpost_id) << 32) | epoch;
   for (auto it = oe_idx.lower_bound(composite);
        it != oe_idx.end() && it->by_outpost_epoch() == composite; ++it) {
      if (it->batch_op_name == batch_op_name) {
         sysio::print_f("operator already delivered for this outpost+epoch: %s", batch_op_name.to_string().c_str());
         return;
      }
   }

   // Store envelope
   uint64_t env_id = envs.available_primary_key();

   envs.emplace(get_self(), id_key{env_id}, envelope_entry{
      .id            = env_id,
      .outpost_id    = outpost_id,
      .epoch_index   = epoch,
      .batch_op_name = batch_op_name,
      .chain_kind    = op_row.chain_kind,
      .checksum      = cs,
      .raw_data      = data,
      .received_at   = current_time_point(),
   });

   // Evaluate consensus inline
   action(
      permission_level{get_self(), "active"_n},
      get_self(),
      "evalcons"_n,
      std::make_tuple(outpost_id, epoch)
   ).send();
}

// ---------------------------------------------------------------------------
//  evalcons — evaluate consensus on inbound envelopes for outpost+epoch
// ---------------------------------------------------------------------------
void msgch::evalcons(uint64_t outpost_id, uint32_t epoch_index) {
   require_auth(get_self());

   envelopes_t envs(get_self());
   auto oe_idx = envs.get_index<"byoutepoch"_n>();
   uint64_t composite = (static_cast<uint64_t>(outpost_id) << 32) | epoch_index;

   // Group envelopes by checksum (CDT-compatible parallel vectors)
   std::vector<checksum256>       seen_checksums;
   std::vector<uint32_t>          checksum_counts;
   std::vector<std::vector<char>> checksum_data;
   uint32_t total_deliveries = 0;

   for (auto it = oe_idx.lower_bound(composite);
        it != oe_idx.end() && it->by_outpost_epoch() == composite; ++it) {
      bool found = false;
      for (size_t g = 0; g < seen_checksums.size(); ++g) {
         if (seen_checksums[g] == it->checksum) {
            checksum_counts[g]++;
            found = true;
            break;
         }
      }
      if (!found) {
         seen_checksums.push_back(it->checksum);
         checksum_counts.push_back(1);
         checksum_data.push_back(it->raw_data);
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

   if (!consensus_reached) return;

   // === CONSENSUS REACHED ===
   auto& raw    = checksum_data[consensus_group];
   auto  now    = current_time_point();
   auto  now_sec = static_cast<uint64_t>(now.sec_since_epoch());
   uint32_t epoch = current_epoch_index();

   // Decode protobuf Envelope from the consensus data (raw protobuf, no size prefix)
   opp::Envelope envelope;
   auto in = zpp::bits::in{std::span{raw.data(), raw.size()}, zpp::bits::no_size{}};
   auto decode_result = in(envelope);
   check(decode_result == zpp::bits::errc{}, "failed to decode inbound OPP Envelope");

   // Store the raw envelope as an inbound message
   messages_t msgs(get_self());
   uint64_t msg_id = msgs.available_primary_key();

   msgs.emplace(get_self(), id_key{msg_id}, message_entry{
      .id           = msg_id,
      .outpost_id   = outpost_id,
      .epoch_index  = epoch,
      .direction    = MessageDirection::MESSAGE_DIRECTION_INBOUND,
      .status       = MessageStatus::MESSAGE_STATUS_PROCESSED,
      .raw_payload  = raw,
      .received_at  = now,
      .processed_at = now,
   });

   // Extract individual AttestationEntries from each Message in the Envelope.
   // Each attestation is BOTH (a) recorded in the `attestations` table for
   // audit / re-dispatch / outbound packing AND (b) inline-dispatched to the
   // matching depot-side handler contract. Dispatch is best-effort — unknown
   // / out-of-scope types fall through silently so the inbound stream keeps
   // flowing while later tasks land their handlers.
   attestations_t atts(get_self());
   ChainKind from_chain = ChainKind::CHAIN_KIND_UNKNOWN;
   {
      epoch::outposts_t outposts(EPOCH_ACCOUNT);
      auto opost = outposts.get(epoch::outpost_key{outpost_id});
      from_chain = opost.chain_kind;
   }
   for (auto& msg : envelope.messages) {
      for (auto& entry : msg.payload.attestations) {
         uint64_t att_id = atts.available_primary_key();
         atts.emplace(get_self(), id_key{att_id}, attestation_entry{
            .id                  = att_id,
            .outpost_id          = outpost_id,
            .epoch_index         = epoch,
            .type                = entry.type,
            .status              = AttestationStatus::ATTESTATION_STATUS_READY,
            .data                = entry.data,
            .pending_timestamp   = 0,
            .ready_timestamp     = now_sec,
            .processed_timestamp = 0,
         });
         // Reconstruct the OPP message_id as a checksum256 so downstream
         // handlers (DEPOSIT_REVERT correlation, future audit trails) can
         // pin per-attestation outcomes back to the originating message.
         checksum256 msg_id;
         {
            auto& mid = msg.header.message_id;
            std::array<uint8_t, 32> raw{};
            const size_t n = std::min<size_t>(mid.size(), 32);
            for (size_t i = 0; i < n; ++i) raw[i] = static_cast<uint8_t>(mid[i]);
            msg_id = checksum256{raw};
         }
         dispatch_attestation(get_self(), att_id, entry.type, entry.data,
                              from_chain, outpost_id, msg_id);
      }
   }

   // === AUDIT LOG + INLINE CLEANUP OF WORKING STATE ===
   //
   // The envelope's bytes have served their purpose at this point:
   // consensus is reached, attestations are extracted and queued for
   // outbound delivery via `buildenv`. The durable trail is the
   // metadata-only `envelope_log` row written below; the four working
   // tables are drained inline so they don't grow without bound.
   {
      const auto& op_row = [&]() {
         epoch::outposts_t outposts(EPOCH_ACCOUNT);
         return outposts.get(epoch::outpost_key{outpost_id});
      }();

      sysio::opp::Endpoints endpoints;
      endpoints.start.kind = op_row.chain_kind;
      endpoints.start.id   = op_row.chain_id;
      endpoints.end.kind   = ChainKind::CHAIN_KIND_WIRE;
      endpoints.end.id     = WIRE_CHAIN_ID;

      write_envelope_log(get_self(), endpoints, epoch, seen_checksums[consensus_group]);

      // Drop the HEAVY `raw_data` from each per-batch-op `envelopes` row,
      // but KEEP the metadata tuple `(outpost_id, epoch_index, batch_op_name)`
      // intact. `epoch::advance` reads this metadata via the `byoutepoch`
      // index to compute `did_deliver` per group member — erasing the rows
      // here destroys that signal and miscredits every batchop as
      // `delivered=false`, cascading bootstrapped operators into termination
      // within a few rotations. The metadata rows are evicted by
      // `epoch::advance` after `recorddel` has read them.
      std::vector<uint64_t> ids_to_clear;
      auto modify_idx = envs.get_index<"byoutepoch"_n>();
      for (auto it = modify_idx.lower_bound(composite);
           it != modify_idx.end() && it->by_outpost_epoch() == composite; ++it) {
         ids_to_clear.push_back(it->id);
      }
      for (auto id : ids_to_clear) {
         envs.modify(same_payer, id_key{id}, [](auto& r) {
            r.raw_data.clear();
            r.raw_data.shrink_to_fit();
         });
      }

      // Drop the just-inserted `messages` row. Its raw_payload mirrors
      // the envelope bytes we already discarded; downstream consumers
      // read the audit log for trail and the attestations table for
      // queued outbound work.
      if (msgs.contains(id_key{msg_id})) {
         msgs.erase(id_key{msg_id});
      }
   }

   // === RECORD PER-OUTPOST CONSENSUS ===
   outpost_consensus_t opcons(get_self());
   auto opc_pk = outpost_consensus_key{outpost_id};
   if (!opcons.contains(opc_pk)) {
      opcons.emplace(get_self(), opc_pk, outpost_consensus_entry{
         .outpost_id        = outpost_id,
         .epoch_index       = epoch_index,
         .consensus_reached = true,
      });
   } else {
      opcons.modify(same_payer, opc_pk, [&](auto& r) {
         r.epoch_index       = epoch_index;
         r.consensus_reached = true;
      });
   }

   // Consensus state recorded — advance is triggered by chkcons
   // once next_epoch_start has passed.
}

// ---------------------------------------------------------------------------
//  chkcons — check all-outpost consensus + time gate, trigger advance
// ---------------------------------------------------------------------------
void msgch::chkcons() {
   uint32_t epoch = current_epoch_index();

   // Check all outposts have consensus for the current epoch
   outpost_consensus_t opcons(get_self());
   epoch::outposts_t outposts(EPOCH_ACCOUNT);
   bool all_consensus = true;
   uint32_t outpost_count = 0;

   for (auto it = outposts.begin(); it != outposts.end(); ++it) {
      ++outpost_count;
      auto opc_pk = outpost_consensus_key{it->id};
      if (!opcons.contains(opc_pk)) {
         all_consensus = false;
         break;
      }
      auto opc = opcons.get(opc_pk);
      if (!opc.consensus_reached || opc.epoch_index != epoch) {
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

   // All conditions met — reset consensus and advance
   for (auto it = opcons.begin(); it != opcons.end(); ++it) {
      auto opc_pk = outpost_consensus_key{it.key().outpost_id};
      opcons.modify(same_payer, opc_pk, [&](auto& r) { r.consensus_reached = false; });
   }

   action(
      permission_level{get_self(), "active"_n},
      EPOCH_ACCOUNT,
      "advance"_n,
      std::make_tuple()
   ).send();
}

// ---------------------------------------------------------------------------
//  queueout — queue outbound attestation for an outpost
// ---------------------------------------------------------------------------
void msgch::queueout(uint64_t outpost_id,
                     opp::types::AttestationType attest_type,
                     std::vector<char> data) {
   auto now_sec = static_cast<uint64_t>(current_time_point().sec_since_epoch());

   attestations_t atts(get_self());
   uint64_t att_id = atts.available_primary_key();

   atts.emplace(get_self(), id_key{att_id}, attestation_entry{
      .id                  = att_id,
      .outpost_id          = outpost_id,
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
void msgch::buildenv(uint64_t outpost_id) {
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
      if (it->outpost_id != outpost_id) continue;

      opp::AttestationEntry entry;
      entry.type = it->type;
      entry.data_size = zpp::bits::vuint32_t{static_cast<uint32_t>(it->data.size())};
      entry.data = it->data;
      candidate_entries.push_back(std::move(entry));
      candidate_ids.push_back(it->id);
   }

   if (candidate_entries.empty()) return;

   // ── Phase 2: packing loop. Walk candidates in order, accumulating a
   //    conservative byte estimate; stop once the next one would push the
   //    envelope over MAX_ENVELOPE_BYTES. The trailing serialised-size check
   //    is the hard backstop in case the estimator under-counts.
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

   // First-attestation-too-big guard — surface so the operator sees it
   // instead of a silently-stuck queue. Never expected at protocol level
   // because individual attestations are bounded well below MAX_ENVELOPE_BYTES.
   check(included_count > 0,
         "sysio.msgch::buildenv: a single READY attestation exceeds "
         "MAX_ENVELOPE_BYTES — cannot pack into an envelope");

   std::vector<opp::AttestationEntry> entries(
      std::make_move_iterator(candidate_entries.begin()),
      std::make_move_iterator(candidate_entries.begin() + included_count));
   const std::vector<uint64_t> included_ids(
      candidate_ids.begin(),
      candidate_ids.begin() + included_count);

   // Mark only the included attestations as PROCESSED. Remaining candidates
   // stay READY for the next epoch's `buildenv` call.
   for (uint64_t aid : included_ids) {
      auto att_pk = id_key{aid};
      if (atts.contains(att_pk)) {
         atts.modify(same_payer, att_pk, [&](auto& a) {
            a.status              = AttestationStatus::ATTESTATION_STATUS_PROCESSED;
            a.processed_timestamp = now_sec;
         });
      }
   }

   // Build a Message containing the included attestations.
   opp::MessagePayload payload;
   payload.version = zpp::bits::vuint32_t{1};
   payload.attestations = std::move(entries);

   opp::MessageHeader header;
   header.timestamp = zpp::bits::vuint64_t{now_sec};

   opp::Message msg;
   msg.header = std::move(header);
   msg.payload = std::move(payload);

   // Build OPP Envelope wrapping the message
   opp::Envelope env;
   env.epoch_index = zpp::bits::vuint32_t{epoch};
   env.epoch_timestamp = zpp::bits::vuint64_t{now_sec};
   env.messages.push_back(std::move(msg));

   // Serialize envelope (no size prefix — raw protobuf wire format)
   std::vector<char> packed;
   auto out = zpp::bits::out{packed, zpp::bits::no_size{}};
   (void)out(env);

   // Hard backstop — the estimator should always over-count, but a final
   // size check guarantees the envelope cannot exceed the cross-chain cap
   // even if the conservative estimator drifts.
   check(packed.size() <= MAX_ENVELOPE_BYTES,
         "sysio.msgch::buildenv: serialised envelope exceeds MAX_ENVELOPE_BYTES "
         "(estimator drift)");

   // Store outbound envelope
   outenvelopes_t envelopes(get_self());
   uint64_t out_id = envelopes.available_primary_key();

   envelopes.emplace(get_self(), id_key{out_id}, outbound_envelope{
      .id            = out_id,
      .outpost_id    = outpost_id,
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
      const auto& op_row = [&]() {
         epoch::outposts_t outposts(EPOCH_ACCOUNT);
         return outposts.get(epoch::outpost_key{outpost_id});
      }();

      sysio::opp::Endpoints endpoints;
      endpoints.start.kind = ChainKind::CHAIN_KIND_WIRE;
      endpoints.start.id   = WIRE_CHAIN_ID;
      endpoints.end.kind   = op_row.chain_kind;
      endpoints.end.id     = op_row.chain_id;

      write_envelope_log(get_self(), endpoints, epoch,
                         sha256(packed.data(), packed.size()));

      // Drop previous outpost emits — keep only the row we just inserted.
      auto by_outpost = envelopes.get_index<"byoutpost"_n>();
      for (auto it = by_outpost.lower_bound(outpost_id);
           it != by_outpost.end() && it->outpost_id == outpost_id; ) {
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
         if (it->outpost_id != outpost_id) { ++it; continue; }
         it = processed_idx.erase(std::move(it));
      }
   }
}


} // namespace sysio

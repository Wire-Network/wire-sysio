#include <sysio.uwrit/sysio.uwrit.hpp>
#include <sysio.epoch/sysio.epoch.hpp>
#include <sysio.opreg/sysio.opreg.hpp>
#include <sysio.reserv/sysio.reserv.hpp>
#include <sysio.authex/sysio.authex.hpp>
#include <sysio/opp/attestations/attestations.pb.hpp>
#include <sysio/permission.hpp>
#include <sysio/crypto.hpp>
#include <zpp_bits.h>

#include <cstring>
#include <limits>

namespace sysio {

using opp::types::ChainKind;
using opp::types::TokenKind;
using opp::types::AttestationType;
using opp::types::UnderwriteRequestStatus;
using opp::types::UnderwriteStatus;
using opp::types::OperatorStatus;
using opp::types::OperatorType;
using opp::attestations::SwapRequest;

namespace {

uint64_t current_time_ms() {
   return static_cast<uint64_t>(current_time_point().sec_since_epoch()) * 1000;
}

uint32_t get_current_epoch() {
   sysio::epoch::epochstate_t es(uwrit::EPOCH_ACCOUNT);
   if (!es.exists()) return 0;
   return es.get().current_epoch_index;
}

/// Sum the underwriter's pending withdraws on opreg for the given (chain, token).
uint64_t opreg_pending_withdraws(name underwriter, ChainKind chain, TokenKind token_kind) {
   opreg::wtdwqueue_t queue(uwrit::OPREG_ACCOUNT);
   auto idx = queue.get_index<"byaccountck"_n>();
   uint128_t composite = (static_cast<uint128_t>(underwriter.value) << 64)
                       | (static_cast<uint64_t>(chain) << 32)
                       | static_cast<uint64_t>(token_kind);

   uint64_t total = 0;
   auto it  = idx.lower_bound(composite);
   auto end = idx.upper_bound(composite);
   for (; it != end; ++it) {
      total += it->amount;
   }
   return total;
}

/// Sum this contract's active locks for the given (underwriter, chain, token).
uint64_t sum_locks_inline(name self, name underwriter,
                          ChainKind chain, TokenKind token_kind) {
   uwrit::locks_t locks(self);
   auto idx = locks.get_index<"byuwck"_n>();
   uint128_t composite = (static_cast<uint128_t>(underwriter.value) << 64)
                       | (static_cast<uint64_t>(chain) << 32)
                       | static_cast<uint64_t>(token_kind);

   uint64_t total = 0;
   auto it  = idx.lower_bound(composite);
   auto end = idx.upper_bound(composite);
   for (; it != end; ++it) {
      total += it->amount;
   }
   return total;
}

/// Look up an underwriter's balance on opreg for the given (chain, token).
/// Returns the raw stored balance — caller subtracts active locks + pending
/// withdraws to get the spendable amount.
uint64_t opreg_balance(name underwriter, ChainKind chain, TokenKind token_kind,
                        OperatorStatus& out_status) {
   opreg::operators_t ops(uwrit::OPREG_ACCOUNT);
   opreg::operator_key op_pk{underwriter.value};
   if (!ops.contains(op_pk)) {
      out_status = OperatorStatus::OPERATOR_STATUS_UNKNOWN;
      return 0;
   }
   auto op = ops.get(op_pk);
   out_status = op.status;
   for (const auto& b : op.balances) {
      if (b.chain == chain && b.token_kind == token_kind) {
         return b.balance;
      }
   }
   return 0;
}

/// Compute the underwriter's spendable balance on (chain, token_kind).
/// Mirrors the sysio.opreg::available() formula:
///   balance - sum(active locks here in uwrit) - sum(pending withdraws on opreg)
/// gated by status (SLASHED / TERMINATED -> 0).
uint64_t available_via_mirrors(name self, name underwriter,
                                ChainKind chain, TokenKind token_kind) {
   OperatorStatus status;
   uint64_t balance = opreg_balance(underwriter, chain, token_kind, status);
   if (status == OperatorStatus::OPERATOR_STATUS_SLASHED ||
       status == OperatorStatus::OPERATOR_STATUS_TERMINATED) {
      return 0;
   }
   uint64_t locked  = sum_locks_inline(self, underwriter, chain, token_kind);
   uint64_t pending = opreg_pending_withdraws(underwriter, chain, token_kind);
   uint64_t reserved = locked + pending;
   return balance > reserved ? balance - reserved : 0;
}

/// Constant-product output computed locally — mirrors sysio.reserve::cp_output
/// (the uwrit mirror reads the same `lps` rows; the math is replicated here so
/// uwrit doesn't need to action-call into reserve from inside createuwreq).
uint64_t cp_output(uint64_t reserve_src, uint64_t reserve_dst, uint64_t src_amount) {
   if (reserve_src == 0 || reserve_dst == 0 || src_amount == 0) return 0;
   uint128_t numerator   = static_cast<uint128_t>(reserve_dst) * src_amount;
   uint128_t denominator = static_cast<uint128_t>(reserve_src) + src_amount;
   uint128_t result      = numerator / denominator;
   if (result > static_cast<uint128_t>(std::numeric_limits<uint64_t>::max())) {
      return std::numeric_limits<uint64_t>::max();
   }
   return static_cast<uint64_t>(result);
}

/// Quote `src_amount` of (src_chain, src_token) into (dst_chain, dst_token)
/// via the WIRE-paired reserves on sysio.reserv. Returns 0 if any required
/// reserve is missing — caller treats 0 as "no quote available, skip
/// variance check". Mirrors the math in `sysio.reserv::swapquote` so the
/// variance check at SWAP_REQUEST receipt time doesn't pay for an inline
/// action call.
///
/// Renamed from `reserve_quote` to `swap_quote` alongside the action-name
/// rename of `quote` -> `swapquote` on `sysio.reserv`.
uint64_t swap_quote(ChainKind src_chain, TokenKind src_token,
                    ChainKind dst_chain, TokenKind dst_token,
                    uint64_t src_amount) {
   if (src_amount == 0) return 0;
   if (src_token == TokenKind::TOKEN_KIND_WIRE && dst_token == TokenKind::TOKEN_KIND_WIRE) {
      return src_amount;
   }
   reserve::reserves_t reserves(uwrit::RESERVE_ACCOUNT);

   auto outpost_amt = [](const reserve::reserve_entry& r) -> uint64_t {
      return r.reserve_outpost_amount.amount < 0
         ? uint64_t{0}
         : static_cast<uint64_t>(r.reserve_outpost_amount.amount);
   };
   auto wire_amt = [](const reserve::reserve_entry& r) -> uint64_t {
      return r.reserve_wire_amount.amount < 0
         ? uint64_t{0}
         : static_cast<uint64_t>(r.reserve_wire_amount.amount);
   };

   if (src_token == TokenKind::TOKEN_KIND_WIRE) {
      reserve::reserve_key pk{reserve::pack_chain_token(dst_chain, dst_token)};
      if (!reserves.contains(pk)) return 0;
      auto r = reserves.get(pk);
      return cp_output(wire_amt(r), outpost_amt(r), src_amount);
   }
   if (dst_token == TokenKind::TOKEN_KIND_WIRE) {
      reserve::reserve_key pk{reserve::pack_chain_token(src_chain, src_token)};
      if (!reserves.contains(pk)) return 0;
      auto r = reserves.get(pk);
      return cp_output(outpost_amt(r), wire_amt(r), src_amount);
   }
   reserve::reserve_key src_pk{reserve::pack_chain_token(src_chain, src_token)};
   reserve::reserve_key dst_pk{reserve::pack_chain_token(dst_chain, dst_token)};
   if (!reserves.contains(src_pk) || !reserves.contains(dst_pk)) return 0;
   auto src_r = reserves.get(src_pk);
   auto dst_r = reserves.get(dst_pk);
   uint64_t intermediate = cp_output(outpost_amt(src_r), wire_amt(src_r), src_amount);
   if (intermediate == 0) return 0;
   return cp_output(wire_amt(dst_r), outpost_amt(dst_r), intermediate);
}

/// Encode + queue a SWAP_REVERT attestation back to the source outpost when
/// the variance check fails. The outpost matches the original SWAP_REQUEST
/// via `original_swap_message_id` (low 8 bytes carry the depot's
/// attestation_id; see msgch's SWAP_REMIT dispatch for the matching decode
/// convention).
void emit_swap_revert(name self, uint64_t outpost_id, uint64_t attestation_id,
                      const opp::attestations::SwapRequest& sr,
                      const std::string& reason) {
   opp::attestations::SwapRevert rev;
   rev.original_swap_message_id.assign(32, 0);
   for (size_t i = 0; i < 8; ++i) {
      rev.original_swap_message_id[i] = static_cast<char>((attestation_id >> (i * 8)) & 0xff);
   }
   rev.depositor     = sr.actor;
   rev.refund_amount = sr.source_amount;
   rev.reason        = reason;

   // `no_size{}` — raw protobuf bytes for the outpost decoder; the default
   // `zpp::bits::data_out` form prepends a 4-byte LE length prefix that
   // corrupts the first field tag on the receiving side.
   std::vector<char> encoded;
   auto out = zpp::bits::out{encoded, zpp::bits::no_size{}};
   (void)out(rev);

   action(
      permission_level{self, "active"_n},
      uwrit::MSGCH_ACCOUNT, "queueout"_n,
      std::make_tuple(outpost_id,
         opp::types::AttestationType::ATTESTATION_TYPE_SWAP_REVERT, encoded)
   ).send();
}

/// Look up the depot's outpost id for `chain` via `sysio.epoch::outposts`.
/// Returns `std::nullopt` when no outpost is registered for the chain
/// (per `feedback_no_zero_sentinels` — outpost id 0 is a real id, so
/// 0 must not double as "missing").
std::optional<uint64_t> find_outpost_id_for_chain(ChainKind chain) {
   sysio::epoch::outposts_t outposts(uwrit::EPOCH_ACCOUNT);
   for (auto it = outposts.begin(); it != outposts.end(); ++it) {
      if (it->chain_kind == chain) {
         return it->id;
      }
   }
   return std::nullopt;
}

/// Build + queue the outbound SWAP_REMIT envelope for a confirmed race.
///
/// Fired inline from `try_select_winner` after the depot has committed
/// to a winning underwriter pair. Two side-effects, both must land in
/// this transaction:
///   1. Inline-action `sysio.reserv::debit(dst_chain, dst_amount)` —
///      decrements `reserve_outpost_amount` so the depot's reserve
///      view is tight against the outbound SWAP_REMIT. A failed debit
///      (insufficient reserve) aborts the entire commit; no half-state.
///   2. Inline-action `sysio.msgch::queueout(dst_outpost_id,
///      ATTESTATION_TYPE_SWAP_REMIT, encoded)` — pushes the envelope
///      for the next epoch's outbound drain. The destination outpost's
///      Reserve.sol (ETH) / opp-outpost Reserve PDA (SOL) handles it
///      via `_handleSwapRemit` / `handle_swap_remit`.
void emit_swap_remit(name self,
                      const uwrit::uw_request_t& req,
                      name candidate) {
   // Decode the stored SwapRequest payload so we can populate the
   // outbound SwapRemit's `recipient` field. The depot otherwise
   // wouldn't know which address on the destination chain the swap
   // user wants paid (the row only stores chain/kind/amount summaries).
   opp::attestations::SwapRequest sr;
   {
      auto in = zpp::bits::in{
         std::span{req.attestation_inbound_data.data(),
                    req.attestation_inbound_data.size()},
         zpp::bits::no_size{}};
      auto rc = in(sr);
      check(rc == zpp::bits::errc{},
            "emit_swap_remit: failed to decode stored SwapRequest");
   }

   auto dst_outpost_opt = find_outpost_id_for_chain(req.dst_chain);
   check(dst_outpost_opt.has_value(),
         "emit_swap_remit: no outpost registered for destination chain");
   const uint64_t dst_outpost_id = *dst_outpost_opt;

   // Reserve debit FIRST — if the reserve is insufficient the entire
   // commit aborts and the race is unwound by the caller's surrounding
   // transaction failing. Depot is the ground truth; no half-state.
   // TokenAmount is split into (kind, amount) on the inline action per
   // the no-proto-messages-in-actions rule.
   action(
      permission_level{self, "active"_n},
      uwrit::RESERVE_ACCOUNT, "debit"_n,
      std::make_tuple(req.dst_chain, req.dst_token_kind, req.dst_amount)
   ).send();

   // Build the SwapRemit. `original_message_id` encodes the uwreq_id
   // in its low 8 bytes; the destination outpost's reflected SWAP_REMIT
   // envelope back to msgch's dispatch uses this for the release-trigger
   // decode (see sysio.msgch.cpp's SWAP_REMIT case).
   opp::attestations::SwapRemit remit;
   remit.recipient        = sr.recipient;
   remit.amount           = opp::types::TokenAmount{
      .kind   = req.dst_token_kind,
      .amount = static_cast<int64_t>(req.dst_amount),
   };
   remit.original_message_id.assign(32, 0);
   for (size_t i = 0; i < 8; ++i) {
      remit.original_message_id[i] =
         static_cast<char>((req.id >> (i * 8)) & 0xff);
   }
   // Resolve the winning underwriter's destination-chain pubkey from
   // `sysio.authex::links` (`bynamechain` index) so the SwapRemit
   // carries the underwriter's auditable identity on the dst chain.
   // The destination outpost cross-references this against the
   // matching UNDERWRITE_INTENT_COMMIT it already saw on its leg;
   // downstream auditors / on-chain consumers can verify the
   // payout against the underwriter that won the race without
   // back-tracking through the depot.
   //
   // An underwriter without an authex link for `dst_chain` cannot be
   // a valid race winner (they have no on-chain identity there to
   // commit a signature against). `try_select_winner`'s caller has
   // already accepted their COMMIT, so this lookup should always
   // succeed; if it doesn't, abort the commit rather than ship a
   // SwapRemit with a blank underwriter — auditing depends on the
   // field being populated.
   remit.underwriter.kind = req.dst_chain;
   {
      sysio::authex::links_t links(uwrit::AUTHEX_ACCOUNT);
      auto idx = links.get_index<"bynamechain"_n>();
      const uint128_t key = sysio::to_namechain_key(candidate, req.dst_chain);
      auto it = idx.find(key);
      check(it != idx.end(),
            "emit_swap_remit: winning underwriter has no authex link "
            "for the destination chain — cannot emit a SwapRemit "
            "with a blank underwriter address (audit requirement)");
      remit.underwriter.address = sysio::pubkey_to_bytes(it->pub_key);
      check(!remit.underwriter.address.empty(),
            "emit_swap_remit: underwriter's authex pub_key variant index "
            "unsupported (pubkey_to_bytes returned empty)");
   }
   remit.unlock_timestamp = 0;

   // `no_size{}` — see emit_swap_revert for the rationale.
   std::vector<char> encoded;
   auto out = zpp::bits::out{encoded, zpp::bits::no_size{}};
   (void)out(remit);

   action(
      permission_level{self, "active"_n},
      uwrit::MSGCH_ACCOUNT, "queueout"_n,
      std::make_tuple(dst_outpost_id,
         opp::types::AttestationType::ATTESTATION_TYPE_SWAP_REMIT, encoded)
   ).send();
}

/// Verify the embedded signature in `uic_bytes` was produced by a key on
/// EITHER the `underwriter` account's `active` OR `owner` permission, over
/// the digest `sha256(serialize(uic_with_signature_blanked))`. Returns
/// true on first matching key. Returns false (never throws) when the bytes
/// are empty, the proto fails to decode, the embedded signature is missing,
/// or no `active`/`owner` key recovers to the signature. Other permissions
/// (custom permission names) are intentionally NOT checked: the
/// underwriter_plugin's signature_provider_manager_plugin config is pinned
/// to one of `active`/`owner`, so accepting a custom permission would let
/// an attacker bypass the configuration constraint.
///
/// **Per `feedback_opp_handlers_never_throw.md` — this MUST stay
/// non-throwing.** It's called from `try_select_winner`, which runs inside
/// the evalcons inline-action chain; a `check()` failure here halts
/// consensus. The defensive size+tag bounds catch the obvious cases; the
/// `sysio::recover_key_nothrow` intrinsic catches everything else the
/// host crypto path can raise (malformed bytes, unactivated signature
/// type, recovery math failure, subjective-size limit) and returns
/// `std::nullopt` instead.
bool verify_uic_signature(name underwriter,
                           const std::vector<char>& uic_bytes) {
   if (uic_bytes.empty()) return false;

   // Decode the UIC payload.
   opp::attestations::UnderwriteIntentCommit uic;
   {
      auto in = zpp::bits::in{
         std::span{uic_bytes.data(), uic_bytes.size()},
         zpp::bits::no_size{}};
      if (in(uic) != zpp::bits::errc{}) return false;
   }

   // Save the signature, blank it, recompute the digest.
   std::vector<char> sig_bytes_view{uic.signature.begin(), uic.signature.end()};
   if (sig_bytes_view.empty()) return false;
   uic.signature.clear();

   std::vector<char> blanked;
   auto out = zpp::bits::out{blanked, zpp::bits::no_size{}};
   if (out(uic) != zpp::bits::errc{}) return false;

   sysio::checksum256 digest =
      sysio::sha256(blanked.data(), blanked.size());

   // Defensive bounds before invoking the chain signature intrinsic. The
   // first byte of a packed `sysio::signature` is the variant tag
   // (0=K1, 1=R1, 2=WebAuthN, 3=EM, 4=ED25519, 5=BLS). Anything outside
   // that range or sized outside the smallest/largest legal variant is
   // tossed before the intrinsic gets a chance to attempt recovery.
   if (sig_bytes_view.size() < 2 || sig_bytes_view.size() > 1024) return false;
   const uint8_t tag = static_cast<uint8_t>(sig_bytes_view[0]);
   if (tag > 5) return false;

   // Unpack the underwriter's signature variant from the embedded bytes.
   sysio::signature parsed_sig;
   {
      sysio::datastream<const char*> ds{sig_bytes_view.data(),
                                          sig_bytes_view.size()};
      ds >> parsed_sig;
   }

   // Recover the public key — non-throwing variant. The host wraps the
   // throwing recovery path in try/catch and returns `std::nullopt` on
   // any failure (malformed bytes, unactivated sig type, recovery math
   // failure, subjective-size limit). Required because CDT compiles
   // with `-fno-exceptions` and `try_select_winner` cannot halt the
   // dispatch on attacker-controlled bytes (per
   // `feedback_opp_handlers_never_throw.md`).
   auto recovered_opt = sysio::try_recover_key(digest, parsed_sig);
   if (!recovered_opt) return false;
   const sysio::public_key& recovered = *recovered_opt;

   // Only `owner` and `active` permissions are considered. The
   // underwriter_plugin's signature_provider_manager_plugin config is
   // pinned to one of those two (see plugin docs); accepting a custom
   // permission would open a separate authorization surface that nothing
   // else on the platform validates.
   constexpr sysio::name OWNER_PERM  = "owner"_n;
   constexpr sysio::name ACTIVE_PERM = "active"_n;
   for (auto perm : { ACTIVE_PERM, OWNER_PERM }) {
      auto rec_opt = sysio::get_permission(underwriter, perm);
      if (!rec_opt) continue;
      for (const auto& kw : rec_opt->auth.keys) {
         if (kw.key == recovered) return true;
      }
   }
   return false;
}

/// Allocate a fresh `lock_id` from the uwcounters singleton.
uint64_t next_lock_id(name self) {
   uwrit::uwcounters_t ctr_tbl(self);
   auto ctr = ctr_tbl.get_or_default(uwrit::uw_counters{});
   uint64_t id = ctr.next_lock_id;
   ctr.next_lock_id = id + 1;
   ctr_tbl.set(ctr, self);
   return id;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
//  setconfig
// ---------------------------------------------------------------------------
void uwrit::setconfig(uint32_t fee_bps,
                      uint32_t collateral_lock_duration_epoch_count,
                      uint8_t  fee_split_winner_pct,
                      uint8_t  fee_split_other_uw_pct,
                      uint8_t  fee_split_batch_op_pct) {
   require_auth(get_self());
   check(fee_bps <= 10000, "fee_bps cannot exceed 10000 (100%)");
   check(collateral_lock_duration_epoch_count > 0,
         "collateral_lock_duration_epoch_count must be positive");
   const uint32_t split_total = static_cast<uint32_t>(fee_split_winner_pct)
                              + static_cast<uint32_t>(fee_split_other_uw_pct)
                              + static_cast<uint32_t>(fee_split_batch_op_pct);
   check(split_total == 100,
         "fee_split_*_pct must sum to 100");

   uwconfig_t cfg_tbl(get_self());
   uw_config cfg = cfg_tbl.get_or_default(uw_config{});
   cfg.fee_bps                              = fee_bps;
   cfg.collateral_lock_duration_epoch_count = collateral_lock_duration_epoch_count;
   cfg.fee_split_winner_pct                 = fee_split_winner_pct;
   cfg.fee_split_other_uw_pct               = fee_split_other_uw_pct;
   cfg.fee_split_batch_op_pct               = fee_split_batch_op_pct;
   cfg_tbl.set(cfg, get_self());
}

// ---------------------------------------------------------------------------
//  createuwreq — called inline from sysio.msgch when SWAP arrives
// ---------------------------------------------------------------------------
void uwrit::createuwreq(uint64_t attestation_id,
                         opp::types::AttestationType type,
                         uint64_t outpost_id,
                         std::vector<char> data) {
   require_auth(MSGCH_ACCOUNT);

   uwreqs_t reqs(get_self());
   auto pk = id_key{attestation_id};
   check(!reqs.contains(pk),
         "underwrite request already exists for this attestation");

   // Only SWAP_REQUEST attestations create UWREQs — msgch's dispatch routes
   // other types directly to their handlers, not through createuwreq.
   check(type == AttestationType::ATTESTATION_TYPE_SWAP_REQUEST,
         "createuwreq currently supports only SWAP_REQUEST attestations");

   SwapRequest sr;
   {
      auto in = zpp::bits::in{std::span{data.data(), data.size()}, zpp::bits::no_size{}};
      auto rc = in(sr);
      check(rc == zpp::bits::errc{}, "failed to decode SwapRequest");
   }

   // Hard-fail any SwapRequest without a populated `source_tx_id`. The
   // off-chain underwriter verify path uses this id to confirm a real
   // on-chain deposit backs the swap before committing collateral; a
   // SwapRequest without it can't be verified, and an outpost is
   // required to populate the field at swap-emit time. Per
   // `feedback_opp_handlers_never_throw.md` we cannot `check()`/throw
   // here (we're inside the evalcons dispatch chain — a throw stalls
   // consensus); instead emit a SwapRevert back to the source outpost so
   // the user's deposit is refunded and the run continues.
   if (sr.source_tx_id.empty()) {
      emit_swap_revert(get_self(), outpost_id, attestation_id, sr,
                       "SwapRequest rejected: source_tx_id is required "
                       "(no SwapRequest may be emitted without a "
                       "populated source-chain transaction id)");
      return;
   }

   // Variance-tolerance check via sysio.reserve mirror. If no LP is
   // provisioned for the (chain, token) pair on either side the quote
   // returns 0 and the variance check is implicitly skipped — the swap
   // proceeds to the underwriter race. This lets dev / smoke clusters
   // without provisioned LPs continue to operate while still applying the
   // check the moment LPs are present.
   const TokenKind src_token  = sr.source_amount.kind;
   const ChainKind src_chain  = sr.actor.kind;
   const ChainKind dst_chain  = sr.target_chain.kind;
   const TokenKind dst_token  = sr.target_token;
   const uint64_t  src_amount = static_cast<uint64_t>(static_cast<int64_t>(sr.source_amount.amount));

   uint64_t current_quote = swap_quote(src_chain, src_token, dst_chain, dst_token, src_amount);
   if (current_quote != 0 && sr.quoted_destination_amount != 0) {
      uint64_t quoted   = sr.quoted_destination_amount;
      uint64_t diff     = current_quote > quoted ? current_quote - quoted : quoted - current_quote;
      // tolerance_bps / 10000 of quoted; computed in uint128 to avoid overflow.
      uint128_t allowed = (static_cast<uint128_t>(quoted) * sr.quote_tolerance_bps) / 10000u;
      if (static_cast<uint128_t>(diff) > allowed) {
         emit_swap_revert(get_self(), outpost_id, attestation_id, sr,
                          "variance exceeded tolerance: quoted=" + std::to_string(quoted)
                          + " current=" + std::to_string(current_quote)
                          + " tolerance_bps=" + std::to_string(sr.quote_tolerance_bps));
         return;   // no UWREQ created
      }
   }

   reqs.emplace(get_self(), pk, uw_request_t{
      .id                        = attestation_id,
      .type                      = type,
      .status                    = UnderwriteRequestStatus::UNDERWRITE_REQUEST_STATUS_PENDING,
      .src_chain                 = sr.actor.kind,
      .src_token_kind            = sr.source_amount.kind,
      .src_amount                = static_cast<uint64_t>(static_cast<int64_t>(sr.source_amount.amount)),
      .dst_chain                 = sr.target_chain.kind,
      .dst_token_kind            = sr.target_token,
      .dst_amount                = sr.quoted_destination_amount,
      .variance_tolerance_bps    = sr.quote_tolerance_bps,
      .source_tx_id              = sr.source_tx_id,
      .depositor                 = sr.actor.address,
      .commits_by                = {},
      .winner                    = name{},
      .committed_at_ms           = 0,
      .settled_at_ms             = 0,
      .expires_at_epoch          = 0,
      .attestation_inbound_data  = std::move(data),
      .attestation_outbound_data = {},
   });
}

// ---------------------------------------------------------------------------
//  Internal: try_select_winner — race resolver
// ---------------------------------------------------------------------------

namespace {

/// Helper: find or create the commit_entry for `underwriter` inside an
/// uw_request_t. Returns iterator-like reference into the in-place vector.
uwrit::commit_entry* find_or_create_commit(uwrit::uw_request_t& req, name underwriter) {
   for (auto& c : req.commits_by) {
      if (c.underwriter == underwriter) return &c;
   }
   req.commits_by.push_back(uwrit::commit_entry{
      .underwriter = underwriter,
   });
   return &req.commits_by.back();
}

/// Resolve the race once both legs of a (uwreq, underwriter) pair have
/// arrived. If the underwriter has sufficient bond on each chain, push two
/// lock rows + mark them winner + emit REMIT to the destination outpost.
/// Otherwise mark their commit_entry as INSUFFICIENT_BOND (status=SLASHED in
/// the proto enum, used here as a sentinel for "race-disqualified").
void try_select_winner(name self, uint64_t uwreq_id, name candidate) {
   uwrit::uwreqs_t reqs(self);
   auto pk = uwrit::id_key{uwreq_id};
   if (!reqs.contains(pk)) return;
   auto req = reqs.get(pk);
   if (req.status != UnderwriteRequestStatus::UNDERWRITE_REQUEST_STATUS_PENDING) return;

   // ── T7: signature verification on both legs ──────────────────────────
   // Look up the candidate's commit_entry to get the per-leg UIC bytes.
   const uwrit::commit_entry* ce_ptr = nullptr;
   for (const auto& c : req.commits_by) {
      if (c.underwriter == candidate) { ce_ptr = &c; break; }
   }
   if (!ce_ptr) return;
   if (!verify_uic_signature(candidate, ce_ptr->source_uic_bytes) ||
       !verify_uic_signature(candidate, ce_ptr->dest_uic_bytes)) {
      // Per `feedback_opp_handlers_never_throw.md`: dispatch handlers
      // must NOT throw — a check() halts evalcons and stalls consensus.
      // Log + skip instead. The race resolves with the remaining valid
      // commit if any.
      // TODO: decide how to handle invalid sigs before launch @jglanz
      sysio::print("invalid sig for uwreq ", uwreq_id, " from ", candidate, "\n");
      return;
   }

   uint64_t src_avail = available_via_mirrors(self, candidate, req.src_chain, req.src_token_kind);
   uint64_t dst_avail = available_via_mirrors(self, candidate, req.dst_chain, req.dst_token_kind);
   if (src_avail < req.src_amount || dst_avail < req.dst_amount) {
      // Insufficient bond — mark the commit_entry but don't promote.
      reqs.modify(same_payer, pk, [&](auto& r) {
         auto* c = find_or_create_commit(r, candidate);
         c->status = UnderwriteStatus::UNDERWRITE_STATUS_SLASHED;
         c->reason = "insufficient bond on one or both legs";
      });
      return;
   }

   // ── T6: race-time variance recheck ────────────────────────────────────
   // The createuwreq path validated the LP quote at ingestion. Between
   // then and now the LP may have drifted; if the drift now exceeds the
   // user's tolerance, emit SWAP_REVERT instead of locking. Skipped when
   // the local mirror returns 0 (no LP provisioned) — same convention as
   // createuwreq.
   {
      const uint64_t current_quote = swap_quote(
         req.src_chain, req.src_token_kind,
         req.dst_chain, req.dst_token_kind,
         req.src_amount);
      const uint64_t quoted = req.dst_amount;
      if (current_quote != 0 && quoted != 0) {
         const uint64_t diff = current_quote > quoted
                                  ? current_quote - quoted
                                  : quoted - current_quote;
         const uint128_t allowed = (static_cast<uint128_t>(quoted)
                                       * req.variance_tolerance_bps) / 10000u;
         if (static_cast<uint128_t>(diff) > allowed) {
            // Recover originating outpost from the swap-request payload
            // so the SWAP_REVERT routes back correctly.
            opp::attestations::SwapRequest sr;
            {
               auto in = zpp::bits::in{
                  std::span{req.attestation_inbound_data.data(),
                              req.attestation_inbound_data.size()},
                  zpp::bits::no_size{}};
               if (in(sr) == zpp::bits::errc{}) {
                  auto src_outpost_opt = find_outpost_id_for_chain(req.src_chain);
                  if (src_outpost_opt) {
                     emit_swap_revert(self, *src_outpost_opt, req.id, sr,
                        "variance exceeded tolerance at race resolution: "
                        "quoted=" + std::to_string(quoted)
                        + " current=" + std::to_string(current_quote)
                        + " tolerance_bps=" + std::to_string(req.variance_tolerance_bps));
                  }
               }
            }
            reqs.modify(same_payer, pk, [&](auto& r) {
               r.status           = UnderwriteRequestStatus::UNDERWRITE_REQUEST_STATUS_REJECTED;
               r.settled_at_ms    = current_time_ms();
               r.expires_at_epoch = get_current_epoch() + uwrit::UWREQ_RETENTION_EPOCHS;
               for (auto& c : r.commits_by) {
                  if (c.status == UnderwriteStatus::UNDERWRITE_STATUS_INTENT_SUBMITTED) {
                     c.status = UnderwriteStatus::UNDERWRITE_STATUS_RELEASED;
                     c.reason = "uwreq reverted at race resolution (variance drift)";
                  }
               }
            });
            return;
         }
      }
   }

   // Winner — push two locks (one per leg) + mark uwreq CONFIRMED.
   uint32_t now_ep = get_current_epoch();
   // Lock duration comes from uwconfig; default (10 epochs) used when no
   // setconfig has been issued yet.
   uwrit::uwconfig_t uwcfg_tbl(self);
   auto uwcfg = uwcfg_tbl.get_or_default(uwrit::uw_config{});
   uint32_t expires_ep = now_ep + uwcfg.collateral_lock_duration_epoch_count;

   uwrit::locks_t locks(self);

   uint64_t src_lock_id = next_lock_id(self);
   locks.emplace(self, uwrit::lock_key{src_lock_id}, uwrit::lock_entry{
      .lock_id          = src_lock_id,
      .uwreq_id         = uwreq_id,
      .underwriter      = candidate,
      .chain            = req.src_chain,
      .token_kind       = req.src_token_kind,
      .amount           = req.src_amount,
      .created_at_epoch = now_ep,
      .expires_at_epoch = expires_ep,
   });

   uint64_t dst_lock_id = next_lock_id(self);
   locks.emplace(self, uwrit::lock_key{dst_lock_id}, uwrit::lock_entry{
      .lock_id          = dst_lock_id,
      .uwreq_id         = uwreq_id,
      .underwriter      = candidate,
      .chain            = req.dst_chain,
      .token_kind       = req.dst_token_kind,
      .amount           = req.dst_amount,
      .created_at_epoch = now_ep,
      .expires_at_epoch = expires_ep,
   });

   reqs.modify(same_payer, pk, [&](auto& r) {
      r.status          = UnderwriteRequestStatus::UNDERWRITE_REQUEST_STATUS_CONFIRMED;
      r.winner          = candidate;
      r.committed_at_ms = current_time_ms();
      // Mark the winner's commit_entry CONFIRMED, others RELEASED (loser).
      for (auto& c : r.commits_by) {
         if (c.underwriter == candidate) {
            c.status = UnderwriteStatus::UNDERWRITE_STATUS_INTENT_CONFIRMED;
         } else if (c.status == UnderwriteStatus::UNDERWRITE_STATUS_INTENT_SUBMITTED) {
            // Eligible loser — promote to RELEASED for retention/debugging.
            c.status = UnderwriteStatus::UNDERWRITE_STATUS_RELEASED;
            c.reason = "lost the COMMIT race";
         }
      }
   });

   // Re-read the row post-modify so emit_swap_remit sees the
   // CONFIRMED snapshot (and gets a stable copy of the bytes /
   // chain / amount fields).
   auto confirmed = reqs.get(pk);

   // Queue the outbound SWAP_REMIT envelope to the destination outpost
   // + debit the depot's reserve view in the same transaction. Per
   // protocol: the depot is the ground truth; SWAP_REMIT emission and
   // the reserve_outpost_amount debit are atomic. The reflected
   // SWAP_REMIT envelope from the destination outpost back to msgch
   // triggers `uwrit::release` (the depot doesn't wait on a separate
   // confirmation message; reflection IS the ack). Outpost-side
   // failures emit SWAP_REJECTED back, which calls
   // sysio.reserv::onreject to undo this debit so the depot's view
   // reconciles with the outpost's actual (still-holding-the-token)
   // balance.
   emit_swap_remit(self, confirmed, candidate);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
//  rcrdcommit — record a per-leg COMMIT arrival
// ---------------------------------------------------------------------------
void uwrit::rcrdcommit(uint64_t uwreq_id,
                       name underwriter,
                       uint64_t outpost_id,
                       opp::types::ChainKind from_chain,
                       opp::types::TokenKind from_token_kind,
                       std::vector<char> uic_bytes) {
   require_auth(MSGCH_ACCOUNT);

   uwreqs_t reqs(get_self());
   auto pk = id_key{uwreq_id};
   check(reqs.contains(pk), "uwreq not found");
   auto req = reqs.get(pk);
   check(req.status == UnderwriteRequestStatus::UNDERWRITE_REQUEST_STATUS_PENDING,
         "uwreq not open for commits");

   reqs.modify(same_payer, pk, [&](auto& r) {
      auto* c = find_or_create_commit(r, underwriter);
      uint64_t now_ms = current_time_ms();
      // Route by the `(from_chain, from_token_kind)` pair so same-chain
      // swaps (e.g. ERC20 → ETH-native on one outpost) land in the
      // correct per-leg slot.
      const bool is_source = (from_chain == r.src_chain
                              && from_token_kind == r.src_token_kind);
      const bool is_dest   = (from_chain == r.dst_chain
                              && from_token_kind == r.dst_token_kind);
      if (is_source) {
         c->source_received_at_ms = now_ms;
         c->source_outpost_id     = outpost_id;
         c->source_uic_bytes      = uic_bytes;
      } else if (is_dest) {
         c->dest_received_at_ms = now_ms;
         c->dest_outpost_id     = outpost_id;
         c->dest_uic_bytes      = uic_bytes;
      }
      // Re-set status to INTENT_SUBMITTED if the underwriter is re-arming
      // a previously-disqualified entry (e.g. they topped up bond and want
      // back in the race). The next try_select_winner call re-evaluates.
      if (c->status == UnderwriteStatus::UNDERWRITE_STATUS_SLASHED) {
         c->status = UnderwriteStatus::UNDERWRITE_STATUS_INTENT_SUBMITTED;
         c->reason.clear();
      }
   });

   // Re-read after modify — try_select_winner needs the latest commit_entry.
   auto refreshed = reqs.get(pk);
   for (const auto& c : refreshed.commits_by) {
      if (c.underwriter == underwriter &&
          c.source_received_at_ms != 0 && c.dest_received_at_ms != 0) {
         try_select_winner(get_self(), uwreq_id, underwriter);
         break;
      }
   }
}

// ---------------------------------------------------------------------------
//  release — settle an UWREQ; deferred-slash / deferred-remit each lock
// ---------------------------------------------------------------------------
void uwrit::release(uint64_t uwreq_id) {
   // Two callers expected:
   //   * sysio.msgch::dispatch on REMIT_CONFIRM inbound (msgch auth).
   //   * uwrit::expirelock self-inline when a lock has aged past its deadline
   //     (uwrit's own auth, sent from the expirelock action body).
   check(has_auth(MSGCH_ACCOUNT) || has_auth(get_self()),
         "release requires sysio.msgch or sysio.uwrit authority");

   uwreqs_t reqs(get_self());
   auto pk = id_key{uwreq_id};
   check(reqs.contains(pk), "uwreq not found");
   auto req = reqs.get(pk);
   check(req.status == UnderwriteRequestStatus::UNDERWRITE_REQUEST_STATUS_CONFIRMED,
         "uwreq not in CONFIRMED state");

   // Iterate locks for this uwreq via secondary index, copy out keys (we'll
   // erase as we go), then for each: call opreg::releaselock + erase.
   locks_t locks(get_self());
   auto idx = locks.get_index<"byuwreq"_n>();
   std::vector<lock_key> to_erase;
   for (auto it = idx.lower_bound(uwreq_id);
        it != idx.end() && it->uwreq_id == uwreq_id; ++it) {
      // TokenAmount is split into (kind, amount) on the inline action per
      // the no-proto-messages-in-actions rule.
      action(
         permission_level{get_self(), "active"_n},
         OPREG_ACCOUNT, "releaselock"_n,
         std::make_tuple(it->underwriter, it->chain, it->token_kind, it->amount)
      ).send();
      to_erase.push_back(lock_key{it->lock_id});
   }
   for (const auto& k : to_erase) {
      locks.erase(k);
   }

   uint32_t now_ep = get_current_epoch();
   reqs.modify(same_payer, pk, [&](auto& r) {
      r.status           = UnderwriteRequestStatus::UNDERWRITE_REQUEST_STATUS_COMPLETED;
      r.settled_at_ms    = current_time_ms();
      r.expires_at_epoch = now_ep + UWREQ_RETENTION_EPOCHS;
   });
}

// ---------------------------------------------------------------------------
//  expirelock — permissionless watchdog for stale locks
// ---------------------------------------------------------------------------
void uwrit::expirelock(uint64_t uwreq_id) {
   uwreqs_t reqs(get_self());
   auto pk = id_key{uwreq_id};
   check(reqs.contains(pk), "uwreq not found");
   auto req = reqs.get(pk);
   check(req.status == UnderwriteRequestStatus::UNDERWRITE_REQUEST_STATUS_CONFIRMED,
         "uwreq not in CONFIRMED state");

   // Check the oldest lock for this uwreq has aged past the unlock deadline.
   // The deadline is the 10-epoch UWREQ retention window — generous enough
   // that the destination outpost has had ample time to confirm REMIT.
   locks_t locks(get_self());
   auto idx = locks.get_index<"byuwreq"_n>();
   auto it = idx.lower_bound(uwreq_id);
   check(it != idx.end() && it->uwreq_id == uwreq_id, "no locks for uwreq");

   uint32_t now_ep = get_current_epoch();
   check(now_ep >= it->created_at_epoch + UWREQ_RETENTION_EPOCHS,
         "uwreq lock has not yet aged past the unlock deadline");

   // Self-call release inline.
   action(
      permission_level{get_self(), "active"_n},
      get_self(), "release"_n,
      std::make_tuple(uwreq_id)
   ).send();
}

// ---------------------------------------------------------------------------
//  sumlocks — read-only helper
// ---------------------------------------------------------------------------
uint64_t uwrit::sumlocks(name underwriter,
                         opp::types::ChainKind chain,
                         opp::types::TokenKind token_kind) {
   return sum_locks_inline(get_self(), underwriter, chain, token_kind);
}

// ---------------------------------------------------------------------------
//  chklocks — epoch-boundary sweep of expired locks
// ---------------------------------------------------------------------------
void uwrit::chklocks(uint32_t up_to_epoch) {
   // Two valid callers:
   //   * sysio.epoch::advance — inlined at every epoch boundary so an
   //     epoch advance naturally evicts whatever just aged out.
   //   * sysio.uwrit — manual cleanup invocation, e.g. from a migration.
   check(has_auth(EPOCH_ACCOUNT) || has_auth(get_self()),
         "chklocks requires sysio.epoch or sysio.uwrit authority");

   locks_t locks(get_self());
   auto idx = locks.get_index<"byexpire"_n>();

   // Walk in ascending `expires_at_epoch` and erase while expired. We can't
   // hold an iterator across `locks.erase(*it)` (it invalidates the index
   // cursor), so collect keys first and erase in a second pass.
   std::vector<lock_key> doomed;
   for (auto it = idx.begin();
        it != idx.end() && it->expires_at_epoch <= up_to_epoch; ++it) {
      doomed.push_back(lock_key{it->lock_id});
   }
   for (const auto& k : doomed) {
      locks.erase(k);
   }
}

} // namespace sysio

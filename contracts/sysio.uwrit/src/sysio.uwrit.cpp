#include <sysio.uwrit/sysio.uwrit.hpp>
#include <sysio.epoch/sysio.epoch.hpp>
#include <sysio.opreg/sysio.opreg.hpp>
#include <sysio.reserv/sysio.reserv.hpp>
#include <sysio/opp/attestations/attestations.pb.hpp>
#include <zpp_bits.h>

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
/// via the WIRE-paired LPs on sysio.reserve. Returns 0 if any required LP
/// is missing — caller treats 0 as "no quote available, skip variance check".
uint64_t reserve_quote(ChainKind src_chain, TokenKind src_token,
                       ChainKind dst_chain, TokenKind dst_token,
                       uint64_t src_amount) {
   if (src_amount == 0) return 0;
   if (src_token == TokenKind::TOKEN_KIND_WIRE && dst_token == TokenKind::TOKEN_KIND_WIRE) {
      return src_amount;
   }
   reserve::lps_t lps(uwrit::RESERVE_ACCOUNT);

   if (src_token == TokenKind::TOKEN_KIND_WIRE) {
      reserve::lp_key pk{reserve::pack_chain_token(dst_chain, dst_token)};
      if (!lps.contains(pk)) return 0;
      auto lp = lps.get(pk);
      return cp_output(lp.reserve_wire, lp.reserve_paired, src_amount);
   }
   if (dst_token == TokenKind::TOKEN_KIND_WIRE) {
      reserve::lp_key pk{reserve::pack_chain_token(src_chain, src_token)};
      if (!lps.contains(pk)) return 0;
      auto lp = lps.get(pk);
      return cp_output(lp.reserve_paired, lp.reserve_wire, src_amount);
   }
   reserve::lp_key src_pk{reserve::pack_chain_token(src_chain, src_token)};
   reserve::lp_key dst_pk{reserve::pack_chain_token(dst_chain, dst_token)};
   if (!lps.contains(src_pk) || !lps.contains(dst_pk)) return 0;
   auto src_lp = lps.get(src_pk);
   auto dst_lp = lps.get(dst_pk);
   uint64_t intermediate = cp_output(src_lp.reserve_paired, src_lp.reserve_wire, src_amount);
   if (intermediate == 0) return 0;
   return cp_output(dst_lp.reserve_wire, dst_lp.reserve_paired, intermediate);
}

/// Encode + queue a SWAP_REVERT attestation back to the source outpost when
/// the variance check fails. The outpost matches the original SWAP via
/// `original_swap_message_id` (low 8 bytes carry the depot's attestation_id;
/// see msgch's REMIT_CONFIRM dispatch for the matching decode convention).
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

   auto [encoded, out] = zpp::bits::data_out<char>();
   (void)out(rev);

   action(
      permission_level{self, "active"_n},
      uwrit::MSGCH_ACCOUNT, "queueout"_n,
      std::make_tuple(outpost_id,
         opp::types::AttestationType::ATTESTATION_TYPE_SWAP_REVERT, encoded)
   ).send();
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
void uwrit::setconfig(uint32_t fee_bps) {
   require_auth(get_self());
   check(fee_bps <= 10000, "fee_bps cannot exceed 10000 (100%)");

   uwconfig_t cfg_tbl(get_self());
   uw_config cfg = cfg_tbl.get_or_default(uw_config{});
   cfg.fee_bps = fee_bps;
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

   // Only SWAP attestations create UWREQs — msgch's dispatch routes other
   // types directly to their handlers, not through createuwreq.
   check(type == AttestationType::ATTESTATION_TYPE_SWAP,
         "createuwreq currently supports only SWAP attestations");

   SwapRequest sr;
   {
      auto in = zpp::bits::in{std::span{data.data(), data.size()}, zpp::bits::no_size{}};
      auto rc = in(sr);
      check(rc == zpp::bits::errc{}, "failed to decode SwapRequest");
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

   uint64_t current_quote = reserve_quote(src_chain, src_token, dst_chain, dst_token, src_amount);
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

   // Winner — push two locks (one per leg) + mark uwreq CONFIRMED.
   uint32_t now_ep = get_current_epoch();
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

   // REMIT emit — wired up in Task 4 (sysio.msgch dispatch + msgch::queueout
   // round-trip). The msgch dispatch routes ATTESTATION_TYPE_REMIT_CONFIRM
   // back into uwrit::release once the destination outpost confirms; this
   // closes the loop. For now the uwreq sits in CONFIRMED status until the
   // expirelock watchdog fires or msgch's dispatch routes the REMIT.
}

} // anonymous namespace

// ---------------------------------------------------------------------------
//  rcrdcommit — record a per-leg COMMIT arrival
// ---------------------------------------------------------------------------
void uwrit::rcrdcommit(uint64_t uwreq_id,
                       name underwriter,
                       uint64_t outpost_id,
                       opp::types::ChainKind from_chain) {
   require_auth(MSGCH_ACCOUNT);
   (void)outpost_id;   // outpost_id is informational; race math uses from_chain

   uwreqs_t reqs(get_self());
   auto pk = id_key{uwreq_id};
   check(reqs.contains(pk), "uwreq not found");
   auto req = reqs.get(pk);
   check(req.status == UnderwriteRequestStatus::UNDERWRITE_REQUEST_STATUS_PENDING,
         "uwreq not open for commits");

   reqs.modify(same_payer, pk, [&](auto& r) {
      auto* c = find_or_create_commit(r, underwriter);
      uint64_t now_ms = current_time_ms();
      if (from_chain == r.src_chain) {
         c->source_received_at_ms = now_ms;
      } else if (from_chain == r.dst_chain) {
         c->dest_received_at_ms = now_ms;
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
//  rcrdreject — underwriter (or outpost) rejects an intent
// ---------------------------------------------------------------------------
void uwrit::rcrdreject(uint64_t uwreq_id, name underwriter, std::string reason) {
   require_auth(MSGCH_ACCOUNT);

   uwreqs_t reqs(get_self());
   auto pk = id_key{uwreq_id};
   check(reqs.contains(pk), "uwreq not found");
   auto req = reqs.get(pk);
   check(req.status == UnderwriteRequestStatus::UNDERWRITE_REQUEST_STATUS_PENDING,
         "uwreq not open for rejects");

   reqs.modify(same_payer, pk, [&](auto& r) {
      auto* c = find_or_create_commit(r, underwriter);
      c->status = UnderwriteStatus::UNDERWRITE_STATUS_RELEASED;
      c->reason = std::move(reason);
   });
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
      opp::types::TokenAmount ta;
      ta.kind   = it->token_kind;
      ta.amount = zpp::bits::vint64_t{static_cast<int64_t>(it->amount)};
      action(
         permission_level{get_self(), "active"_n},
         OPREG_ACCOUNT, "releaselock"_n,
         std::make_tuple(it->underwriter, it->chain, ta)
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

} // namespace sysio

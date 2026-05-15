#include <sysio.opreg/sysio.opreg.hpp>
#include <sysio.epoch/sysio.epoch.hpp>
#include <sysio.authex/sysio.authex.hpp>
#include <sysio.uwrit/sysio.uwrit.hpp>
#include <sysio/opp/attestations/attestations.pb.hpp>
#include <zpp_bits.h>

namespace sysio {

using opp::types::OperatorType;
using opp::types::OperatorStatus;
using opp::types::ChainKind;
using opp::types::TokenKind;
using opp::types::AttestationType;
using opp::attestations::OperatorAction;
using opp::attestations::OperatorActionLog;
using opp::attestations::DepositRevert;

namespace {

uint64_t current_time_ms() {
   return static_cast<uint64_t>(current_time_point().sec_since_epoch()) * 1000;
}

/// Compute the composite key matching `withdraw_request::by_account_ck` /
/// `sysio::uwrit::lock_entry::by_underwriter_ck`. Centralized so both the
/// indexer and the lookups stay in lockstep.
uint128_t make_account_chain_token_key(name account, ChainKind chain, TokenKind token_kind) {
   return (static_cast<uint128_t>(account.value) << 64)
        | (static_cast<uint64_t>(chain) << 32)
        | static_cast<uint64_t>(token_kind);
}

/// Find the outpost id registered with sysio.epoch for a given chain. Returns
/// `std::nullopt` if no matching outpost exists (the caller is responsible
/// for handling that case — typically by skipping the queueout for chains
/// without an outpost, e.g. WIRE-direct flows).
///
/// Returning `std::optional` rather than a 0 sentinel matters: outpost
/// ids start at 0, so a 0-as-not-found sentinel collides with the
/// canonical Ethereum outpost id and silently drops every REMIT / SLASH
/// queueout for that chain. The caller now uses `has_value()` to
/// distinguish "no outpost" from "outpost #0".
std::optional<uint64_t> find_outpost_id_for_chain(ChainKind chain) {
   sysio::epoch::outposts_t outposts(opreg::EPOCH_ACCOUNT);
   for (auto it = outposts.begin(); it != outposts.end(); ++it) {
      if (it->chain_kind == chain) {
         return it->id;
      }
   }
   return std::nullopt;
}

/// Enforce uniqueness of `(chain, token_kind)` within a collateral-
/// requirements vector. Duplicates would cause the same (chain, token)
/// pair to be checked twice during eligibility evaluation — harmless
/// behaviorally but a clear configuration error worth surfacing at the
/// boundary rather than silently absorbing.
void require_no_duplicate_chain_token(const std::vector<opreg::chain_min_bond>& v,
                                      const char* role_label) {
   for (auto outer = v.begin(); outer != v.end(); ++outer) {
      for (auto inner = std::next(outer); inner != v.end(); ++inner) {
         check(!(outer->chain == inner->chain && outer->token_kind == inner->token_kind),
               std::string(role_label) +
                  ": duplicate (chain, token_kind) in collateral requirements");
      }
   }
}

} // anonymous namespace

// ---------------------------------------------------------------------------
//  setconfig
// ---------------------------------------------------------------------------
void opreg::setconfig(uint32_t max_available_producers,
                      uint32_t max_available_batch_ops,
                      uint32_t max_available_underwriters,
                      uint64_t terminate_prune_delay_ms,
                      uint32_t terminate_max_consecutive_misses,
                      uint32_t terminate_max_pct_misses_24h,
                      uint64_t terminate_window_ms,
                      std::vector<chain_min_bond> req_prod_collat,
                      std::vector<chain_min_bond> req_batchop_collat,
                      std::vector<chain_min_bond> req_uw_collat) {
   require_auth(get_self());

   check(max_available_producers > 0, "max_available_producers must be positive");
   check(max_available_batch_ops > 0, "max_available_batch_ops must be positive");
   check(max_available_underwriters > 0, "max_available_underwriters must be positive");
   check(terminate_prune_delay_ms > 0, "terminate_prune_delay_ms must be positive");
   check(terminate_max_consecutive_misses > 0,
         "terminate_max_consecutive_misses must be positive");
   check(terminate_max_pct_misses_24h > 0 && terminate_max_pct_misses_24h <= 100,
         "terminate_max_pct_misses_24h must be in (0, 100]");
   check(terminate_window_ms > 0, "terminate_window_ms must be positive");

   require_no_duplicate_chain_token(req_prod_collat,    "req_prod_collat");
   require_no_duplicate_chain_token(req_batchop_collat, "req_batchop_collat");
   require_no_duplicate_chain_token(req_uw_collat,      "req_uw_collat");

   // Stamp every entry's `config_timestamp_ms` with the on-chain time
   // so consumers can detect stale configuration without trusting the
   // caller's clock — the action's value for that field is ignored.
   const auto now = current_time_ms();
   const auto stamp = [now](std::vector<chain_min_bond>& v) {
      for (auto& entry : v) {
         entry.config_timestamp_ms = now;
      }
   };
   stamp(req_prod_collat);
   stamp(req_batchop_collat);
   stamp(req_uw_collat);

   opconfig_t cfg_tbl(get_self());
   op_config cfg = cfg_tbl.get_or_default(op_config{});
   cfg.max_available_producers          = max_available_producers;
   cfg.max_available_batch_ops          = max_available_batch_ops;
   cfg.max_available_underwriters       = max_available_underwriters;
   cfg.terminate_prune_delay_ms         = terminate_prune_delay_ms;
   cfg.terminate_max_consecutive_misses = terminate_max_consecutive_misses;
   cfg.terminate_max_pct_misses_24h     = terminate_max_pct_misses_24h;
   cfg.terminate_window_ms              = terminate_window_ms;
   cfg.req_prod_collat                  = std::move(req_prod_collat);
   cfg.req_batchop_collat               = std::move(req_batchop_collat);
   cfg.req_uw_collat                    = std::move(req_uw_collat);
   cfg_tbl.set(cfg, get_self());
}

// ---------------------------------------------------------------------------
//  regoperator
// ---------------------------------------------------------------------------
void opreg::regoperator(name account,
                        opp::types::OperatorType type,
                        bool is_bootstrapped) {
   // Privileged sysio.opreg can register any operator.
   // Otherwise the account must authorize its own registration.
   if (!has_auth(get_self())) {
      require_auth(account);
   }

   // Only privileged callers can set is_bootstrapped=true
   if (is_bootstrapped) {
      require_auth(get_self());
   }

   // Validate type
   check(type == OperatorType::OPERATOR_TYPE_PRODUCER ||
         type == OperatorType::OPERATOR_TYPE_BATCH ||
         type == OperatorType::OPERATOR_TYPE_UNDERWRITER ||
         type == OperatorType::OPERATOR_TYPE_CHALLENGER,
         "invalid operator type");

   // Underwriters can NEVER be bootstrapped
   check(!(type == OperatorType::OPERATOR_TYPE_UNDERWRITER && is_bootstrapped),
         "underwriter type cannot be bootstrapped");

   // Check not already registered (non-pruned)
   operators_t ops(get_self());
   auto op_pk = operator_key{account.value};
   if (ops.contains(op_pk)) {
      auto existing = ops.get(op_pk);
      check(existing.status == OperatorStatus::OPERATOR_STATUS_TERMINATED,
            "operator already registered");
      // If terminated, erase old row to allow re-registration
      ops.erase(op_pk);
   }

   // Verify authex links exist for all active outpost chains.
   // Skip when: bootstrapped OR privileged caller (sysio.opreg registering on behalf)
   if (!is_bootstrapped && !has_auth(get_self())) {
      epoch::outposts_t outposts(EPOCH_ACCOUNT);
      authex::links_t links(AUTHEX_ACCOUNT);
      auto namechain_idx = links.get_index<"bynamechain"_n>();

      for (auto op_it = outposts.begin(); op_it != outposts.end(); ++op_it) {
         // authex now keys on `opp::types::ChainKind` directly — same
         // type as `outpost_info.chain_kind`, no cast.
         uint128_t composite_key = to_namechain_key(account, op_it->chain_kind);
         auto link_it = namechain_idx.find(composite_key);
         check(link_it != namechain_idx.end(),
               "missing authex link for outpost chain");
      }
   }

   auto now = current_time_ms();
   ops.emplace(get_self(), op_pk, operator_entry{
      .account         = account,
      .type            = type,
      .status          = is_bootstrapped ? OperatorStatus::OPERATOR_STATUS_ACTIVE
                                         : OperatorStatus::OPERATOR_STATUS_UNKNOWN,
      .is_bootstrapped = is_bootstrapped,
      .balances        = {},
      .registered_at   = now,
      .available_at    = is_bootstrapped ? now : 0,
   });
}

// ---------------------------------------------------------------------------
//  Internal helpers — balance / lock / withdraw rollup
// ---------------------------------------------------------------------------

namespace {

/// Sum the active locks on `sysio.uwrit::locks` for a given (op, chain, token).
/// Returns 0 if uwrit's locks table is empty (Task 3 not yet wired up) or if
/// the operator has no locks on that chain/token.
uint64_t sum_locks_inline(name account, ChainKind chain, TokenKind token_kind) {
   uwrit::locks_t locks(opreg::UWRIT_ACCOUNT);
   auto idx = locks.get_index<"byuwck"_n>();
   uint128_t composite = make_account_chain_token_key(account, chain, token_kind);

   uint64_t total = 0;
   auto it  = idx.lower_bound(composite);
   auto end = idx.upper_bound(composite);
   for (; it != end; ++it) {
      total += it->amount;
   }
   return total;
}

/// Sum the pending (not-yet-flushed) withdraws on this contract for a given
/// (op, chain, token). Subtracted by `available()` so a queued withdraw
/// effectively reserves the funds for its 2-epoch wait.
uint64_t sum_pending_withdraws(name account, ChainKind chain, TokenKind token_kind) {
   opreg::wtdwqueue_t queue(opreg::SYSTEM_ACCOUNT == name{} ? name{} : name{"sysio.opreg"_n});
   // The queue is scoped to the contract itself; use opreg's account.
   // (We can't reference get_self() from a free function — but the queue
   // table is always rooted on opreg, so its scope name is fixed.)
   opreg::wtdwqueue_t real_queue(name{"sysio.opreg"_n});
   auto idx = real_queue.get_index<"byaccountck"_n>();
   uint128_t composite = make_account_chain_token_key(account, chain, token_kind);

   uint64_t total = 0;
   auto it  = idx.lower_bound(composite);
   auto end = idx.upper_bound(composite);
   for (; it != end; ++it) {
      total += it->amount;
   }
   return total;
}

/// Look up the operator's balance row for a given (chain, token_kind).
/// Returns nullptr if no row exists.
const opreg::balance_entry*
find_balance(const opreg::operator_entry& op, ChainKind chain, TokenKind token_kind) {
   for (const auto& b : op.balances) {
      if (b.chain == chain && b.token_kind == token_kind) return &b;
   }
   return nullptr;
}

/// Compute available balance for a given (op, chain, token). The single
/// rollup formula: balance - sum(active locks) - sum(pending withdraws),
/// gated by status. Slashed / terminated operators read as zero.
uint64_t available_inline(const opreg::operator_entry& op,
                          ChainKind chain, TokenKind token_kind) {
   if (op.status == OperatorStatus::OPERATOR_STATUS_SLASHED ||
       op.status == OperatorStatus::OPERATOR_STATUS_TERMINATED) {
      return 0;
   }
   const auto* bal = find_balance(op, chain, token_kind);
   if (!bal) return 0;

   uint64_t locked  = sum_locks_inline(op.account, chain, token_kind);
   uint64_t pending = sum_pending_withdraws(op.account, chain, token_kind);
   uint64_t reserved = locked + pending;
   return bal->balance > reserved ? bal->balance - reserved : 0;
}

/// Balance minus active locks (NOT pending withdraws). Used by `slash()` to
/// determine how much can be slashed immediately — pending withdraws of a
/// slashed operator are forfeit (silently dropped at flush time).
uint64_t slashable_now(const opreg::operator_entry& op,
                       ChainKind chain, TokenKind token_kind) {
   const auto* bal = find_balance(op, chain, token_kind);
   if (!bal) return 0;
   uint64_t locked = sum_locks_inline(op.account, chain, token_kind);
   return bal->balance > locked ? bal->balance - locked : 0;
}

/// Check whether the operator's available balance on (chain, token_kind)
/// covers the role's minimum bond on that pair.
///
/// Bootstrapped operators are ACTIVE-by-fiat and bypass the per-outpost
/// bond check regardless of how `req_*_collat` is configured — they
/// represent system-installed operators that the depot trusts without
/// requiring collateral. Non-bootstrapped operators must satisfy every
/// `(chain, token_kind)` entry in the matching `req_*_collat` vector;
/// an empty/unset vector means "no operator of this role can become
/// ACTIVE until configuration lands."
bool meets_role_min(const opreg::operator_entry& op,
                    const opreg::op_config& cfg) {
   if (op.is_bootstrapped) {
      return true;
   }
   const std::vector<opreg::chain_min_bond>* reqs = nullptr;
   switch (op.type) {
      case OperatorType::OPERATOR_TYPE_PRODUCER:    reqs = &cfg.req_prod_collat;    break;
      case OperatorType::OPERATOR_TYPE_BATCH:       reqs = &cfg.req_batchop_collat; break;
      case OperatorType::OPERATOR_TYPE_UNDERWRITER: reqs = &cfg.req_uw_collat;      break;
      default:                                       return false;
   }
   if (!reqs || reqs->empty()) {
      return false;
   }
   for (const auto& req : *reqs) {
      uint64_t avail = available_inline(op, req.chain, req.token_kind);
      if (avail < req.min_bond) return false;
   }
   return true;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
//  available — read-only rollup
// ---------------------------------------------------------------------------
uint64_t opreg::available(name account, ChainKind chain, TokenKind token_kind) {
   operators_t ops(get_self());
   auto op_pk = operator_key{account.value};
   if (!ops.contains(op_pk)) return 0;
   auto op = ops.get(op_pk);
   return available_inline(op, chain, token_kind);
}

// ---------------------------------------------------------------------------
//  Internal balance mutators
// ---------------------------------------------------------------------------

namespace {

/// Add `amount` to the (chain, token_kind) balance row, creating the row if
/// it doesn't exist. Mutates the operator entry in place — caller is
/// expected to be inside an `ops.modify(...)` lambda.
void add_balance(opreg::operator_entry& o,
                 ChainKind chain, TokenKind token_kind,
                 uint64_t amount) {
   for (auto& b : o.balances) {
      if (b.chain == chain && b.token_kind == token_kind) {
         b.balance         += amount;
         b.last_updated_ms  = current_time_ms();
         return;
      }
   }
   o.balances.push_back(opreg::balance_entry{
      .chain           = chain,
      .token_kind      = token_kind,
      .balance         = amount,
      .last_updated_ms = current_time_ms(),
   });
}

/// Subtract `amount` from the (chain, token_kind) balance row. Caller must
/// have already validated the available balance via `available_inline`.
/// Mutates the operator entry in place — caller is expected to be inside an
/// `ops.modify(...)` lambda.
void subtract_balance(opreg::operator_entry& o,
                      ChainKind chain, TokenKind token_kind,
                      uint64_t amount) {
   for (auto& b : o.balances) {
      if (b.chain == chain && b.token_kind == token_kind) {
         check(b.balance >= amount, "balance underflow");
         b.balance         -= amount;
         b.last_updated_ms  = current_time_ms();
         return;
      }
   }
   check(false, "no matching balance row to subtract from");
}

/// Allocate a fresh request_id from the opcounters singleton.
uint64_t next_withdraw_id() {
   opreg::opcounters_t ctr_tbl(opreg::SYSTEM_ACCOUNT == name{} ? name{} : name{"sysio.opreg"_n});
   opreg::opcounters_t real_ctr(name{"sysio.opreg"_n});
   auto ctr = real_ctr.get_or_default(opreg::op_counters{});
   uint64_t id = ctr.next_withdraw_id;
   ctr.next_withdraw_id = id + 1;
   real_ctr.set(ctr, name{"sysio.opreg"_n});
   return id;
}

uint64_t next_dellog_id() {
   opreg::opcounters_t real_ctr(name{"sysio.opreg"_n});
   auto ctr = real_ctr.get_or_default(opreg::op_counters{});
   uint64_t id = ctr.next_dellog_id;
   ctr.next_dellog_id = id + 1;
   real_ctr.set(ctr, name{"sysio.opreg"_n});
   return id;
}

/// Get the current epoch index from sysio.epoch's epochstate singleton.
/// Returns 0 if epochstate isn't initialized yet (cluster bootstrap).
uint32_t get_current_epoch() {
   sysio::epoch::epochstate_t es(opreg::EPOCH_ACCOUNT);
   if (!es.exists()) return 0;
   return es.get().current_epoch_index;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
//  Outbound attestation encoders + audit-log helpers
// ---------------------------------------------------------------------------

namespace {

/// Pull the raw key bytes out of a `sysio::public_key` variant. K1 (0), R1 (1),
/// and EM (3) share the same 33-byte compressed `ecc_public_key` layout
/// (`std::array<char, 33>`); ED (Solana / Ed25519, index 4) is 32 bytes
/// (`std::array<unsigned char, 32>`). Other variant arms (WebAuthn = 2,
/// BLS = 6) are not part of the operator-collateral flow — we drop those by
/// returning an empty vector, which then fails the `bypubkey` lookup on the

/// Look up `account`'s registered public key for `chain` from
/// `sysio.authex::links` (`bynamechain` index) and pack it into a
/// `ChainAddress`. Returns `{kind, []}` when no authex link exists — the
/// downstream outpost / depot lookup will then fail gracefully (the depot's
/// `dispatch_operator_action` rejects empty `op_address.address`).
opp::types::ChainAddress operator_chain_address(name account, ChainKind chain) {
   authex::links_t links(opreg::AUTHEX_ACCOUNT);
   auto idx = links.get_index<"bynamechain"_n>();
   uint128_t key = to_namechain_key(account, chain);

   opp::types::ChainAddress addr;
   addr.kind = chain;
   auto it = idx.find(key);
   if (it != idx.end()) {
      addr.address = pubkey_to_bytes(it->pub_key);
   }
   return addr;
}

/// Build the `OperatorAction(action_type=SLASH)` payload for a given
/// (account, chain, token_kind) slash. Returns the OperatorAction ready
/// for either logging on the operator's row or queueing as an outbound
/// OPERATOR_ACTION attestation. Pure — no side effects.
///
/// LP routing for the slashed funds is depot-side concern resolved via
/// `sysio.reserve::resolve_lp` at slash-handler time; outposts on receipt
/// only need to seize their share, so the attestation does not encode it.
/// The `OperatorActionLog.timestamp` covers the audit trail; once an
/// operator is slashed all subsequent OperatorActions are dropped+logged
/// as failures, so per-action epoch is redundant on the message itself.
OperatorAction build_slash_action(name account,
                                  OperatorType type,
                                  ChainKind chain,
                                  TokenKind token_kind,
                                  uint64_t amount,
                                  const std::string& reason) {
   OperatorAction oa;
   oa.action_type = OperatorAction::ACTION_TYPE_SLASH;
   oa.op_address  = operator_chain_address(account, chain);
   oa.type        = type;
   opp::types::TokenAmount ta;
   ta.kind   = token_kind;
   ta.amount = zpp::bits::vint64_t{static_cast<int64_t>(amount)};
   oa.amount      = ta;
   oa.chain       = chain;
   oa.reason      = reason;
   return oa;
}

/// Queue an OPERATOR_ACTION(SLASH) attestation outbound to the outpost
/// matching `chain`. No-op if the chain is WIRE (slashed funds stay on
/// the WIRE chain) or has no registered outpost.
void emit_slash_attestation(name self, const OperatorAction& slash_action) {
   if (slash_action.chain == ChainKind::CHAIN_KIND_WIRE) return;
   auto outpost_id = find_outpost_id_for_chain(slash_action.chain);
   if (!outpost_id) return;   // no outpost on this chain — nothing to slash through

   // `no_size{}` — raw protobuf bytes, no 4-byte zpp length prefix. The
   // outpost decodes the attestation `data` field as a pure protobuf
   // message; a size prefix would corrupt the first field tag.
   std::vector<char> encoded;
   auto out = zpp::bits::out{encoded, zpp::bits::no_size{}};
   (void)out(slash_action);

   action(
      permission_level{self, "active"_n},
      opreg::MSGCH_ACCOUNT, "queueout"_n,
      std::make_tuple(*outpost_id,
         AttestationType::ATTESTATION_TYPE_OPERATOR_ACTION, encoded)
   ).send();
}

/// Queue a DEPOSIT_REVERT attestation outbound to the source outpost so
/// escrowed funds get refunded to the depositor (minus the outpost-side
/// gas penalty, computed locally on the outpost when the revert is
/// processed). Called by `opreg::depositinle` whenever validation rejects
/// an inbound DEPOSIT_REQUEST.
void emit_deposit_revert(name self,
                         ChainKind source_chain,
                         const opp::types::ChainAddress& depositor,
                         TokenKind token_kind,
                         uint64_t amount,
                         const checksum256& original_message_id,
                         const std::string& reason) {
   auto outpost_id = find_outpost_id_for_chain(source_chain);
   if (!outpost_id) return;     // no outpost on this chain — nothing to refund through

   opp::attestations::DepositRevert dr;
   dr.depositor = depositor;
   opp::types::TokenAmount ta;
   ta.kind   = token_kind;
   ta.amount = zpp::bits::vint64_t{static_cast<int64_t>(amount)};
   dr.refund_amount = ta;
   const auto& mh = original_message_id.extract_as_byte_array();
   dr.original_deposit_message_id.assign(mh.begin(), mh.end());
   dr.reason = reason;

   // `no_size{}` — see emit_slash_attestation for the rationale.
   std::vector<char> encoded;
   auto out = zpp::bits::out{encoded, zpp::bits::no_size{}};
   (void)out(dr);

   action(
      permission_level{self, "active"_n},
      opreg::MSGCH_ACCOUNT, "queueout"_n,
      std::make_tuple(*outpost_id,
         AttestationType::ATTESTATION_TYPE_DEPOSIT_REVERT, encoded)
   ).send();
}

/// Append an OperatorActionLog entry to the operator's `recent_actions`
/// ring buffer (newest at the back). When the buffer is at MAX_RECENT_ACTIONS,
/// drops the oldest (front) before appending. Caps `error_message` at
/// MAX_ERROR_MESSAGE_BYTES so a noisy failure can't grow the row unbounded.
///
/// Caller passes the operator's primary key + the OperatorAction payload
/// (DEPOSIT_REQUEST / WITHDRAW_REQUEST / WITHDRAW_REMIT / SLASH) plus the
/// outcome. No-op if the operator entry doesn't exist (unknown-operator
/// path handles its own audit via DEPOSIT_REVERT outbound).
void append_action_log(opreg::operators_t& ops,
                       const opreg::operator_key& op_pk,
                       const OperatorAction& action,
                       bool success,
                       std::string error_message) {
   if (!ops.contains(op_pk)) return;

   if (error_message.size() > opreg::MAX_ERROR_MESSAGE_BYTES) {
      error_message.resize(opreg::MAX_ERROR_MESSAGE_BYTES);
   }

   ops.modify(same_payer, op_pk, [&](auto& o) {
      OperatorActionLog log_entry;
      log_entry.action        = action;
      log_entry.success       = success;
      log_entry.timestamp     = current_time_point().sec_since_epoch();
      log_entry.error_message = std::move(error_message);

      if (o.recent_actions.size() >= opreg::MAX_RECENT_ACTIONS) {
         // Drop oldest (front) before appending the newest at the back.
         o.recent_actions.erase(o.recent_actions.begin());
      }
      o.recent_actions.push_back(std::move(log_entry));
   });
}

/// Encode + queue an OPERATOR_ACTION(WITHDRAW_REMIT) attestation to the
/// outpost matching `chain`. `op_address` carries the operator's authex-
/// linked chain pubkey so the outpost can derive the destination address.
void emit_withdraw_remit(name self,
                         name account,
                         OperatorType type,
                         ChainKind chain,
                         TokenKind token_kind,
                         uint64_t amount,
                         uint64_t request_id) {
   auto outpost_id = find_outpost_id_for_chain(chain);
   if (!outpost_id) return;

   OperatorAction oa;
   oa.action_type = OperatorAction::ACTION_TYPE_WITHDRAW_REMIT;
   oa.op_address  = operator_chain_address(account, chain);
   oa.type        = type;
   opp::types::TokenAmount ta;
   ta.kind   = token_kind;
   ta.amount = zpp::bits::vint64_t{static_cast<int64_t>(amount)};
   oa.amount = ta;
   oa.request_id = request_id;
   oa.chain      = chain;

   // `no_size{}` — see emit_slash_attestation for the rationale.
   std::vector<char> encoded;
   auto out = zpp::bits::out{encoded, zpp::bits::no_size{}};
   (void)out(oa);

   action(
      permission_level{self, "active"_n},
      opreg::MSGCH_ACCOUNT, "queueout"_n,
      std::make_tuple(*outpost_id,
         AttestationType::ATTESTATION_TYPE_OPERATOR_ACTION, encoded)
   ).send();
}

} // anonymous namespace

// ---------------------------------------------------------------------------
//  Withdraw queue helpers
// ---------------------------------------------------------------------------

namespace {

/// Result of `try_enqueue_withdraw` — non-throwing variant for the
/// msgch-dispatched `withdrawinle` path so failures get logged on the
/// operator's row instead of reverting the inbound dispatch tx.
struct enqueue_result {
   bool        success;
   uint64_t    request_id;   // valid only when success == true
   std::string error_message;
};

/// Validate + insert a `wtdwqueue` row. Does NOT throw on validation
/// failure — returns the diagnostic in `enqueue_result`. Used by both
/// `withdrawinle` (msgch-dispatched, log-don't-revert) and `withdraw`
/// (operator-callable WIRE-direct, also log-don't-revert).
enqueue_result try_enqueue_withdraw(name account, ChainKind chain, TokenKind token_kind,
                                    uint64_t amount) {
   if (amount == 0) {
      return { false, 0, "amount must be positive" };
   }

   opreg::operators_t ops(name{"sysio.opreg"_n});
   auto op_pk = opreg::operator_key{account.value};
   if (!ops.contains(op_pk)) {
      return { false, 0, "operator not registered" };
   }
   auto op = ops.get(op_pk);
   if (op.status != OperatorStatus::OPERATOR_STATUS_ACTIVE &&
       op.status != OperatorStatus::OPERATOR_STATUS_UNKNOWN) {
      return { false, 0, "operator not in a withdraw-eligible state" };
   }

   uint64_t avail = available_inline(op, chain, token_kind);
   if (avail < amount) {
      return { false, 0, "insufficient available balance for withdraw" };
   }

   uint32_t now_ep = get_current_epoch();
   uint64_t request_id = next_withdraw_id();

   opreg::wtdwqueue_t queue(name{"sysio.opreg"_n});
   queue.emplace(name{"sysio.opreg"_n}, opreg::withdraw_key{request_id}, opreg::withdraw_request{
      .request_id          = request_id,
      .account             = account,
      .chain               = chain,
      .token_kind          = token_kind,
      .amount              = amount,
      .eligible_at_epoch   = now_ep + opreg::WITHDRAW_WAIT_EPOCHS,
      .requested_at_epoch  = now_ep,
   });
   return { true, request_id, "" };
}

/// Build an OperatorAction(WITHDRAW_REQUEST) payload for the log on a
/// withdraw call. `request_id` is 0 if the request was rejected before
/// allocation (the assigned id when accepted lives on the wtdwqueue row).
OperatorAction build_withdraw_request_action(name account,
                                             ChainKind chain,
                                             TokenKind token_kind,
                                             uint64_t amount,
                                             uint64_t request_id) {
   OperatorAction oa;
   oa.action_type = OperatorAction::ACTION_TYPE_WITHDRAW_REQUEST;
   oa.op_address  = operator_chain_address(account, chain);
   oa.chain       = chain;
   opp::types::TokenAmount ta;
   ta.kind   = token_kind;
   ta.amount = zpp::bits::vint64_t{static_cast<int64_t>(amount)};
   oa.amount      = ta;
   oa.request_id  = request_id;
   return oa;
}

/// Build an OperatorAction(WITHDRAW_REMIT) payload for the log when
/// `flushwtdw` matures a queue row. Mirror of build_withdraw_request_action
/// shape, with action_type=REMIT and the request_id of the matured row.
OperatorAction build_withdraw_remit_action(name account,
                                           ChainKind chain,
                                           TokenKind token_kind,
                                           uint64_t amount,
                                           uint64_t request_id) {
   OperatorAction oa;
   oa.action_type = OperatorAction::ACTION_TYPE_WITHDRAW_REMIT;
   oa.op_address  = operator_chain_address(account, chain);
   oa.chain       = chain;
   opp::types::TokenAmount ta;
   ta.kind   = token_kind;
   ta.amount = zpp::bits::vint64_t{static_cast<int64_t>(amount)};
   oa.amount      = ta;
   oa.request_id  = request_id;
   return oa;
}

/// Build an OperatorAction(DEPOSIT_REQUEST) payload for the log on a deposit
/// call. `op_address` carries the operator's chain pubkey as encoded by the
/// caller (outpost side has it from OPPInbound's roster cache; depot-direct
/// WIRE deposit looks it up locally from authex::links). Pure — no side
/// effects.
OperatorAction build_deposit_action(const opp::types::ChainAddress& op_address,
                                    ChainKind chain,
                                    TokenKind token_kind,
                                    uint64_t amount) {
   OperatorAction oa;
   oa.action_type = OperatorAction::ACTION_TYPE_DEPOSIT_REQUEST;
   oa.op_address  = op_address;
   oa.chain       = chain;
   opp::types::TokenAmount ta;
   ta.kind   = token_kind;
   ta.amount = zpp::bits::vint64_t{static_cast<int64_t>(amount)};
   oa.amount      = ta;
   return oa;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
//  withdraw — operator-callable WIRE-direct collateral withdraw (queued)
// ---------------------------------------------------------------------------
//
// Operator-authorized; queues a (chain=WIRE, token=WIRE) row in the
// withdraw queue subject to WITHDRAW_WAIT_EPOCHS maturation. Validation
// failures DO NOT revert — they append a failure entry to the operator's
// `recent_actions` ring buffer, matching the OPP-dispatched path
// (`withdrawinle`). The operator reads the outcome via WIRE JSON-RPC.
// Outpost-held collateral is withdrawn by calling the holding outpost's
// withdraw entry point — that path arrives at `withdrawinle` instead.
void opreg::withdraw(name account, uint64_t amount) {
   require_auth(account);

   operators_t ops(get_self());
   auto op_pk = operator_key{account.value};

   auto result = try_enqueue_withdraw(account, ChainKind::CHAIN_KIND_WIRE,
                                      TokenKind::TOKEN_KIND_WIRE, amount);
   auto action = build_withdraw_request_action(account, ChainKind::CHAIN_KIND_WIRE,
                                               TokenKind::TOKEN_KIND_WIRE, amount,
                                               result.request_id);
   append_action_log(ops, op_pk, action, result.success, std::move(result.error_message));
}

// ---------------------------------------------------------------------------
//  withdrawinle — internal: outpost-driven withdraw request (msgch-inline)
// ---------------------------------------------------------------------------
//
// Inline-dispatched from `sysio.msgch::evalcons` for inbound
// OPERATOR_ACTION(WITHDRAW_REQUEST) attestations. Validation failures DO
// NOT revert (revert would kill the entire envelope's dispatch); the
// outcome is appended to the operator's `recent_actions` ring so they
// can read why their request was dropped via WIRE JSON-RPC. Escrowed
// funds stay in outpost custody on rejection — the operator re-issues
// once the underlying condition resolves.
void opreg::withdrawinle(name account,
                         opp::types::ChainKind chain,
                         opp::types::TokenKind token_kind,
                         uint64_t amount) {
   require_auth(get_self());

   operators_t ops(get_self());
   auto op_pk = operator_key{account.value};

   auto result = try_enqueue_withdraw(account, chain, token_kind, amount);
   auto action = build_withdraw_request_action(account, chain, token_kind, amount,
                                               result.request_id);
   append_action_log(ops, op_pk, action, result.success, std::move(result.error_message));
}

// ---------------------------------------------------------------------------
//  cancelwtdw — operator cancels a queued withdraw before it flushes
// ---------------------------------------------------------------------------
void opreg::cancelwtdw(name account, uint64_t request_id) {
   require_auth(account);
   wtdwqueue_t queue(get_self());
   auto wkey = withdraw_key{request_id};
   auto row = queue.get(wkey, "withdraw request not found");
   check(row.account == account, "not your withdraw request");
   queue.erase(wkey);
}

// ---------------------------------------------------------------------------
//  Eligibility re-check helper — invoked at the end of deposit/depositinle
// ---------------------------------------------------------------------------
namespace {

/// After a balance change, re-evaluate whether the operator now meets the
/// minimum-collateral threshold for their type. If the eligibility flipped
/// vs the prior status, fan out to the per-type processor (`processprod` /
/// `processbatch` / `processuw`) which owns the active/standby transition.
void reevaluate_eligibility(opreg::operators_t& ops,
                            const opreg::operator_key& op_pk,
                            name self,
                            name account) {
   opreg::opconfig_t cfg_tbl(self);
   if (!cfg_tbl.exists()) return;
   auto cfg = cfg_tbl.get();
   auto refreshed = ops.get(op_pk);
   bool was_eligible = (refreshed.status == OperatorStatus::OPERATOR_STATUS_ACTIVE);
   bool is_eligible  = meets_role_min(refreshed, cfg);
   if (was_eligible == is_eligible) return;

   name handler;
   switch (refreshed.type) {
      case OperatorType::OPERATOR_TYPE_PRODUCER:    handler = "processprod"_n;  break;
      case OperatorType::OPERATOR_TYPE_BATCH:       handler = "processbatch"_n; break;
      case OperatorType::OPERATOR_TYPE_UNDERWRITER: handler = "processuw"_n;    break;
      default:                                       return;
   }
   action(
      permission_level{self, "active"_n},
      self, handler,
      std::make_tuple(account, was_eligible, is_eligible)
   ).send();
}

} // anonymous namespace

// ---------------------------------------------------------------------------
//  deposit — operator-callable WIRE-direct collateral deposit
// ---------------------------------------------------------------------------
//
// Operator-authorized; reverts on validation failure so the operator's
// signing tx surfaces the diagnostic immediately. There's no escrow yet
// (the operator hasn't transferred funds), so revert is the right
// failure mode — they retry after fixing whatever was wrong (e.g.,
// re-bootstrap their authex links). On success the matching balance row
// is credited, the action is appended to the operator's `recent_actions`
// ring buffer, and the eligibility transition (if any) is fanned out.
void opreg::deposit(name account, uint64_t amount) {
   require_auth(account);
   check(amount > 0, "amount must be positive");

   operators_t ops(get_self());
   auto op_pk = operator_key{account.value};
   auto op = ops.get(op_pk, "operator not found");
   check(op.status != OperatorStatus::OPERATOR_STATUS_SLASHED &&
         op.status != OperatorStatus::OPERATOR_STATUS_TERMINATED,
         "operator not in a deposit-eligible state");

   // Direct WIRE token transfer from operator -> opreg.
   action(
      permission_level{account, "active"_n},
      TOKEN_ACCOUNT, "transfer"_n,
      std::make_tuple(account, get_self(),
         asset(static_cast<int64_t>(amount), CORE_SYM),
         std::string("opreg::deposit"))
   ).send();

   ops.modify(same_payer, op_pk, [&](auto& o) {
      add_balance(o, ChainKind::CHAIN_KIND_WIRE, TokenKind::TOKEN_KIND_WIRE, amount);
   });

   auto deposit_action = build_deposit_action(
      operator_chain_address(account, ChainKind::CHAIN_KIND_WIRE),
      ChainKind::CHAIN_KIND_WIRE, TokenKind::TOKEN_KIND_WIRE, amount);
   append_action_log(ops, op_pk, deposit_action, /*success*/ true, "");

   reevaluate_eligibility(ops, op_pk, get_self(), account);
}

// ---------------------------------------------------------------------------
//  depositinle — internal: outpost-driven collateral credit (msgch-inline)
// ---------------------------------------------------------------------------
//
// Inline-dispatched from `sysio.msgch::evalcons` for inbound
// OPERATOR_ACTION(DEPOSIT_REQUEST) attestations. Validation failures DO
// NOT revert — reverting from inside the inline dispatch would abort the
// entire envelope. Instead, the failure is logged on the operator's
// `recent_actions` ring (when an entry exists) and a `DEPOSIT_REVERT`
// attestation is queued outbound to the source outpost so the escrowed
// funds can be refunded to the depositor (minus the outpost-side gas
// penalty, computed locally on the outpost when the revert is processed).
void opreg::depositinle(name account,
                        opp::types::ChainKind chain,
                        opp::types::TokenKind token_kind,
                        uint64_t amount,
                        opp::types::ChainKind actor_chain,
                        std::vector<char> actor_address,
                        checksum256 original_message_id) {
   require_auth(get_self());

   operators_t ops(get_self());
   auto op_pk = operator_key{account.value};

   // Reconstruct the depositor's ChainAddress locally — the proto message
   // type stays out of the ABI but is fine to use inside the contract for
   // building OPERATOR_ACTION logs and DEPOSIT_REVERT correlation.
   opp::types::ChainAddress actor;
   actor.kind    = actor_chain;
   actor.address = std::move(actor_address);

   auto deposit_action = build_deposit_action(actor, chain, token_kind, amount);

   if (amount == 0) {
      const std::string err = "amount must be positive";
      emit_deposit_revert(get_self(), chain, actor, token_kind, amount,
                          original_message_id, err);
      append_action_log(ops, op_pk, deposit_action, false, err);
      return;
   }
   if (!ops.contains(op_pk)) {
      // No entry to log to. The DEPOSIT_REVERT IS the audit record for the
      // outpost — outpost emits a local refund event the depositor reads.
      emit_deposit_revert(get_self(), chain, actor, token_kind, amount,
                          original_message_id, "operator not registered");
      return;
   }
   auto op = ops.get(op_pk);
   if (op.status == OperatorStatus::OPERATOR_STATUS_SLASHED ||
       op.status == OperatorStatus::OPERATOR_STATUS_TERMINATED) {
      const std::string err = "operator not in a deposit-eligible state";
      emit_deposit_revert(get_self(), chain, actor, token_kind, amount,
                          original_message_id, err);
      append_action_log(ops, op_pk, deposit_action, false, err);
      return;
   }
   // Bootstrapped operators are bonded by fiat — no deposit is ever
   // permitted for them. Reject via DEPOSIT_REVERT (NOT a throw, per
   // the no-throws-in-OPP-handlers rule — a throw here would halt the
   // entire envelope's evalcons → chain stalls). The outpost refunds
   // the depositor when it processes the revert.
   if (op.is_bootstrapped) {
      const std::string err = "bootstrapped operator cannot accept deposits";
      emit_deposit_revert(get_self(), chain, actor, token_kind, amount,
                          original_message_id, err);
      append_action_log(ops, op_pk, deposit_action, false, err);
      return;
   }

   ops.modify(same_payer, op_pk, [&](auto& o) {
      add_balance(o, chain, token_kind, amount);
   });
   append_action_log(ops, op_pk, deposit_action, true, "");

   reevaluate_eligibility(ops, op_pk, get_self(), account);
}

// ---------------------------------------------------------------------------
//  flushwtdw — drain matured rows from the withdraw queue
// ---------------------------------------------------------------------------
void opreg::flushwtdw(uint32_t current_epoch) {
   require_auth(EPOCH_ACCOUNT);

   operators_t ops(get_self());
   wtdwqueue_t queue(get_self());
   auto idx = queue.get_index<"byeligible"_n>();

   // Iterate matured rows. Erase as we go, hence the manual cursor.
   auto it = idx.begin();
   while (it != idx.end() && it->eligible_at_epoch <= current_epoch) {
      auto row     = *it;          // copy out before erase
      auto wkey    = withdraw_key{row.request_id};
      // Advance index iterator BEFORE erasing the row.
      ++it;

      auto op_pk = operator_key{row.account.value};
      // Per-row outcome lands in the operator's recent_actions log so the
      // operator can read flush failures (slashed during wait, defensive
      // rollup mismatches) via JSON-RPC.
      auto remit_action = build_withdraw_remit_action(row.account, row.chain,
                                                     row.token_kind, row.amount,
                                                     row.request_id);

      if (!ops.contains(op_pk)) {
         // Operator entry was removed between queue + flush — nowhere to log.
         queue.erase(wkey);
         continue;
      }
      auto op = ops.get(op_pk);

      if (op.status == OperatorStatus::OPERATOR_STATUS_SLASHED) {
         // Slashed during the wait — funds went to the LP via the slash flow.
         append_action_log(ops, op_pk, remit_action, false,
                           "operator slashed during withdraw-wait window");
         queue.erase(wkey);
         continue;
      }

      // Re-validate available — should still cover (since available()
      // subtracts pending withdraws), but a state shift is possible.
      uint64_t avail_excluding_self = available_inline(op, row.chain, row.token_kind) + row.amount;
      if (avail_excluding_self < row.amount) {
         append_action_log(ops, op_pk, remit_action, false,
                           "insufficient available balance at flush (rollup mismatch)");
         queue.erase(wkey);
         continue;
      }

      // Subtract from balance.
      ops.modify(same_payer, op_pk, [&](auto& o) {
         subtract_balance(o, row.chain, row.token_kind, row.amount);
      });

      // For WIRE-direct: do the token transfer back inline. For outpost
      // chains: queue an OPERATOR_ACTION(WITHDRAW_REMIT) to the outpost
      // so it can release the escrow on its end.
      if (row.chain == ChainKind::CHAIN_KIND_WIRE) {
         action(
            permission_level{get_self(), "active"_n},
            TOKEN_ACCOUNT, "transfer"_n,
            std::make_tuple(get_self(), row.account,
               asset(static_cast<int64_t>(row.amount), CORE_SYM),
               std::string("opreg::withdraw flush"))
         ).send();
      } else {
         emit_withdraw_remit(get_self(), row.account, op.type,
                             row.chain, row.token_kind, row.amount, row.request_id);
      }
      append_action_log(ops, op_pk, remit_action, true, "");

      // Re-check eligibility — this withdraw may have dropped the operator
      // below the role minimum.
      reevaluate_eligibility(ops, op_pk, get_self(), row.account);

      queue.erase(wkey);
   }
}

// ---------------------------------------------------------------------------
//  processprod / processbatch / processuw — eligibility transitions
// ---------------------------------------------------------------------------

namespace {

/// Common body for the three eligibility callbacks. Producers additionally
/// notify SYSTEM_ACCOUNT so the system contract sees their availability flip.
void process_eligibility_change(name self, name account,
                                bool was_eligible, bool is_eligible,
                                bool notify_system) {
   opreg::operators_t ops(self);
   auto op_pk = opreg::operator_key{account.value};
   check(ops.contains(op_pk), "operator not found");

   auto now = current_time_ms();
   if (!was_eligible && is_eligible) {
      ops.modify(same_payer, op_pk, [&](auto& o) {
         o.status       = OperatorStatus::OPERATOR_STATUS_ACTIVE;
         o.available_at = now;
      });
      if (notify_system) {
         require_recipient(opreg::SYSTEM_ACCOUNT);
      }
   } else if (was_eligible && !is_eligible) {
      ops.modify(same_payer, op_pk, [&](auto& o) {
         o.status = OperatorStatus::OPERATOR_STATUS_UNKNOWN;
      });
   }
}

} // anonymous namespace

void opreg::processprod(name account, bool was_eligible, bool is_eligible) {
   require_auth(get_self());
   process_eligibility_change(get_self(), account, was_eligible, is_eligible, /*notify_system*/ true);
}

void opreg::processbatch(name account, bool was_eligible, bool is_eligible) {
   require_auth(get_self());
   process_eligibility_change(get_self(), account, was_eligible, is_eligible, /*notify_system*/ false);
}

void opreg::processuw(name account, bool was_eligible, bool is_eligible) {
   require_auth(get_self());
   process_eligibility_change(get_self(), account, was_eligible, is_eligible, /*notify_system*/ false);
}

// ---------------------------------------------------------------------------
//  slash — punitive removal; routes unlocked funds to LP, defers locked
// ---------------------------------------------------------------------------
void opreg::slash(name account, std::string reason) {
   require_auth(CHALG_ACCOUNT);

   operators_t ops(get_self());
   auto op_pk = operator_key{account.value};
   auto op = ops.get(op_pk, "operator not found");
   check(op.status != OperatorStatus::OPERATOR_STATUS_SLASHED,
         "operator already slashed");
   check(op.status != OperatorStatus::OPERATOR_STATUS_TERMINATED,
         "operator already terminated");

   uint32_t now_ep = get_current_epoch();
   auto now = current_time_ms();

   // Snapshot the slashable amounts per (chain, token_kind) BEFORE marking
   // SLASHED (the status flip would zero `slashable_now` via available()).
   struct slash_pair { ChainKind chain; TokenKind token_kind; uint64_t amount; };
   std::vector<slash_pair> to_slash;
   for (const auto& bal : op.balances) {
      uint64_t amt = slashable_now(op, bal.chain, bal.token_kind);
      if (amt > 0) {
         to_slash.push_back({bal.chain, bal.token_kind, amt});
      }
   }

   // Flip status + decrement balances by the slashable_now portion.
   // Locked portion (== sum_locks) remains in `balance`; sysio.uwrit::release
   // will deferred-slash it as each lock resolves.
   ops.modify(same_payer, op_pk, [&](auto& o) {
      o.status        = OperatorStatus::OPERATOR_STATUS_SLASHED;
      o.updated_at    = now;
      o.status_reason = reason;
      for (const auto& sp : to_slash) {
         subtract_balance(o, sp.chain, sp.token_kind, sp.amount);
      }
   });

   // Emit one OPERATOR_ACTION(SLASH) per (chain, token_kind) with non-zero
   // slashable, AND append each as a recent_actions log entry on the
   // operator's row (success=true since the slash itself was applied).
   for (const auto& sp : to_slash) {
      auto slash_action = build_slash_action(op.account, op.type,
                                             sp.chain, sp.token_kind, sp.amount,
                                             reason);
      emit_slash_attestation(get_self(), slash_action);
      append_action_log(ops, op_pk, slash_action, /*success*/ true, "");
   }
}

// ---------------------------------------------------------------------------
//  releaselock — deferred-slash / deferred-remit / no-op on lock release
// ---------------------------------------------------------------------------
void opreg::releaselock(name account,
                        opp::types::ChainKind chain,
                        opp::types::TokenKind token_kind,
                        uint64_t amount) {
   require_auth(UWRIT_ACCOUNT);
   check(amount > 0, "amount must be positive");

   operators_t ops(get_self());
   auto op_pk = operator_key{account.value};
   if (!ops.contains(op_pk)) return;
   auto op = ops.get(op_pk);

   if (op.status != OperatorStatus::OPERATOR_STATUS_SLASHED &&
       op.status != OperatorStatus::OPERATOR_STATUS_TERMINATED) {
      // Healthy underwriter: balance was never decremented at lock time.
      // uwrit::release just erases the lock row; opreg has no work.
      return;
   }

   // SLASHED or TERMINATED — decrement opreg balance and emit the matching
   // outbound attestation (deferred-slash to LP or deferred-remit to authex).
   ops.modify(same_payer, op_pk, [&](auto& o) {
      subtract_balance(o, chain, token_kind, amount);
   });

   if (op.status == OperatorStatus::OPERATOR_STATUS_SLASHED) {
      auto slash_action = build_slash_action(op.account, op.type,
                                             chain, token_kind, amount,
                                             /*reason*/ "deferred slash on lock release");
      emit_slash_attestation(get_self(), slash_action);
      append_action_log(ops, op_pk, slash_action, /*success*/ true, "");
   } else {
      // TERMINATED — for WIRE-direct, transfer back to operator; otherwise
      // queue WITHDRAW_REMIT so the outpost can transfer to the authex
      // destination. request_id == 0 (this remit isn't queued in wtdwqueue).
      if (chain == ChainKind::CHAIN_KIND_WIRE) {
         action(
            permission_level{get_self(), "active"_n},
            TOKEN_ACCOUNT, "transfer"_n,
            std::make_tuple(get_self(), account,
               asset(static_cast<int64_t>(amount), CORE_SYM),
               std::string("terminate-deferred-remit"))
         ).send();
      } else {
         emit_withdraw_remit(get_self(), op.account, op.type,
                             chain, token_kind, amount, /*request_id*/ 0);
      }
   }
}

// ---------------------------------------------------------------------------
//  terminate / termcheck / recorddel — administrative removal
// ---------------------------------------------------------------------------

namespace {

/// Internal terminate body — used by both the operator-removal path
/// (`termcheck` -> `terminate` inline) and the slashing-equivalent path for
/// completeness. Marks status TERMINATED and remits each (chain, token_kind)
/// balance back to the operator via WITHDRAW_REMIT.
void terminate_inline(name self, name account, const std::string& reason) {
   opreg::operators_t ops(self);
   auto op_pk = opreg::operator_key{account.value};
   auto op = ops.get(op_pk, "operator not found");
   check(op.status == OperatorStatus::OPERATOR_STATUS_ACTIVE ||
         op.status == OperatorStatus::OPERATOR_STATUS_UNKNOWN,
         "operator not in a terminable state");

   auto now = current_time_ms();

   // Snapshot the remitable amounts BEFORE flipping status.
   struct remit_pair { ChainKind chain; TokenKind token_kind; uint64_t amount; };
   std::vector<remit_pair> to_remit;
   for (const auto& bal : op.balances) {
      uint64_t amt = slashable_now(op, bal.chain, bal.token_kind);
      // For termination we route the unlocked portion. The locked portion
      // gets remitted at lock-release time by sysio.uwrit::release (deferred-
      // remit, symmetric with deferred-slash).
      if (amt > 0) {
         to_remit.push_back({bal.chain, bal.token_kind, amt});
      }
   }

   ops.modify(same_payer, op_pk, [&](auto& o) {
      o.status        = OperatorStatus::OPERATOR_STATUS_TERMINATED;
      o.terminated_at = now;
      o.status_reason = reason;
      for (const auto& rp : to_remit) {
         subtract_balance(o, rp.chain, rp.token_kind, rp.amount);
      }
   });

   // Remit each (chain, token_kind). For WIRE-chain: direct token transfer
   // back to the operator. For outpost chains: queue WITHDRAW_REMIT.
   //
   // After each remit, append a WITHDRAW_REMIT entry to the operator's
   // `recent_actions` ring buffer so the audit trail mirrors the
   // operator-initiated withdraw flow (`flushwtdw` does the same at line
   // ~1008). Without this entry, downstream consumers polling
   // `operators[op].recent_actions` for proof of remit emission see only
   // the prior DEPOSIT_REQUESTs and miss the termination payout — same
   // semantic gap on TERMINATED ops as on a normal queued withdraw, only
   // resolvable by querying msgch internals (which are transient — the
   // rows drain on the next `buildenv`).
   for (const auto& rp : to_remit) {
      if (rp.chain == ChainKind::CHAIN_KIND_WIRE) {
         action(
            permission_level{self, "active"_n},
            opreg::TOKEN_ACCOUNT, "transfer"_n,
            std::make_tuple(self, account,
               asset(static_cast<int64_t>(rp.amount), opreg::CORE_SYM),
               std::string("terminate-remit"))
         ).send();
      } else {
         emit_withdraw_remit(self, account, op.type,
                             rp.chain, rp.token_kind, rp.amount, /*request_id*/ 0);
      }
      OperatorAction remit_action = build_withdraw_remit_action(
         account, rp.chain, rp.token_kind, rp.amount, /*request_id*/ 0);
      append_action_log(ops, op_pk, remit_action, /*success*/ true,
                        std::string("terminate-remit"));
   }
}

} // anonymous namespace

void opreg::terminate(name account, std::string reason) {
   require_auth(get_self());
   terminate_inline(get_self(), account, reason);
}

void opreg::recorddel(name account, uint32_t epoch, bool delivered) {
   require_auth(EPOCH_ACCOUNT);

   dellog_t log(get_self());
   uint64_t id = next_dellog_id();
   log.emplace(get_self(), delivery_key{id}, delivery_log_entry{
      .log_id    = id,
      .account   = account,
      .epoch     = epoch,
      .delivered = delivered,
      .ts_ms     = current_time_ms(),
   });
}

void opreg::termcheck(name account) {
   require_auth(EPOCH_ACCOUNT);

   operators_t ops(get_self());
   auto op_pk = operator_key{account.value};
   if (!ops.contains(op_pk)) return;
   auto op = ops.get(op_pk);
   if (op.status != OperatorStatus::OPERATOR_STATUS_ACTIVE) return;
   // Bootstrapped operators are the genesis / chain-of-trust seed set and
   // are NEVER subject to rolling-window termination — see
   // .claude/rules/bootstrapped-operator-invariants.md at the wire root. A
   // transient bug in the deliver / consensus / advance pipeline (or a
   // benign operator-side outage) must not be able to tear the
   // bootstrapped seed set down, because doing so drops the chain below
   // `batch_operator_minimum_active` with no remaining ACTIVE operators
   // to advance consensus and no recovery path.
   if (op.is_bootstrapped) return;
   // Termination on rolling-buffer underperformance is, for now, scoped to
   // batch operators. Producer schedule misses + underwriter offline-too-long
   // are open questions per the plan §1; revisit when those decisions land.
   if (op.type != OperatorType::OPERATOR_TYPE_BATCH) return;

   // Thresholds come from opconfig — tests can dial them down so the
   // miss-window evaluation fits the test timeout budget.
   opconfig_t cfg_tbl(get_self());
   const auto cfg = cfg_tbl.get_or_default(op_config{});

   uint64_t now_ms      = current_time_ms();
   uint64_t window_open = now_ms > cfg.terminate_window_ms ? now_ms - cfg.terminate_window_ms : 0;

   dellog_t log(get_self());
   auto idx = log.get_index<"byaccountts"_n>();
   uint128_t lower_key = (static_cast<uint128_t>(account.value) << 64) | window_open;
   uint128_t upper_key = (static_cast<uint128_t>(account.value) << 64) | std::numeric_limits<uint64_t>::max();

   uint32_t consecutive_misses = 0;
   uint32_t worst_consecutive  = 0;
   uint32_t total_misses       = 0;
   uint32_t total_in_window    = 0;
   for (auto it = idx.lower_bound(lower_key); it != idx.end() && it->by_account_ts() <= upper_key; ++it) {
      if (it->account != account) break;
      total_in_window++;
      if (!it->delivered) {
         total_misses++;
         consecutive_misses++;
         if (consecutive_misses > worst_consecutive) worst_consecutive = consecutive_misses;
      } else {
         consecutive_misses = 0;
      }
   }

   bool exceeds_consecutive = worst_consecutive > cfg.terminate_max_consecutive_misses;
   bool exceeds_percent     = total_in_window > 0 &&
                              (total_misses * 100u / total_in_window) > cfg.terminate_max_pct_misses_24h;
   if (exceeds_consecutive || exceeds_percent) {
      terminate_inline(get_self(), account,
         exceeds_consecutive
            ? std::string{"rolling-window: >"}
                 + std::to_string(cfg.terminate_max_consecutive_misses)
                 + " consecutive misses"
            : std::string{"rolling-window: >"}
                 + std::to_string(cfg.terminate_max_pct_misses_24h)
                 + "% miss rate");
   }
}

// ---------------------------------------------------------------------------
//  prune — remove terminated operator rows past the delay
// ---------------------------------------------------------------------------
void opreg::prune() {
   opconfig_t cfg_tbl(get_self());
   check(cfg_tbl.exists(), "opconfig not initialized");
   auto cfg = cfg_tbl.get();

   auto now = current_time_ms();
   operators_t ops(get_self());
   auto status_idx = ops.get_index<"bystatus"_n>();

   uint32_t removed = 0;
   for (auto it = status_idx.lower_bound(
           static_cast<uint64_t>(OperatorStatus::OPERATOR_STATUS_TERMINATED));
        it != status_idx.end() &&
        it->status == OperatorStatus::OPERATOR_STATUS_TERMINATED;) {
      if (it->terminated_at > 0 && now - it->terminated_at >= cfg.terminate_prune_delay_ms) {
         it = status_idx.erase(std::move(it));
         if (++removed >= 20) break; // Bound CPU
      } else {
         ++it;
      }
   }
}

} // namespace sysio

#include <sysio.opreg/sysio.opreg.hpp>
#include <sysio.epoch/sysio.epoch.hpp>
#include <sysio.chains/sysio.chains.hpp>
#include <sysio.authex/sysio.authex.hpp>
#include <sysio.uwrit/sysio.uwrit.hpp>
#include <sysio.opp.common/slug_name.hpp>
#include <sysio.opp.common/safe_ops.hpp>
#include <sysio/opp/attestations/attestations.pb.hpp>
#include <magic_enum/magic_enum.hpp>
#include <zpp_bits.h>
#include <array>
#include <cstring>

namespace sysio {

using opp::types::OperatorType;
using opp::types::OperatorStatus;
using opp::types::AttestationType;
using opp::attestations::OperatorAction;
using opp::attestations::OperatorActionLog;
using opp::attestations::DepositRevert;

namespace {

using namespace sysio::slug_name_literals;

// System-owned rows bill to the sysio RAM pool, not this contract account (privileged-contract
// model, as sysio.token uses): the account stays finite at code+abi size; growth draws from the pool.
constexpr name ram_payer = "sysio"_n;

/// Well-known chain code for the WIRE depot itself. Comparisons of the form
/// `chain == ChainKind::CHAIN_KIND_WIRE` are now `chain_code == kWireChainCode`.
constexpr sysio::slug_name kWireChainCode = "WIRE"_s;

/// Well-known token code for the WIRE-native token. Replaces the historical
/// `TokenKind::TOKEN_KIND_WIRE` discriminant in (chain, token) tuples.
constexpr sysio::slug_name kWireTokenCode = "WIRE"_s;

uint64_t current_time_ms() {
   return static_cast<uint64_t>(current_time_point().sec_since_epoch()) * 1000;
}

/// Compute the composite key matching `withdraw_request::by_account_ck` /
/// `sysio::uwrit::lock_entry::by_underwriter_ck`. Centralized so both the
/// indexer and the lookups stay in lockstep.
///
/// Phase 6 layout: three uint64s — `account.value`, `chain_code.value`,
/// `token_code.value` — packed in that order into a 24-byte buffer and
/// hashed with sha256, producing a `checksum256`. The previous uint128
/// composite layout (account<<64 | chain<<32 | token) is gone; the wider
/// slug_name values no longer fit in 32 bits each.
checksum256 make_account_chain_token_key(name account,
                                         sysio::slug_name chain_code,
                                         sysio::slug_name token_code) {
   std::array<uint8_t, 24> buf{};
   uint64_t acc_v = account.value;
   std::memcpy(buf.data() +  0, &acc_v,            8);
   std::memcpy(buf.data() +  8, &chain_code.value, 8);
   std::memcpy(buf.data() + 16, &token_code.value, 8);
   return sysio::sha256(reinterpret_cast<const char*>(buf.data()), buf.size());
}

/// Find the outpost id registered with sysio.chains for a given chain. Returns
/// `std::nullopt` if no matching chain row exists (the caller is responsible
/// for handling that case — typically by skipping the queueout for chains
/// without an outpost, e.g. WIRE-direct flows).
///
/// Post v6 cross-contract realignment: chain rows live in
/// `sysio.chains::chains` keyed by `code` (slug_name); the legacy
/// `sysio.epoch::outposts` table is gone. The "outpost id" returned here is
/// the chain's `code.value` (uint64) — callers that still expect a small
/// numeric id should use the slug_name value instead.
std::optional<uint64_t> find_outpost_id_for_chain(sysio::slug_name chain_code) {
   sysio::chains::chains_t chains_tbl(name{"sysio.chains"_n});
   sysio::chains::chain_key pk{chain_code};
   if (!chains_tbl.contains(pk)) return std::nullopt;
   const auto row = chains_tbl.get(pk);
   if (row.is_depot) return std::nullopt;   // WIRE-direct flows don't queueout
   return chain_code.value;
}

/// Resolve a `sysio::slug_name` chain identifier to its `ChainKind` enum by
/// reading the `sysio.chains::chains` registry row. Returns `std::nullopt`
/// when no chain row exists for the code — callers treat that as "chain
/// not registered, drop the operation gracefully".
///
/// The `authex::links` table is still keyed by `(account, ChainKind)`
/// (uint128) and `ChainAddress.kind` is still `ChainKind`; this helper is
/// the bridge for opreg's slug_name-typed paths into those legacy surfaces.
std::optional<opp::types::ChainKind> chain_kind_for_code(sysio::slug_name chain_code) {
   sysio::chains::chains_t chains_tbl(name{"sysio.chains"_n});
   sysio::chains::chain_key pk{chain_code};
   if (!chains_tbl.contains(pk)) return std::nullopt;
   return chains_tbl.get(pk).kind;
}

/// Enforce uniqueness of `(chain_code, token_code)` within a collateral-
/// requirements vector. Duplicates would cause the same (chain, token)
/// pair to be checked twice during eligibility evaluation — harmless
/// behaviorally but a clear configuration error worth surfacing at the
/// boundary rather than silently absorbing.
void require_no_duplicate_chain_token(const std::vector<opreg::chain_min_bond>& v,
                                      const char* role_label) {
   for (auto outer = v.begin(); outer != v.end(); ++outer) {
      for (auto inner = std::next(outer); inner != v.end(); ++inner) {
         check(!(outer->chain_code == inner->chain_code &&
                 outer->token_code == inner->token_code),
               std::string(role_label) +
                  ": duplicate (chain_code, token_code) in collateral requirements");
      }
   }
}

/// Reject a zero `min_bond` in any collateral-requirement entry. Eligibility
/// evaluation gates an operator on `available >= req.min_bond`; a zero minimum
/// makes that comparison vacuously true, so an operator could reach ACTIVE with
/// no collateral posted, defeating the bond requirement entirely. An operator
/// type that should carry no requirement is expressed by an empty requirement
/// vector (which makes the type ineligible), never by a zero-valued entry.
void require_positive_min_bond(const std::vector<opreg::chain_min_bond>& v,
                               const char* role_label) {
   for (const auto& entry : v) {
      check(entry.min_bond > 0,
            std::string(role_label) +
               ": min_bond must be positive (an empty requirement set imposes no bond)");
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

   require_positive_min_bond(req_prod_collat,    "req_prod_collat");
   require_positive_min_bond(req_batchop_collat, "req_batchop_collat");
   require_positive_min_bond(req_uw_collat,      "req_uw_collat");

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
   cfg_tbl.set(cfg, ram_payer);
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
   //
   // Post v6 refactor: the outpost set lives in `sysio.chains::chains` keyed
   // by slug_name. The depot self-row (`is_depot == true`) is skipped; only
   // active outpost chains require an authex link. `authex::links.bynamechain`
   // is still keyed by ChainKind (uint128 of (account, ChainKind)), so we
   // pull `kind` off each chain row.
   if (!is_bootstrapped && !has_auth(get_self())) {
      sysio::chains::chains_t chains_tbl(name{"sysio.chains"_n});
      authex::links_t links(AUTHEX_ACCOUNT);
      auto namechain_idx = links.get_index<"bynamechain"_n>();

      for (auto op_it = chains_tbl.begin(); op_it != chains_tbl.end(); ++op_it) {
         if (op_it->is_depot) continue;       // depot self-row carries no outpost
         if (!op_it->active)  continue;       // pre-active chain rows have no expectation yet
         uint128_t composite_key = to_namechain_key(account, op_it->kind);
         auto link_it = namechain_idx.find(composite_key);
         check(link_it != namechain_idx.end(),
               "missing authex link for outpost chain");
      }
   }

   auto now = current_time_ms();
   ops.emplace(ram_payer, op_pk, operator_entry{
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
/// Returns 0 if uwrit's locks table is empty or if the operator has no locks
/// on that chain/token.
///
/// Per v6 plan §B.2 (split-index design): `sysio.uwrit::locks_t` exposes only
/// uint64 secondary indexes. The `byuw` index keys on `underwriter.value`;
/// rows are filtered on `(chain_code, token_code)` in memory. Per-underwriter
/// lock counts are O(1)-ish in steady state so the scan is cheap.
uint64_t sum_locks_inline(name account, sysio::slug_name chain_code, sysio::slug_name token_code) {
   uwrit::locks_t locks(opreg::UWRIT_ACCOUNT);
   auto idx = locks.template get_index<"byuw"_n>();

   uint64_t total = 0;
   auto it  = idx.lower_bound(account.value);
   auto end = idx.upper_bound(account.value);
   for (; it != end; ++it) {
      if (it->chain_code != chain_code || it->token_code != token_code) continue;
      // Saturating: amounts are uncapped uint64 (external-chain values); a
      // wrapped subtotal would understate `reserved` and overstate availability.
      total = opp::safe::add_sat_u64(total, it->amount);
   }
   return total;
}

/// Sum the pending (not-yet-flushed) withdraws on this contract for a given
/// (op, chain, token). Subtracted by `available()` so a queued withdraw
/// effectively reserves the funds for its 2-epoch wait.
///
/// Per v6 plan §B.2 (split-index design): `wtdwqueue_t` exposes only uint64
/// secondary indexes. `byaccount` keys on `account.value`; rows are filtered
/// on `(chain_code, token_code)` in memory. Per-account pending-withdraw
/// counts are O(1)-ish so the scan is cheap.
uint64_t sum_pending_withdraws(name account, sysio::slug_name chain_code, sysio::slug_name token_code) {
   // The queue is scoped to opreg itself; reference the well-known account.
   opreg::wtdwqueue_t real_queue(name{"sysio.opreg"_n});
   auto idx = real_queue.template get_index<"byaccount"_n>();

   uint64_t total = 0;
   auto it  = idx.lower_bound(account.value);
   auto end = idx.upper_bound(account.value);
   for (; it != end; ++it) {
      if (it->chain_code != chain_code || it->token_code != token_code) continue;
      // Saturating: amounts are uncapped uint64 (external-chain values); a
      // wrapped subtotal would understate `reserved` and overstate availability.
      total = opp::safe::add_sat_u64(total, it->amount);
   }
   return total;
}

/// Look up the operator's balance row for a given (chain_code, token_code).
/// Returns nullptr if no row exists.
const opreg::balance_entry*
find_balance(const opreg::operator_entry& op,
             sysio::slug_name chain_code, sysio::slug_name token_code) {
   for (const auto& b : op.balances) {
      if (b.chain_code == chain_code && b.token_code == token_code) return &b;
   }
   return nullptr;
}

/// Compute available balance for a given (op, chain, token). The single
/// rollup formula: balance - sum(active locks) - sum(pending withdraws),
/// gated by status. Slashed / terminated operators read as zero.
uint64_t available_inline(const opreg::operator_entry& op,
                          sysio::slug_name chain_code, sysio::slug_name token_code) {
   if (op.status == OperatorStatus::OPERATOR_STATUS_SLASHED ||
       op.status == OperatorStatus::OPERATOR_STATUS_TERMINATED) {
      return 0;
   }
   const auto* bal = find_balance(op, chain_code, token_code);
   if (!bal) return 0;

   uint64_t locked  = sum_locks_inline(op.account, chain_code, token_code);
   uint64_t pending = sum_pending_withdraws(op.account, chain_code, token_code);
   // Saturating: a wrap of locked+pending would understate `reserved` and
   // overstate availability — the direction that admits an overcommit. The
   // cap is unreachable for real amounts.
   uint64_t reserved = opp::safe::add_sat_u64(locked, pending);
   return bal->balance > reserved ? bal->balance - reserved : 0;
}

/// Balance minus active locks (NOT pending withdraws). Used by `slash()` to
/// determine how much can be slashed immediately — pending withdraws of a
/// slashed operator are forfeit (silently dropped at flush time).
uint64_t slashable_now(const opreg::operator_entry& op,
                       sysio::slug_name chain_code, sysio::slug_name token_code) {
   const auto* bal = find_balance(op, chain_code, token_code);
   if (!bal) return 0;
   uint64_t locked = sum_locks_inline(op.account, chain_code, token_code);
   return bal->balance > locked ? bal->balance - locked : 0;
}

/// Check whether the operator's available balance on (chain_code, token_code)
/// covers the role's minimum bond on that pair.
///
/// Bootstrapped operators are ACTIVE-by-fiat and bypass the per-outpost
/// bond check regardless of how `req_*_collat` is configured — they
/// represent system-installed operators that the depot trusts without
/// requiring collateral. Non-bootstrapped operators must satisfy every
/// `(chain_code, token_code)` entry in the matching `req_*_collat` vector;
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
      uint64_t avail = available_inline(op, req.chain_code, req.token_code);
      if (avail < req.min_bond) return false;
   }
   return true;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
//  available — read-only rollup
// ---------------------------------------------------------------------------
uint64_t opreg::available(name account, sysio::slug_name chain_code, sysio::slug_name token_code) {
   operators_t ops(get_self());
   auto op_pk = operator_key{account.value};
   if (!ops.contains(op_pk)) return 0;
   auto op = ops.get(op_pk);
   return available_inline(op, chain_code, token_code);
}

// ---------------------------------------------------------------------------
//  Internal balance mutators
// ---------------------------------------------------------------------------

namespace {

/// Maximum collateral a single `(chain_code, token_code)` balance row may hold:
/// the Antelope `asset` magnitude limit (`2^62 - 1`). A stored balance above
/// this cannot be carried by the WIRE-frame `asset()` that the withdraw /
/// terminate remit path constructs — `asset()` `check()`-aborts past
/// `asset::max_amount` — so every credit is gated to keep the running sum within
/// range. The WSA-028 ingress gate (`sysio.msgch`) already bounds a *single*
/// inbound amount to this limit; this caps the *accumulation* across deposits
/// (SEC-103).
constexpr uint64_t MAX_COLLATERAL_AMOUNT = static_cast<uint64_t>(asset::max_amount);

/// Current stored balance of the `(chain_code, token_code)` row, or 0 when the
/// operator has no row for that pair yet. Read-only companion to `add_balance`:
/// a caller checks a pending credit against `MAX_COLLATERAL_AMOUNT` before
/// mutating, so collateral never accumulates past the asset range.
uint64_t balance_of(const opreg::operator_entry& o,
                    sysio::slug_name chain_code, sysio::slug_name token_code) {
   for (const auto& b : o.balances) {
      if (b.chain_code == chain_code && b.token_code == token_code) {
         return b.balance;
      }
   }
   return 0;
}

/// Add `amount` to the (chain_code, token_code) balance row, creating the row
/// if it doesn't exist. Mutates the operator entry in place — caller is
/// expected to be inside an `ops.modify(...)` lambda. Callers MUST first verify
/// the credit keeps the row within `MAX_COLLATERAL_AMOUNT` (see `balance_of`);
/// `add_balance` itself does not cap, mirroring the unchecked `subtract_balance`.
void add_balance(opreg::operator_entry& o,
                 sysio::slug_name chain_code, sysio::slug_name token_code,
                 uint64_t amount) {
   for (auto& b : o.balances) {
      if (b.chain_code == chain_code && b.token_code == token_code) {
         b.balance         += amount;
         b.last_updated_ms  = current_time_ms();
         return;
      }
   }
   o.balances.push_back(opreg::balance_entry{
      .chain_code      = chain_code,
      .token_code      = token_code,
      .balance         = amount,
      .last_updated_ms = current_time_ms(),
   });
}

/// Subtract `amount` from the (chain_code, token_code) balance row. Caller
/// must have already validated the available balance via `available_inline`.
/// Mutates the operator entry in place — caller is expected to be inside an
/// `ops.modify(...)` lambda.
void subtract_balance(opreg::operator_entry& o,
                      sysio::slug_name chain_code, sysio::slug_name token_code,
                      uint64_t amount) {
   for (auto& b : o.balances) {
      if (b.chain_code == chain_code && b.token_code == token_code) {
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
   opreg::opcounters_t real_ctr(name{"sysio.opreg"_n});
   auto ctr = real_ctr.get_or_default(opreg::op_counters{});
   uint64_t id = ctr.next_withdraw_id;
   ctr.next_withdraw_id = id + 1;
   real_ctr.set(ctr, ram_payer);
   return id;
}

uint64_t next_dellog_id() {
   opreg::opcounters_t real_ctr(name{"sysio.opreg"_n});
   auto ctr = real_ctr.get_or_default(opreg::op_counters{});
   uint64_t id = ctr.next_dellog_id;
   ctr.next_dellog_id = id + 1;
   real_ctr.set(ctr, ram_payer);
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

/// Look up `account`'s registered public key for `chain_code` from
/// `sysio.authex::links` (`bynamechain` index) and pack it into a
/// `ChainAddress`. Returns `{UNKNOWN, []}` when the chain isn't registered
/// or no authex link exists — the downstream outpost / depot lookup then
/// fails gracefully (the depot's `dispatch_operator_action` rejects empty
/// `op_address.address`).
///
/// Post v6: `authex::links.bynamechain` is still keyed by `(name, ChainKind)`
/// and `ChainAddress.kind` is still `ChainKind`. opreg now stores chains by
/// slug_name; resolve via `chain_kind_for_code` first.
opp::types::ChainAddress operator_chain_address(name account, sysio::slug_name chain_code) {
   opp::types::ChainAddress addr;
   auto kind_opt = chain_kind_for_code(chain_code);
   if (!kind_opt) return addr;   // chain not registered — empty address
   const opp::types::ChainKind kind = *kind_opt;
   addr.kind = kind;

   authex::links_t links(opreg::AUTHEX_ACCOUNT);
   auto idx = links.get_index<"bynamechain"_n>();
   uint128_t key = to_namechain_key(account, kind);
   auto it = idx.find(key);
   if (it != idx.end()) {
      addr.address = pubkey_to_bytes(it->pub_key);
   }
   return addr;
}

/// Build the `OperatorAction(action_type=SLASH)` payload for a given
/// (account, chain_code, token_code) slash. Returns the OperatorAction
/// ready for either logging on the operator's row or queueing as an
/// outbound OPERATOR_ACTION attestation. Pure — no side effects.
///
/// LP routing for the slashed funds is depot-side concern resolved via
/// `sysio.reserve::resolve_lp` at slash-handler time; outposts on receipt
/// only need to seize their share, so the attestation does not encode it.
/// The `OperatorActionLog.timestamp` covers the audit trail; once an
/// operator is slashed all subsequent OperatorActions are dropped+logged
/// as failures, so per-action epoch is redundant on the message itself.
OperatorAction build_slash_action(name account,
                                  OperatorType type,
                                  sysio::slug_name chain_code,
                                  sysio::slug_name token_code,
                                  uint64_t amount,
                                  const std::string& reason) {
   OperatorAction oa;
   oa.action_type = OperatorAction::ACTION_TYPE_SLASH;
   oa.op_address  = operator_chain_address(account, chain_code);
   oa.type        = type;
   opp::types::TokenAmount ta;
   ta.token_code = token_code.value;
   ta.amount     = zpp::bits::vint64_t{static_cast<int64_t>(amount)};
   oa.amount      = ta;
   oa.chain_code  = chain_code.value;
   oa.reason      = reason;
   return oa;
}

/// Queue an OPERATOR_ACTION(SLASH) attestation outbound to the outpost
/// matching `chain_code`. No-op if the chain is WIRE (slashed funds stay on
/// the WIRE chain) or has no registered outpost.
void emit_slash_attestation(name self, const OperatorAction& slash_action) {
   const sysio::slug_name chain_code{slash_action.chain_code};
   if (chain_code == kWireChainCode) return;
   auto resolved = find_outpost_id_for_chain(chain_code);
   if (!resolved) return;   // no outpost on this chain — nothing to slash through

   // `no_size{}` — raw protobuf bytes, no 4-byte zpp length prefix. The
   // outpost decodes the attestation `data` field as a pure protobuf
   // message; a size prefix would corrupt the first field tag.
   std::vector<char> encoded;
   auto out = zpp::bits::out{encoded, zpp::bits::no_size{}};
   (void)out(slash_action);

   action(
      permission_level{self, "active"_n},
      opreg::MSGCH_ACCOUNT, "queueout"_n,
      std::make_tuple(*resolved,
         AttestationType::ATTESTATION_TYPE_OPERATOR_ACTION, encoded)
   ).send();
}

/// Queue a DEPOSIT_REVERT attestation outbound to the source outpost so
/// escrowed funds get refunded to the depositor (minus the outpost-side
/// gas penalty, computed locally on the outpost when the revert is
/// processed). Called by `opreg::depositinle` whenever validation rejects
/// an inbound DEPOSIT_REQUEST.
void emit_deposit_revert(name self,
                         sysio::slug_name source_chain_code,
                         const opp::types::ChainAddress& depositor,
                         sysio::slug_name token_code,
                         uint64_t amount,
                         const checksum256& original_message_id,
                         const std::string& reason) {
   auto chain_code = find_outpost_id_for_chain(source_chain_code);
   if (!chain_code) return;     // no outpost on this chain — nothing to refund through

   opp::attestations::DepositRevert dr;
   dr.depositor = depositor;
   opp::types::TokenAmount ta;
   ta.token_code = token_code.value;
   ta.amount     = zpp::bits::vint64_t{static_cast<int64_t>(amount)};
   dr.refund_amount = ta;
   const auto& mh = original_message_id.extract_as_byte_array();
   dr.original_deposit_message_id.assign(mh.begin(), mh.end());
   dr.reason     = reason;
   dr.chain_code = source_chain_code.value;

   // `no_size{}` — see emit_slash_attestation for the rationale.
   std::vector<char> encoded;
   auto out = zpp::bits::out{encoded, zpp::bits::no_size{}};
   (void)out(dr);

   action(
      permission_level{self, "active"_n},
      opreg::MSGCH_ACCOUNT, "queueout"_n,
      std::make_tuple(*chain_code,
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
/// outpost matching `chain_code`. `op_address` carries the operator's authex-
/// linked chain pubkey so the outpost can derive the destination address.
void emit_withdraw_remit(name self,
                         name account,
                         OperatorType type,
                         sysio::slug_name chain_code,
                         sysio::slug_name token_code,
                         uint64_t amount,
                         uint64_t request_id) {
   auto resolved = find_outpost_id_for_chain(chain_code);
   if (!resolved) return;

   OperatorAction oa;
   oa.action_type = OperatorAction::ACTION_TYPE_WITHDRAW_REMIT;
   oa.op_address  = operator_chain_address(account, chain_code);
   oa.type        = type;
   opp::types::TokenAmount ta;
   ta.token_code = token_code.value;
   ta.amount     = zpp::bits::vint64_t{static_cast<int64_t>(amount)};
   oa.amount     = ta;
   oa.request_id = request_id;
   oa.chain_code = chain_code.value;

   // `no_size{}` — see emit_slash_attestation for the rationale.
   std::vector<char> encoded;
   auto out = zpp::bits::out{encoded, zpp::bits::no_size{}};
   (void)out(oa);

   action(
      permission_level{self, "active"_n},
      opreg::MSGCH_ACCOUNT, "queueout"_n,
      std::make_tuple(*resolved,
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
enqueue_result try_enqueue_withdraw(name account,
                                    sysio::slug_name chain_code,
                                    sysio::slug_name token_code,
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

   uint64_t avail = available_inline(op, chain_code, token_code);
   if (avail < amount) {
      return { false, 0, "insufficient available balance for withdraw" };
   }

   uint32_t now_ep = get_current_epoch();
   uint64_t request_id = next_withdraw_id();

   opreg::wtdwqueue_t queue(name{"sysio.opreg"_n});
   queue.emplace(ram_payer, opreg::withdraw_key{request_id}, opreg::withdraw_request{
      .request_id          = request_id,
      .account             = account,
      .chain_code          = chain_code,
      .token_code          = token_code,
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
                                             sysio::slug_name chain_code,
                                             sysio::slug_name token_code,
                                             uint64_t amount,
                                             uint64_t request_id) {
   OperatorAction oa;
   oa.action_type = OperatorAction::ACTION_TYPE_WITHDRAW_REQUEST;
   oa.op_address  = operator_chain_address(account, chain_code);
   oa.chain_code  = chain_code.value;
   opp::types::TokenAmount ta;
   ta.token_code = token_code.value;
   ta.amount     = zpp::bits::vint64_t{static_cast<int64_t>(amount)};
   oa.amount      = ta;
   oa.request_id  = request_id;
   return oa;
}

/// Build an OperatorAction(WITHDRAW_REMIT) payload for the log when
/// `flushwtdw` matures a queue row. Mirror of build_withdraw_request_action
/// shape, with action_type=REMIT and the request_id of the matured row.
OperatorAction build_withdraw_remit_action(name account,
                                           sysio::slug_name chain_code,
                                           sysio::slug_name token_code,
                                           uint64_t amount,
                                           uint64_t request_id) {
   OperatorAction oa;
   oa.action_type = OperatorAction::ACTION_TYPE_WITHDRAW_REMIT;
   oa.op_address  = operator_chain_address(account, chain_code);
   oa.chain_code  = chain_code.value;
   opp::types::TokenAmount ta;
   ta.token_code = token_code.value;
   ta.amount     = zpp::bits::vint64_t{static_cast<int64_t>(amount)};
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
                                    sysio::slug_name chain_code,
                                    sysio::slug_name token_code,
                                    uint64_t amount) {
   OperatorAction oa;
   oa.action_type = OperatorAction::ACTION_TYPE_DEPOSIT_REQUEST;
   oa.op_address  = op_address;
   oa.chain_code  = chain_code.value;
   opp::types::TokenAmount ta;
   ta.token_code = token_code.value;
   ta.amount     = zpp::bits::vint64_t{static_cast<int64_t>(amount)};
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

   auto result = try_enqueue_withdraw(account, kWireChainCode, kWireTokenCode, amount);
   auto action = build_withdraw_request_action(account, kWireChainCode, kWireTokenCode, amount,
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
                         sysio::slug_name chain_code,
                         sysio::slug_name token_code,
                         uint64_t amount) {
   require_auth(get_self());

   operators_t ops(get_self());
   auto op_pk = operator_key{account.value};

   auto result = try_enqueue_withdraw(account, chain_code, token_code, amount);
   auto action = build_withdraw_request_action(account, chain_code, token_code, amount,
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

   // Credit collateral BEFORE the outbound WIRE transfer, with the cap check
   // performed ATOMICALLY inside the same `modify` as the credit — reading the
   // live row, not the `op` copy read above. This closes a reentrancy window on
   // operator accounts that carry contract code: `sysio.token::transfer` notifies
   // `from` (the operator), and that notification handler could re-enter
   // `deposit`. A separate pre-read-then-check-then-credit could let two credits
   // pass against the same stale balance and push the WIRE row past
   // `asset::max_amount` — recreating the withdraw/terminate remit abort
   // (`asset(balance, CORE_SYM)` aborts above the limit). Checking inside the
   // modify makes check+credit indivisible, and crediting before the transfer
   // means any re-entry observes the committed balance. A direct user deposit may
   // legitimately `check()`-throw here (unlike the never-throw OPP `depositinle`);
   // if the transfer below aborts (operator lacks the WIRE) the whole
   // transaction — including this credit — rolls back. (SEC-103; PR #449 review.)
   ops.modify(same_payer, op_pk, [&](auto& o) {
      check(amount <= MAX_COLLATERAL_AMOUNT &&
               balance_of(o, kWireChainCode, kWireTokenCode) <= MAX_COLLATERAL_AMOUNT - amount,
            "deposit would exceed max collateral");
      add_balance(o, kWireChainCode, kWireTokenCode, amount);
   });

   // Direct WIRE token transfer from operator -> opreg, sent after the credit so
   // a transfer-notification re-entry observes the already-committed balance.
   action(
      permission_level{account, "active"_n},
      TOKEN_ACCOUNT, "transfer"_n,
      std::make_tuple(account, get_self(),
         asset(static_cast<int64_t>(amount), CORE_SYM),
         std::string("opreg::deposit"))
   ).send();

   auto deposit_action = build_deposit_action(
      operator_chain_address(account, kWireChainCode),
      kWireChainCode, kWireTokenCode, amount);
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
//
// `actor_chain` is retained as `opp::types::ChainKind` per the
// ChainAddress flattening pattern — the depositor's source-chain
// `ChainAddress.kind` field is still ChainKind on the wire and is not
// part of the v6 slug_name refactor.
void opreg::depositinle(name account,
                        sysio::slug_name chain_code,
                        sysio::slug_name token_code,
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

   auto deposit_action = build_deposit_action(actor, chain_code, token_code, amount);

   if (amount == 0) {
      const std::string err = "amount must be positive";
      emit_deposit_revert(get_self(), chain_code, actor, token_code, amount,
                          original_message_id, err);
      append_action_log(ops, op_pk, deposit_action, false, err);
      return;
   }
   if (!ops.contains(op_pk)) {
      // No entry to log to. The DEPOSIT_REVERT IS the audit record for the
      // outpost — outpost emits a local refund event the depositor reads.
      emit_deposit_revert(get_self(), chain_code, actor, token_code, amount,
                          original_message_id, "operator not registered");
      return;
   }
   auto op = ops.get(op_pk);
   if (op.status == OperatorStatus::OPERATOR_STATUS_SLASHED ||
       op.status == OperatorStatus::OPERATOR_STATUS_TERMINATED) {
      const std::string err = "operator not in a deposit-eligible state";
      emit_deposit_revert(get_self(), chain_code, actor, token_code, amount,
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
      emit_deposit_revert(get_self(), chain_code, actor, token_code, amount,
                          original_message_id, err);
      append_action_log(ops, op_pk, deposit_action, false, err);
      return;
   }
   // SEC-103 (WSA-028 follow-up): the credited collateral must stay within the
   // asset magnitude range so the WIRE-direct remit path's `asset(balance,
   // CORE_SYM)` can never abort — an abort on this OPP-inbound path would stall
   // consensus. The msgch ingress gate already bounds a single `amount` to
   // `asset::max_amount`; this additionally bounds the running sum. Fail closed
   // by refunding via DEPOSIT_REVERT — never `check()`.
   if (amount > MAX_COLLATERAL_AMOUNT ||
       balance_of(op, chain_code, token_code) > MAX_COLLATERAL_AMOUNT - amount) {
      const std::string err = "deposit would exceed max collateral";
      emit_deposit_revert(get_self(), chain_code, actor, token_code, amount,
                          original_message_id, err);
      append_action_log(ops, op_pk, deposit_action, false, err);
      return;
   }

   ops.modify(same_payer, op_pk, [&](auto& o) {
      add_balance(o, chain_code, token_code, amount);
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

   // Iterate matured rows. Erase as we go, hence the manual cursor. Bounded to
   // MAX_WTDW_FLUSH_PER_EPOCH rows per advance (SEC-78): the remaining matured
   // rows flush on the next advance, which keeps this epoch-inline action inside
   // the transaction CPU deadline it shares with the rest of advance's fan-out.
   // `flushed` is incremented at the top of every iteration, before any of the
   // per-row `continue` branches, so it counts every attempt.
   uint32_t flushed = 0;
   auto it = idx.begin();
   while (it != idx.end() && it->eligible_at_epoch <= current_epoch &&
          flushed < MAX_WTDW_FLUSH_PER_EPOCH) {
      auto row     = *it;          // copy out before erase
      auto wkey    = withdraw_key{row.request_id};
      // Advance index iterator BEFORE erasing the row.
      ++it;
      ++flushed;

      auto op_pk = operator_key{row.account.value};
      // Per-row outcome lands in the operator's recent_actions log so the
      // operator can read flush failures (slashed during wait, defensive
      // rollup mismatches) via JSON-RPC.
      auto remit_action = build_withdraw_remit_action(row.account, row.chain_code,
                                                     row.token_code, row.amount,
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

      if (op.status == OperatorStatus::OPERATOR_STATUS_TERMINATED) {
         // Terminated during the wait — terminate_inline already remitted the operator's full
         // unlocked balance (which covered this queued amount), so the row is moot. Erase without
         // subtracting; otherwise subtract_balance would underflow and abort this epoch-inline
         // action, permanently stalling epoch advancement (the balance was reduced to sum(locks)
         // by terminate_inline, leaving nothing to cover the matured withdraw).
         append_action_log(ops, op_pk, remit_action, false,
                           "operator terminated during withdraw-wait window");
         queue.erase(wkey);
         continue;
      }

      // Re-validate the actual stored balance covers this withdraw before subtracting. The prior
      // guard `available_inline(...) + row.amount < row.amount` was dead code — available_inline
      // is unsigned so it reduces to `available_inline(...) < 0` and never fired. Compare the real
      // (chain,token) balance against the debit and skip-and-log on a shortfall rather than let
      // subtract_balance's underflow check abort this epoch-inline action.
      const auto* bal = find_balance(op, row.chain_code, row.token_code);
      if (!bal || bal->balance < row.amount) {
         append_action_log(ops, op_pk, remit_action, false,
                           "insufficient balance at flush (rollup mismatch)");
         queue.erase(wkey);
         continue;
      }

      // Subtract from balance.
      ops.modify(same_payer, op_pk, [&](auto& o) {
         subtract_balance(o, row.chain_code, row.token_code, row.amount);
      });

      // For WIRE-direct: do the token transfer back inline. For outpost
      // chains: queue an OPERATOR_ACTION(WITHDRAW_REMIT) to the outpost
      // so it can release the escrow on its end.
      if (row.chain_code == kWireChainCode) {
         action(
            permission_level{get_self(), "active"_n},
            TOKEN_ACCOUNT, "transfer"_n,
            std::make_tuple(get_self(), row.account,
               asset(static_cast<int64_t>(row.amount), CORE_SYM),
               std::string("opreg::withdraw flush"))
         ).send();
      } else {
         emit_withdraw_remit(get_self(), row.account, op.type,
                             row.chain_code, row.token_code, row.amount, row.request_id);
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

   auto now = current_time_ms();

   // Snapshot the slashable amounts per (chain_code, token_code) BEFORE
   // marking SLASHED (the status flip would zero `slashable_now` via
   // available()).
   struct slash_pair { sysio::slug_name chain_code; sysio::slug_name token_code; uint64_t amount; };
   std::vector<slash_pair> to_slash;
   for (const auto& bal : op.balances) {
      uint64_t amt = slashable_now(op, bal.chain_code, bal.token_code);
      if (amt > 0) {
         to_slash.push_back({bal.chain_code, bal.token_code, amt});
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
         subtract_balance(o, sp.chain_code, sp.token_code, sp.amount);
      }
   });

   // Emit one OPERATOR_ACTION(SLASH) per (chain_code, token_code) with non-zero
   // slashable, AND append each as a recent_actions log entry on the
   // operator's row (success=true since the slash itself was applied).
   for (const auto& sp : to_slash) {
      auto slash_action = build_slash_action(op.account, op.type,
                                             sp.chain_code, sp.token_code, sp.amount,
                                             reason);
      emit_slash_attestation(get_self(), slash_action);
      append_action_log(ops, op_pk, slash_action, /*success*/ true, "");
   }
}

// ---------------------------------------------------------------------------
//  releaselock — deferred-slash / deferred-remit / no-op on lock release
// ---------------------------------------------------------------------------
void opreg::releaselock(name account,
                        sysio::slug_name chain_code,
                        sysio::slug_name token_code,
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
   //
   // Clamp the settled amount to the live balance bucket. In correct
   // operation sum(active locks) <= balance for every (underwriter,
   // chain_code, token_code) — the sysio.uwrit winner check reserves the
   // aggregate per collateral bucket before writing locks — so `amount`
   // never exceeds the remaining balance and this clamp is a no-op.
   // It is the safety net for any residual/over-committed lock set:
   // releaselock runs INLINE inside `sysio.uwrit::chklocks` at
   // `sysio.epoch::advance`, and a `subtract_balance` underflow `check()`
   // there would abort the advance and permanently stall epoch advancement
   // chain-wide (`epoch-stall-is-fatal`). Settle — and attest — only what the
   // balance can actually back.
   uint64_t settle_amount = amount;
   if (const auto* bal = find_balance(op, chain_code, token_code)) {
      if (settle_amount > bal->balance) settle_amount = bal->balance;
   } else {
      settle_amount = 0;   // no balance row for this bucket — nothing to settle
   }
   if (settle_amount == 0) {
      // Bucket already fully drained (e.g. by prior releases of an
      // over-committed set) — the caller has erased the lock row; emit no
      // zero-value slash/remit attestation.
      return;
   }

   ops.modify(same_payer, op_pk, [&](auto& o) {
      subtract_balance(o, chain_code, token_code, settle_amount);
   });

   if (op.status == OperatorStatus::OPERATOR_STATUS_SLASHED) {
      auto slash_action = build_slash_action(op.account, op.type,
                                             chain_code, token_code, settle_amount,
                                             /*reason*/ "deferred slash on lock release");
      emit_slash_attestation(get_self(), slash_action);
      append_action_log(ops, op_pk, slash_action, /*success*/ true, "");
   } else {
      // TERMINATED — for WIRE-direct, transfer back to operator; otherwise
      // queue WITHDRAW_REMIT so the outpost can transfer to the authex
      // destination. request_id == 0 (this remit isn't queued in wtdwqueue).
      if (chain_code == kWireChainCode) {
         action(
            permission_level{get_self(), "active"_n},
            TOKEN_ACCOUNT, "transfer"_n,
            std::make_tuple(get_self(), account,
               asset(static_cast<int64_t>(settle_amount), CORE_SYM),
               std::string("terminate-deferred-remit"))
         ).send();
      } else {
         emit_withdraw_remit(get_self(), op.account, op.type,
                             chain_code, token_code, settle_amount, /*request_id*/ 0);
      }
   }
}

// ---------------------------------------------------------------------------
//  terminate / termcheck / recorddel — administrative removal
// ---------------------------------------------------------------------------

namespace {

/// Internal terminate body — used by both the operator-removal path
/// (`termcheck` -> `terminate` inline) and the slashing-equivalent path for
/// completeness. Marks status TERMINATED and remits each
/// (chain_code, token_code) balance back to the operator via WITHDRAW_REMIT.
void terminate_inline(name self, name account, const std::string& reason) {
   opreg::operators_t ops(self);
   auto op_pk = opreg::operator_key{account.value};
   auto op = ops.get(op_pk, "operator not found");
   check(op.status == OperatorStatus::OPERATOR_STATUS_ACTIVE ||
         op.status == OperatorStatus::OPERATOR_STATUS_UNKNOWN,
         "operator not in a terminable state");

   auto now = current_time_ms();

   // Snapshot the remitable amounts BEFORE flipping status.
   struct remit_pair { sysio::slug_name chain_code; sysio::slug_name token_code; uint64_t amount; };
   std::vector<remit_pair> to_remit;
   for (const auto& bal : op.balances) {
      uint64_t amt = slashable_now(op, bal.chain_code, bal.token_code);
      // For termination we route the unlocked portion. The locked portion
      // gets remitted at lock-release time by sysio.uwrit::release (deferred-
      // remit, symmetric with deferred-slash).
      if (amt > 0) {
         to_remit.push_back({bal.chain_code, bal.token_code, amt});
      }
   }

   ops.modify(same_payer, op_pk, [&](auto& o) {
      o.status        = OperatorStatus::OPERATOR_STATUS_TERMINATED;
      o.terminated_at = now;
      o.status_reason = reason;
      for (const auto& rp : to_remit) {
         subtract_balance(o, rp.chain_code, rp.token_code, rp.amount);
      }
   });

   // Remit each (chain_code, token_code). For WIRE-chain: direct token transfer
   // back to the operator. For outpost chains: queue WITHDRAW_REMIT.
   //
   // After each remit, append a WITHDRAW_REMIT entry to the operator's
   // `recent_actions` ring buffer so the audit trail mirrors the
   // operator-initiated withdraw flow (`flushwtdw` does the same). Without
   // this entry, downstream consumers polling `operators[op].recent_actions`
   // for proof of remit emission see only the prior DEPOSIT_REQUESTs and
   // miss the termination payout — same semantic gap on TERMINATED ops as on
   // a normal queued withdraw, only resolvable by querying msgch internals
   // (which are transient — the rows drain on the next `buildenv`).
   for (const auto& rp : to_remit) {
      if (rp.chain_code == kWireChainCode) {
         action(
            permission_level{self, "active"_n},
            opreg::TOKEN_ACCOUNT, "transfer"_n,
            std::make_tuple(self, account,
               asset(static_cast<int64_t>(rp.amount), opreg::CORE_SYM),
               std::string("terminate-remit"))
         ).send();
      } else {
         emit_withdraw_remit(self, account, op.type,
                             rp.chain_code, rp.token_code, rp.amount, /*request_id*/ 0);
      }
      OperatorAction remit_action = build_withdraw_remit_action(
         account, rp.chain_code, rp.token_code, rp.amount, /*request_id*/ 0);
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
   log.emplace(ram_payer, delivery_key{id}, delivery_log_entry{
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
           magic_enum::enum_integer(OperatorStatus::OPERATOR_STATUS_TERMINATED));
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

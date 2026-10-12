#include <sysio.opreg/sysio.opreg.hpp>
#include <sysio.epoch/sysio.epoch.hpp>
#include <sysio.chains/sysio.chains.hpp>
#include <sysio.authex/sysio.authex.hpp>
#include <sysio.liq/sysio.liq.hpp>
#include <sysio/slug_name.hpp>
#include <sysio.opp.common/safe_ops.hpp>
#include <sysio.opp.common/claimable.hpp>
#include <sysio.opp.common/shadow_custody.hpp>
#include <sysio.opp.common/depot_native_token.hpp>
#include <sysio.opp.common/registry_codes.hpp>
#include <sysio/opp/attestations/attestations.pb.hpp>
#include <magic_enum/magic_enum.hpp>
#include <zpp_bits.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace sysio {

using opp::types::OperatorType;
using opp::types::OperatorStatus;
using opp::types::AttestationType;
using opp::attestations::OperatorAction;
using opp::attestations::OperatorActionLog;

namespace {

/// Forward declaration -- defined with the other eligibility helpers further down. `regoperator`
/// needs it so registering a PRODUCER notifies sysio.system to score the new operator row.
void reevaluate_eligibility(opreg::operators_t& ops,
                            const opreg::operator_key& op_pk,
                            name self,
                            name account);

// System-owned rows bill to the sysio RAM pool, not this contract account (privileged-contract
// model, as sysio.token uses): the account stays finite at code+abi size; growth draws from the pool.
constexpr name ram_payer = "sysio"_n;

/// Memo on the transfer that moves a `deposit` into the registry's custody.
constexpr std::string_view deposit_transfer_memo = "opreg::deposit";

/// Memo on the transfer that pays a `claimremit` out of the registry's custody.
constexpr std::string_view claimremit_transfer_memo = "opreg::claimremit collateral payout";

/// Message of the `check` a depot-native action raises for a token it cannot custody.
constexpr std::string_view unsupported_token_msg = "unsupported depot-native collateral token";

/// Message of the `check` `sweepyield` raises when the registry's `sysio.liq` row is owed nothing.
constexpr std::string_view no_yield_to_sweep_msg = "no yield to sweep";

/// Message of the `check` `claimyield` raises when the operator's row has earned nothing.
constexpr std::string_view no_yield_owed_msg = "no yield owed";

/// Message of the `check` `claimyield` raises when the row is owed yield the pool cannot yet cover.
constexpr std::string_view yield_not_covered_msg =
   "owed yield is rounding dust the WIRE received from sysio.liq does not cover; only later slack can cover it";

/// A corrupt or exhausted archived-debt accumulator cannot be erased with its operator record.
constexpr std::string_view yield_debt_overflow_msg = "yield debt overflow";

/// Collecting yield must not saturate away value already removed from a debt and backing pool.
constexpr std::string_view yield_remit_full_msg = "claim WIRE remit before collecting more yield";

/// Message of the `check` `sweepyield` raises for WIRE, which is not a shadow token.
constexpr std::string_view wire_earns_no_yield_msg = "WIRE collateral earns no shadow yield";

/// Resolve a depot-native collateral `token_code` through the shared resolver, against this
/// registry's custody contracts (`sysio.token` for WIRE, `sysio.liq` for a shadow LIQ symbol).
///
/// Returns `std::nullopt` for any other code; callers are the operator-signed actions
/// (`deposit`, `withdraw`, `claimremit`), which `check()` on it. The never-throw remit paths do
/// not resolve: they credit a claim row by the `token_code` they already hold.
std::optional<opp::custody::depot_native_token> resolve_depot_native_token(sysio::slug_name token_code) {
   return opp::custody::resolve_depot_native_token(opreg::LIQ_ACCOUNT, opreg::TOKEN_ACCOUNT, token_code);
}

/// True iff the `(chain_code, token_code)` balance row is bonded depot-native shadow, and so earns
/// WIRE yield on the registry's `sysio.liq` holder row: the depot chain and any token but WIRE. The ONE
/// place that decides it; every yield path (settle, sweep, claim, termination payout) asks here.
bool earns_shadow_yield(sysio::slug_name chain_code, sysio::slug_name token_code) {
   return chain_code == opp::wire::chain_code && token_code != opp::wire::token_code;
}

/// Credit a WIRE-chain remit of `token_code` to the operator's claimable row instead of
/// transferring it.
///
/// Every caller (withdraw flush and termination payout) is reachable from
/// `sysio.epoch::advance`, which must never abort. A custody contract's `transfer` notifies the
/// operator, and the chain runs notified receivers with no exception isolation, so a pushed remit
/// would let an operator's notify handler abort `advance` and halt epoch advancement chain-wide.
/// In the termination case the operator would be blocking its own removal, so the retry never
/// converges.
///
/// Never throws: the credit saturates rather than overflowing, and `token_code` is taken as the
/// caller holds it — it is NOT resolved here, so an unresolvable code cannot abort `advance`.
/// Resolution happens when the operator claims.
void credit_remit_claim(name self, name account, sysio::slug_name token_code, uint64_t amount) {
   if (amount == 0) return;

   opreg::remitclaims_t claims(self);
   sysio::opp::claimable::credit(claims, ram_payer, opreg::remitclaim_key{account.value, token_code},
                                 opreg::remit_claim{.account = account, .token_code = token_code}, amount);
}

uint64_t current_time_ms() {
   return static_cast<uint64_t>(current_time_point().sec_since_epoch()) * 1000;
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

/// Reject a collateral-requirement entry whose codes have no canonical string
/// spelling. These entries persist on the config row and are rendered by every
/// reader of it — and an uncanonical code does not announce itself: it can render
/// a valid spelling that re-parses to a different value, aliasing onto another
/// code. See `registry_codes.hpp`. `setconfig` is a privileged top-level action,
/// so it refuses rather than absorbing the value the way a dispatch handler must.
void require_canonical_codes(const std::vector<opreg::chain_min_bond>& v,
                             const char* role_label) {
   for (const auto& entry : v) {
      opp::registry::check_codes({entry.chain_code, entry.token_code}, role_label);
   }
}

bool is_fully_settled(const opreg::operator_entry& op) {
   for (const auto& bal : op.balances) {
      if (bal.balance > 0) return false;
   }
   return true;
}

/// Preserve already-banked yield before erasing a principal-settled operator. Both callers are
/// caller-signed actions, so an impossible accumulator overflow rejects the erase atomically.
/// This does not create backed claims or touch a yield pool; claimyield later applies its cap.
/// Cast explicitly because CDT defines uint128_t as a multi-token macro.
void preserve_yield_debt(name self, const opreg::operator_entry& op) {
   opreg::yielddebts_t debts(self);
   for (const auto& balance : op.balances) {
      const uint64_t owed = balance.shadow_yield.owed_wire;
      if (owed == 0) continue;
      const opreg::remitclaim_key key{op.account.value, balance.token_code};
      auto debt = debts.try_get(key).value_or(opreg::yield_debt{
         .account = op.account, .token_code = balance.token_code});
      check(debt.owed_wire <= ~static_cast<uint128_t>(0) - owed, yield_debt_overflow_msg);
      debt.owed_wire += owed;
      debts.upsert(ram_payer, key, debt);
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
   check(terminate_max_consecutive_misses >= MIN_TERMINATE_MAX_CONSECUTIVE_MISSES &&
         terminate_max_consecutive_misses <= MAX_TERMINATE_MAX_CONSECUTIVE_MISSES,
         "terminate_max_consecutive_misses must be in [1, 5]");
   check(terminate_max_pct_misses_24h >= MIN_TERMINATE_MAX_PCT_MISSES_24H &&
         terminate_max_pct_misses_24h <= MAX_TERMINATE_MAX_PCT_MISSES_24H,
         "terminate_max_pct_misses_24h must be in [1, 99]");
   check(terminate_window_ms > 0, "terminate_window_ms must be positive");

   // SEC-28 residual: delivery records accrue only on duty epochs -- one per
   // `batch_op_groups`-epoch rotation for a resident operator -- so a rolling
   // window narrower than the full consecutive-miss run of duty epochs makes
   // the consecutive rail structurally vacuous (records age out and are
   // pruned before `termcheck` can observe the run) and leaves only the
   // hair-trigger percent rail. Validate against the live epoch schedule.
   // Bootstrap installs opreg config before sysio.epoch is configured, so an
   // absent epochcfg skips the check here; sysio.epoch::setconfig performs
   // the mirror validation, so no ordering of the two setters can accept a
   // vacuous pair.
   {
      sysio::epoch::epochcfg_t epoch_cfg_tbl(EPOCH_ACCOUNT);
      if (epoch_cfg_tbl.exists()) {
         const auto epoch_cfg = epoch_cfg_tbl.get();
         check(terminate_window_ms >= min_terminate_window_ms(terminate_max_consecutive_misses,
                                                              epoch_cfg.epoch_duration_sec,
                                                              epoch_cfg.batch_op_groups),
               "terminate_window_ms must span at least terminate_max_consecutive_misses + 1 duty rotations");
      }
   }

   require_canonical_codes(req_prod_collat,    "req_prod_collat");
   require_canonical_codes(req_batchop_collat, "req_batchop_collat");
   require_canonical_codes(req_uw_collat,      "req_uw_collat");

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

   // sysio.system scores producer rank on the ratio of posted collateral to these minimums, so
   // every stored score is stale the moment they move. Tell it on the same channel processprod
   // uses; it opens a bounded rescore sweep on the notification.
   require_recipient(opreg::SYSTEM_ACCOUNT);
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
      check(is_fully_settled(existing),
            "operator has unsettled collateral: a terminated operator may only "
            "re-register once its balances are drained");
      preserve_yield_debt(get_self(), existing);
      ops.erase(op_pk);
   }

   // Verify authex links exist for all active outpost chains.
   // Skip when: bootstrapped OR privileged caller (sysio.opreg registering on behalf)
   //
   // After the refactor: the outpost set lives in `sysio.chains::chains` keyed
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

   // Producer rank is scored from the operator row, so registering one -- which is what decides its
   // tier -- must bring sysio.system's stored score in step. reevaluate_eligibility dispatches
   // processprod for producers regardless of transition, which is the notification that does it.
   // Declared below; see the forward declaration above regoperator.
   reevaluate_eligibility(ops, op_pk, get_self(), account);
}

// ---------------------------------------------------------------------------
//  Internal helpers — balance / withdraw rollup
// ---------------------------------------------------------------------------

namespace {

/// Sum the pending (not-yet-flushed) withdraws on this contract for a given
/// (op, chain, token). Subtracted by `available()` so a queued withdraw
/// effectively reserves the funds for its 2-epoch wait.
///
/// Per the split-index design: `wtdwqueue_t` exposes only uint64
/// secondary indexes. `byaccount` keys on `account.value`; rows are filtered
/// on `(chain_code, token_code)` in memory. Per-account pending-withdraw
/// counts are bounded by the operator's collateral-bucket count.
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

/// Return true when the operator has entered a permanent punishment or
/// removal state that collateral changes must never reverse.
bool has_terminal_status(OperatorStatus status) {
   return status == OperatorStatus::OPERATOR_STATUS_SLASHED ||
          status == OperatorStatus::OPERATOR_STATUS_TERMINATED;
}

/// Compute available balance for a given (op, chain, token). The single
/// rollup formula: balance - sum(pending withdraws),
/// gated by status. Slashed / terminated operators read as zero.
uint64_t available_inline(const opreg::operator_entry& op,
                          sysio::slug_name chain_code, sysio::slug_name token_code) {
   if (has_terminal_status(op.status)) {
      return 0;
   }
   const auto* bal = find_balance(op, chain_code, token_code);
   if (!bal) return 0;

   uint64_t pending = sum_pending_withdraws(op.account, chain_code, token_code);
   return bal->balance > pending ? bal->balance - pending : 0;
}

/// The full balance is slashable, including queued withdrawals. Withdrawals of a
/// slashed operator are forfeit (silently dropped at flush time).
uint64_t slashable_now(const opreg::operator_entry& op,
                       sysio::slug_name chain_code, sysio::slug_name token_code) {
   const auto* bal = find_balance(op, chain_code, token_code);
   if (!bal) return 0;
   return bal->balance;
}

/// Check whether the operator's available balance on (chain_code, token_code)
/// covers the role's minimum bond on that pair.
///
/// Bootstrapped operators are ACTIVE-by-fiat and bypass the per-token
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
/// this cannot be carried by the `asset()` that the withdraw /
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

namespace custody = opp::shadow::custody;

/// Take backed yield up to the destination remit's remaining capacity, leaving the untaken
/// entitlement on the original position. The custody helper remains the sole backing cap.
uint64_t take_bounded_yield(custody::position& position, custody::yield_pool& pool, uint64_t limit) {
   custody::position payable{.owed_wire = std::min(position.owed_wire, limit)};
   const uint64_t taken = custody::take(payable, pool);
   position.owed_wire -= taken;
   return taken;
}

/// The `sysio.liq` shadow symbol behind depot-native `token_code`, or `std::nullopt` when the
/// resolver finds none (WIRE, or an unknown code). Never throws, so the never-throw paths skip on
/// `std::nullopt` -- impossible for a row that was bonded through the resolver.
std::optional<symbol_code> shadow_symbol_of(sysio::slug_name token_code) {
   const auto custody_token = resolve_depot_native_token(token_code);
   if (!custody_token || custody_token->contract != opreg::LIQ_ACCOUNT) return std::nullopt;
   return custody_token->sym.code();
}

/// The `sysio.liq` shadow symbol a balance row earns yield on, or `std::nullopt` for a row that earns
/// none (or whose token the resolver no longer finds). Never throws. The never-throw paths that
/// call it -- and the `custody` calls that follow, which read `yieldidx` -- therefore deserialize
/// `sysio.liq`'s `stat` and `yieldidx` rows: a change to either row layout must ship together with
/// a redeploy of this contract.
std::optional<symbol_code> earning_symbol(const opreg::balance_entry& b) {
   if (!earns_shadow_yield(b.chain_code, b.token_code)) return std::nullopt;
   return shadow_symbol_of(b.token_code);
}

/// Settle a shadow row and take what it has earned from its token's `yieldpool`, as far as the pool
/// covers (`custody::settle_and_take`); the uncovered rest stays banked on the row. Returns the
/// amount taken, which the caller credits to the operator's WIRE claim; 0 for any other row. Never
/// throws.
uint64_t take_yield(name self, opreg::balance_entry& b,
                    uint64_t limit = std::numeric_limits<uint64_t>::max()) {
   const auto sym = earning_symbol(b);
   if (!sym) return 0;
   opreg::yieldpool_t          pools(self);
   const opreg::yield_pool_key key{b.token_code};
   auto pool = pools.try_get(key).value_or(custody::yield_pool{});
   custody::settle(b.shadow_yield, b.balance, custody::live_index(opreg::LIQ_ACCOUNT, *sym));
   const uint64_t taken = take_bounded_yield(b.shadow_yield, pool, limit);
   if (taken > 0) pools.upsert(ram_payer, key, pool);
   return taken;
}

/// Settle every shadow row the operator holds and take what each has earned, as far as each pool
/// covers, for the TERMINATED payout. Returns
/// the amount taken per row, for the caller to credit to the operator's WIRE claim alongside the
/// principal; what a pool could not cover stays banked for `claimyield`. Never throws.
std::vector<uint64_t> take_all_earned_yield(name self, opreg::operator_entry& o) {
   std::vector<uint64_t> taken;
   for (auto& b : o.balances) {
      if (earns_shadow_yield(b.chain_code, b.token_code)) taken.push_back(take_yield(self, b));
   }
   return taken;
}

/// Credit each amount in `yield_credits` to `account`'s WIRE claim row. Never throws.
void credit_yield(name self, name account, const std::vector<uint64_t>& yield_credits) {
   for (const uint64_t amount : yield_credits) {
      credit_remit_claim(self, account, opp::wire::token_code, amount);
   }
}

/// Pull what `sysio.liq` owes the registry's holder row for `sym` into `token_code`'s `yieldpool`:
/// `custody::pull` records it and sends the claim that pays it, or does nothing when nothing is
/// owed; this wrapper is the pool table's I/O around it. Returns the amount pulled.
/// Caller-signed actions only (the checked `owed` and the pushed claim can throw).
uint64_t pull_registry_yield(name self, sysio::slug_name token_code, symbol_code sym) {
   opreg::yieldpool_t          pools(self);
   const opreg::yield_pool_key key{token_code};
   auto pool = pools.try_get(key).value_or(custody::yield_pool{});
   const uint64_t pulled = custody::pull(pool, opreg::LIQ_ACCOUNT, self, sym);
   if (pulled > 0) pools.upsert(ram_payer, key, pool);
   return pulled;
}

/// Apply a balance change of `delta` to `b`. A shadow row goes through `custody::settle_and_adjust`,
/// which settles its position at `sysio.liq`'s live index before the change, so accrual at the old
/// balance is banked; every other row changes directly. `delta` fits `int64_t`: every balance is
/// capped at `MAX_COLLATERAL_AMOUNT` (2^62 - 1). The caller has already enforced underflow.
void adjust_balance(opreg::balance_entry& b, int64_t delta) {
   if (const auto sym = earning_symbol(b)) {
      custody::settle_and_adjust(b.shadow_yield, b.balance, delta, opreg::LIQ_ACCOUNT, *sym);
   } else if (delta >= 0) {
      b.balance += static_cast<uint64_t>(delta);
   } else {
      b.balance -= static_cast<uint64_t>(-delta);
   }
   b.last_updated_ms = current_time_ms();
}

/// Add `amount` to the (chain_code, token_code) balance row, creating the row
/// if it doesn't exist. Mutates the operator entry in place — caller is
/// expected to be inside an `ops.modify(...)` lambda. Callers MUST first verify
/// the credit keeps the row within `MAX_COLLATERAL_AMOUNT` (see `balance_of`);
/// `add_balance` itself does not cap, mirroring the unchecked `subtract_balance`.
///
/// A shadow row is settled first (`adjust_balance` → `custody::settle_and_adjust`). A row created
/// here settles at balance 0, so its checkpoint starts at `sysio.liq`'s current index and it earns
/// nothing distributed before it was bonded.
void add_balance(opreg::operator_entry& o,
                 sysio::slug_name chain_code, sysio::slug_name token_code,
                 uint64_t amount) {
   auto it = std::find_if(o.balances.begin(), o.balances.end(), [&](const opreg::balance_entry& b) {
      return b.chain_code == chain_code && b.token_code == token_code;
   });
   if (it == o.balances.end()) {
      it = o.balances.insert(o.balances.end(),
                             opreg::balance_entry{.chain_code = chain_code, .token_code = token_code});
   }
   adjust_balance(*it, static_cast<int64_t>(amount));
}

/// Subtract `amount` from the (chain_code, token_code) balance row. Caller
/// must have already validated the available balance via `available_inline`.
/// Mutates the operator entry in place — caller is expected to be inside an
/// `ops.modify(...)` lambda.
///
/// The underflow check stays here, before the change: a shadow row reaches `custody` only with a
/// debit its balance covers. A shadow row is then settled BEFORE the subtraction (`adjust_balance` →
/// `custody::settle_and_adjust`): what the row earned up to now stays banked on it, and only the
/// remaining balance earns from here. The slash and withdraw-flush paths rely on this to keep the
/// yield earned while bonded claimable.
void subtract_balance(opreg::operator_entry& o,
                      sysio::slug_name chain_code, sysio::slug_name token_code,
                      uint64_t amount) {
   for (auto& b : o.balances) {
      if (b.chain_code == chain_code && b.token_code == token_code) {
         check(b.balance >= amount, "balance underflow");
         adjust_balance(b, -static_cast<int64_t>(amount));
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

/// Opening edge of the rolling termination window: dellog rows whose `ts_ms`
/// is strictly below this are invisible to `termcheck` and safe to discard.
uint64_t termination_window_open_ms(uint64_t now_ms, const opreg::op_config& cfg) {
   return now_ms > cfg.terminate_window_ms ? now_ms - cfg.terminate_window_ms : 0;
}

/// Bounded oldest-first sweep of dellog rows that have aged out of the
/// rolling termination window. `log_id` allocation order matches `ts_ms`
/// order (both are monotonic), so the primary index walks oldest-first and
/// the sweep stops at the first still-in-window row.
void prune_dellog(uint64_t window_open_ms, uint32_t max_rows) {
   opreg::dellog_t log(name{"sysio.opreg"_n});
   uint32_t removed = 0;
   for (auto it = log.begin();
        it != log.end() && removed < max_rows && it->ts_ms < window_open_ms; ) {
      it = log.erase(std::move(it));
      ++removed;
   }
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
/// After the refactor: `authex::links.bynamechain` is still keyed by `(name, ChainKind)`
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
/// This payload records a depot-local slash in the operator audit trail.
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

} // anonymous namespace

// ---------------------------------------------------------------------------
//  Withdraw queue helpers
// ---------------------------------------------------------------------------

namespace {

/// Re-evaluate whether an operator's available collateral still satisfies the
/// configured minimums after a balance or reservation change.
void reevaluate_eligibility(opreg::operators_t& ops,
                            const opreg::operator_key& op_pk,
                            name self,
                            name account);

struct enqueue_result {
   bool        success;
   uint64_t    request_id;   // valid only when success == true
   std::string error_message;
};

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

   opreg::wtdwqueue_t queue(name{"sysio.opreg"_n});
   auto by_account = queue.get_index<"byaccount"_n>();
   auto it  = by_account.lower_bound(account.value);
   auto end = by_account.upper_bound(account.value);
   uint32_t outstanding = 0;
   for (; it != end &&
          outstanding < opreg::MAX_OUTSTANDING_WITHDRAWS_PER_COLLATERAL_BUCKET;
        ++it) {
      if (it->account != account) break;
      if (it->chain_code != chain_code || it->token_code != token_code) continue;
      ++outstanding;
   }
   if (outstanding >= opreg::MAX_OUTSTANDING_WITHDRAWS_PER_COLLATERAL_BUCKET) {
      return {
         false, 0, "operator already has an outstanding withdraw request for this collateral bucket"
      };
   }

   uint32_t now_ep = get_current_epoch();
   uint64_t request_id = next_withdraw_id();

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
//  withdraw — operator-callable depot-native collateral withdraw (queued)
// ---------------------------------------------------------------------------
//
// Operator-authorized; queues a (chain=WIRE, token=token_code) row in the
// withdraw queue subject to WITHDRAW_WAIT_EPOCHS maturation. A token this
// registry cannot custody reverts, so a request that could never be paid out
// is never queued. Every other validation failure DOES NOT revert — it
// appends a failure entry to the operator's `recent_actions` ring buffer,
// The operator reads the recorded result through WIRE JSON-RPC.
void opreg::withdraw(name account, sysio::slug_name token_code, uint64_t amount) {
   require_auth(account);
   check(resolve_depot_native_token(token_code).has_value(), unsupported_token_msg);

   operators_t ops(get_self());
   auto op_pk = operator_key{account.value};

   auto result = try_enqueue_withdraw(account, opp::wire::chain_code, token_code, amount);
   auto action = build_withdraw_request_action(account, opp::wire::chain_code, token_code, amount,
                                               result.request_id);
   append_action_log(ops, op_pk, action, result.success, std::move(result.error_message));
   if (result.success) {
      reevaluate_eligibility(ops, op_pk, get_self(), account);
   }
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

   operators_t ops(get_self());
   auto op_pk = operator_key{account.value};
   if (ops.contains(op_pk)) {
      reevaluate_eligibility(ops, op_pk, get_self(), account);
   }
}

// ---------------------------------------------------------------------------
//  Eligibility re-check helper — invoked after collateral availability changes
// ---------------------------------------------------------------------------
namespace {

/// After a balance or reservation change, re-evaluate whether the operator now
/// meets the minimum-collateral threshold for their type. Terminal punishment
/// and removal states are never reevaluated. If eligibility flipped versus the
/// prior status, fan out to the per-type processor
/// (`processprod` / `processbatch` / `processuw`) which owns the active/standby
/// transition.
void reevaluate_eligibility(opreg::operators_t& ops,
                            const opreg::operator_key& op_pk,
                            name self,
                            name account) {
   // An absent config must not silently skip evaluation: `meets_role_min` already treats a default
   // (empty) requirement vector as "no operator of this role can activate", and a bootstrapped
   // operator bypasses it either way. Returning early here also suppressed the producer rescore
   // notification on chains that had not yet installed opconfig.
   opreg::opconfig_t cfg_tbl(self);
   auto cfg = cfg_tbl.get_or_default(opreg::op_config{});
   auto refreshed = ops.get(op_pk);
   if (has_terminal_status(refreshed.status)) return;
   bool was_eligible = (refreshed.status == OperatorStatus::OPERATOR_STATUS_ACTIVE);
   bool is_eligible  = meets_role_min(refreshed, cfg);

   name handler;
   switch (refreshed.type) {
      case OperatorType::OPERATOR_TYPE_PRODUCER:    handler = "processprod"_n;  break;
      case OperatorType::OPERATOR_TYPE_BATCH:       handler = "processbatch"_n; break;
      case OperatorType::OPERATOR_TYPE_UNDERWRITER: handler = "processuw"_n;    break;
      default:                                       return;
   }

   // Producers dispatch on EVERY balance change, not only on an eligibility transition, because
   // sysio.system scores producer rank on the collateral actually posted: a top-up while already
   // ACTIVE must raise that score, and a partial withdraw must lower it. `processprod` is a no-op
   // on the status when was == is; its notification is the point. Batch operators and underwriters
   // have no such score, so they keep the transition-only dispatch.
   if (was_eligible == is_eligible && refreshed.type != OperatorType::OPERATOR_TYPE_PRODUCER) {
      return;
   }
   action(
      permission_level{self, "active"_n},
      self, handler,
      std::make_tuple(account, was_eligible, is_eligible)
   ).send();
}

/// Tell sysio.system that a producer's standing ended through a path
/// `reevaluate_eligibility` does not cover -- a terminal transition (slash,
/// termination). Same `processprod` channel, no eligibility transition
/// (was == is), so the notification is the whole effect: sysio.system rescores
/// the producer from its live status and sinks its rank key at once, instead of
/// leaving a slashed or terminated producer in the healthy tier until some
/// unrelated event rescored it.
void notify_producer_standing(name self, const opreg::operator_entry& op) {
   if (op.type != OperatorType::OPERATOR_TYPE_PRODUCER) return;
   action(
      permission_level{self, "active"_n},
      self, "processprod"_n,
      std::make_tuple(op.account, false, false)
   ).send();
}

} // anonymous namespace

// ---------------------------------------------------------------------------
//  deposit — operator-callable depot-native collateral deposit
// ---------------------------------------------------------------------------
//
// Operator-authorized; reverts on validation failure so the operator's
// signing tx surfaces the diagnostic immediately. There's no escrow yet
// (the operator hasn't transferred funds), so revert is the right
// failure mode — they retry after fixing whatever was wrong (e.g.,
// re-bootstrap their authex links). On success the matching balance row
// is credited, the action is appended to the operator's `recent_actions`
// ring buffer, and the eligibility transition (if any) is fanned out.
void opreg::deposit(name account, sysio::slug_name token_code, uint64_t amount) {
   require_auth(account);
   check(amount > 0, "amount must be positive");
   const auto custody = resolve_depot_native_token(token_code);
   check(custody.has_value(), unsupported_token_msg);

   operators_t ops(get_self());
   auto op_pk = operator_key{account.value};
   auto op = ops.get(op_pk, "operator not found");
   check(op.status != OperatorStatus::OPERATOR_STATUS_SLASHED &&
         op.status != OperatorStatus::OPERATOR_STATUS_TERMINATED,
         "operator not in a deposit-eligible state");
   check(!op.is_bootstrapped, "bootstrapped operators cannot deposit collateral");

   ops.modify(same_payer, op_pk, [&](auto& o) {
      check(amount <= MAX_COLLATERAL_AMOUNT &&
               balance_of(o, opp::wire::chain_code, token_code) <= MAX_COLLATERAL_AMOUNT - amount,
            "deposit would exceed max collateral");
      add_balance(o, opp::wire::chain_code, token_code, amount);
   });

   // Custody transfer from operator -> opreg on the token's own contract, under the
   // operator's authority, sent after the credit so a transfer-notification
   // re-entry observes the already-committed balance.
   opp::custody::pull_depot_native(get_self(), account, *custody, amount, deposit_transfer_memo);

   auto deposit_action = build_deposit_action(
      operator_chain_address(account, opp::wire::chain_code),
      opp::wire::chain_code, token_code, amount);
   append_action_log(ops, op_pk, deposit_action, /*success*/ true, "");

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
         // Slashed during the wait; the operator no longer owns the collateral.
         append_action_log(ops, op_pk, remit_action, false,
                           "operator slashed during withdraw-wait window");
         queue.erase(wkey);
         continue;
      }

      if (op.status == OperatorStatus::OPERATOR_STATUS_TERMINATED) {
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
         reevaluate_eligibility(ops, op_pk, get_self(), row.account);
         continue;
      }

      // Subtract from balance.
      ops.modify(same_payer, op_pk, [&](auto& o) {
         subtract_balance(o, row.chain_code, row.token_code, row.amount);
      });

      // For depot-native rows: CREDIT the operator's `remitclaims` row in the row's own token -- no
      // transfer happens here, and the operator receives nothing until it calls `claimremit`. The
      // row's `token_code` is carried as-is, never resolved here. This path runs inline from
      // `sysio.epoch::advance`, where a pushed transfer would let the operator's notify handler
      // abort epoch advancement chain-wide.
      credit_remit_claim(get_self(), row.account, row.token_code, row.amount);
      append_action_log(ops, op_pk, remit_action, true, "");

      // Remove the matured reservation before eligibility is recomputed. The
      // balance debit already accounts for this withdrawal; leaving the queue
      // row visible would make available_inline subtract the same amount twice.
      queue.erase(wkey);

      // Re-check eligibility — this withdraw may have dropped the operator
      // below the role minimum.
      reevaluate_eligibility(ops, op_pk, get_self(), row.account);
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

   // Eligibility callbacks are inline today, but the terminal-state invariant
   // belongs at the transition sink as well as at each caller. A stale or
   // newly introduced callback must never reactivate an operator after slash
   // or termination merely because its collateral predicate says eligible.
   // The notification below is NOT gated on it: a producer's terminal
   // transition is exactly what sysio.system must hear about, and slash and
   // termination dispatch through here (`notify_producer_standing`) to say so.
   const bool terminal = has_terminal_status(ops.get(op_pk).status);

   auto now = current_time_ms();
   if (!terminal && !was_eligible && is_eligible) {
      ops.modify(same_payer, op_pk, [&](auto& o) {
         o.status       = OperatorStatus::OPERATOR_STATUS_ACTIVE;
         o.available_at = now;
      });
   } else if (!terminal && was_eligible && !is_eligible) {
      ops.modify(same_payer, op_pk, [&](auto& o) {
         o.status = OperatorStatus::OPERATOR_STATUS_UNKNOWN;
      });
   }

   // Notify OUTSIDE the transition branches. sysio.system rescores the producer's rank from its
   // live standing, so it must hear about a top-up that changed no status, about a drop out of
   // ACTIVE, and about a slash or termination -- not only about a promotion. A stale score is not
   // merely cosmetic: it leaves a de-collateralized, slashed or terminated producer holding an
   // index slot ahead of bonded ones.
   if (notify_system) {
      require_recipient(opreg::SYSTEM_ACCOUNT);
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
//  slash — punitive removal of the depot collateral balance
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

   // Snapshot the full collateral balance for the slash and audit log.
   struct slash_pair { sysio::slug_name chain_code; sysio::slug_name token_code; uint64_t amount; };
   std::vector<slash_pair> to_slash;
   for (const auto& bal : op.balances) {
      uint64_t amt = slashable_now(op, bal.chain_code, bal.token_code);
      if (amt > 0) {
         to_slash.push_back({bal.chain_code, bal.token_code, amt});
      }
   }

   // Mark SLASHED and remove each collateral balance; custody remains here.
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
      append_action_log(ops, op_pk, slash_action, /*success*/ true, "");
   }

   notify_producer_standing(get_self(), op);
}

// ---------------------------------------------------------------------------
//  terminate / termcheck / recorddel — administrative removal
// ---------------------------------------------------------------------------

namespace {

/// Internal terminate body — used by both the operator-removal path
/// (`termcheck` -> `terminate` inline) and the slashing-equivalent path for
/// completeness. Marks status TERMINATED and remits each
/// depot-native token balance to a pull claim for the operator.
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
      // Termination returns the full remaining balance through pull claims.
      if (amt > 0) {
         to_remit.push_back({bal.chain_code, bal.token_code, amt});
      }
   }

   // Settle earned shadow yield alongside principal, up to the yield pool balance.
   // Uncovered yield stays banked for claimyield indefinitely, including after operator removal.
   std::vector<uint64_t> yield_credits;
   ops.modify(same_payer, op_pk, [&](auto& o) {
      o.status        = OperatorStatus::OPERATOR_STATUS_TERMINATED;
      o.terminated_at = now;
      o.status_reason = reason;
      for (const auto& rp : to_remit) {
         subtract_balance(o, rp.chain_code, rp.token_code, rp.amount);
      }
      yield_credits = take_all_earned_yield(self, o);
   });

   // Credit each token's pull claim and append its local withdrawal audit entry.
   // No recipient notification can interrupt epoch-driven termination.
   for (const auto& rp : to_remit) {
      credit_remit_claim(self, account, rp.token_code, rp.amount);
      OperatorAction remit_action = build_withdraw_remit_action(
         account, rp.chain_code, rp.token_code, rp.amount, /*request_id*/ 0);
      append_action_log(ops, op_pk, remit_action, /*success*/ true,
                        std::string("terminate-remit"));
   }
   // No OperatorAction names a yield payout, so it is credited without a recent-actions entry.
   credit_yield(self, account, yield_credits);

   notify_producer_standing(self, op);
}

} // anonymous namespace

void opreg::terminate(name account, std::string reason) {
   require_auth(get_self());
   terminate_inline(get_self(), account, reason);
}

// claimremit - pull depot-native collateral credited by a WIRE-chain remit, one token at a time.
//
// The remit paths (withdraw flush and termination payout) credit rather than
// transfer because every one of them is reachable from `sysio.epoch::advance`, which must never
// abort. This is the only place such a balance becomes a transfer, and it carries the operator's
// own authority, so a hostile transfer-notify handler blocks nothing but this caller's own claim.
//
// The row is erased before the transfer is queued (inside pay_out), so a notify handler that
// re-enters claimremit finds no row and cannot double spend -- the same ordering guard `deposit`
// applies by crediting before it transfers.
void opreg::claimremit(name account, sysio::slug_name token_code) {
   require_auth(account);
   const auto custody = resolve_depot_native_token(token_code);
   check(custody.has_value(), unsupported_token_msg);

   remitclaims_t claims(get_self());
   sysio::opp::claimable::pay_out(
      claims, remitclaim_key{account.value, token_code}, get_self(), custody->contract,
      account, custody->sym, std::string(claimremit_transfer_memo),
      "no claimable remit for this account");
}

// sweepyield - claim the WIRE yield sysio.liq owes the registry's holder row into the registry.
//
// Permissionless: it only moves WIRE the registry is already owed into the registry. Attribution
// does not depend on it -- every bonded row checkpoints sysio.liq's own index -- so when it runs
// changes no operator's entitlement; it changes only what `yieldpool` can cover.
// `custody::pull` computes the amount from the same rows with the same formula `sysio.liq::claim`
// settles with, records it on the pool and sends the claim, in the same transaction, so the claim
// pays exactly `swept`. With nothing owed it does neither, and the check below reverts.
void opreg::sweepyield(sysio::slug_name token_code) {
   const auto custody_token = resolve_depot_native_token(token_code);
   check(custody_token.has_value(), unsupported_token_msg);
   check(earns_shadow_yield(opp::wire::chain_code, token_code), wire_earns_no_yield_msg);

   const uint64_t swept = pull_registry_yield(get_self(), token_code, custody_token->sym.code());
   check(swept > 0, no_yield_to_sweep_msg);
}

// claimyield - credit an operator's earned shadow yield to its WIRE claim row.
//
// Permissionless: the credit can only land in `account`'s own `remitclaims{account, WIRE}` row, so
// anyone may crank it, including after pruning or re-registration archived the debt.
// First pulls whatever sysio.liq owes the registry's row into the pool, then credits the
// row's earned yield up to what the pool covers. Credits rather than transfers, like every other
// payout here, so the WIRE leaves only through `claimremit` under the operator's own authority.
// Allowed in any status: yield earned before a slash stays the operator's.
void opreg::claimyield(name account, sysio::slug_name token_code) {
   operators_t ops(get_self());
   const auto  op_pk = operator_key{account.value};
   yielddebts_t debts(get_self());
   const remitclaim_key debt_key{account.value, token_code};
   auto debt = debts.try_get(debt_key);
   const bool has_operator = ops.contains(op_pk);
   check(has_operator || debt.has_value(), "operator not found");

   remitclaims_t claims(get_self());
   const auto existing = claims.try_get(remitclaim_key{account.value, opp::wire::token_code});
   const uint64_t balance = existing ? existing->balance : 0;
   check(balance < opp::safe::depot_amount_max, yield_remit_full_msg);
   const uint64_t room = opp::safe::depot_amount_max - balance;

   // WIRE and unknown codes have no shadow symbol and fall through to "no yield owed" below.
   if (const auto sym = shadow_symbol_of(token_code)) {
      pull_registry_yield(get_self(), token_code, *sym);
   }

   uint64_t credited = 0;
   bool has_remaining = false;
   if (debt) {
      yieldpool_t pools(get_self());
      const yield_pool_key pool_key{token_code};
      auto pool = pools.try_get(pool_key).value_or(custody::yield_pool{});
      custody::position banked{.owed_wire = static_cast<uint64_t>(
         std::min(debt->owed_wire, static_cast<uint128_t>(std::numeric_limits<uint64_t>::max())))};
      const uint64_t taken = take_bounded_yield(banked, pool, room);
      credited = taken;
      debt->owed_wire -= taken;
      has_remaining = debt->owed_wire > 0;
      if (taken > 0) {
         pools.upsert(ram_payer, pool_key, pool);
         if (debt->owed_wire == 0) debts.erase(debt_key);
         else debts.upsert(ram_payer, debt_key, *debt);
      }
   }
   if (has_operator) {
      ops.modify(same_payer, op_pk, [&](auto& o) {
         for (auto& b : o.balances) {
            if (b.chain_code == opp::wire::chain_code && b.token_code == token_code) {
               credited += take_yield(get_self(), b, room - credited);
               has_remaining = has_remaining || b.shadow_yield.owed_wire > 0;
            }
         }
      });
   }
   check(credited > 0 || has_remaining, no_yield_owed_msg);
   check(credited > 0, yield_not_covered_msg);

   // Both takes were limited to the same remaining room, so credit cannot saturate away debt.
   // No OperatorAction names a yield claim, so none is appended to `recent_actions`.
   credit_remit_claim(get_self(), account, opp::wire::token_code, credited);
}

void opreg::recorddel(name account, uint32_t epoch, bool delivered) {
   require_auth(EPOCH_ACCOUNT);

   // A temporarily ineligible operator cannot submit deliveries because
   // sysio.msgch requires ACTIVE status. Do not turn that expected downtime
   // into termination misses after collateral is restored. Terminal-state
   // observations remain durable audit records; termcheck already excludes
   // those operators permanently.
   operators_t ops(get_self());
   auto op_pk = operator_key{account.value};
   if (ops.contains(op_pk)) {
      auto status = ops.get(op_pk).status;
      if (status != OperatorStatus::OPERATOR_STATUS_ACTIVE &&
          !has_terminal_status(status)) {
         return;
      }
   }

   uint64_t now_ms = current_time_ms();

   // On-write half of the dellog retention contract: sweep a bounded number
   // of rows that have aged out of the rolling window before adding one.
   opconfig_t cfg_tbl(get_self());
   const auto cfg = cfg_tbl.get_or_default(op_config{});
   prune_dellog(termination_window_open_ms(now_ms, cfg), MAX_DELLOG_PRUNE_PER_WRITE);

   dellog_t log(get_self());
   uint64_t id = next_dellog_id();
   log.emplace(ram_payer, delivery_key{id}, delivery_log_entry{
      .log_id    = id,
      .account   = account,
      .epoch     = epoch,
      .delivered = delivered,
      .ts_ms     = now_ms,
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
   // Termination on rolling-buffer underperformance is scoped to batch operators, and for
   // producers that is now a DECISION rather than an open question.
   //
   // A producer that misses `max_consecutive_missed_rounds` consecutive scheduled rounds is
   // DEMOTED by sysio.system -- moved to a categorical tier no score can climb out of, so it
   // leaves the schedule and draws no pay. Demotion is deliberately recoverable: the producer
   // re-registers via `regproducer` when it is ready again. Termination is not recoverable, and
   // it also returns the bond, so applying it to an offline-but-bonded producer would convert a
   // reversible outage into a permanent exit and hand back the collateral that makes the operator
   // accountable. An indefinitely-demoted producer therefore stays demoted -- holding its row and
   // its bond -- until it either re-registers or withdraws of its own accord.
   //
   // Underwriter offline-too-long remains open; they have no committee and no schedule to miss.
   if (op.type != OperatorType::OPERATOR_TYPE_BATCH) return;

   // Thresholds come from opconfig — tests can dial them down so the
   // miss-window evaluation fits the test timeout budget.
   opconfig_t cfg_tbl(get_self());
   const auto cfg = cfg_tbl.get_or_default(op_config{});

   uint64_t now_ms      = current_time_ms();
   uint64_t window_open = termination_window_open_ms(now_ms, cfg);

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
      if (!it->delivered) total_misses++;

      // A transition back to ACTIVE starts a new duty interval. Earlier rows
      // remain part of the rolling miss-rate sample, but must not join a
      // consecutive run across time when the operator could not deliver.
      if (it->ts_ms < op.available_at) {
         consecutive_misses = 0;
         continue;
      }

      if (!it->delivered) {
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
//  prune — remove terminated operator rows past the delay + expired dellog rows
// ---------------------------------------------------------------------------
void opreg::prune() {
   opconfig_t cfg_tbl(get_self());
   check(cfg_tbl.exists(), "opconfig not initialized");
   auto cfg = cfg_tbl.get();

   auto now = current_time_ms();
   operators_t ops(get_self());
   auto status_idx = ops.get_index<"bystatus"_n>();

   // Wait for the retention delay and principal settlement; preserve unpaid yield separately.
   // Count examined rows so unsettled entries cannot make this crank unbounded.
   uint32_t examined = 0;
   for (auto it = status_idx.lower_bound(
           magic_enum::enum_integer(OperatorStatus::OPERATOR_STATUS_TERMINATED));
        it != status_idx.end() &&
        it->status == OperatorStatus::OPERATOR_STATUS_TERMINATED &&
        examined < MAX_OPERATOR_PRUNE_PER_CRANK;) {
      ++examined;
      const bool delay_elapsed = it->terminated_at > 0
                                 && now - it->terminated_at >= cfg.terminate_prune_delay_ms;
      if (delay_elapsed && is_fully_settled(*it)) {
         preserve_yield_debt(get_self(), *it);
         it = status_idx.erase(std::move(it));
      } else {
         ++it;
      }
   }

   // Crank half of the dellog retention contract: clear delivery-log rows
   // that have aged out of the rolling termination window.
   prune_dellog(termination_window_open_ms(now, cfg), MAX_DELLOG_PRUNE_PER_CRANK);
}

} // namespace sysio

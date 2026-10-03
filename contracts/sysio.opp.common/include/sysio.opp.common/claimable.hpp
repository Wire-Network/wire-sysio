#pragma once
/**
 * @file claimable.hpp
 * @brief Pull-payment primitives for payouts that originate on never-throw paths.
 *
 * `sysio.token::transfer` calls `require_recipient(from)` and `require_recipient(to)`, and the
 * chain executes notified receivers with no exception isolation (`apply_context::exec`). An
 * assert inside a recipient's `on_notify("sysio.token::transfer")` handler therefore aborts the
 * WHOLE transaction, including every parent inline action -- and a handler can equally burn CPU
 * until the enclosing action blows its deadline.
 *
 * That makes a pushed transfer unusable on any never-throw path. `sysio.epoch::advance` and the
 * `sysio.msgch::deliver -> evalcons -> dispatch` chain both pre-validate every `check()` they can
 * reach so they cannot abort (`feedback_opp_handlers_never_throw.md`), but that discipline stops
 * at the contract's own guards: once value is pushed to an account the protocol does not control,
 * the counterparty decides whether the transaction commits. A single uncooperative recipient can
 * stall epoch advancement chain-wide.
 *
 * The fix is to never push. A never-throw path credits a claimable balance and emits no transfer;
 * the recipient later pulls it with an action carrying its own authority. A handler that aborts
 * then blocks only its own claim.
 *
 * `sysio.dclaim` established this pattern (`onreward` credits `pending_claims`, `claim` pays out);
 * these helpers generalize it so `sysio.system`, `sysio.reserv` and `sysio.opreg` share one
 * audited implementation rather than three copies.
 *
 * ## Row contract
 *
 * Each contract declares its OWN `[[sysio::table]]`-attributed row and key, because the table name
 * is baked into both the attribute and the `kv::table` template argument, and because a
 * `[[sysio::table]]`-attributed struct cannot be shared into `sysio.system`'s translation unit
 * without corrupting that contract's read-only-action return codegen (see the note on
 * `sysio.reserv::rewards_bucket`). The helpers below are templated over the table instead, and
 * require only that the row expose:
 *
 *   * `uint64_t balance`            -- required, the claimable amount in atomic units of THAT
 *                                      contract's token (currently WIRE for every consumer) --
 *                                      these helpers never name a symbol; `pay_out`'s caller
 *                                      supplies it
 * All consumers retain credited balances indefinitely, until the recipient claims them.
 */

#include <sysio/action.hpp>
#include <sysio/asset.hpp>
#include <sysio/check.hpp>
#include <sysio/name.hpp>

#include <sysio.opp.common/safe_ops.hpp>

#include <cstdint>
#include <string>

namespace sysio::opp::claimable {

/// Saturating credit, capped at `safe::depot_amount_max` (2^62-1) rather than `UINT64_MAX`.
///
/// The cap is deliberately the `sysio::asset` magnitude limit, not the integer limit: `pay_out`
/// carries the stored balance out as an `asset`, and `asset`'s constructor `check()`-aborts above
/// `max_amount`. Saturating at the integer limit here would merely move the abort from credit time
/// (on a never-throw path) to claim time, stranding the balance permanently. Capping at the asset
/// limit keeps the row payable end to end. The cap is unreachable for any real payout.
inline uint64_t add_capped(uint64_t balance, uint64_t amount) {
   constexpr uint64_t cap = static_cast<uint64_t>(safe::depot_amount_max);
   if (balance >= cap) return cap;
   const uint64_t room = cap - balance;
   return amount >= room ? cap : balance + amount;
}

/// Credit `amount` to a claimable row, creating it when absent and accumulating when present.
///
/// Never throws: a zero amount is a silent no-op and the credit saturates rather than aborting, so
/// this is safe to call from `sysio.epoch::advance` and from OPP inbound dispatch handlers.
///
/// @param tbl      the contract's claimable kv table.
/// @param payer    RAM payer for a newly created row.
/// @param key      primary key for the recipient.
/// @param fresh    prototype row used when the key is absent; the caller pre-fills the identifying
///                 fields (`account`, ...) and this function sets `balance`.
/// @param amount   atomic units to credit, in the caller's token (see the row contract above).
template<class Table, class Key, class Row>
void credit(Table& tbl, sysio::name payer, const Key& key, Row fresh, uint64_t amount) {
   if (amount == 0) return;

   fresh.balance = add_capped(0, amount);

   tbl.upsert(payer, key, fresh, [&](Row& r) {
      r.balance = add_capped(r.balance, amount);
   });
}

/// Drain a claimable row and emit the single `sysio.token::transfer` that pays it out.
///
/// This is the ONLY place a claimable balance becomes a transfer, and it is reached only from an
/// action carrying the claimant's own authority. A recipient whose notify handler aborts therefore
/// blocks nothing but its own claim.
///
/// The row is erased BEFORE the transfer is queued. The transfer notifies `to`, whose handler may
/// re-enter the claim action; erasing first means the re-entry observes no row and cannot double
/// spend. (Same ordering rationale as the credit-before-transfer guard in `sysio.opreg::deposit`.)
///
/// Unlike `credit`, this DOES `check()`-throw when there is nothing to claim -- correct here,
/// because the throw reaches only the claimant who asked for it.
///
/// @return the amount paid out, in atomic units of the `symbol` passed in.
template<class Table, class Key>
uint64_t pay_out(Table& tbl, const Key& key, sysio::name self, sysio::name token_account,
                 sysio::name to, const sysio::symbol& sym, const std::string& memo,
                 const char* nothing_to_claim_msg) {
   auto it = tbl.find(key);
   sysio::check(it != tbl.end(), nothing_to_claim_msg);

   const uint64_t amount = it->balance;
   sysio::check(amount > 0, nothing_to_claim_msg);

   tbl.erase(key);

   sysio::action(
      sysio::permission_level{self, "active"_n},
      token_account, "transfer"_n,
      std::make_tuple(self, to, sysio::asset(static_cast<int64_t>(amount), sym), memo)
   ).send();

   return amount;
}

} // namespace sysio::opp::claimable

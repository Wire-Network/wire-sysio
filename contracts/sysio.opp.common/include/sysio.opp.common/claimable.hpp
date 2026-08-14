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
 *                                      contract's token (WIRE for sysio.system / sysio.reserv,
 *                                      CORE_SYM for sysio.opreg) -- these helpers never name a
 *                                      symbol; `pay_out`'s caller supplies it
 *   * `uint32_t expires_at_sec`     -- optional; when present it is maintained by `credit` and
 *                                      makes the row eligible for `sweep_expired`
 */

#include <sysio/action.hpp>
#include <sysio/asset.hpp>
#include <sysio/check.hpp>
#include <sysio/name.hpp>

#include <sysio.opp.common/safe_ops.hpp>

#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace sysio::opp::claimable {

/// Compile-time detection of the optional `expires_at_sec` member on a claimable row.
///
/// A row that omits the field opts out of expiry entirely: nothing stamps it and `sweep_expired`
/// cannot select it. Every row in this tree currently CARRIES the field -- `payclaims`,
/// `wireclaims` and `remitclaims` alike -- so the false branch is the shape a future bounded-set
/// contract may choose, not a description of any table today. Whether a stamped row is ever acted
/// on is a separate, per-contract decision (only `wireclaims` is swept; see WIRE-339).
template<class Row, class = void>
struct has_expiry : std::false_type {};

template<class Row>
struct has_expiry<Row, std::void_t<decltype(std::declval<Row&>().expires_at_sec)>> : std::true_type {};

template<class Row>
inline constexpr bool has_expiry_v = has_expiry<Row>::value;

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
///                 fields (`account`, ...) and this function sets `balance` (and `expires_at_sec`).
/// @param amount   atomic units to credit, in the caller's token (see the row contract above).
/// @param expires_at_sec  absolute expiry stamp, ignored unless the row carries the field. Passing
///                 the refreshed expiry on every credit means an account with ongoing activity
///                 never expires mid-stream.
template<class Table, class Key, class Row>
void credit(Table& tbl, sysio::name payer, const Key& key, Row fresh, uint64_t amount,
            uint32_t expires_at_sec = 0) {
   if (amount == 0) return;

   fresh.balance = add_capped(0, amount);
   if constexpr (has_expiry_v<Row>) {
      fresh.expires_at_sec = expires_at_sec;
   }

   tbl.upsert(payer, key, fresh, [&](Row& r) {
      r.balance = add_capped(r.balance, amount);
      if constexpr (has_expiry_v<Row>) {
         r.expires_at_sec = expires_at_sec;
      }
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

/// Bounded sweep of rows past their expiry, returning the reclaimed total.
///
/// Iterates the caller's expiry-ordered secondary index so the oldest rows are visited first and
/// the scan can stop at the first live row -- a bounded scan over the PRIMARY (account-ordered)
/// index would repeatedly re-walk the same low-key live rows and might never reach an expired one.
///
/// Expired keys are collected first and erased afterwards, rather than erasing through the
/// secondary iterator mid-walk: mutating a kv secondary index while iterating it is the same
/// foot-gun `sysio.system::payepoch` avoids with its `to_reset` snapshot.
///
/// Never throws, so it is safe to call from the credit path as an on-write retention contract (the
/// shape `sysio.opreg::prune_dellog` uses).
///
/// @param tbl        the contract's claimable kv table.
/// @param by_expiry  secondary index ordered by `expires_at_sec`.
/// @param to_key     maps a row to its primary key.
/// @param now_sec    current wall-clock seconds.
/// @param max_rows   hard bound on rows erased in one call, keeping the caller inside its CPU
///                   deadline.
template<class Table, class Index, class ToKey>
uint64_t sweep_expired(Table& tbl, Index& by_expiry, ToKey&& to_key, uint32_t now_sec,
                       uint32_t max_rows) {
   using Key = std::decay_t<decltype(to_key(*by_expiry.begin()))>;

   std::vector<Key> doomed;
   uint64_t reclaimed = 0;

   for (auto it = by_expiry.begin(); it != by_expiry.end() && doomed.size() < max_rows; ++it) {
      // A zero stamp means "never expires"; such rows sort first, so skip rather than stop.
      if (it->expires_at_sec == 0) continue;
      // Index is expiry-ordered: the first live row means every later row is live too.
      if (it->expires_at_sec > now_sec) break;
      reclaimed = safe::add_sat_u64(reclaimed, it->balance);
      doomed.push_back(to_key(*it));
   }

   for (const auto& k : doomed) {
      tbl.erase(k);
   }

   return reclaimed;
}

} // namespace sysio::opp::claimable

#pragma once
/**
 * @file shadow_custody.hpp
 * @brief Custodian-side shadow yield: attribute the yield a custodian's single `sysio.liq` holder
 *        row earns to the sub-holders whose shadow it holds, strictly within the WIRE it has pulled.
 *
 * Integration guide: `docs/shadow-custody-integration.md`. The row types are in
 * `shadow_custody_types.hpp`, so a contract header can declare its tables with those alone.
 *
 * ## Model
 *
 * A custodian contract holds shadow LIQ on behalf of sub-holders -- `sysio.opreg` holds operators'
 * bonded collateral. `sysio.liq` sees ONE holder, the custodian, and pays that row's WIRE yield
 * through the cumulative per-symbol index `shadow_yield.hpp` describes. This header divides that
 * yield among the sub-holders by the same index, one level down:
 *
 *   * each sub-holder row EMBEDS a `position`, which checkpoints `sysio.liq`'s LIVE index for the
 *     symbol and banks what was earned. Every change to the sub-holder's balance goes through
 *     `settle_and_adjust`, which settles first -- a new position settles at balance 0 and so starts
 *     at the current index. A sub-holder unit therefore earns exactly what a unit of the
 *     custodian's liq row earns, from the moment it is held until it leaves, however the custodian
 *     times its pulls;
 *   * the custodian keeps one `yield_pool` per symbol, in its own table under its own key. `pull`
 *     records on it exactly what the `sysio.liq::claim` it sends will pay, in the same transaction;
 *     every credit to a sub-holder goes through `settle_and_take`, which never credits more than
 *     the pool has received and not yet credited.
 *
 * ## Entry points
 *
 * Three composites are THE documented way to use this header. Each reads `sysio.liq`'s live index
 * itself and fuses a sequence a custodian must never split:
 *
 *   * `settle_and_adjust(position&, balance&, delta, liq_account, sym)` -- settle, then change the
 *     balance (the shape of `sysio.liq`'s own `settle_and_adjust`); returns the change applied;
 *   * `settle_and_take(position&, balance, yield_pool&, liq_account, sym)` -- settle, then take
 *     what the pool covers; returns the amount to credit;
 *   * `pull(yield_pool&, liq_account, custodian, sym)` -- record what `sysio.liq` owes the
 *     custodian's row AND send the claim that pays it, or do neither.
 *
 * The primitives they are built from (`live_index`, `settle`, the index-taking `settle_and_adjust`,
 * `take`, `custodian_owed`, `record_pull`, `claim_action`) stay public as building blocks for a
 * custodian with unusual needs, which then owns the sequencing the composites guarantee.
 *
 * ## The custodian's obligations
 *
 *   0. **Hold what you attribute.** For each symbol, the sum of sub-holder balances never exceeds
 *      the custodian's own `sysio.liq` holding, and a sub-holder is credited a balance before the
 *      shadow arrives only within the same action that brings it in. Otherwise positions accrue on
 *      shadow the custodian's liq row does not hold, the row is owed less than the positions, and
 *      because the pool is first-come-first-served (see Solvency) the first sub-holders to claim
 *      take yield that the others earned.
 *   1. Embed a `position` in each sub-holder row and change that row's balance only through
 *      `settle_and_adjust` (a new row: create it at balance 0, then adjust).
 *   2. Pull only through `pull`, from a caller-signed action, and at most ONCE per symbol per
 *      action. `pull` records a PREDICTED payout -- the claim it sends executes after the calling
 *      action returns -- so a second `pull` in the same action would see the same owed amount
 *      again. `pull` refuses that case itself: when the live index and the custodian's owed both
 *      equal its last recorded pull, that claim has not executed yet, and it records and sends
 *      nothing.
 *   3. Queue nothing that touches the custodian's `sysio.liq` row or the symbol's index ahead of
 *      `pull` in the same action (a shadow transfer, an `addyield`, a claim of your own): an inline
 *      action queued earlier executes before `pull`'s claim, the claim then pays a different
 *      amount than was recorded, and the difference is stranded in the custodian's account.
 *   4. Credit sub-holders only with what `settle_and_take` returns, into a ledger the sub-holder
 *      pulls from (`claimable.hpp`), never by pushing a transfer from a never-throw path.
 *   5. Decide, as its own policy, what happens to yield `settle_and_take` could not yet cover and
 *      to yield left unclaimed when a sub-holder row is erased.
 *
 * ## Never-throw contract
 *
 * `live_index`, `settle`, both `settle_and_adjust`, `settle_and_take`, `record_pull` and `take`
 * read at most `sysio.liq`'s `yieldidx` row and otherwise do saturating arithmetic on the
 * custodian's own values: no `check()`, safe on a never-throw path. `pull` and `custodian_owed`
 * also read the custodian's `sysio.liq` `accounts` row with the CHECKED `opp::shadow::owed` (it
 * must match what `sysio.liq::claim` computes), and the claim `pull` / `claim_action` push can throw
 * inside `sysio.liq`: those belong on caller-signed actions only. Every custodian deserializes
 * `sysio.liq`'s `yieldidx` and `accounts` rows, so a change to either layout must ship with a
 * redeploy of every custodian together with `sysio.liq`.
 *
 * ## Authority
 *
 * `pull` pushes `sysio.liq::claim(custodian, sym)` under `custodian@active`
 * (`CLAIM_PERMISSION`). A privileged custodian (`sysio.opreg`) may do so as is; any other custodian
 * needs its own `sysio.code` permission on its `active` authority.
 *
 * ## Solvency
 *
 * Credits never exceed the WIRE actually pulled from `sysio.liq`: `take` caps every credit at the
 * pool's `received - credited`, and `received` grows only by what a pushed claim pays. So a
 * sub-holder credit is always backed by WIRE the custodian holds, never by other balances in the
 * same account. The attribution itself can ask for slightly more than was pulled: `sysio.liq`
 * floors the custodian's row at every one of its settles (each claim, each shadow transfer in or
 * out), while a position floors once over its whole interval, so positions can be owed up to one
 * atomic WIRE per custodian-row settle more than was received. That dust stays banked on the
 * position until slack covers it -- yield on shadow the custodian holds for no sub-holder, which no
 * position claims -- and what becomes of dust still owed is the custodian's policy (obligation 5).
 * The pool is first-come-first-served across sub-holders: a dust shortfall falls on whoever takes
 * last, not on the position whose flooring created it.
 */

#include <sysio/action.hpp>
#include <sysio/asset.hpp>
#include <sysio/kv_scoped_table.hpp>
#include <sysio/kv_table.hpp>
#include <sysio/name.hpp>
#include <sysio/symbol.hpp>

#include <sysio.opp.common/safe_ops.hpp>
#include <sysio.opp.common/shadow_custody_types.hpp>
#include <sysio.opp.common/shadow_yield.hpp>

#include <algorithm>
#include <cstdint>
#include <tuple>

namespace sysio::opp::shadow::custody {

/// The permission a custodian pushes its `sysio.liq::claim` under: its own `active`.
inline constexpr name CLAIM_PERMISSION = "active"_n;

/// `actor@CLAIM_PERMISSION`, the authority of a custodian's pushed claim (`sysio.liq`'s
/// `active_of` shape).
inline permission_level claim_authority(name actor) { return permission_level{actor, CLAIM_PERMISSION}; }

/// Largest yield a position banks: the `asset` magnitude limit, so the WIRE credit it becomes can
/// always be carried by an `asset`. `settle` clamps here instead of `check()`-ing.
inline constexpr uint64_t MAX_OWED_WIRE = static_cast<uint64_t>(asset::max_amount);

// ---------------------------------------------------------------------------------------------
//  Primitives
// ---------------------------------------------------------------------------------------------

/// `sysio.liq`'s cumulative index for `sym` -- the raw `yieldidx.index` its own `claim` settles
/// with -- or 0 before the symbol's first distribution. Never throws.
inline u128 live_index(name liq_account, symbol_code sym) {
   yield_index_table indexes(liq_account);
   const auto        row = indexes.try_get(symbol_key{sym.raw()});
   return row ? row->index : u128{0};
}

/// Settle `pos`, holding `balance` units, at `index`: bank what it earned since its checkpoint
/// (`opp::shadow::owed`) and move the checkpoint to `index`. The result saturates at MAX_OWED_WIRE,
/// including when `balance * (index - checkpoint)` itself would pass 2^128, so nothing wraps.
/// `sysio.liq`'s index only grows; should it ever read below the checkpoint (a reset), the position
/// keeps what it banked, accrues nothing, and re-checkpoints. Never throws.
inline void settle(position& pos, uint64_t balance, u128 index) {
   if (index >= pos.index_checkpoint) {
      const u128 delta = index - pos.index_checkpoint;
      if (balance != 0 && delta > ~u128{0} / balance) {
         pos.owed_wire = MAX_OWED_WIRE;   // the product alone passes 2^128
      } else {
         const u128 total = owed(balance, pos.owed_wire, pos.index_checkpoint, index);
         pos.owed_wire    = total > MAX_OWED_WIRE ? MAX_OWED_WIRE : static_cast<uint64_t>(total);
      }
   }
   pos.index_checkpoint = index;
}

/// Settle `pos` at `index` against the balance it held, then apply `delta` to `balance`. Returns
/// the change actually applied, which differs from `delta` only where a credit saturates at
/// 2^64 - 1 or a debit past zero clamps at 0. Underflow is the CALLER's policy: check
/// `balance >= -delta` before calling where a violation must abort. Never throws.
inline int64_t settle_and_adjust(position& pos, uint64_t& balance, int64_t delta, u128 index) {
   settle(pos, balance, index);
   const uint64_t before = balance;
   if (delta >= 0) {
      balance = safe::add_sat_u64(balance, static_cast<uint64_t>(delta));
      return static_cast<int64_t>(balance - before);
   }
   const uint64_t debit = static_cast<uint64_t>(-(delta + 1)) + 1;   // |delta| without overflow at INT64_MIN
   balance              = balance > debit ? balance - debit : 0;
   return -static_cast<int64_t>(before - balance);
}

/// Take what an already-settled `pos` is owed, up to what `pool` still covers: returns
/// `min(pos.owed_wire, received - credited)`, removes it from the position and adds it to
/// `credited`. What the pool cannot cover stays banked on the position. Never throws.
inline uint64_t take(position& pos, yield_pool& pool) {
   const uint64_t available = pool.received > pool.credited ? pool.received - pool.credited : 0;
   const uint64_t taken     = std::min(pos.owed_wire, available);
   pos.owed_wire  -= taken;
   pool.credited  += taken;
   return taken;
}

/// What `sysio.liq` owes `custodian`'s own holder row for `sym` now, or 0 without a row: exactly
/// what `sysio.liq::claim(custodian, sym)` pays later in the same transaction, because it reads the
/// same rows with the same, CHECKED, formula. Caller-signed actions only: throws on corrupt state.
inline uint64_t custodian_owed(name liq_account, name custodian, symbol_code sym) {
   accounts_table holdings(liq_account, custodian.value);
   const auto   holding = holdings.try_get(symbol_key{sym.raw()});
   return holding ? owed(*holding, live_index(liq_account, sym)) : 0;
}

/// Record on `pool` a pull of `amount` WIRE predicted at `index`: add it to `received` and remember
/// both as the last recorded pull, which `pull` uses to refuse a second pull before the first
/// claim has executed. Saturating; never throws.
inline void record_pull(yield_pool& pool, uint64_t amount, u128 index) {
   pool.received     = safe::add_sat_u64(pool.received, amount);
   pool.pulled_index = index;
   pool.pulled_owed  = amount;
}

/// The inline `sysio.liq::claim(custodian, sym)` a custodian pushes under `claim_authority` to pull
/// what `custodian_owed` reported. The claim can throw inside `sysio.liq`: caller-signed actions
/// only.
inline action claim_action(name custodian, name liq_account, symbol_code sym) {
   return action(claim_authority(custodian), liq_account, CLAIM_ACTION, std::make_tuple(custodian, sym));
}

// ---------------------------------------------------------------------------------------------
//  Entry points
// ---------------------------------------------------------------------------------------------

/// Settle `pos` at `sysio.liq`'s live index for `sym`, then apply `delta` to `balance`: the one call
/// a custodian makes for every change to a sub-holder's balance, so accrual at the old balance is
/// banked before the new balance starts earning. Returns the change applied (see the index-taking
/// form). Never throws; underflow is the caller's policy.
inline int64_t settle_and_adjust(position& pos, uint64_t& balance, int64_t delta, name liq_account,
                                 symbol_code sym) {
   return settle_and_adjust(pos, balance, delta, live_index(liq_account, sym));
}

/// Settle `pos`, holding `balance`, at `sysio.liq`'s live index for `sym`, then take what it is owed
/// up to what `pool` covers. Returns the amount taken, for the custodian to credit to the
/// sub-holder; the uncovered rest stays banked on the position. Never throws.
inline uint64_t settle_and_take(position& pos, uint64_t balance, yield_pool& pool, name liq_account,
                                symbol_code sym) {
   settle(pos, balance, live_index(liq_account, sym));
   return take(pos, pool);
}

/// Pull into the custodian what `sysio.liq` owes its holder row for `sym`: when that is non-zero,
/// record it on `pool` and send the `sysio.liq::claim` that pays exactly it later in this
/// transaction; otherwise do neither. Returns the amount pulled. Recording and sending are one
/// step, so `received` never grows by an amount no claim delivers.
///
/// Refuses (returns 0, records and sends nothing) when the live index and the custodian's owed
/// both equal the last recorded pull: the claim that pull sent has not executed yet, so recording
/// again would count the same WIRE twice (obligation 2). A claim that did execute leaves the row
/// owed nothing at that index, so the refusal never withholds a genuine pull. The caller must also
/// queue nothing that touches `sysio.liq` ahead of this in the same action (obligation 3).
///
/// Caller-signed actions only: throws through the checked `owed`, and the claim can throw inside
/// `sysio.liq`.
inline uint64_t pull(yield_pool& pool, name liq_account, name custodian, symbol_code sym) {
   const uint64_t owed_now = custodian_owed(liq_account, custodian, sym);
   if (owed_now == 0) return 0;
   const u128 index = live_index(liq_account, sym);
   if (index == pool.pulled_index && owed_now == pool.pulled_owed) return 0;
   record_pull(pool, owed_now, index);
   claim_action(custodian, liq_account, sym).send();
   return owed_now;
}

} // namespace sysio::opp::shadow::custody

# Integrating shadow custody

This guide is for a contract that holds shadow LIQ on behalf of other accounts and wants to pass
the WIRE yield that shadow earns on to them. `sysio.opreg` does this for bonded operator
collateral. The library is header-only:

- `contracts/sysio.opp.common/include/sysio.opp.common/shadow_custody.hpp` holds the functions.
- `shadow_custody_types.hpp` holds the two row types.

## The model

1. `sysio.liq` sees one holder, your contract (the custodian), and pays that row's yield through a
   cumulative per-symbol index.
2. Each of your sub-holder rows embeds a `position`. The position checkpoints that same live index,
   so a sub-holder unit earns exactly what a unit of your liq row earns.
3. Before every change to a sub-holder's balance, the position is settled at the live index. A new
   row starts at the current index and never earns earlier distributions.
4. The WIRE comes in only when you `pull`, which records the amount on a per-symbol `yield_pool`
   and sends the `sysio.liq::claim` that pays it.
5. A sub-holder is credited only out of that pool, so credits never exceed WIRE you actually
   received.

## What to embed, what to store

- **In each sub-holder row:** a `custody::position` next to the balance it tracks, for example
  `balance_entry::shadow_yield` in `sysio.opreg`.
- **In a table of your own:** one `custody::yield_pool` per symbol, under your own key. `sysio.opreg`
  uses `yieldpool`, keyed by token code.
- **In your contract header:** include `shadow_custody_types.hpp` only.
- **In your implementation file:** include `shadow_custody.hpp`.

## Entry points

Use the three composites. Each one reads the live index itself.

| Call | Use it for | Throws? |
|---|---|---|
| `settle_and_adjust(pos, balance, delta, liq, sym)` | every change to a sub-holder's balance; returns the change applied | never |
| `settle_and_take(pos, balance, pool, liq, sym)` | crediting a sub-holder; returns what the pool covers, the rest stays banked | never |
| `pull(pool, liq, self, sym)` | bringing the WIRE in; returns what it recorded and claimed, or 0 | caller-signed actions only |

The primitives are `live_index`, `settle`, the index-taking `settle_and_adjust`, `take`,
`custodian_owed`, `record_pull` and `claim_action`. They stay public for unusual needs. If you use
them directly, you own the sequencing the composites guarantee.

A worked custodian:

```cpp
#include <sysio.opp.common/shadow_custody.hpp>
namespace custody = sysio::opp::shadow::custody;

// Bond `amount` shadow for `holder`: the shadow transfer into this contract is sent in this same action.
void vault::bond(name holder, uint64_t amount) {
   require_auth(holder);
   holders_t rows(get_self());
   auto row = rows.get_or_create(holder);                                   // balance 0, position {}
   custody::settle_and_adjust(row.pos, row.balance, int64_t(amount), LIQ, SYM);
   rows.set(holder, row);
   action({holder, "active"_n}, LIQ, "transfer"_n,                          // the shadow arrives
          std::make_tuple(holder, get_self(), asset(amount, SHADOW), std::string{})).send();
}

// Credit what `holder` has earned into a ledger it pulls from; permissionless is fine.
void vault::claim(name holder) {
   pools_t pools(get_self());
   auto pool = pools.get_or_default(SYM);
   custody::pull(pool, LIQ, get_self(), SYM);                               // at most once per action
   holders_t rows(get_self());
   auto row = rows.get(holder);
   const uint64_t credit = custody::settle_and_take(row.pos, row.balance, pool, LIQ, SYM);
   check(credit > 0, "nothing to claim");
   rows.set(holder, row);
   pools.set(SYM, pool);
   sysio::opp::claimable::credit(claims, payer, holder, {}, credit);       // holder pulls it later
}
```

In `vault::bond`, the position is settled and the balance credited before the shadow arrives. This
is allowed only because the transfer is sent in the same action (obligation 0).

## Obligations

0. **Hold what you attribute.** For each symbol, the sum of sub-holder balances never exceeds your
   own `sysio.liq` holding. A balance may be credited before the shadow arrives only within the
   same action that brings the shadow in. Otherwise positions accrue on shadow your liq row does not
   hold, so your row is owed less than the positions. Because the pool is first-come-first-served,
   the first sub-holders to claim then take yield the others earned.
1. **Change balances only through `settle_and_adjust`.** For a new row, create it at balance 0 and
   then adjust.
2. **Pull only through `pull`, from a caller-signed action, at most once per symbol per action.**
   `pull` records a predicted payout, because the claim it sends executes after your action
   returns. It refuses a second pull at the same index and owed amount itself, since that state
   means the first claim has not executed yet.
3. **Queue nothing ahead of `pull` that touches `sysio.liq`.** This covers a shadow transfer, an
   `addyield`, or a claim of your own, if sent in the same action before `pull`. That inline action
   executes first, the claim then pays a different amount than was recorded, and WIRE is stranded
   in your account.
4. **Credit sub-holders only with what `settle_and_take` returns**, into a ledger they pull from
   (`claimable.hpp`). Never push a transfer from a path that must not throw.
5. **Decide your policy** for yield the pool cannot yet cover, and for yield left unclaimed when a
   row is erased. `sysio.opreg` keeps uncovered yield on the live row, then archives banked debt in
   `yielddebts` before pruning or replacing a terminated operator. The original account can collect
   it indefinitely through `claimyield`, without an operator registration. Archived debt earns no
   further yield and uses the same token-specific backing cap; it never draws on WIRE collateral.

## What never throws

These calls read at most `sysio.liq`'s `yieldidx` row and never `check()`:

- `live_index`, `settle`, `settle_and_adjust`, `settle_and_take`, `take` and `record_pull`.

They are safe on paths that must not abort, such as `sysio.opreg`'s inline paths from
`sysio.epoch::advance`.

`pull`, `custodian_owed` and `claim_action` are for caller-signed actions only. They use the
checked yield formula, and the claim can throw inside `sysio.liq`.

## Authority

`pull` pushes `sysio.liq::claim(self, sym)` under `self@active`. A privileged custodian can do this
as is. Any other custodian needs its own `sysio.code` permission added to its `active` authority.

## Dust and first-come-first-served

`sysio.liq` floors your row's yield at every settle of that row: each claim, and each shadow
transfer in or out. A position floors once over its whole interval. So positions can be owed up to
one atomic WIRE per settle of your row more than you received. That dust stays banked until slack
covers it; slack is yield on shadow you hold for nobody. The pool pays first come, first served, so
a shortfall falls on whoever claims last, not on the position whose rounding caused it.

## Upgrades

Your contract deserializes three `sysio.liq` rows:

- `yieldidx` on every settle;
- `accounts` in `pull`;
- if you resolve symbols the way `sysio.opreg` does, `stat`.

Any change to those layouts means `sysio.liq` and every custodian redeploy together.

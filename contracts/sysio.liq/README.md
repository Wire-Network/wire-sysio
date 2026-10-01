# sysio.liq

The depot's shadow token for syndicated liq. One shadow symbol per outpost liq
token (`LIQETH`, `LIQSOL`, precision 9), minted 1:1 against liq the outpost holds
in its syndicated pool and burned when a holder de-syndicates. Holders earn WIRE
yield through the cumulative index of `sysio.opp.common/shadow_yield.hpp`: every
balance move settles the row first, and `claim` pays what `shadow::owed` says.

The contract is privileged (`sysio.roa::setsyscode`): holder rows bill the `sysio`
RAM pool, and every inline action carries the authority it needs, so no
`sysio.code` grant exists anywhere.

## Flows

**Supply (sysio.synd only).** `sysio.synd` holds every inbound SYNDICATE_LIQ and
is the only account that grows or shrinks the supply: `mint(to, token_code,
amount)` mints into a holder row and `burn(token_code, amount)` burns out of
`sysio.synd`'s own row, settled first. Both refuse an unknown token, a zero
amount, and a mint past the headroom (the asset range net of the supply and the
pending yield, `headroom_of`, which `sysio.synd` reads before it mints). The
supply plus the pending yield is the depot's outstanding shadow of the symbol,
`outstanding_of`, which `headroom_of` is the complement of and which
`sysio.synd` carries on every DESYNDICATE_LIQ as `total_syndicated`. The
parked hold, its sweeps, de-syndication and the pre-launch import all live in
`sysio.synd` and reach this ledger only through `mint`, `burn` and `transfer`.

**Yield (LIQ_YIELD).** `mintyield`, signed by `sysio.synd` when it releases a
report, holds the yield in the symbol's pending balance, outside supply but
reserved against the asset range beside it: every supply-growing path measures its
headroom net of what is pending, a report past that headroom is dropped whole, and
queueing always fits. It never throws; replay is `sysio.synd`'s to refuse. The permissionless `queueyield` mints it
to this contract, announces it to `sysio.swap` with `fundyield` and transfers it,
in one transaction, so it lands in the pool's reservoir and never accrues to this
contract. The swap sells it in clips through the pool and pays the proceeds in
through `addyield`, which advances the index by `quantity / supply`, carries the
remainder and pulls the WIRE by inline transfer.

**The kicker.** On the swap's intake, the one that is yield, `addyield` requests
`kicker_bps` (default 200) of the intake from T5 through
`sysio.system::fundclaim(sysio.liq, amount)`, then folds what actually landed into
the same index with `addkicker`, measured against the balance the pull left. A
donation from any other account distributes only itself: the treasury never tops
up what is not yield, or a near-sole holder could donate, claim it back with the
kicker on top, and repeat. A short T5 reduces the kicker only. `setkicker` (auth
`sysio`, the account council proposals execute as) changes the next intake.

**De-syndication reconciliation.** `sysio.synd::desyndicate` burns through `burn`;
the burn is final. An outpost refusal is reconciled from its log by governance
through `recredit`.

**Launch ingestion (epoch-0 bootstrap window, privileged caller).** `regliqpool`
mints the LCO liq to `sysio`, deposits it with the T5 dex earmark WIRE into
`sysio.swap`, creates the pair (`sysio` fee authority, the shadow as yield leg)
and sets the tick parameters. The pre-launch positions are replayed by
`sysio.synd::importsynd`.

## Tables

| Table | Scope / key | Row |
|---|---|---|
| `stat` | symbol code | `supply`, `chain_code`, `token_code`, `pair_symbol` (empty until `regliqpool`); index `bytoken` |
| `accounts` | holder / symbol code | `balance`, `index_checkpoint` (uint128), `owed_wire` |
| `yieldidx` | symbol code | `index` (uint128), `pot`, `carry` |
| `liqpending` | symbol code | `quantity` minted by LIQ_YIELD and not yet queued; counts against the asset range beside supply |
| `liqconfig` | singleton | `kicker_bps` |

## Actions

| Action | Auth | Purpose |
|---|---|---|
| `create(sym, chain_code, token_code)` | self | Register a shadow for an active `TOKEN_KIND_LIQ` token bound to an active outpost |
| `setkicker(bps)` | `sysio` | Kicker for the intakes from now on |
| `recredit(holder, quantity)` | self | Mint back after an outpost refused a de-syndication |
| `mint(to, token_code, amount)` | `sysio.synd` | Mint shadow into a holder row |
| `burn(token_code, amount)` | `sysio.synd` | Burn shadow out of `sysio.synd`'s own row |
| `mintyield(chain_code, token_code, amount)` | `sysio.synd` | LIQ_YIELD into the pending balance |
| `queueyield(sym)` | none | Pending yield into the swap's reservoir |
| `transfer`, `open`, `close`, `claim` | holder | `sysio.token`'s shape; `close` refuses while yield is owed |
| `addyield(from, quantity, target)` | `from` | Distribute WIRE to `target`'s holders |
| `addkicker(sym, base_balance, requested)` | self | Inline from `addyield` |
| `regliqpool(...)` | privileged caller, epoch 0 | Seed a shadow's yield pool |

## Deployment

In order: `sysio.roa::setsyscode` for `sysio.liq`; the chain and its liq token
registered and active in `sysio.chains` / `sysio.tokens`; `create` per shadow
symbol; `setkicker` if the default is not wanted; `regliqpool` per pool; then
`sysio.synd`'s `importsynd` batches and `importdone`. `sysio.swap` must be configured
(`setconfig`) before `regliqpool`, and the batch-operator crank pushes
`queueyield` per symbol and `sysio.swap::tickyield` per pair.

The Solana relay must carry the `DESYNDICATE_LIQ` effect shape before
`sysio.synd` is deployed: a delivered `DesyndicateLIQ` without its accounts aborts
the outpost's handler and wedges the epoch.

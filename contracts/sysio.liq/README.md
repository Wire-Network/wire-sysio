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

**Syndication (SYNDICATE_LIQ, inbound).** `sysio.msgch` resolves the user's pubkey
through `sysio.authex` and calls `mintsynd` (linked) or `park` (not linked). A
parked row accrues like any holder; `linkswept` (inline from `createlink`) or the
permissionless `sweep` delivers it, accrued WIRE included, to the account the
pubkey later links. The inbound actions never throw: a replayed `sequence`, an
unknown token, a token of another chain or an out-of-range amount is dropped with
a diagnostic.

**Yield (LIQ_YIELD, inbound).** `mintyield` holds the reported yield in the
symbol's pending balance, outside supply. The permissionless `queueyield` mints it
to this contract, announces it to `sysio.swap` with `fundyield` and transfers it,
in one transaction, so it lands in the pool's reservoir and never accrues to this
contract. The swap sells it in clips through the pool and pays the proceeds in
through `addyield`, which advances the index by `quantity / supply`, carries the
remainder and pulls the WIRE by inline transfer.

**The kicker.** On every intake `addyield` requests `kicker_bps` (default 200) of
the intake from T5 through `sysio.system::fundclaim(sysio.liq, amount)`, then
folds what actually landed into the same index with `addkicker`, measured against
the balance the pull left. A short T5 reduces the kicker only. `setkicker`
(auth `sysio`, the account council proposals execute as) changes the next intake.

**De-syndication (DESYNDICATE_LIQ, outbound).** `desyndicate` requires the holder
to be AuthX-linked for the token's chain, settles and burns the shadow, and queues
`DesyndicateLIQ{chain_code, user = linked pubkey, amount, request_id}` through
`sysio.msgch::queueout`. Request ids start at 1. The burn is final: an outpost
refusal is reconciled from its log by governance through `recredit`.

**Launch ingestion (epoch-0 bootstrap window, privileged caller).** `regliqpool`
mints the LCO liq to `sysio`, deposits it with the T5 dex earmark WIRE into
`sysio.swap`, creates the pair (`sysio` fee authority, the shadow as yield leg)
and sets the tick parameters. `importsynd` replays pre-launch positions in
batches (the LCO yield already folded into each amount); `importdone` closes the
import.

## Tables

| Table | Scope / key | Row |
|---|---|---|
| `stat` | symbol code | `supply`, `chain_code`, `token_code`, `pair_symbol` (empty until `regliqpool`); index `bytoken` |
| `accounts` | holder / symbol code | `balance`, `index_checkpoint` (uint128), `owed_wire` |
| `yieldidx` | symbol code | `index` (uint128), `pot`, `carry` |
| `parked` | symbol code, chain kind, pubkey | `chain_kind`, `pubkey`, `holding` (an account row) |
| `liqpending` | symbol code | `quantity` minted by LIQ_YIELD and not yet queued |
| `liqcursors` | chain code | `last_sequence`, `last_epoch` |
| `liqconfig` | singleton | `kicker_bps`, `import_complete` |
| `liqcounters` | singleton | `next_request_id` |

## Actions

| Action | Auth | Purpose |
|---|---|---|
| `create(sym, chain_code, token_code)` | self | Register a shadow for an active `TOKEN_KIND_LIQ` token bound to an active outpost |
| `setkicker(bps)` | `sysio` | Kicker for the intakes from now on |
| `recredit(holder, quantity)` | self | Mint back after an outpost refused a de-syndication |
| `mintsynd(chain_code, sequence, account, token_code, amount)` | `sysio.msgch` | SYNDICATE_LIQ, linked user |
| `park(chain_code, sequence, chain_kind, pubkey, token_code, amount)` | `sysio.msgch` | SYNDICATE_LIQ, unlinked user |
| `mintyield(chain_code, sequence, epoch, token_code, amount)` | `sysio.msgch` | LIQ_YIELD into the pending balance |
| `queueyield(sym)` | none | Pending yield into the swap's reservoir |
| `sweep(account, chain_kind)` | none | Deliver parked rows for an existing link |
| `transfer`, `open`, `close`, `claim` | holder | `sysio.token`'s shape; `close` refuses while yield is owed |
| `addyield(from, quantity, target)` | `from` | Distribute WIRE to `target`'s holders |
| `addkicker(sym, base_balance, requested)` | self | Inline from `addyield` |
| `linkswept(account, chain_kind, pubkey)` | `sysio.authex` | Deliver parked rows on link |
| `desyndicate(holder, quantity)` | holder | Burn and queue DESYNDICATE_LIQ |
| `regliqpool(...)`, `importsynd(...)`, `importdone()` | privileged caller, epoch 0 | Launch ingestion |

## Deployment

In order: `sysio.roa::setsyscode` for `sysio.liq`; the chain and its liq token
registered and active in `sysio.chains` / `sysio.tokens`; `create` per shadow
symbol; `setkicker` if the default is not wanted; `regliqpool` per pool;
`importsynd` batches; `importdone`. `sysio.swap` must be configured
(`setconfig`) before `regliqpool`, and the batch-operator crank pushes
`queueyield` per symbol and `sysio.swap::tickyield` per pair.

The Solana relay must carry the `DESYNDICATE_LIQ` effect shape before this
contract is deployed: a delivered `DesyndicateLIQ` without its accounts aborts
the outpost's handler and wedges the epoch.

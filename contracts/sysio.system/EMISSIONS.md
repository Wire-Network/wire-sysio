# sysio.system emissions

WIRE emission and reward distribution. The T5 treasury emits WIRE on a decaying
per-epoch curve; the per-epoch amount is split by basis points into categories
that are either pushed automatically each pay-epoch or pulled by the recipient
through a claim action. All amounts are `WIRE` (9-decimal subunits).

Every "epoch" on this page is an **economic epoch** of `sysio.epoch`'s economic
clock (`economycfg`): a fixed wall-clock grid that never waits on outpost
consensus. Each outpost chain's OPP consensus epoch only feeds batch-operator duty.

## Per-epoch flow

`sysio.epoch::tickeconomy` opens each economic epoch and drives emissions inline:

- Every economic epoch whose readiness gate allows it calls `accrueepoch`, which
  adds the curve share of the epoch the tick closes (`compute_epoch_emission`) to
  `t5state.pending_emission_amount`.
- On a pay-epoch boundary (`emitcfg.pay_cadence_epochs`) it then calls
  `payepoch`, which distributes the accumulated `period_emission` and opens the
  period's batch-duty settlement.
- Every economic epoch then calls `settleduty`, one bounded step of any open
  settlement.

Separately, every consensus epoch advance of an outpost chain (`sysio.epoch::advance`)
calls `rcrdduty` for the epoch before the closing one, crediting each member of the
group that served it with the chain minimum (`epochcfg.epoch_duration_sec`)
configured when its books close, in seat-seconds, however long the epoch ran: an
epoch's duty is credited when the following epoch closes. Both clocks skip the
duty actions until `initt5` has run: nothing can be paid before then, and
consensus must not depend on `sysio.system`.

A start time in the past makes the clock catch up, one epoch per tick, through
the elapsed grid, and so does a cluster whose first tick runs more than one
economic epoch late. A payout spends only if the clock had already caught up
before its tick: `tickeconomy` reads `economystate.caught_up` before it updates it
and passes it to `payepoch` as `spend`. Every pay epoch's payout up to and
including the tick that first finds no later boundary passed therefore spends
nothing: the period's whole emission stays in the treasury, and the gate never
holds such a payout for balance. It still drains the period's accrual and slots,
starts the next period, writes the `epochlog` row (its total, with every category
zero) and resets the snapshot-attestation counters. It credits and pushes nothing,
clears no block count and leaves the duty period open: the blocks and duty counted
before and during the catch-up carry to the first payout that spends, which pays
them from its own period's pools. Payouts therefore start at the first pay epoch
after the tick that reaches the grid, which for a launch whose first tick runs on
time is that first tick.

### Work counts toward the open period

Blocks (`onblock`) and batch duty (`rcrdduty`) count toward the pay period open
when they are recorded, and that period's payout pays them. A tick that runs late
counts the work recorded between its boundary and the tick toward the period it
pays, at that period's rate, and the next period's work starts that much later.

Once the clock has reached the grid, a pay epoch ticked while a later boundary has
already passed defers its payout: the period stays open, and the tick that catches
up to the grid pays every overdue period in one payout. That payout pairs all their
pools, and all their block slots in the divisor, with every block and seat-second
of duty counted since the previous payout, at one rate. Producers are paid per block, so a
slot nobody filled in any of those periods leaves its share in the treasury. The
batch pools of all of them are split by the duty counted across them, so the pool of
a period nobody served in goes to the operators who served in the others. A
deferral is not a block and leaves the `blocklog` as it is. The batch operators keep
the ticks current, and the economic clock is monitored.

The curve decays from `annual_initial_emission` toward `annual_min_emission`
(clamped by `annual_max_emission`), stops at `t5_floor`, and auto-throttles as
`total_distributed` rises.

### Emission follows elapsed time

Each tick accrues the share of the economic epoch it closes, from the previous
boundary to its own, sized and decayed over that epoch's length; the first tick
closes one epoch of the configured duration. The decay base is the last positive
share, rescaled from the length it accrued over (`last_epoch_duration_sec`) to the
length of the epoch being accrued. A duration change spaces only the boundaries
after the next one, so it moves neither the emission per second nor the producer
pay per block slot: the epoch it finds open is accrued at its own length, and
later epochs at the new one.

## Budget headroom

The T5 budget is `t5_distributable - t5_floor`. Accruals are sized against the
part not yet paid out or accrued (`net_headroom` in `emissions.hpp`), so
`total_distributed + pending_emission_amount` never exceeds the budget. `setemitcfg` refuses a `t5_distributable` below
`t5_floor + total_distributed + pending_emission_amount`.

Once the open pay period has accrued the rest of the budget, its remaining
epochs accrue zero, and its `payepoch` settles what was accrued. Emissions are
held with `TREASURY_EXHAUSTED` once the budget is spent, or when the curve
yields zero with nothing pending; a pay epoch whose period total exceeds the
spendable balance is held with `BALANCE_INSUFFICIENT` and pays at the first
economic epoch that can cover it; a payout that spends nothing is never held. A
held gate holds emissions only: the economic clock keeps ticking (withdrawals
mature) and consensus never waits on it.

A paid period's batch pool that has not yet been credited to its operators
(`t5state.unsettled_batch_emission`) is committed too: `net_headroom` and the
gate's spendable balance both exclude it until its settlement completes.

## How each bucket reaches its recipient

`payepoch` runs inline from `sysio.epoch::tickeconomy`, which must never abort. A
`sysio.token::transfer` notifies its recipient, and the chain executes notified
receivers with no exception isolation, so a pushed payout would let any operator
halt the economic clock chain-wide. Every payout to an account the protocol does
not control is therefore **credited** to `payclaims` and pulled later with
`claimpay`; only the protocol's own holding accounts are still pushed.

| Bucket | Share | Destination | How | Memo |
|--------|-------|-------------|-----|------|
| Producer reward | `compute_bps` x `producer_bps` | each active producer / standby (rank <= `standby_end_rank`, opreg-ACTIVE) | **credited — `claimpay`** | none at credit; `T5 epoch pay claim` at the claim |
| Batch-op reward | `compute_bps` x `batch_op_bps` | each opreg-ACTIVE batch operator, pro rata to its seat-seconds | **credited — `claimpay`** | none at credit; `T5 epoch pay claim` at the claim |
| Capex | `capex_bps` | `sysio.ops` | pushed | `T5 capex` |
| Governance | `governance_bps` | `sysio.gov` | pushed | `T5 governance` |

**Producers, standbys and batch operators are not paid by transfer.** Their WIRE
sits in `payclaims` until they call `sysio.system::claimpay`; a balance watcher
waiting for it to arrive will wait forever. Monitoring reads `payclaims`.

**Do not key monitoring on a per-bucket memo for the credited rows.** What is lost is
the CATEGORY, not the trace:

- **At credit time** there is no transfer and no memo at all — the credit is a kv
  write, so `payclaims` deltas are the only signal.
- **At claim time** `claimpay` does emit a normal `sysio.token::transfer` to the
  claimant, so a per-recipient transfer trace exists and is worth watching. But it
  drains the whole row in ONE transfer under the single memo `T5 epoch pay claim`, so
  a claimant who earned both a producer and a batch-op share receives one transfer
  carrying neither category.

And `epochlog` does not recover the split either. It records top-level PERIOD aggregates
— `total_emission`, `compute_amount`, `capex_amount`, `governance_amount` — where `compute_amount` is the COMBINED producer + batch-operator
pool. Neither operator category's actually-credited total is stored, so once both kinds
of credit accumulate in one `payclaims` row, **the producer-versus-batch-operator split
is not available anywhere on chain**. What you can attribute: period totals per top-level
category from `epochlog`, and per-recipient amounts from `payclaims` deltas plus the
claim transfer.

**`sysio.ops` and `sysio.gov` stay pushed** because no claim path can reach them:
a claim needs `require_auth(account_name)`, `sysio.roa` forces
`net_weight`/`cpu_weight` to zero for every `sysio`-prefixed account, and neither
carries a contract that could emit the claim inline. They are protocol-owned
holding accounts with no code, so the notify-handler risk does not apply.

Producer pay is per counted block. `onblock` counts the blocks a producer makes,
at most one round per rotation of the schedule (a switch that hands a producer a
second round in the same rotation does not count it). The active slice of the
producer pool is divided by the period's nominal slot count (each tick adds the
block slots of the economic epoch it closes, from the previous boundary to its
own, whatever duration is set when the tick runs), raised to the blocks counted
when carried blocks, or blocks a late tick counts, push the count past it, and
each producer is paid for its counted blocks. Standbys
draw from a separate slice, at a rank-decreasing weight, and are not paid per
block. Batch operators are paid from emission only, so `producer_bps` /
`batch_op_bps` split the whole compute share.

### Batch-operator duty

Batch-op pay follows **seat-seconds**. Each consensus epoch of an outpost chain
credits every member of the group that served it with that chain's minimum
duration (`epochcfg.epoch_duration_sec`) as configured when the epoch's books
close, in seconds, however long the epoch ran (`rcrdduty`, a `batchduty` row per
operator and chain): pay follows epochs served, so a seat on a chain with a 120 s
minimum earns twice a seat on a 60 s one, and an idle or stretched epoch earns no
more. The credit is made when the epoch's books close, once its late deliveries
can no longer come: when the following epoch's late-delivery window has passed
(`sysio.msgch::chkcons`), or at the advance that closes the following epoch, if
that runs first. Every chain's duty accrues to the one
open pay period. Each `payepoch` that spends closes that period and splits its
batch pool pro rata to the period's seat-seconds: an operator's share is
`batch_pool * seat_seconds / period_seat_seconds`. Only opreg-ACTIVE batch
operators are credited, at settlement time; the share of an
operator slashed or terminated since stays in the treasury, as do
integer-division remainders and the whole pool of a period with no duty.

Settlement is bounded: one step examines at most `MAX_DUTY_SETTLEMENT_ROWS` (50)
`batchduty` rows. `payepoch` takes the first step, and every economic epoch's
`settleduty` takes another until the walk reaches the end of the table. An
operator touched by `rcrdduty` while still holding seat-seconds of the settling
period on that chain is settled first, at that period's rate. Two periods never
settle at once: a `payepoch` that finds the previous settlement still open steps
it and, if it is still not done, leaves its own period pending for a later
economic epoch.

An eviction from a chain forfeits the operator's unsettled duty there.
`sysio.opreg::termcheck` sends `forfeitduty(account, chain)` when it evicts,
which erases the operator's row for that chain, whether it belongs to the open
or the settling period, and leaves the period's seat-seconds as they were: no
operator is credited the share, so it stays in the treasury when the settlement
completes. `rcrdduty` credits an `EVICTED` member nothing on the chain, including
the seat whose books' close evicted it (`termcheck` runs before `rcrdduty` in the
same advance), until it rejoins. Its other chains keep paying.
Slashing and governance `terminate` still forfeit every chain's unsettled duty,
since settlement pays only ACTIVE operators. A terminated account that registers
again could be ACTIVE before that duty settles, so `sysio.opreg::regoperator`
sends `forfeitduty` for every chain the account still holds duty on: the new
registration is paid only for the seats it serves.

`setemitcfg` accepts a payment cadence only in `[1, 10]` economic epochs.

`epoch_log_retention_count` counts payment rows, not elapsed epoch indexes, so
cadence values greater than one retain the configured number of audit records.
Each `epochlog` row records the period's `batch_emission`; when its settlement
completes it records the `batch_emission_retained` in the treasury and sets
`batch_settled`.

## Retrieved via a claim action (pulled by recipient)

### Epoch pay -- `sysio.system::claimpay(account_name)`

Producer, standby and batch-operator shares credited by `payepoch`. The caller
(auth = their own account) drains their whole `payclaims` row in one transfer —
there is no partial claim — and the action throws `no epoch pay to claim` when
nothing is owed. `payclaims` is readable to show a "you have X to claim" balance
first.

The credited WIRE never leaves the treasury's token balance until it is claimed,
so every gate that spends against that balance reserves it via the
`payclaimtot.outstanding` counter: `sysio.epoch`'s
emissions readiness gate (which also reserves `t5state.unsettled_batch_emission`), and `claimnodedis`, which refuses a node-owner
withdrawal that would eat into pay already owed. Without that reserve the
treasury double-commits and a later claim fails on overdraw.

### Node-owner vesting -- `sysio.system::claimnodedis(account_name)`

Node owners are registered by `sysio.roa` through `addnodeowner` (tiers 1-3,
each with a fixed `tN_allocation` and `tN_duration`). The allocation vests
linearly over the tier duration from `emission_state.node_rewards_start`. The
owner calls `claimnodedis` (auth = their own account) to transfer the
newly-vested amount, gated by `min_claimable` (memo `Node Owner distribution`).
`viewnodedist` is the read-only preview.

### LIQ yield and the independent kicker earmark

LIQ holders receive supplied WIRE proceeds and donations through `sysio.liq::addyield`.
The retired per-intake bonus and `fundclaim` action remain removed. `sysio.kicker` is a
separate privileged contract: a permissionless `kick(sym)` draws from `sysio` within a
governance earmark and distributes the gift to all holders through `addyield`.

The gift is simple accrual on the **smaller** of the LIQ supply at the pool's last
checkpoint (`supply_snapshot`) and the current `stat.supply`, at a **TWAP** of the
registered LIQ/WIRE swap pool, for at most the last 72 hours since payment.
Each token has its own clock and rate (default 200 bps); a year is 31,557,600 seconds.
The two arithmetic floors favour the treasury. Kicker snapshots swap's `priceaccum.price1`
on `addpool` and after each payment, full or partial. It extends that accumulator to the action time
using the unchanged-reserve tail, then divides the cumulative difference by elapsed time.
Governance sets `kickcfg.twap_window_sec` through
`setconfig({budget_remaining, min_interval_sec, twap_window_sec, max_spot_deviation_bps})`. The window defaults
to **86,400 seconds (one day)** and must be at least **60 seconds**; production keeps
one day. Test clusters can configure a shorter window to observe payments, with
`min_interval_sec` and `min_gift` also low enough to permit them.
The observation must span at least the configured window; there is no spot fallback during warm-up.
The next `kick` applies a changed window to the in-flight observation without resetting
`price_time`, `price_snapshot` or the accrual clock: lowering it can permit payment
on that attempt; raising it delays eligibility until that same observation is long enough.
Nonpaying attempts retain the observation, so its TWAP can span more than the configured
window. Every payment, full or partial, restarts it: history that priced one payment never
prices another. A same-block trade has zero weight until time passes at the new price.

Sizing on the smaller supply bounds every payment: **a gift never exceeds the honest gift of
the supply present at the last checkpoint; LIQ minted after a checkpoint is sized from the
next full payment or checkpointing reset.** Without it a holder could mint LIQ just before a
mature kick and be paid up to 72 hours of accrual on supply that existed for one block.
`addpool` checkpoints the current supply, and so does a full payment. A partial payment keeps the supply it priced, so its
remainder cannot be re-sized on LIQ minted in between. `addyield` still divides each
correctly sized gift by the current supply, so LIQ minted after the checkpoint takes its
pro-rata share of that gift and the earlier holders' share shrinks by the same amount; that
dilution is the accepted residual, and it is never more than the gift itself. A burn below the
snapshot lowers the gift at once. A pool whose payments are all partial (the sized gift exceeds
the daily cap, or budget top-ups stay below it) never re-checkpoints, so LIQ minted meanwhile
stays unsized until a full payment or a reset lands.

A second residual sits beside the pro-rata one: sizing sees the supply at the two ends of the
interval, not inside it. LIQ present at both checkpoints is sized for the whole interval even if
it was burned in between (for example desyndicated after a full-payment checkpoint and
re-syndicated before the next payment or checkpointing reset; a kick that neither pays nor
checkpoints leaves the gap open), so "not paid for the interval it did not hold" does not hold; the treasury bound
(never above the honest gift of the supply present at the last checkpoint) and the daily cap
still do.

A one-block price spike in a shallow pool contributes its duration weight to the
arithmetic TWAP. A large trade held across one block can push one kick's gift to the daily
cap: the attacker then partly unwinds so spot sits near the inflated TWAP, kicks, and
finishes the unwind in the same block, and same-block trades carry no TWAP weight, so no
spot threshold stops it. A holder of most of the LIQ supply, who receives most of that
gift, can then profit from the round trip. This residual risk is accepted.

The spot guard bounds the straddle. A pump raised just before one kick and unwound just
after it would otherwise put its area into two adjacent observations and fund two capped
payouts. A kick pays only while the pool's spot is within `kickcfg.max_spot_deviation_bps`
of the window TWAP (`|spot - TWAP| / TWAP`, default **5,000 bps**, set from 1 to 10,000).
Past it the kick pays nothing, records `spot_deviation`, and holds both the clock and the
observation. The straddle's first kick sees spot far above the TWAP, and its second, after
a full unwind, prices at no more than `spot / (1 - threshold)` (twice spot at the default)
instead of the pumped TWAP. **One price excursion therefore funds one capped payout plus at
most `d * cap` of excess on the next payout** (500 WIRE at the default 5,000 bps), where `d`
is the threshold as a fraction: that payout's excess over its honest value is
`min(cap, honest / (1 - d)) - honest`, largest when its honest gift is `cap * (1 - d)`. A
straddle whose first kick follows a partial unwind reaches it; the regression
`partial_unwind_straddle_overpays_at_most_deviation_times_cap` measures 50.02 WIRE of excess
in a construction near the cap and 499.98 WIRE at the worst point. Minting LIQ between the two
kicks does not widen it: the second payout is sized on the supply checkpointed by the first, and
`partial_unwind_straddle_with_mint_stays_within_bound` checks a tenfold mint stays within it. The bound holds for every
threshold from 1 to 10,000 bps: at the 10,000 bps maximum (`d = 1`) the second kick's price
is no longer bounded by spot, so its excess can reach the cap itself, two capped payouts, and
the sizing rule below becomes `2 * max_gift_per_day`. A trip holds rather than restarts the
observation: a restart would let anyone discard a pool's accrued observation with one atomic
trade and delay its payment by a full window at almost no cost. Governance registers only pools with deep liquidity and sizes
`max_gift_per_day` so that `(1 + d) * max_gift_per_day` stays well under the cheaper of two
costs: holding that pool's price for a full window, or a one-block spike that buys a window's
TWAP area.

`max_spot_deviation_bps` is a liveness and griefing knob, not a manipulation-cost one: the
one-block path above passes any threshold. A trip holds payment until the observation's
TWAP is back within the threshold of spot. For an honest move by a factor `r` when the
observation is `D` old, payment resumes after `D * ((r - 1) / d - 1) / r` for a rise and
`D * ((1 - r) / d - 1) / r` for a fall, where `d` is the threshold as a fraction (zero or
less means at once). At the 5,000 bps default a fall of up to 50% or a rise of up to 50%
clears at once, a doubling clears after half of `D`, and a tripling after `D`.

Griefing cuts the other way, by two routes. The one-block route holds payment past
observation length `L` with a single 0.5 s spike of more than `1 + 2L * d / (1 - d)` times
spot: more than 172,801x per day of delay at the default, about 31 million WIRE of capital
on a 75,000/75,000 WIRE-LIQ pool. A continuous hold needs far less capital: keeping spot
just above `(1 + d) * P * (t / D) ^ d`, where `P` is the TWAP of an observation `D` old and
`t` is its age, trips the guard throughout. At the default, a one-day delay on a one-day-old
observation ramps spot from 1.5x to about 2.12x `P`, about 34 thousand WIRE of capital on
that pool, at the cost of holding the pool off-price for the day against arbitrage. A
tighter threshold holds payment through more honest volatility and makes both routes
cheaper; a wider one does the reverse and loosens the straddle bound.

Governance sets an absolute `budget_remaining` outside `t5_distributable`. Kicker draws
never modify `t5state.total_distributed`. Every draw reserves `pending_emission_amount`,
`payclaimtot.outstanding`, and the nonnegative remaining emission ceiling
(`t5_distributable - t5_floor - total_distributed`) from the live WIRE balance.
Absent emissions configuration or T5 state permits no draw.

Funding or a daily allowance below the accrued gift pays the available amount if it
meets `min_gift`, then sets `last_kick` to `now - elapsed + floor(elapsed * paid / gift)`,
where `elapsed = min(now - last_kick, 72 hours)`. A successful payment forfeits older
accrual. Within the bounded 72 hours, the unpaid remainder keeps accruing; the next
payment prices it, with any new accrual, at the TWAP of a fresh observation that
starts at the partial payment.
A below-minimum allowance or Andon hold leaves the clock open. An empty supply, or a
zero `supply_snapshot` under a nonzero supply, checkpoints instead of pricing: the clock
restarts at the current supply and all shortfall diagnostics clear, except during an Andon
hold. The price observation is kept. A zero snapshot means none of today's supply was present
at the last checkpoint, so its accrual starts at this kick. A gift below `min_gift` writes
nothing, with one exception: while the supply has grown past the snapshot, the full 72
hours are unpaid, and the same interval sized on the current supply at the same TWAP would reach
`min_gift` (or overflow), the small snapshot is the binding constraint and nothing more can accrue on it,
so the kick forfeits the interval and checkpoints the current supply instead of holding the grown supply
unpaid forever. When the TWAP is the cause (a price dip sinks the current-supply gift below `min_gift`
too), the kick holds and the refilled pool still pays the interval. A dip that leaves only the
snapshot's gift below `min_gift` (the current-supply gift still reaches it) still forfeits: the
criterion is the snapshot being binding at that TWAP, not the TWAP being honest.
Governance must set a positive UTC-day ceiling at least `min_gift` and no larger
than **1,000 WIRE per pool per day**, which is also the C++ default. Zero is invalid.
Retries and configuration changes retain the current day's spending. Set `min_gift`
no higher than a day's expected accrual if the pool is intended to pay.

TWAP, bounded accrual and the mandatory cap apply independently of keeper cadence.
`min_interval_sec` is a floor, not a schedule:
every ACTIVE batch operator pushes `kick(sym)` once a pool could pay — its interval has
elapsed, its LIQ supply is not zero, its observation spans the window, and its budget and
UTC-day allowance cover `min_gift` (`batch_operator_plugin`, `--batch-kick-crank`) — and governance still monitors
`last_kick` for a pool that stops advancing. An attempt that still does not pay is retried
with a doubling backoff of up to an hour. Each poll reads and attempts at most ten pools,
resuming the next page on the next poll; a complete sweep prunes removed symbols. For
the supplies it looks up only those pools' `sysio.liq::stat` rows, one exact read each.

`shortfall_amount` records `gift - paid` for the latest shortfall, not cumulative
debt. `shortfall_time` records that observation. `gift_overflow` records a gift
outside the asset range: this implementation pays nothing and retains the clock,
because it does not compute an exact partial-payment denominator for overflow.
`spot_deviation` records whether the latest attempt that reached the guard found spot past
it; that attempt's gift is the shortfall. An attempt that stops earlier (below `min_gift`,
during warm-up, under an Andon hold) writes nothing and leaves the flag as it was, except the saturated
below-minimum checkpoint above, which clears it. A later full payment clears all four diagnostics.
An unallocated emissions remainder stays in the treasury; it does not authorize a kicker budget by itself.

The external staking-reward message and `sysio.dclaim::onreward` are removed. Pre-launch imports
and already funded DClaim balances still link through AuthX and pay through
`claim(wire_account)`. These balances never expire, including credits awaiting linking;
they remain owed until claimed, and their rows retain RAM indefinitely.

## Category split (basis points)

`compute_bps + capex_bps + governance_bps <= 10000`; the remainder is the
unallocated treasury remainder. `producer_bps +
batch_op_bps == 10000` (sub-split of compute). All set via `setemitcfg`.

```
period_emission
  |
  |-- compute_bps ----> compute_amount
  |                       |-- producer_bps --> producers/standbys   (claimpay)
  |                       '-- batch_op_bps --> batch operators       (claimpay)
  |-- capex_bps ------> sysio.ops                                    (pushed)
  |-- governance_bps -> sysio.gov                                    (pushed)
  '-- remainder ------> retained in sysio treasury
```

## Emission actions

| Action | Auth | Description |
|--------|------|-------------|
| `setemitcfg` | `sysio.system` | Set / update emission config (validated against live state) |
| `setinittime` | `sysio.system` | Set node-owner vesting start time (once) |
| `initt5` | `sysio.system` | Initialize T5 treasury state (once), starting at the economic clock's start time |
| `addnodeowner` | `sysio.roa` | Register a node owner in a tier |
| `claimnodedis` | node owner | Claim vested node-owner allocation (refused when it would spend pay reserved in `payclaimtot`) |
| `claimpay` | the claiming account | Claim epoch pay credited by `payepoch` (producer / standby / batch-operator share) |
| `accrueepoch` | `sysio.epoch` | Accrue the curve share and the block slots of the economic epoch its tick closes (`closed_epoch_duration_sec`) |
| `payepoch` | `sysio.epoch` | Distribute the period's compute / capex / governance (credits `payclaims`; pushes only the category buckets) and open its batch-duty settlement; waits while an earlier settlement is open; spends nothing unless `spend` (the economic clock had caught up before its tick) |
| `rcrdduty` | `sysio.epoch` | Credit the chain minimum duration configured at the books' close, in seat-seconds, to each member of the group that served a consensus epoch of one chain, except a member evicted from it, when the following epoch closes, in the pay period open when it is recorded |
| `forfeitduty` | `sysio.opreg` | Erase an operator's unsettled seat-seconds on one chain, leaving the period totals as they were: on its eviction from the chain, and on each chain when the account registers again |
| `settleduty` | `sysio.epoch` | One bounded step of the open batch-duty settlement |
| `viewnodedist` | read-only | Preview a node owner's claimable amount |
| `viewepoch` | read-only | Remaining budget / next-emission estimate |
| `viewemitcfg` | read-only | Current emission config |

## Tables

| Table | Type | Description |
|-------|------|-------------|
| `emitcfg` | Singleton | Emission configuration (allocations, curve, BPS splits, cadence) |
| `emissionmngr` | Singleton | Node-owner vesting start time |
| `nodecount` | Singleton | Per-tier node-owner registration counts |
| `nodedist` | Table | Per-account node-owner vesting rows |
| `payclaims` | Table | Per-account epoch pay credited by `payepoch` and not yet claimed |
| `payclaimtot` | Singleton | Running total of outstanding `payclaims`, reserved by every gate that spends the treasury balance |
| `t5state` | Singleton | Treasury state: pending emission, unsettled batch emission, total distributed, decay continuity (the last positive share and the epoch length it accrued over) |
| `batchduty` | Table | Per-(operator, chain) unpaid seat-seconds and the pay period they belong to; erased as they settle, when the operator is evicted from the chain, or when the account registers again |
| `dutystate` | Singleton | The open pay period's seat-second total and the settling period's pool, seat-seconds and walk cursor |
| `epochlog` | Table | Per-pay-epoch audit log (head-pruned to `epoch_log_retention_count`) |

## Dependencies

- Driven inline by `sysio.epoch::tickeconomy` (`accrueepoch` + `payepoch` + `settleduty`),
  `sysio.epoch::advance` (`rcrdduty`) and `sysio.opreg::termcheck` / `regoperator` (`forfeitduty`).
- Reads producer eligibility, operator status and batch operators' chain memberships from `sysio.opreg`.
- Reads the canonical economic epoch duration and start time from `sysio.epoch::economycfg`.

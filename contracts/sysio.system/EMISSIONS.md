# sysio.system emissions

WIRE emission and reward distribution. The T5 treasury emits WIRE on a decaying
per-epoch curve; the per-epoch amount is split by basis points into categories
that are either pushed automatically each pay-epoch or pulled by the recipient
through a claim action. All amounts are `WIRE` (9-decimal subunits).

## Per-epoch flow

`sysio.epoch::advance` drives emissions inline:

- Every epoch calls `accrueepoch`, which adds that epoch's curve share
  (`compute_epoch_emission`) to `t5state.pending_emission_amount`.
- It then calls `rcrdbatch`, which records the immutable batch-operator
  roster that accrued that epoch.
- On a pay-epoch boundary (`emitcfg.pay_cadence_epochs`) it then calls
  `payepoch`, which distributes the accumulated `period_emission`, consumes
  the recorded rosters, and clears that period's history.

The curve decays from `annual_initial_emission` toward `annual_min_emission`
(clamped by `annual_max_emission`), stops at `t5_floor`, and auto-throttles as
`total_distributed` rises.

## Budget headroom

The T5 budget is `t5_distributable - t5_floor`. Accruals are sized against the
part not yet paid out or accrued (`net_headroom` in `emissions.hpp`), so
`total_distributed + pending_emission_amount` never exceeds the budget. `setemitcfg` refuses a budget below
`t5_floor + total_distributed + pending_emission_amount`.

Once the open pay period has accrued the rest of the budget, its remaining
epochs accrue zero and still advance, and its `payepoch` settles what was
accrued. Epochs block with `TREASURY_EXHAUSTED` once the budget is spent, or
when the curve yields zero with nothing pending.

## How each bucket reaches its recipient

`payepoch` runs inline from `sysio.epoch::advance`, which must never abort. A
`sysio.token::transfer` notifies its recipient, and the chain executes notified
receivers with no exception isolation, so a pushed payout would let any operator
abort epoch advancement chain-wide. Every payout to an account the protocol does
not control is therefore **credited** to `payclaims` and pulled later with
`claimpay`; only the protocol's own holding accounts are still pushed.

| Bucket | Share | Destination | How | Memo |
|--------|-------|-------------|-----|------|
| Producer reward | `compute_bps` x `producer_bps` | each active producer / standby (rank <= `standby_end_rank`, opreg-ACTIVE) | **credited — `claimpay`** | none at credit; `T5 epoch pay claim` at the claim |
| Batch-op reward | `compute_bps` x `batch_op_bps` | each active batch-operator group member | **credited — `claimpay`** | none at credit; `T5 epoch pay claim` at the claim |
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
producer pool is divided by the period's nominal slot count, raised to the
blocks counted when a period runs long, and each producer is paid for its
counted blocks. Standbys
draw from a separate slice, at a rank-decreasing weight, and are
not paid per block. Batch-op pay is weighted by the roster captured for each
accrued epoch, then split evenly across all
members of each historical roster (the per-member slice is the group pool divided
by the full group size). Equal rosters are coalesced before their weighted slice
is calculated, preserving one group-level rounding step per roster. A credit is
made only for members that are opreg-ACTIVE, so the slices of skipped (inactive /
slashed / terminated) members stay in the treasury rather than being redistributed
to the active ones. Batch operators are paid from emission only, so
`producer_bps` / `batch_op_bps` split the whole compute share.

Allocated is not the same as paid: only **eligible** shares are actually
credited. Emission WIRE stays in the treasury when an **empty historical
roster** owns an accrued epoch, when a **member is not opreg-ACTIVE**, as the
**remainder** of the two integer divisions (per-roster weighting, then the even
per-member split), or when roster history is incomplete and the whole batch
emission slice takes the bounded recovery path. Every
`batchepochs` row represents one accrued epoch, so a zero-epoch historical
roster cannot arise.

That divisor is the sum of `t5state.batch_group_epochs`, **not** the configured
`pay_cadence_epochs`. The two can differ — a mid-period `setemitcfg` cadence
change, or the shortened genesis period — and normalizing by the configured value
is what caused a payout to be multiplied. The positional counters establish the
actual period length only; `batchepochs` supplies roster identity, so a schedule
rotation cannot reassign an earlier epoch to the current front group.

### Roster-history bounds and recovery

`setemitcfg` accepts a payment cadence only in `[1, 10]`, which bounds an
ordinary payment period to at most ten immutable roster snapshots. It also
validates `pay_cadence_epochs * operators_per_epoch <= 100`, preserving the
prior one-roster ceiling on expensive per-recipient credits; `sysio.epoch`
enforces the same constraint when its operator count changes. At the intended
production topology of 21 operators per epoch, the maximum accepted cadence is
4 (about 24 minutes at six-minute epochs); the conservative 100-credit ceiling
limits peak work in the mandatory `advance` transaction. When
`sysio.epoch` reads an older serialized configuration during an upgrade, it
effectively shortens the cadence to satisfy both bounds; the stored value is not
rewritten until a normal `setemitcfg` update. `payepoch` independently refuses
to issue more than 100 recipient credits summed across distinct historical
rosters and takes the non-halting recovery path if malformed legacy history
exceeds that limit.

The `rcrdbatch` inline action also prunes the exact oldest retained row before
writing a new one. `payepoch` caps cleanup at 20 rows per payment, twice the
largest accepted period, so even a malformed gapped table cannot turn recovery
into an unbounded transaction; stale history drains monotonically.
If `payepoch` sees missing, stale, non-contiguous, or over-cap history, it does
not guess a roster or abort the enclosing `advance`: it retains that period's
batch-emission slice in the treasury, drains the unusable history within
the cleanup bound, and begins clean history after stale rows are gone. A complete
history remains required to credit batch-operator rewards. Every `epochlog` row
records whether history was complete and the exact batch-emission amount
retained, making recovery distinguishable from an ordinary zero-eligible payout.

`epoch_log_retention_count` counts payment rows, not elapsed epoch indexes, so
cadence values greater than one retain the configured number of audit records.

`epochlog.batch_emission_retained` records undistributed batch emission.

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
emissions readiness gate, and `claimnodedis`, which refuses a node-owner
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

The gift is simple accrual on the **current** LIQ `stat.supply` at a **TWAP**
of the registered LIQ/WIRE swap pool, for at most the last 72 hours since payment.
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
in a construction near the cap and 499.98 WIRE at the worst point. The bound holds for every
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
A below-minimum allowance or Andon hold leaves the clock open. An empty supply
resets the clock and all shortfall diagnostics, except during an Andon hold.
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
during warm-up, under an Andon hold) writes nothing and leaves the flag as it was. A later full payment clears all four diagnostics. An unallocated emissions
remainder stays in the treasury; it does not authorize a kicker budget by itself.

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
| `initt5` | `sysio.system` | Initialize T5 treasury state (once) |
| `addnodeowner` | `sysio.roa` | Register a node owner in a tier |
| `claimnodedis` | node owner | Claim vested node-owner allocation (refused when it would spend pay reserved in `payclaimtot`) |
| `claimpay` | the claiming account | Claim epoch pay credited by `payepoch` (producer / standby / batch-operator share) |
| `accrueepoch` | `sysio.epoch` | Accrue this epoch's curve share |
| `rcrdbatch` | `sysio.epoch` | Record the batch-operator roster for this accrued epoch |
| `payepoch` | `sysio.epoch` | Distribute the period's compute / capex / governance (credits `payclaims`; pushes only the category buckets) |
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
| `t5state` | Singleton | Treasury state: pending emission, total distributed, decay continuity |
| `batchepochs` | Table | Up to ten immutable batch-operator rosters for unprocessed accrued epochs; cleared by `payepoch` |
| `epochlog` | Table | Per-pay-epoch audit log (head-pruned to `epoch_log_retention_count`) |

## Dependencies

- Driven inline by `sysio.epoch::advance` (`accrueepoch` + `rcrdbatch` + `payepoch`).
- Reads producer eligibility and operator status from `sysio.opreg`.
- Reads the canonical epoch duration from `sysio.epoch::epochcfg`.

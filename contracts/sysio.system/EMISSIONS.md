# sysio.system emissions

WIRE emission and reward distribution. The T5 treasury emits WIRE on a decaying
per-epoch curve; the per-epoch amount is split by basis points into categories
that are either pushed automatically each pay-epoch or pulled by the recipient
through a claim action. All amounts are `WIRE` (9-decimal subunits).

## Per-epoch flow

`sysio.epoch::advance` drives emissions inline:

- Every epoch calls `accrueepoch`, which adds that epoch's curve share
  (`compute_epoch_emission`) to `t5state.pending_emission_amount`.
- On a pay-epoch boundary (`emitcfg.pay_cadence_epochs`) it then calls
  `payepoch`, which distributes the accumulated `period_emission` by the
  configured basis-point splits.

The curve decays from `annual_initial_emission` toward `annual_min_emission`
(clamped by `annual_max_emission`), stops at `t5_floor`, and auto-throttles as
`total_distributed` rises (capital claims count against it).

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
— `total_emission`, `compute_amount`, `capex_amount`, `governance_amount`,
`fee_distributed` — where `compute_amount` is the COMBINED producer + batch-operator
pool. Neither operator category's actually-credited total is stored, so once both kinds
of credit accumulate in one `payclaims` row, **the producer-versus-batch-operator split
is not available anywhere on chain**. What you can attribute: period totals per top-level
category from `epochlog`, and per-recipient amounts from `payclaims` deltas plus the
claim transfer.

**`sysio.ops` and `sysio.gov` stay pushed** because no claim path can reach them:
a claim needs `require_auth(account_name)`, `sysio.roa` forces
`net_weight`/`cpu_weight` to zero for every `sysio`-prefixed account, and neither
carries a contract that could emit the claim inline. They are protocol-owned
holding accounts with no code, so the notify-handler risk does not apply — the
same exception `fundclaim` makes for `sysio.dclaim`.

Producer pay is weight-scaled: active producers carry a flat weight and are
additionally scaled by their eligible rounds over the pay period, while standbys
carry a rank-decreasing weight and are not round-scaled. Batch-op pay is weighted
per group by that group's active-epoch count over the pay period, then split
evenly across all of the group's scheduled members (the per-member slice is the
group pool divided by the full group size). A credit is made only for members
that are opreg-ACTIVE, so the slices of skipped (inactive / slashed / terminated)
members stay in the treasury rather than being redistributed to the active
ones. Swap-fee rewards from `sysio.reserv`'s `rewards_bucket` are swept in
(`drainrewards`) and allocated **exclusively to the batch-operator
distribution**, on top of their emission share and weighted by that same
per-group active-epoch count. Producers are not paid out of swap fees, so
`producer_bps` / `batch_op_bps` govern the emission split only.

Allocated is not the same as paid: as with emissions, only **eligible** shares
are actually credited. WIRE stays in the treasury when there are **no groups
at all**, when an **empty group owns positive active epochs** (its weighted slice
is skipped), when a **member is not opreg-ACTIVE**, or as the **remainder** of the
two integer divisions (per-group weighting, then the even per-member split). A
group active in **zero** epochs is *not* one of these cases — its weighted
allocation is already zero, and because the per-group counts sum to the **actual
accrued-epoch divisor** the remaining groups absorb the whole pool.

That divisor is the sum of the per-group counters, **not** the configured
`pay_cadence_epochs`. The two can differ — a mid-period `setemitcfg` cadence
change, or the shortened genesis period — and normalizing by the configured value
is what caused a payout to be multiplied. Deriving it from the counters is what
makes the weights partition each pool by construction.

`epochlog.fee_distributed` records what was actually paid, so it can be lower
than the swept amount — and is `0` when no eligible batch operator existed at all,
even though `drainrewards` swept the bucket to zero regardless.

## Retrieved via a claim action (pulled by recipient)

### Epoch pay -- `sysio.system::claimpay(account_name)`

Producer, standby and batch-operator shares credited by `payepoch`. The caller
(auth = their own account) drains their whole `payclaims` row in one transfer —
there is no partial claim — and the action throws `no epoch pay to claim` when
nothing is owed. `payclaims` is readable to show a "you have X to claim" balance
first.

The credited WIRE never leaves the treasury's token balance until it is claimed,
so every gate that spends against that balance reserves it via the
`payclaimtot.outstanding` counter: `fundclaim`'s balance cap, `sysio.epoch`'s
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

### Staking rewards / capital -- `sysio.dclaim::claim(wire_account)`

The capital bucket is the implicit remainder
(`10000 - compute_bps - capex_bps - governance_bps`). It is NOT paid at
payepoch; it stays in sysio's balance and drains lazily:

1. A cross-chain `STAKING_REWARD` OPP message lands on `sysio.dclaim::onreward`.
2. `onreward` credits the staker's `pclaims` (pending-claims) row (or parks it
   in `unmapped` when the account is not yet AuthX-linked) and calls
   `sysio.system::fundclaim` to pull matching WIRE from the pool into
   `sysio.dclaim`.
3. The staker calls `dclaim::claim` (auth = their own account) to transfer the
   accumulated balance out (memo `sysio.dclaim claim`).

Unclaimed rows expire after `cap_config.claim_window_sec` and revert to the
dclaim pool via `flushexpired`. `fundclaim` and the whole OPP inbound path are
never-throw (transfers are capped / soft-dropped so a bad row cannot abort the
message chain), whereas `claimnodedis` and `claim` are ordinary user actions
that `check`-abort on bad input.

## Category split (basis points)

`compute_bps + capex_bps + governance_bps <= 10000`; the remainder is the
implicit capital reserve drained through `fundclaim`. `producer_bps +
batch_op_bps == 10000` (sub-split of compute). All set via `setemitcfg`.

```
period_emission
  |
  |-- compute_bps ----> compute_amount
  |                       |-- producer_bps --> producers/standbys   (claimpay)
  |                       '-- batch_op_bps --> batch operators       (claimpay)
  |-- capex_bps ------> sysio.ops                                    (pushed)
  |-- governance_bps -> sysio.gov                                    (pushed)
  '-- remainder ------> capital reserve -> sysio.dclaim (fundclaim)  (claim)
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
| `payepoch` | `sysio.epoch` | Distribute the period's compute / capex / governance (credits `payclaims`; pushes only the category buckets) |
| `fundclaim` | `sysio.dclaim` | Lazy capital drain into dclaim (never-throw) |
| `viewnodedist` | read-only | Preview a node owner's claimable amount |
| `viewepoch` | read-only | Current treasury / next-emission estimate |
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
| `epochlog` | Table | Per-pay-epoch audit log (head-pruned to `epoch_log_retention_count`) |

## Dependencies

- Driven inline by `sysio.epoch::advance` (`accrueepoch` + `payepoch`).
- Reads producer eligibility and operator status from `sysio.opreg`.
- Reads the canonical epoch duration from `sysio.epoch::epochcfg`.
- Folds swap-fee rewards from `sysio.reserv` (`drainrewards`).
- Funds `sysio.dclaim` on demand via `fundclaim` for the capital / staking-reward path.

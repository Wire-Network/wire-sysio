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

## Auto-transferred (pushed at `payepoch`, no claim)

Sent as `sysio.token::transfer` the moment the pay-epoch fires; the recipient
does nothing.

| Bucket | Share | Destination | Memo |
|--------|-------|-------------|------|
| Producer reward | `compute_bps` x `producer_bps` | each active producer / standby (rank <= `standby_end_rank`, opreg-ACTIVE) | `T5 producer reward` |
| Batch-op reward | `compute_bps` x `batch_op_bps` | each active batch-operator group member | `T5 batch operator reward` |
| Capex | `capex_bps` | `sysio.ops` | `T5 capex` |
| Governance | `governance_bps` | `sysio.gov` | `T5 governance` |

Producer pay is weight-scaled: active producers carry a flat weight and are
additionally scaled by their eligible rounds over the pay period, while standbys
carry a rank-decreasing weight and are not round-scaled. Batch-op pay is weighted
per group by that group's active-epoch count over the pay period, then split
evenly across all of the group's scheduled members (the per-member slice is the
group pool divided by the full group size). A transfer is sent only to members
that are opreg-ACTIVE, so the slices of skipped (inactive / slashed / terminated)
members stay in the treasury rather than being redistributed to the active
ones. Swap-fee rewards from `sysio.reserv`'s `rewards_bucket` are swept in
(`drainrewards`) and folded into the same compute distribution on top of
emissions, split by the same `producer_bps` / `batch_op_bps`.

## Retrieved via a claim action (pulled by recipient)

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
  |                       |-- producer_bps --> producers/standbys   (auto)
  |                       '-- batch_op_bps --> batch operators       (auto)
  |-- capex_bps ------> sysio.ops                                    (auto)
  |-- governance_bps -> sysio.gov                                    (auto)
  '-- remainder ------> capital reserve -> sysio.dclaim (fundclaim)  (claim)
```

## Emission actions

| Action | Auth | Description |
|--------|------|-------------|
| `setemitcfg` | `sysio.system` | Set / update emission config (validated against live state) |
| `setinittime` | `sysio.system` | Set node-owner vesting start time (once) |
| `initt5` | `sysio.system` | Initialize T5 treasury state (once) |
| `addnodeowner` | `sysio.roa` | Register a node owner in a tier |
| `claimnodedis` | node owner | Claim vested node-owner allocation |
| `accrueepoch` | `sysio.epoch` | Accrue this epoch's curve share |
| `payepoch` | `sysio.epoch` | Distribute the period's compute / capex / governance |
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
| `t5state` | Singleton | Treasury state: pending emission, total distributed, decay continuity |
| `epochlog` | Table | Per-pay-epoch audit log (head-pruned to `epoch_log_retention_count`) |

## Dependencies

- Driven inline by `sysio.epoch::advance` (`accrueepoch` + `payepoch`).
- Reads producer eligibility and operator status from `sysio.opreg`.
- Reads the canonical epoch duration from `sysio.epoch::epochcfg`.
- Folds swap-fee rewards from `sysio.reserv` (`drainrewards`).
- Funds `sysio.dclaim` on demand via `fundclaim` for the capital / staking-reward path.

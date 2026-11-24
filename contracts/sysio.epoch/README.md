# sysio.epoch

The depot's two clocks, and batch operator scheduling, for the OPP (Outpost Protocol) system.

## Responsibility

- Owns the **consensus epoch** (`epochcfg.epoch_duration_sec`, default 360 s, bounded to 60 s .. 30 days):
  the OPP envelope cadence. It advances only once every active outpost has reached consensus and its
  wall-clock boundary has passed.
- Owns the **economic clock** (`economycfg`): a fixed grid of economic epochs from a configured start
  time that may lie in the past. It drives emissions accrual and pay, collateral-withdrawal maturity and
  the producer pay slot count, and never waits on outpost consensus.
- Builds the initial batch operator schedule with `schbatchgps` and maintains it as a sliding window
  of `batch_op_groups` groups of `operators_per_epoch` members; the group on duty is always the front
  of the window
- Advances consensus epochs (`advance`), crank-driven by `sysio.msgch::chkcons` after genesis
- Opens economic epochs (`tickeconomy`), a permissionless crank the batch operators push
- Gates each economic epoch's emissions on readiness: if `sysio.system` cannot accrue or pay, the
  emissions are held and the episode recorded in `blocklog`; both clocks keep running
- Provides the global pause/unpause used by `sysio.chalg` while an envelope dispute is open (it stops
  the consensus epoch only)

Operator registration, collateral and status live on `sysio.opreg`; the outpost set lives on
`sysio.chains`. This contract reads both and mirrors neither.

## Tables

| Table | Type | Description |
|-------|------|-------------|
| `epochcfg` | `kv::global` | Consensus epoch: `epoch_duration_sec`, `operators_per_epoch` (default 7), `batch_operator_minimum_active` (default 21), `batch_op_groups` (default 3), `epoch_retention_envelope_log_count` (default 200) |
| `epochstate` | `kv::global` | Consensus epoch index, current / next epoch start, active group cursor, the resident group window, last consensus hash, pause flag |
| `economycfg` | `kv::global` | Economic clock: `epoch_duration_sec` (60 s .. 30 days) and the grid's `start_time` |
| `economystate` | `kv::global` | Economic epoch index and its current / next boundary |
| `blocklog` | `kv::table` | At most one row: the open emissions block episode, keyed by its first blocked economic epoch, with the reason, attempted emission, treasury and balance figures, and how many economic epochs it has held. Erased when an economic epoch's emissions pass |

## Actions

| Action | Auth | Description |
|--------|------|-------------|
| `setconfig` | `sysio.epoch` | Set the consensus epoch configuration. Requires `operators_per_epoch * batch_op_groups == batch_operator_minimum_active` and enforces the outposts' limits (`MAX_OPERATORS_PER_EPOCH` 7, Ethereum's group size; `MAX_BATCH_OP_GROUPS` 8, Solana's group count), so the window holds at most `MAX_SCHEDULED_BATCH_OPERATORS` (56) |
| `seteconomy` | `sysio.epoch` | Set the economic clock's duration and grid start. The grid may start in the past, but no change is accepted while a boundary has passed without its tick. Until the first tick a start still ahead may move, into the past too; after it the start is fixed, and a new duration spaces only the boundaries after the next one. So no boundary that has passed ever moves, and once the clock has ticked neither does the next one. Re-validated against `sysio.system`'s pay-period accrual bound |
| `schbatchgps` | `sysio.epoch` | Build the initial window from the `ACTIVE` batch operators on `sysio.opreg` (non-bootstrapped first, then bootstrapped, trimmed to `batch_operator_minimum_active`) |
| `advance` | permissionless at genesis (epoch 0 → 1); afterwards `sysio.msgch` or `sysio.epoch` | Advance the consensus epoch once its wall-clock boundary has passed and the contract is not paused |
| `tickeconomy` | permissionless | Open economic epoch `epoch_index`: refused unless it is the next index, its grid boundary has passed, and the consensus genesis advance has run |
| `pause` | `sysio.chalg` | Set the global pause |
| `unpause` | `sysio.chalg` | Clear the global pause |

## What `advance` does

1. Delivery audit of the expiring group for every active outpost: every operator that delivered a
   non-canonical envelope is slashed through `sysio.chalg::slashop`; then each member gets one
   `sysio.opreg::recorddel` (delivered only if it reached every active outpost), followed by
   `sysio.opreg::termcheck`, and the group earns one duty unit each through `sysio.system::rcrdduty`
   (skipped until `sysio.system::initt5` has run).
2. Bump the consensus epoch index, drop the expired group from the front of the window and append a
   new tail group drawn from `ACTIVE` batch operators that are not already resident (non-bootstrapped
   first). The resident groups stay disjoint, so an operator serves at most once every
   `batch_op_groups` epochs.
3. Queue `OPERATORS` (the operators the window seats, with their status and authex links; none for
   an empty window) and `BATCH_OPERATOR_GROUPS` (withheld when the next group is empty) to every
   active outpost through `sysio.msgch::queueout`, then `sysio.msgch::buildenv` per outpost.

## What `tickeconomy` does

1. Emissions readiness gate. A missing config or T5 state, or an exhausted treasury, accrues nothing;
   a pay epoch whose period total exceeds the spendable balance still accrues and leaves the payout to
   the first economic epoch that can cover it. Either way the episode is recorded in `blocklog`.
2. Bump the economic epoch index; its boundaries come from the grid, never from when the crank ran,
   so a late or caught-up crank accrues exactly what an on-time one would.
3. `sysio.opreg::flushwtdw` matures queued collateral withdrawals whose economic wait and consensus
   guard have both passed.
4. `sysio.system::creditwork` for the epochs before this one (rows an earlier tick's bound left, which
   belong to the period being paid), `accrueepoch`, plus `payepoch` on pay epochs, then `settleduty`
   (one bounded step of any open batch-duty settlement) and `creditwork` for this epoch (the blocks and
   duty recorded in it before the tick opened it join the period after the payout). None of them runs
   until `initt5` has: the gate holds accrual and payout, and the rest are skipped. A catch-up tick pays
   its period exactly as an on-time one: work done after the period's boundary is parked until its own
   epoch's tick.

## Dependencies

- Consensus epochs advanced by `sysio.msgch` (`chkcons`, and `bootstrap` at epoch 0); economic epochs
  opened by the batch operators' `tickeconomy` crank
- Reads operators from `sysio.opreg`, authex links from `sysio.authex`, outposts from `sysio.chains`
- Calls `sysio.chalg::slashop`, `sysio.opreg::{recorddel,termcheck,flushwtdw}`,
  `sysio.msgch::{queueout,buildenv}` and `sysio.system::{rcrdduty,accrueepoch,payepoch,settleduty,creditwork}`
- Pause/unpause controlled by `sysio.chalg`

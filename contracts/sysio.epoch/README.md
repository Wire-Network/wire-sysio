# sysio.epoch

The depot's two clocks, and batch operator scheduling, for the OPP (Outpost Protocol) system.

## Responsibility

- Owns one **consensus epoch per outpost chain** (`epochcfg.epoch_duration_sec`, default 360 s, bounded
  to 60 s .. 30 days): that chain's OPP envelope cadence, counter, schedule window and roster. A chain's
  epoch advances once that outpost has reached consensus and its wall-clock boundary has passed; the
  first chain's genesis advance is the network genesis (`netgenesis`).
- Owns the **economic clock** (`economycfg`): a fixed grid of economic epochs from a configured start
  time that may lie in the past. It drives emissions accrual and pay, collateral-withdrawal maturity and
  the producer pay slot count, and never waits on outpost consensus.
- Builds each chain's initial batch operator schedule with `schbatchgps` and maintains it as a sliding
  window of `batch_op_groups` groups of `operators_per_epoch` members; the group on duty is always the
  front of the window. A chain seats only the `ACTIVE` batch operators that serve it (`SERVING` in
  `sysio.opreg::servchains`)
- Advances a chain's consensus epoch (`advance`), crank-driven by `sysio.msgch::chkcons` after its genesis
- Opens economic epochs (`tickeconomy`), a permissionless crank the batch operators push
- Gates each economic epoch's emissions on readiness: if `sysio.system` cannot accrue or pay, the
  emissions are held and the episode recorded in `blocklog`; both clocks keep running
- Provides the per-chain pause/unpause used by `sysio.chalg` while an envelope dispute is open (it stops
  that chain's consensus epoch only)

Operator registration, collateral and status live on `sysio.opreg`; the outpost set lives on
`sysio.chains`. This contract reads both and mirrors neither.

## Tables

| Table | Type | Description |
|-------|------|-------------|
| `epochcfg` | `kv::table` | One row per outpost chain, keyed by its `sysio.chains` code: `epoch_duration_sec`, `operators_per_epoch` (default 7), `batch_operator_minimum_active` (default 21), `batch_op_groups` (default 3), `epoch_retention_envelope_log_count` (default 200) |
| `epochstate` | `kv::table` | One row per outpost chain: its consensus epoch index, current / next epoch start, active group cursor, the resident group window, pause flag |
| `netgenesis` | `kv::global` | Network genesis marker, written by the first chain's genesis advance; closes the bootstrap window and starts the economic clock |
| `economycfg` | `kv::global` | Economic clock: `epoch_duration_sec` (60 s .. 30 days) and the grid's `start_time` |
| `economystate` | `kv::global` | Economic epoch index and its current / next boundary |
| `blocklog` | `kv::table` | At most one row: the open emissions block episode, keyed by its first blocked economic epoch, with the reason, attempted emission, treasury and balance figures, and how many economic epochs it has held. Erased when an economic epoch's emissions pass |
| `rostersent` | `kv::table` | One row per outpost a roster has been queued to, keyed by its `sysio.chains` code: the SHA-256 of the last `OPERATORS` roster queued to it |

## Actions

| Action | Auth | Description |
|--------|------|-------------|
| `setconfig` | `sysio.epoch` | Set one chain's consensus epoch configuration. Requires `operators_per_epoch * batch_op_groups == batch_operator_minimum_active` and enforces the outposts' limits (`MAX_OPERATORS_PER_EPOCH` 7, Ethereum's group size; `MAX_BATCH_OP_GROUPS` 8, Solana's group count), so the window holds at most `MAX_SCHEDULED_BATCH_OPERATORS` (56) |
| `seteconomy` | `sysio.epoch` | Set the economic clock's duration and grid start. The start is fixed once the clock has ticked; it is re-validated against `sysio.system`'s pay-period accrual bound |
| `schbatchgps` | `sysio.epoch` | Build one chain's initial window from the `ACTIVE` batch operators on `sysio.opreg` that serve the chain (non-bootstrapped first, then bootstrapped, trimmed to `batch_operator_minimum_active`) |
| `advance` | permissionless at a chain's genesis (epoch 0 → 1); afterwards `sysio.msgch` or `sysio.epoch` | Advance one chain's consensus epoch once its wall-clock boundary has passed and it is not paused |
| `tickeconomy` | permissionless | Open economic epoch `epoch_index`: refused unless it is the next index, its grid boundary has passed, and the network genesis has happened |
| `pause` | `sysio.chalg` | Pause one chain's epoch advancement |
| `unpause` | `sysio.chalg` | Resume one chain's epoch advancement |
| `resendroster` | `sysio.epoch` | Erase an outpost's `rostersent` row, so the next `advance` queues the roster to it again. Refused when no roster has been queued to that outpost |

## What `advance(chain)` does

1. Delivery audit of the chain's expiring group, when the chain is an active outpost: every operator
   that delivered a non-canonical envelope to it is slashed through `sysio.chalg::slashop`; then each
   member gets one `sysio.opreg::recorddel` for this chain (delivered if it delivered to it), followed
   by `sysio.opreg::termcheck` for this chain, which evicts a member breaching the chain's miss rails
   from this chain alone, and each member of the group earns the epoch's scheduled length in
   seat-seconds through `sysio.system::rcrdduty` (skipped until `sysio.system::initt5` has run).
2. Bump the chain's consensus epoch index, drop the expired group from the front of its window and
   append a new tail group drawn from the `ACTIVE` batch operators serving the chain that are not
   already resident (non-bootstrapped first). The resident groups stay disjoint, so an operator serves
   at most once every `batch_op_groups` epochs.
3. Queue the chain's outpost `OPERATORS` (the operators its window seats, in account order, with their
   status and authex links; none for an empty window) when its `rostersent` hash differs, and
   `BATCH_OPERATOR_GROUPS` (withheld when the next group is empty), stamped with the chain's epoch
   index and duration, through `sysio.msgch::queueout`, then `sysio.msgch::buildenv`. An outpost keeps
   the last roster it received.

## What `tickeconomy` does

1. Emissions readiness gate. A missing config or T5 state, or an exhausted treasury, accrues nothing;
   a pay epoch whose period total exceeds the spendable balance still accrues and leaves the payout to
   the first economic epoch that can cover it. Either way the episode is recorded in `blocklog`.
2. Bump the economic epoch index; its boundaries come from the grid, never from when the crank ran,
   so a late or caught-up crank accrues exactly what an on-time one would.
3. `sysio.opreg::flushwtdw` matures queued collateral withdrawals whose economic wait and consensus
   guard (the slowest started chain's epoch index) have both passed.
4. `sysio.system::accrueepoch`, plus `payepoch` on pay epochs, then `settleduty` (one bounded step of
   any open batch-duty settlement, skipped until `initt5` has run).

## Dependencies

- Consensus epochs advanced by `sysio.msgch` (`chkcons`, and `bootstrap` at epoch 0); economic epochs
  opened by the batch operators' `tickeconomy` crank
- Reads operators from `sysio.opreg`, authex links from `sysio.authex`, outposts from `sysio.chains`
- Calls `sysio.chalg::slashop`, `sysio.opreg::{recorddel,termcheck,flushwtdw}`,
  `sysio.msgch::{queueout,buildenv}` and `sysio.system::{rcrdduty,accrueepoch,payepoch,settleduty}`
- Pause/unpause controlled by `sysio.chalg`

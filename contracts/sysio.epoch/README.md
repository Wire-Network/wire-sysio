# sysio.epoch

The depot's two clocks, and batch operator scheduling, for the OPP (Outpost Protocol) system.

## Responsibility

- Owns one **consensus epoch per outpost chain**: that chain's OPP envelope counter, schedule window and
  roster. `epochcfg.epoch_duration_sec` (default 360 s, bounded per chain to 10 s .. 30 days) is the
  chain's minimum time between two envelopes. A chain's epoch advances once that outpost has reached
  consensus and the chain is due: it has traffic, and its minimum has passed since the later of its
  epoch start and that traffic. Traffic is a general-lane outbound attestation queued for the chain, a
  backlog its last envelope left behind, a backlog on the outpost that a strict majority of the chain's
  eligible group reports once its current epoch is decided (`sysio.msgch::reportqueue`, since the
  majority-th earliest reported time; Solana reports the start of its current backlog, Ethereum its oldest
  queued message), or a governance `resendroster`. `sysio.msgch` records the first three. An idle chain
  never advances. The first chain's genesis advance is the network genesis (`netgenesis`); a genesis
  advance needs no traffic. An epoch's books (deliveries, misses, duty) close at the advance after it.
- On an idle chain the schedule window and `previous_group` stay as they are, so an operator they seat
  stays blocked from `sysio.opreg::regoperator` and `joinchain` until enough advances happen, each of
  which needs traffic or a `resendroster`.
- Owns the **economic clock** (`economycfg`): a fixed grid of economic epochs from a configured start
  time that may lie in the past. It drives emissions accrual and pay, collateral-withdrawal maturity and
  the producer pay slot count, and never waits on outpost consensus.
- Builds each chain's initial batch operator schedule with `schbatchgps` and maintains it as a sliding
  window of `batch_op_groups` groups of `operators_per_epoch` members; the group on duty is always the
  front of the window. A chain seats only the `ACTIVE` batch operators that serve it (`SERVING` in
  `sysio.opreg::servchains`).
- Advances a chain's consensus epoch (`advance`): its genesis from `sysio.msgch::bootstrap` /
  `bootchain`, every later epoch crank-driven by `sysio.msgch::chkcons(chain)`
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
| `epochstate` | `kv::table` | One row per outpost chain: its consensus epoch index, current epoch start, `traffic_since` (when its pending traffic began) and `due_at` (when it may next advance), both zero while it is idle, active group cursor, the resident group window, pause flag, and `previous_group`, the group that served the previous epoch (empty before epoch 2) |
| `netgenesis` | `kv::global` | Network genesis marker, written by the first chain's genesis advance; closes the bootstrap window and starts the economic clock |
| `economycfg` | `kv::global` | Economic clock: `epoch_duration_sec` (60 s .. 30 days) and the grid's `start_time` |
| `economystate` | `kv::global` | Economic epoch index and its current / next boundary |
| `blocklog` | `kv::table` | At most one row: the open emissions block episode, keyed by its first blocked economic epoch, with the reason, attempted emission, treasury and balance figures, and how many economic epochs it has held. Erased when an economic epoch's emissions pass |
| `rostersent` | `kv::table` | One row per outpost a roster has been queued to, keyed by its `sysio.chains` code: the SHA-256 of the last `OPERATORS` roster queued to it |

## Actions

| Action | Auth | Description |
|--------|------|-------------|
| `setconfig` | `sysio.epoch` | Set one chain's consensus epoch configuration. Bounds `epoch_duration_sec` to `MIN_EPOCH_DURATION_SEC` (10 s) .. `MAX_EPOCH_DURATION_SEC` (30 days). Requires `operators_per_epoch * batch_op_groups == batch_operator_minimum_active` and enforces the outposts' limits (`MAX_OPERATORS_PER_EPOCH` 7, Ethereum's group size; `MAX_BATCH_OP_GROUPS` 8, Solana's group count), so the window holds at most `MAX_SCHEDULED_BATCH_OPERATORS` (56). Recomputes `due_at` with the new minimum while the chain has traffic |
| `seteconomy` | `sysio.epoch` | Set the economic clock's duration and grid start. The start is fixed once the clock has ticked; it is re-validated against `sysio.system`'s pay-period accrual bound |
| `schbatchgps` | `sysio.epoch` | Build one chain's initial window from the `ACTIVE` batch operators on `sysio.opreg` that serve the chain (non-bootstrapped first, then bootstrapped, trimmed to `batch_operator_minimum_active`) |
| `advance` | `sysio.msgch` or `sysio.epoch` | Advance one chain's consensus epoch once it is due (`due_at` set and passed) and it is not paused; otherwise it returns without effect, under either authority. The genesis advance (epoch 0 → 1) needs no traffic and requires an active outpost whose schedule window `schbatchgps` has seated |
| `notetraffic` | `sysio.msgch` (`buildenv`, `reportqueue`) | Record traffic for one chain since `since` (a zero or future `since` counts as now): `traffic_since` keeps the earliest, `due_at` = max(`current_epoch_start`, `traffic_since`) + the chain's minimum. No effect before the chain's genesis advance. A paused chain records traffic but does not advance |
| `tickeconomy` | permissionless | Open economic epoch `epoch_index`: refused unless it is the next index, its grid boundary has passed, and the network genesis has happened |
| `pause` | `sysio.chalg` | Pause one chain's epoch advancement |
| `unpause` | `sysio.chalg` | Resume one chain's epoch advancement |
| `resendroster` | `sysio.epoch` | Erase an outpost's `rostersent` row, so the next `advance` queues the roster to it again, and record traffic for the chain at now, so it is due one minimum later at the latest. Refused when no roster has been queued to that outpost |

## What `advance(chain)` does

After the chain's genesis, `advance` returns without effect unless the chain is due. Then:

1. Close the books of the previous epoch: an epoch's books close one advance after it, so the late
   deliveries `sysio.msgch` accepted for it during the closing epoch count. An epoch the chain closed as
   an active outpost is booked even if the chain is no longer one at this advance; an epoch it closed
   while not an active outpost keeps no books. Each member of `previous_group` gets one
   `sysio.opreg::recorddel` for this chain and that epoch (delivered if it delivered to it, in the epoch
   or late, whatever its envelope; a non-canonical deliverer was already slashed when `sysio.msgch`
   accepted the epoch's winner), followed by `sysio.opreg::termcheck` for this chain, which evicts a
   member breaching the chain's miss rails from this chain alone and forfeits its unsettled duty there,
   and each member of the group not evicted from the chain earns the chain's minimum duration, as
   configured at this advance, in seat-seconds through `sysio.system::rcrdduty`, however long the epoch
   ran (skipped until
   `sysio.system::initt5` has run). The genesis advance and the advance from epoch 1, whose
   `previous_group` is empty, record nothing.
2. Bump the chain's consensus epoch index and open the next epoch at the current time with no traffic.
   Keep the window's front group, which served the closing epoch, as `previous_group` (none when the
   chain is not an active outpost), drop it from the window and append a new tail group drawn from the
   `ACTIVE` batch operators serving the chain that are not already resident (non-bootstrapped first).
   The resident groups stay disjoint, so an operator serves at most once every `batch_op_groups` epochs.
   A member that an advance evicts keeps the seat the previous slide may already have drawn for it,
   which it cannot deliver; that one group runs a member short until the seat slides out. Bootstrapped
   operators are never evicted.
3. Queue the chain's outpost `OPERATORS` (the operators its window seats, in account order, with their
   status and authex links; none for an empty window) when its `rostersent` hash differs, and
   `BATCH_OPERATOR_GROUPS` (withheld when the next group is empty), stamped with the chain's epoch
   index and duration, through `sysio.msgch::queueout`, then `sysio.msgch::buildenv`. An outpost keeps
   the last roster it received. General-lane rows the envelope leaves behind record traffic again, since
   the oldest of them became ready; a chain that is not an active outpost builds no envelope, and its
   queued general-lane rows record traffic the same way.

## What `tickeconomy` does

1. Emissions readiness gate. A missing config or T5 state, or an exhausted treasury, accrues nothing;
   a pay epoch whose period total exceeds the spendable balance still accrues and leaves the payout to
   the first economic epoch that can cover it. Either way the episode is recorded in `blocklog`.
2. Bump the economic epoch index; its boundaries come from the grid, never from when the crank ran,
   so a late or caught-up crank accrues exactly what an on-time one would.
3. `sysio.opreg::flushwtdw` matures queued collateral withdrawals whose economic wait has passed and
   whose waited-on chains have each reached the request's consensus guard for that chain.
4. `sysio.system::accrueepoch`, plus `payepoch` on pay epochs, then `settleduty` (one bounded step of
   any open batch-duty settlement, skipped until `initt5` has run).

## Dependencies

- Each chain's consensus epoch advanced by `sysio.msgch` (`chkcons(chain)`, and `bootstrap` /
  `bootchain` at the chain's epoch 0), and its traffic recorded by `sysio.msgch` (`notetraffic` from
  `buildenv` and `reportqueue`, and its `queueout` notification); economic epochs opened by the batch
  operators' `tickeconomy` crank
- Reads operators and their served chains from `sysio.opreg`, authex links from `sysio.authex`, outposts
  from `sysio.chains`
- Calls `sysio.opreg::{recorddel,termcheck,flushwtdw}`,
  `sysio.msgch::{queueout,buildenv}` and `sysio.system::{rcrdduty,accrueepoch,payepoch,settleduty}`
- Per-chain pause/unpause controlled by `sysio.chalg`

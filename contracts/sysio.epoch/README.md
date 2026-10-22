# sysio.epoch

Global epoch clock and batch operator scheduling contract for the OPP (Outpost Protocol) system.

## Responsibility

- Owns the single global epoch clock (`epoch_duration_sec`, default 360 s, bounded to 60 s .. 30 days)
- Builds the initial batch operator schedule with `schbatchgps` and maintains it as a sliding window
  of `batch_op_groups` groups of `operators_per_epoch` members; the group on duty is always the front
  of the window
- Advances epochs (`advance`), crank-driven by `sysio.msgch::chkcons` after genesis
- Gates every advance on emissions readiness: if `sysio.system` cannot pay the epoch, the advance is
  recorded in `blocklog` and skipped (never thrown), and retried on the next `chkcons`
- Provides the global pause/unpause used by `sysio.chalg` while an envelope dispute is open

Operator registration, collateral and status live on `sysio.opreg`; the outpost set lives on
`sysio.chains`. This contract reads both and mirrors neither.

## Tables

| Table | Type | Description |
|-------|------|-------------|
| `epochcfg` | `kv::global` | `epoch_duration_sec`, `operators_per_epoch` (default 7), `batch_operator_minimum_active` (default 21), `batch_op_groups` (default 3), `epoch_retention_envelope_log_count` (default 200) |
| `epochstate` | `kv::global` | Current epoch index, current / next epoch start, active group cursor, the resident group window, last consensus hash, pause flag |
| `blocklog` | `kv::table` | One row per epoch the emissions gate blocked: reason, attempted emission, treasury and balance figures, first/last retry and retry count. Pruned when the gate passes |

## Actions

| Action | Auth | Description |
|--------|------|-------------|
| `setconfig` | `sysio.epoch` | Set the epoch configuration. Requires `operators_per_epoch * batch_op_groups == batch_operator_minimum_active` and enforces the size caps (`MAX_OPERATORS_PER_EPOCH` 100, `MAX_BATCH_OP_GROUPS` 255, `MAX_SCHEDULED_BATCH_OPERATORS` 1000) |
| `schbatchgps` | `sysio.epoch` | Build the initial window from the `ACTIVE` batch operators on `sysio.opreg` (non-bootstrapped first, then bootstrapped, trimmed to `batch_operator_minimum_active`) |
| `advance` | permissionless at genesis (epoch 0 → 1); afterwards `sysio.msgch` or `sysio.epoch` | Advance the epoch once the wall-clock boundary has passed and the contract is not paused |
| `pause` | `sysio.chalg` | Set the global pause |
| `unpause` | `sysio.chalg` | Clear the global pause |

## What `advance` does

1. Emissions readiness gate (block-and-retry via `blocklog`, never a throw).
2. Delivery audit of the expiring group for every active outpost: every operator that delivered a
   non-canonical envelope is slashed through `sysio.chalg::slashop`; each member's delivery result is
   recorded with `sysio.opreg::recorddel`, followed by `sysio.opreg::termcheck`.
3. Bump the epoch index, drop the expired group from the front of the window and append a new tail
   group drawn from `ACTIVE` batch operators that are not already resident (non-bootstrapped first).
   The resident groups stay disjoint, so an operator serves at most once every `batch_op_groups`
   epochs.
4. `sysio.opreg::flushwtdw` matures queued collateral withdrawals.
5. Queue `OPERATORS` and `BATCH_OPERATOR_GROUPS` to every active outpost through
   `sysio.msgch::queueout` (an empty group is withheld), then `sysio.msgch::buildenv` per outpost.
6. `sysio.system::accrueepoch` and `rcrdbatch`, plus `payepoch` on pay epochs.

## Dependencies

- Advanced by `sysio.msgch` (`chkcons`, and `bootstrap` at epoch 0)
- Reads operators from `sysio.opreg`, authex links from `sysio.authex`, outposts from `sysio.chains`
- Calls `sysio.chalg::slashop`, `sysio.opreg::{recorddel,termcheck,flushwtdw}`,
  `sysio.msgch::{queueout,buildenv}` and `sysio.system::{accrueepoch,rcrdbatch,payepoch}`
- Pause/unpause controlled by `sysio.chalg`

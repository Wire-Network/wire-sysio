# sysio.epoch

Epoch lifecycle and batch operator scheduling contract for the OPP (Outpost Protocol) system.

## Responsibility

- Manages the synthetic epoch clock (default: 6-minute epochs)
- Maintains the batch operator roster (registration, warmup, cooldown, blacklist)
- Assigns operators into 3 rotation groups of 7 via `initgroups`
- Advances epochs with deterministic group rotation (`epoch_index % 3`)
- Supports in-place operator replacement when an operator is slashed or deregistered
- Registers external chain outposts (ETH, SOL, etc.)
- Provides global pause/unpause (controlled by `sysio.chalg`)

## Tables

| Table | Type | Description |
|-------|------|-------------|
| `epochcfg` | Singleton | Epoch duration, group count, warmup/cooldown config |
| `epochstate` | Singleton | Current epoch index, active group, 3 groups of 7, pause flag |
| `operators` | Multi-index | Operator roster with type, status, collateral, group assignment |
| `outposts` | Multi-index | Registered external chain outposts |

## Actions

| Action | Auth | Description |
|--------|------|-------------|
| `setconfig` | `sysio.epoch` | Set epoch configuration |
| `regoperator` | `sysio.epoch` | Register an operator |
| `unregoper` | operator | Begin deregistration (cooldown) |
| `advance` | permissionless | Advance epoch if duration elapsed |
| `initgroups` | `sysio.epoch` | One-time group assignment (21 operators) |
| `replaceop` | `sysio.epoch` | Replace operator in-place within group |
| `regoutpost` | `sysio.epoch` | Register an outpost chain |
| `pause` | `sysio.chalg` | Set global pause |
| `unpause` | `sysio.chalg` | Clear global pause |

## Dependencies

- Notifies `sysio.msgch` on epoch advancement
- Pause/unpause controlled by `sysio.chalg`

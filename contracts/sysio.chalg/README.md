# sysio.chalg

Challenge request/response, slash execution, and global pause contract.

## Responsibility

- Initiates challenge rounds when `sysio.msgch` detects consensus failure
- Supports up to 2 automatic challenge rounds before manual escalation
- Manages manual resolution via `sysio.msig` governance (2/3 vote)
- Executes slashing of faulty operators through `sysio.uwrit`
- Controls global pause/unpause on `sysio.epoch`

## Tables

| Table | Type | Description |
|-------|------|-------------|
| `challenges` | Multi-index | Challenge entries with round, status, operator lists |
| `resolutions` | Multi-index | Manual resolution records linked to msig proposals |

## Actions

| Action | Auth | Description |
|--------|------|-------------|
| `initchal` | `sysio.msgch` | Initiate challenge on consensus failure |
| `submitresp` | `sysio.chalg` | Process challenge response from outpost |
| `escalate` | `sysio.chalg` | Escalate to next round or manual resolution |
| `submitres` | submitter | Submit manual resolution with 3 hashes |
| `enforce` | `sysio.chalg` | Enforce resolution after msig approval |
| `slashop` | `sysio.chalg` | Execute slash on an operator |

## Challenge Flow

1. **Round 1**: `sysio.msgch` → `initchal` → CHALLENGE_REQUEST sent to outpost
2. **Response**: Outpost responds → `submitresp` evaluates, slashes faulty operators if identified
3. **Round 2** (if needed): `escalate` → new challenge round
4. **Manual** (if Round 2 fails): `escalate` → GLOBAL PAUSE → `submitres` → `sysio.msig` vote → `enforce`

## Dependencies

- Triggered by `sysio.msgch` on consensus failure
- Slashes via `sysio.uwrit::slash`
- Pauses/unpauses via `sysio.epoch::pause/unpause`
- Manual resolution via `sysio.msig`

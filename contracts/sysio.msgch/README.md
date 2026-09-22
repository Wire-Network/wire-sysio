# sysio.msgch

Inbound/outbound OPP message chain management and consensus tracking contract.

## Responsibility

- Manages inbound chain requests from external outposts
- Collects deliveries from all 7 elected batch operators per epoch
- Evaluates consensus (Option A: unanimous 7/7, Option B: 4+ majority at epoch boundary)
- Stores and routes individual messages by attestation type
- Queues outbound messages and builds OPP envelopes for delivery to outposts

## Tables

| Table | Type | Description |
|-------|------|-------------|
| `inchainreq` | Multi-index | Inbound chain requests per outpost/epoch |
| `deliveries` | Multi-index | Per-operator delivery records with chain hashes |
| `messages` | Multi-index | Individual OPP messages (inbound + outbound) |
| `outenvelopes` | Multi-index | Built outbound envelopes pending delivery |

## Actions

| Action | Auth | Description |
|--------|------|-------------|
| `crank` | permissionless | Main depot crank, advances processing |
| `createreq` | `sysio.msgch` | Create inbound chain request for an outpost |
| `deliver` | operator | Batch operator delivers a chain with hash + messages |
| `evalcons` | `sysio.msgch` | Evaluate consensus on a chain request |
| `processmsg` | `sysio.msgch` | Process a READY message (unpack, route attestations) |
| `queueout` | `sysio.epoch`, `sysio.opreg`, `sysio.uwrit`, `sysio.reserv`, `sysio.liq`, or `sysio.msgch` | Queue an outbound message to an outpost |
| `buildenv` | `sysio.msgch` | Build outbound envelope from queued messages |

## Dependencies

- Reads epoch state from `sysio.epoch`
- Notifies `sysio.chalg` on consensus failure
- Routes attestations to `sysio.epoch`, `sysio.uwrit`, `sysio.chalg`
- Routes `SYNDICATE_LIQ` and `LIQ_YIELD` to `sysio.liq` (`mintsynd` for an AuthX-linked
  user, `park` for an unlinked pubkey, `mintyield` for a yield report) once the payload's
  token is an active `TOKEN_KIND_LIQ` row on `sysio.tokens` bound to the proven outpost.
  Every refusal is a logged drop, never an abort. `DESYNDICATE_LIQ` is outbound only:
  `sysio.liq::desyndicate` queues it through `queueout`.

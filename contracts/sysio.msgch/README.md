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
| `queueout` | `sysio.msgch` | Queue an outbound message to an outpost |
| `buildenv` | `sysio.msgch` | Build outbound envelope from queued messages |

## Dependencies

- Reads epoch state from `sysio.epoch`
- Notifies `sysio.chalg` on consensus failure
- Routes attestations to `sysio.epoch`, `sysio.uwrit`, `sysio.chalg`

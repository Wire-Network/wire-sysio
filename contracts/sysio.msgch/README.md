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
| `queueout` | `sysio.epoch`, `sysio.opreg`, `sysio.uwrit`, `sysio.reserv`, `sysio.liq`, or `sysio.msgch` | Queue an outbound attestation for an outpost |
| `buildenv` | `sysio.epoch` | Pack an outpost's queued attestations into its outbound envelope (see below) |

## Outbound queue

`queueout` appends a READY row to `attestations`. Its `byqueue` index keys rows by
(status, chain_code, lane), so one outpost's queue sits together in packing order. The operator
schedule (`OPERATORS` and `BATCH_OPERATOR_GROUPS`) comes first, then everything else, each oldest
first. The schedule goes first because an outpost seats the next epoch's batch-operator group only
from `BATCH_OPERATOR_GROUPS`: other traffic queued ahead of it would leave that epoch's deliverers
unseated.

`sysio.epoch::advance` sends `buildenv` inline for every active outpost once per epoch. `buildenv`
walks that outpost's READY rows in packing order and packs them into one envelope until the next
row would push it past 32 KiB; the rest wait for the next epoch. However deep the queue, the walk
reads the rows it packs (at most one envelope's worth), the rows it drops, and the one row it stops
on.

The schedule may use 24 KiB of an envelope (`SCHEDULE_LANE_BUDGET_BYTES`), which equals the Solana
outpost's OPERATORS payload byte cap. That is the only Solana roster limit the budget covers: Solana
also skips a roster over its entry cap, one that maps two names to one key, and one that resolves to
no operators. `advance` withholds a roster that would take the schedule past the budget, and
`buildenv` prints a diagnostic when schedule rows queued some other way do. Each lane has a size
bound:

| Lane | Largest `data` that ships |
|---|---|
| schedule | 32 232 bytes: fits an otherwise empty envelope |
| other | 7 656 bytes: fits beside a full schedule lane |

A row over its lane's bound can never ship. `buildenv` erases it and prints `DROP attestation`
instead of aborting `advance`, and the rows behind it still ship. While the schedule stays within
its budget, every row within its bound ships once it reaches the front of its lane. If the drops
empty the queue, the envelope still goes out with no attestations, because each outpost answers the
depot's envelope with its own.

## Dependencies

- Reads epoch state from `sysio.epoch`
- Notifies `sysio.chalg` on consensus failure
- Routes attestations to `sysio.epoch`, `sysio.uwrit`, `sysio.chalg`
- Routes `SYNDICATE_LIQ` and `LIQ_YIELD` to `sysio.liq` (`mintsynd` for an AuthX-linked
  user, `park` for an unlinked pubkey, `mintyield` for a yield report) once the payload's
  token is an active `TOKEN_KIND_LIQ` row on `sysio.tokens` bound to the proven outpost
  and, for a syndication, the user's key family is the outpost's own.
  Every refusal is a logged drop, never an abort. `DESYNDICATE_LIQ` is outbound only:
  `sysio.liq::desyndicate` queues it through `queueout`.

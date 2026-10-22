# sysio.msgch

Inbound/outbound OPP message chain management and consensus tracking contract.

## Responsibility

- Accepts inbound OPP envelopes delivered by the batch operators of the active group, one delivery per
  operator per outpost per epoch (`MAX_ENVELOPE_BYTES` = 32,768; larger deliveries are rejected)
- Evaluates consensus per (outpost, epoch): every eligible operator delivers an identical envelope
  (fast path), or a strict majority of the group agrees once the epoch boundary has passed. A
  post-boundary split with no strict majority opens a `sysio.chalg` dispute
- Validates the per-outpost envelope chain and message sequence, stores the accepted messages and
  attestations, and routes each attestation to its depot handler
- Queues outbound attestations and packs them into one OPP envelope per outpost per epoch; whatever
  does not fit under the envelope cap stays queued for the next epoch
- Drives the epoch: `chkcons` advances `sysio.epoch` once every active outpost has reached
  consensus and the wall-clock boundary has passed

## Tables

| Table | Type | Description |
|-------|------|-------------|
| `envelopes` | `kv::table` | Inbound deliveries, one row per batch operator per outpost per epoch (working state, pruned to the current and previous epoch) |
| `messages` | `kv::table` | Individual OPP messages extracted from consensus-verified envelopes |
| `attestations` | `kv::table` | Attestations extracted inbound, and outbound attestations queued (`READY`) for envelope packing |
| `outenvelopes` | `kv::table` | Built outbound envelopes awaiting relay, with their canonical epoch digest |
| `outpcons` | `kv::table` | Per-outpost consensus record: accepted envelope digest and inbound message-chain tip |
| `attseq` | `kv::table` | Attestation sequence counter |
| `envlog` | `kv::table` | Metadata-only envelope audit log, capped at `active_outposts * 2 * epoch_retention_envelope_log_count` rows |

## Actions

| Action | Auth | Description |
|--------|------|-------------|
| `bootstrap` | `sysio.msgch` | Epoch-0 only: asserts the `sysio.system` emissions config is set, then triggers the first `sysio.epoch::advance` |
| `deliver` | batch operator (resident active group, `ACTIVE` in `sysio.opreg`) | Deliver an inbound envelope for an outpost; calls `evalcons` inline |
| `evalcons` | `sysio.msgch` | Evaluate consensus for an (outpost, epoch); on consensus store and dispatch the attestations; on an eligible split open a `sysio.chalg` dispute |
| `chkcons` | permissionless | If no dispute is open, every active outpost has consensus and the boundary has passed, call `sysio.epoch::advance` |
| `resolvedisp` | `sysio.chalg` | Dispatch the winning envelope of a resolved dispute |
| `queueout` | `sysio.epoch`, `sysio.opreg`, `sysio.synd`, or `sysio.msgch` | Queue an outbound attestation for an outpost |
| `buildenv` | `sysio.epoch` | Pack `READY` attestations into the outpost's outbound envelope for the epoch |

## Attestation routing

Inbound (outpost → depot):

- `SYNDICATE_LIQ` → `sysio.synd::onsynd`, `LIQ_YIELD` → `sysio.synd::onyield`. Each carries the
  envelope's epoch and canonical digest, and is forwarded only once the payload's token is an active
  `TOKEN_KIND_LIQ` row on `sysio.tokens` bound to the proven outpost and, for a syndication, the
  user's key family is the outpost's own. After the envelope's last attestation it sends
  `sysio.synd::closeenv`, only when at least one of them reached `sysio.synd`.
- `NODE_OWNER_REG` → `sysio.roa` (account creation and node-owner registration, recording the
  outpost link).
- Every other type is dropped. Every refusal is a logged drop, never an abort.

Outbound (depot → outpost), queued through `queueout`:

- `OPERATORS` and `BATCH_OPERATOR_GROUPS` from `sysio.epoch::advance`, to every active outpost
- `DESYNDICATE_LIQ` from `sysio.synd::desyndicate`

## Dependencies

- Reads epoch state from `sysio.epoch` and operator status from `sysio.opreg`
- Opens disputes on `sysio.chalg` (`opendispute`); `sysio.chalg::chkdispute` calls back into `resolvedisp`
- Triggers `sysio.epoch::advance` from `chkcons` (and from `bootstrap` at epoch 0)
- Routes inbound attestations to `sysio.synd` and `sysio.roa`; node-owner linking can indirectly sweep funded DClaim imports

# sysio.msgch

Inbound/outbound OPP message chain management and consensus tracking contract.

## Responsibility

- Accepts inbound OPP envelopes delivered by the batch operators of the active group, one delivery per
  operator per outpost per epoch (`MAX_ENVELOPE_BYTES` = 32,768; larger deliveries are rejected)
- Evaluates consensus for each outpost's current epoch: the first strict majority of the eligible group
  (`count * 2 > eligible`) decides it, at any time. A split of at least two delivered versions, with fewer
  than a majority of the eligible operators still silent, opens a `sysio.chalg` dispute once no version
  can still reach a majority with the silent operators, or once the epoch has been idle for
  `DISPUTE_FALLBACK_EPOCHS` (3) of the chain's configured durations: since the later of its start and its
  newest eligible delivery, each delivery restarting the timer. The silent minority then counts as
  absent. An epoch that reached consensus stays decided: a later delivery of the accepted bytes is
  recorded as delivered, and a differing one is refused
- Slashes non-canonical deliverers when an epoch's winner is accepted, by consensus or by dispute
  resolution: every operator whose recorded delivery for the epoch differs from the winner, one that
  delivered while a dispute was open included, is slashed through `sysio.chalg::slashop` in the
  accepting transaction. Silence is never slashed
- Validates the per-outpost envelope chain and message sequence, stores the accepted messages and
  attestations, and routes each attestation to its depot handler
- Queues outbound attestations and packs them into one OPP envelope per outpost per epoch, schedule
  attestations first; whatever does not fit under the envelope caps stays queued for the next epoch
- Drives each outpost chain's epoch: `chkcons(chain)` re-tallies an undecided current epoch, whose
  eligible group can shrink with no new delivery, and advances that chain's `sysio.epoch` epoch once the
  outpost has reached consensus and the chain's wall-clock boundary has passed

## Tables

| Table | Type | Description |
|-------|------|-------------|
| `envelopes` | `kv::table` | Inbound deliveries, one row per batch operator per outpost per epoch (working state, pruned per outpost to its current and previous epoch) |
| `messages` | `kv::table` | Individual OPP messages extracted from consensus-verified envelopes |
| `attestations` | `kv::table` | Attestations extracted inbound, and outbound attestations queued (`READY`) for envelope packing. The `byqueue` index orders them by outpost, status and lane |
| `outqueue` | `kv::table` | Per-outpost count of queued general-lane attestations, capped at `MAX_QUEUED_ATTESTATIONS` |
| `outenvelopes` | `kv::table` | Built outbound envelopes awaiting relay, with their canonical epoch digest |
| `outpcons` | `kv::table` | Per-outpost consensus record: accepted envelope digest and inbound message-chain tip |
| `attseq` | `kv::table` | Attestation sequence counter |
| `envlog` | `kv::table` | Metadata-only envelope audit log; each outpost keeps its newest `2 * epoch_retention_envelope_log_count` rows |

## Actions

| Action | Auth | Description |
|--------|------|-------------|
| `bootstrap` | `sysio.msgch` | Network genesis only: asserts the economic clock and `sysio.system` emissions config are set, then runs `sysio.epoch::advance` for every active outpost chain; fails if any has no seated schedule window |
| `bootchain` | `sysio.msgch` | After the network genesis: runs the genesis `sysio.epoch::advance` of one active outpost chain activated later, whose schedule window must be seated |
| `deliver` | batch operator (the chain's resident active group, `ACTIVE` in `sysio.opreg`, and a member of the chain that is not `EVICTED`) | Deliver an inbound envelope for an outpost's current epoch, from its epoch 1; once the epoch has reached consensus, only the accepted envelope's bytes. Calls `evalcons` inline |
| `evalcons` | `sysio.msgch` | Evaluate consensus for a started chain's current epoch; on the first strict majority of the eligible group store and dispatch the attestations and slash every operator whose delivery differs from the winner; with at least two versions delivered and fewer than a majority silent, open a `sysio.chalg` dispute once no version can still reach a majority, or once no eligible delivery has arrived for `DISPUTE_FALLBACK_EPOCHS` (3) of the chain's configured durations (timed from the later of the epoch's start and its newest eligible delivery). No effect for another epoch, or once the epoch has reached consensus or has a dispute |
| `chkcons` | permissionless | For one active outpost chain past its genesis, unless a dispute is open on its current epoch: re-tally an undecided current epoch (`evalcons` inline, at any time); call `sysio.epoch::advance` for a decided one whose boundary has passed |
| `resolvedisp` | `sysio.chalg` | Dispatch the winning envelope of a resolved dispute and slash every operator whose delivery for the epoch differs from it |
| `queueout` | `sysio.epoch`, `sysio.opreg`, `sysio.synd`, or `sysio.msgch` | Queue an outbound attestation for an outpost. Rejects `data` larger than `MAX_ATTESTATION_DATA_BYTES`, or `MAX_GENERAL_ATTESTATION_DATA_BYTES` (7,656) for a general-lane row, which must fit beside a full schedule lane, so every queued row ships. Also rejects a general-lane row once the outpost already holds `MAX_QUEUED_ATTESTATIONS` (512) of them |
| `buildenv` | `sysio.epoch` | Pack the outpost's `READY` attestations into its outbound envelope for the epoch: the schedule lane (`OPERATORS`, `BATCH_OPERATOR_GROUPS`) first, then the general lane in `queueout` order, up to `MAX_ENVELOPE_BYTES` and `MAX_ENVELOPE_ATTESTATIONS` (64). It reads only that outpost's rows |

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

- `OPERATORS` and `BATCH_OPERATOR_GROUPS` from `sysio.epoch::advance(chain)`, to that chain's outpost.
  They form the schedule lane, which ships first and which the queue cap never refuses.
- `DESYNDICATE_LIQ` from `sysio.synd::desyndicate`. A full queue refuses it, and the whole
  `desyndicate` reverts.

## Dependencies

- Reads each chain's epoch state from `sysio.epoch`, and operator status and served chains from
  `sysio.opreg`
- Opens disputes on `sysio.chalg` (`opendispute`); `sysio.chalg::chkdispute` calls back into `resolvedisp`
- Slashes non-canonical deliverers through `sysio.chalg::slashop`, under `sysio.msgch@active`, as it sends
  `opendispute`
- Triggers `sysio.epoch::advance` from `chkcons` (and from `bootstrap` / `bootchain` at a chain's genesis)
- Routes inbound attestations to `sysio.synd` and `sysio.roa`; node-owner linking can indirectly sweep funded DClaim imports

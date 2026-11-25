# sysio.msgch

Inbound/outbound OPP message chain management and consensus tracking contract.

## Responsibility

- Accepts inbound OPP envelopes delivered by the batch operators of the active group, one delivery per
  operator per outpost per epoch (`MAX_ENVELOPE_BYTES` = 32,768; larger deliveries are rejected)
- Evaluates consensus for each outpost's current epoch: the first strict majority of the seats of the
  chain's group on duty (`count * 2 > seats`) decides it, at any time. Only the deliveries of members
  eligible to deliver (`ACTIVE` and not `EVICTED` from the chain) count, while every seat counts toward the
  majority, as on both outposts: a slashed, evicted or inactive member never lowers the number needed. A split of at least two delivered versions opens a
  `sysio.chalg` dispute once no version can still reach a majority with the eligible operators still
  silent. A split that a silent operator can still decide waits for that operator's delivery; while the
  operator stays offline, the epoch waits for governance. An epoch that reached consensus stays decided:
  a later delivery of the accepted bytes is recorded as delivered, and a differing one is refused
- Accepts a late delivery, for an outpost's previous epoch, from a member of the group that served it
  (`sysio.epoch` `previous_group`) while the chain sits in the next epoch, within its late-delivery window
  (`epochcfg.late_window_sec` from that epoch's start): only byte-identical to that epoch's accepted
  envelope (`winner_of`), stored without its bytes, and never re-evaluated. The advance
  that closes the previous epoch's books records it as delivered; a differing one is refused and is that
  member's miss. Any older epoch is refused
- Slashes non-canonical deliverers when an epoch's winner is accepted, by consensus or by dispute
  resolution: every operator whose recorded delivery for the epoch differs from the winner, one that
  delivered while a dispute was open included, is slashed through `sysio.chalg::slashop` in the
  accepting transaction. Silence is never slashed
- Validates the per-outpost envelope chain and message sequence, stores the accepted messages and
  attestations, and routes each attestation to its depot handler
- Queues outbound attestations and packs them into one OPP envelope per outpost per epoch, schedule
  attestations first; whatever does not fit under the envelope caps stays queued for the next epoch
- Records each outpost chain's traffic on `sysio.epoch`: the first general-lane attestation queued to an
  empty queue (notifying `sysio.epoch`, which records it at now), general-lane rows an envelope leaves
  behind (`sysio.epoch::notetraffic` since the oldest of them became ready), and an outbound backlog on
  the outpost that eligible members as many as a strict majority of the group's seats report for its
  decided current epoch
  (`reportqueue`; `notetraffic` since the majority-th earliest reported time, so a minority can neither
  make the chain due nor move its traffic earlier than the other members report), and a dispute's
  resolution (`resolvedisp`; `notetraffic` at the resolution). A chain with no traffic is idle and never
  advances
- Drives each outpost chain's epoch: `chkcons(chain)` re-tallies an undecided current epoch, whose
  eligible members can change with no new delivery, and advances that chain's `sysio.epoch` epoch once the
  outpost has reached consensus and the chain is due (`sysio.epoch` `due_at`: its minimum duration has
  passed since its epoch start, and its traffic wait since its traffic began)

## Tables

| Table | Type | Description |
|-------|------|-------------|
| `envelopes` | `kv::table` | Inbound deliveries, one row per batch operator per outpost per epoch (working state, pruned per outpost to its current and previous epoch; `sysio.epoch::advance` reads the previous epoch's rows, late deliveries included, when it closes that epoch's books) |
| `messages` | `kv::table` | Individual OPP messages extracted from consensus-verified envelopes |
| `attestations` | `kv::table` | Attestations extracted inbound, and outbound attestations queued (`READY`) for envelope packing. The `byqueue` index orders them by outpost, status and lane |
| `outqueue` | `kv::table` | Per-outpost count of queued general-lane attestations, capped at `MAX_QUEUED_ATTESTATIONS` |
| `qreports` | `kv::table` | Outbound backlog reports, one row per (outpost, batch operator): the epoch it reported in, `pending_count` and `oldest_pending_at` (the report's time when zero or later). The tally counts the outpost's rows of its current epoch from the eligible members of its group on duty. Each report erases the outpost's rows of earlier epochs, so an outpost holds its current epoch's rows and, until its next report, its last reported epoch's: one per reporting member of the group on duty, or of a group that a governance `schbatchgps` replaced mid-epoch, which the tally ignores |
| `outenvelopes` | `kv::table` | Built outbound envelopes awaiting relay, with their canonical epoch digest |
| `outpcons` | `kv::table` | Per-outpost consensus record: the latest accepted epoch and its winning checksum, the epoch and winner before it (`previous_epoch_index`, `previous_winning_checksum`, shifted on each acceptance), accepted envelope digest and inbound message-chain tip |
| `attseq` | `kv::table` | Attestation sequence counter |
| `envlog` | `kv::table` | Metadata-only envelope audit log; each outpost keeps its newest `2 * epoch_retention_envelope_log_count` rows |

## Actions

| Action | Auth | Description |
|--------|------|-------------|
| `bootstrap` | `sysio.msgch` | Network genesis only: asserts the economic clock and `sysio.system` emissions config are set, then runs `sysio.epoch::advance` for every active outpost chain; fails if any has no seated schedule window |
| `bootchain` | `sysio.msgch` | After the network genesis: runs the genesis `sysio.epoch::advance` of one active outpost chain activated later, whose schedule window must be seated |
| `deliver` | batch operator (`ACTIVE` in `sysio.opreg` and a member of the chain that is not `EVICTED`; the chain's resident active group, or for a late delivery the group that served its previous epoch) | Deliver an inbound envelope for an outpost's current epoch, from its epoch 1; once the epoch has reached consensus, only the accepted envelope's bytes. Calls `evalcons` inline. A late delivery, for the previous epoch, must come within the chain's late-delivery window (`epochcfg.late_window_sec` from the current epoch's start) and be that epoch's accepted envelope; it is recorded without `evalcons`. Any older epoch is refused |
| `evalcons` | `sysio.msgch` | Evaluate consensus for a started chain's current epoch; on the first strict majority of the group's seats, counting eligible members' deliveries, store and dispatch the attestations and slash every operator whose delivery differs from the winner; with at least two versions delivered, open a `sysio.chalg` dispute once no version can still reach a majority with the eligible operators still silent. No effect for another epoch, or once the epoch has reached consensus or has a dispute |
| `chkcons` | permissionless | For one active outpost chain past its genesis, unless a dispute is open on its current epoch: re-tally an undecided current epoch (`evalcons` inline, at any time); call `sysio.epoch::advance` for a decided one once the chain is due. A decided epoch on an idle chain stays current |
| `reportqueue` | batch operator (a member of the chain's group on duty, `ACTIVE` in `sysio.opreg` and a member of the chain that is not `EVICTED`) | Report the outpost's outbound backlog for the chain's decided current epoch: `pending_count` queued messages, pending since `oldest_pending_ms` (Solana: the start of its current backlog; Ethereum: its oldest queued message; zero, or a time later than now, counts as now). The decided epoch proves the outpost emitted it, so no message that left in that emit counts. Replaces the operator's `qreports` row, erases the outpost's rows of earlier epochs and runs the tally. Once eligible members as many as a strict majority of the group's seats have reported in the current epoch, sends `sysio.epoch::notetraffic` since the majority-th earliest reported time. Refused for a chain that is not an active outpost or has not started, for another epoch, for an undecided epoch (a disputed one included), and with a zero `pending_count`. A paused chain records the traffic but does not advance until it resumes |
| `resolvedisp` | `sysio.chalg` | Dispatch the winning envelope of a resolved dispute and slash every operator whose delivery for the epoch differs from it, then record the chain's traffic at the resolution (`sysio.epoch::notetraffic`), so the chain becomes due and advances to its next group |
| `queueout` | `sysio.epoch`, `sysio.opreg`, `sysio.synd`, or `sysio.msgch` | Queue an outbound attestation for an outpost. Rejects `data` larger than `MAX_ATTESTATION_DATA_BYTES`, or `MAX_GENERAL_ATTESTATION_DATA_BYTES` (7,656) for a general-lane row, which must fit beside a full schedule lane, so every queued row ships. Also rejects a general-lane row once the outpost already holds `MAX_QUEUED_ATTESTATIONS` (512) of them. A general-lane row that finds the outpost's queue empty notifies `sysio.epoch`, which records the chain's traffic at now |
| `buildenv` | `sysio.epoch` | Pack the outpost's `READY` attestations into its outbound envelope for the epoch: the schedule lane (`OPERATORS`, `BATCH_OPERATOR_GROUPS`) first, then the general lane in `queueout` order, up to `MAX_ENVELOPE_BYTES` and `MAX_ENVELOPE_ATTESTATIONS` (64). It reads only that outpost's rows. General-lane rows left behind record the chain's traffic since the oldest of them became ready (`sysio.epoch::notetraffic`) |

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
- Triggers `sysio.epoch::advance` from `chkcons` (and from `bootstrap` / `bootchain` at a chain's genesis),
  and records chain traffic on `sysio.epoch` (`notetraffic` from `buildenv`, `reportqueue` and `resolvedisp`; a `queueout`
  notification)
- Routes inbound attestations to `sysio.synd` and `sysio.roa`; node-owner linking can indirectly sweep funded DClaim imports

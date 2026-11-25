# sysio.chalg

OPP envelope dispute resolution and slash-execution contract.

## Responsibility

- Resolves conflicting OPP outpost envelopes via a Tier-1 node-owner vote when the automatic
  consensus rules in `sysio.msgch` see a split for one (outpost, epoch) in which no delivered version
  can still reach a strict majority of the group's seats with the eligible operators still silent. A
  split that a silent operator can still decide is not disputed: it waits for that operator, or for
  governance.
- Pauses the disputed chain's epoch advancement while its dispute is open and releases it on
  resolution; other chains keep advancing
- Dispatches the winning envelope (via `sysio.msgch::resolvedisp`) once a checksum wins
- Executes slashing of operators through `sysio.opreg` -- the single slashing chokepoint that holds
  `opreg::slash` authority

## Tables

| Table | Type | Description |
|-------|------|-------------|
| `disputes` | `kv::table` | One row per disputed (outpost, epoch): candidate checksums, frozen Tier-1 electorate/quorum, status, winning checksum, opened-at / deadline |
| `disputevote` | `kv::scoped_table` (scope = `dispute_id`) | One Tier-1 node-owner vote per row: owner, chosen checksum |

## Actions

| Action | Auth | Description |
|--------|------|-------------|
| `opendispute` | `sysio.msgch` | Open a dispute, snapshot its Tier-1 electorate/quorum, and pause the disputed chain's epoch |
| `votedispute` | `owner` (Tier-1) | Cast a Tier-1 node-owner vote for the canonical envelope checksum |
| `chkdispute` | permissionless | Tally the votes; on resolution dispatch the winner and unpause the chain's epoch |
| `slashop` | `sysio.chalg` or `sysio.msgch` | Execute a slash on an operator via `sysio.opreg` |

## Dispute-vote flow

1. **Open**: `sysio.msgch::evalcons` calls `opendispute` inline, for an epoch that has not reached
   consensus, on the evaluation at which no delivered version can still reach a strict majority of
   the group's seats with the eligible operators still silent (so at least two versions). The dispute
   records the candidate checksums, snapshots the active ROA generation's Tier-1 electorate
   and fixed quorum, and pauses the disputed chain's `sysio.epoch` advancement; other chains keep
   advancing.
2. **Vote**: owners in the dispute's frozen Tier-1 electorate call `votedispute` with one of the
   candidate checksums. Later ROA registrations cannot join an in-flight dispute. One vote per owner.
   Ballots stay open past the deadline while the tally has no winner; once it has one,
   `votedispute` refuses further ballots, so none can undo it before `chkdispute` records it.
3. **Tally**: anyone cranks `chkdispute`. With `N` equal to the snapshotted electorate size and
   fixed `Q = floor(N/2)+1`, a checksum reaching `Q` votes wins at any time (fast path); after the 24h
   deadline the bar relaxes to a quorum of cast votes (`cast >= Q`) plus a strict majority of cast
   (`2*votes > cast`). No plurality / tie-break -- an undecided tally remains open and keeps the
   chain's epoch paused until Tier-1 supplies a resolvable vote.
4. **Resolve**: the winning checksum is recorded and dispatched via `sysio.msgch::resolvedisp`, whose
   acceptance of the winner slashes, through `slashop`, every operator whose delivery for the epoch
   differs from it, one delivered after the dispute opened included, and records the chain's traffic at
   the resolution, so the chain becomes due and advances to its next group. The disputed chain's epoch is
   then unpaused.

## Dependencies

- Triggered by `sysio.msgch::evalcons`; dispatches the winning envelope via `sysio.msgch::resolvedisp`
- `slashop` is sent by `sysio.msgch` when it accepts an epoch's winner, by consensus or by dispute
  resolution
- Tier-1 electorate snapshot from `sysio.roa::nodeowners`; the active `network_gen` is read through
  the shared canonical ROA accessor
- Slashes via `sysio.opreg::slash`: opreg marks the operator `SLASHED` and removes its whole
  depot-native collateral balance from the operator's row; the slashed tokens stay in
  `sysio.opreg`'s custody
- Pauses / unpauses the disputed chain via `sysio.epoch::pause` / `sysio.epoch::unpause`

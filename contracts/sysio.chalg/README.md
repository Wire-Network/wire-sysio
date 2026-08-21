# sysio.chalg

OPP envelope dispute resolution and slash-execution contract.

## Responsibility

- Resolves conflicting OPP outpost envelopes via a Tier-1 node-owner vote when the automatic
  consensus rules in `sysio.msgch` see two or more versions with no strict majority after the epoch
  boundary for one (outpost, epoch), including when an otherwise eligible operator was silent
- Pauses epoch advancement while a dispute is open and releases it on resolution
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
| `opendispute` | `sysio.msgch` | Open a dispute, snapshot its Tier-1 electorate/quorum, and pause the epoch |
| `votedispute` | `owner` (Tier-1) | Cast a Tier-1 node-owner vote for the canonical envelope checksum |
| `chkdispute` | permissionless | Tally the votes; on resolution dispatch the winner and unpause the epoch |
| `slashop` | `sysio.chalg` or `sysio.epoch` | Execute a slash on an operator via `sysio.opreg` |

## Dispute-vote flow

1. **Open**: `sysio.msgch::evalcons` calls `opendispute` inline for a post-boundary no-majority
   split with at least two versions, regardless of whether every eligible operator delivered. The
   dispute records the candidate checksums, snapshots the active ROA generation's Tier-1 electorate
   and fixed quorum, and pauses `sysio.epoch`.
2. **Vote**: owners in the dispute's frozen Tier-1 electorate call `votedispute` with one of the
   candidate checksums. Later ROA registrations cannot join an in-flight dispute. One vote per owner.
3. **Tally**: anyone cranks `chkdispute`. With `N` equal to the snapshotted electorate size and
   fixed `Q = floor(N/2)+1`, a checksum reaching `Q` votes wins at any time (fast path); after the 24h
   deadline the bar relaxes to a quorum of cast votes (`cast >= Q`) plus a strict majority of cast
   (`2*votes > cast`). No plurality / tie-break -- an undecided tally remains open and keeps the
   epoch paused until Tier-1 supplies a resolvable vote.
4. **Resolve**: the winning checksum is recorded and dispatched via `sysio.msgch::resolvedisp`.
   `sysio.epoch` is unpaused when the final open dispute resolves. The next
   `sysio.epoch::advance` then slashes every operator that delivered a non-canonical checksum for
   the epoch (via `slashop`), deduped across outposts.

## Dependencies

- Triggered by `sysio.msgch::evalcons`; dispatches the winning envelope via `sysio.msgch::resolvedisp`
- Tier-1 electorate snapshot from `sysio.roa::nodeowners`; the active `network_gen` is read through
  the shared canonical ROA accessor
- Slashes via `sysio.opreg::slash` (opreg routes the unlocked bond to the matching LP and defers the
  locked portion through `sysio.uwrit::release`)
- Pauses / unpauses via `sysio.epoch::pause` / `sysio.epoch::unpause`

# sysio.chalg

OPP envelope dispute resolution and slash-execution contract.

## Responsibility

- Resolves conflicting OPP outpost envelopes via a Tier-1 node-owner vote when the automatic
  consensus rules in `sysio.msgch` cannot (a 3+-way split with no majority for one (outpost, epoch))
- Pauses epoch advancement while a dispute is open and releases it on resolution
- Dispatches the winning envelope (via `sysio.msgch::resolvedisp`) once a checksum wins
- Executes slashing of operators through `sysio.opreg` -- the single slashing chokepoint that holds
  `opreg::slash` authority

## Tables

| Table | Type | Description |
|-------|------|-------------|
| `disputes` | `kv::table` | One row per disputed (outpost, epoch): candidate checksums, status, winning checksum, opened-at / deadline |
| `disputevote` | `kv::scoped_table` (scope = `dispute_id`) | One Tier-1 node-owner vote per row: owner, chosen checksum |

## Actions

| Action | Auth | Description |
|--------|------|-------------|
| `opendispute` | `sysio.msgch` | Open a dispute for an (outpost, epoch) carrying 3+ candidate envelope versions; pauses the epoch |
| `votedispute` | `owner` (Tier-1) | Cast a Tier-1 node-owner vote for the canonical envelope checksum |
| `chkdispute` | permissionless | Tally the votes; on resolution dispatch the winner and unpause the epoch |
| `slashop` | `sysio.chalg` or `sysio.epoch` | Execute a slash on an operator via `sysio.opreg` |

## Dispute-vote flow

1. **Open**: `sysio.msgch::evalcons` sees the active batch operators deliver 3+ distinct envelope
   versions for one (outpost, epoch) with no majority, and calls `opendispute` inline. The dispute
   records the candidate checksums and pauses `sysio.epoch`.
2. **Vote**: Tier-1 node owners (looked up in `sysio.roa::nodeowners`, scoped by the active
   `network_gen` from `sysio.roa::roastate`) call `votedispute` with one of the candidate checksums.
   One vote per owner.
3. **Tally**: anyone cranks `chkdispute`. With `N = sysio.system::nodecount.t1_count` and
   `Q = floor(N/2)+1`: a checksum reaching `Q` votes wins at any time (fast path); after the 24h
   deadline the bar relaxes to a quorum of cast votes (`cast >= Q`) plus a strict majority of cast
   (`2*votes > cast`). No plurality / tie-break -- an undecided tally keeps waiting for votes.
4. **Resolve**: the winning checksum is recorded, dispatched via `sysio.msgch::resolvedisp`, and
   `sysio.epoch` is unpaused. The next `sysio.epoch::advance` then slashes every operator that
   delivered a non-canonical checksum for the epoch (via `slashop`), deduped across outposts.

## Dependencies

- Triggered by `sysio.msgch::evalcons`; dispatches the winning envelope via `sysio.msgch::resolvedisp`
- Tier-1 voter eligibility from `sysio.roa::nodeowners` / `roastate`; live Tier-1 count from
  `sysio.system::nodecount`
- Slashes via `sysio.opreg::slash` (opreg routes the unlocked bond to the matching LP and defers the
  locked portion through `sysio.uwrit::release`)
- Pauses / unpauses via `sysio.epoch::pause` / `sysio.epoch::unpause`

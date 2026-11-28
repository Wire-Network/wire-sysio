# Snapshot Attestation -- Design and Approach

## Overview

A node can join the network two ways: replay every block from genesis, or load a recent
snapshot of chain state and sync forward from there. Replaying is slow; loading a snapshot
is fast but raises a trust question -- how does the joining node know the snapshot it loaded
is the real chain state and not a corrupted or tampered copy?

Wire answers this with *on-chain snapshot attestation*. Snapshot provider nodes run for the
top-ranked producers independently generate a snapshot at the same block height, compute a
deterministic hash of it, and record their agreement on-chain. Once a quorum of producers
vote for the same hash at a given block, that hash becomes an attested record in
system-contract state. Any node that auto-fetches a snapshot can then verify its block ID and
hash against the attested record after it has synced past the snapshot's block.

This document covers the design and its moving parts:

1. A deterministic snapshot format, so honest providers always compute the same hash.
2. An on-chain attestation contract in `sysio.system`.
3. A snapshot provider mode in `nodeop` that generates snapshots and submits votes.
4. Snapshot verification on load.

The HTTP distribution layer (serving snapshots to peers and bootstrapping directly from a
provider over HTTP) builds on this foundation and is documented separately with the
`snapshot_api_plugin` (see `plugins/snapshot_api_plugin/README.md`).

## Goals

- Let a new node bootstrap quickly from a snapshot instead of replaying all blocks.
- Establish snapshot authenticity through agreement of a quorum of top-ranked producers,
  recorded on-chain.
- Keep snapshot generation deterministic, so every honest provider produces an identical
  file and identical root hash for the same block.
- Halt an auto-fetching node when its loaded snapshot lacks a usable attestation or its block ID or
  hash contradicts the on-chain record. Keep manual `--snapshot` as an operator-trusted escape hatch.
- Reuse existing transaction and table infrastructure rather than adding a parallel
  signing and vote-accumulation layer.

## Design at a glance

```
  provider A  --\
  provider B  ---+--> votesnaphash(producer, block_id, hash) ------> [ sysio.system ]
  provider C  --/                                                        snapvotes
                                                                            |
                                              quorum reached  ------------> snaprecords (attested)
                                                                            ^
  joining node:  load snapshot -> sync to head -> read snaprecords -> compare hash
                                                                            |
                                                  match: continue   mismatch: fatal, halt
```

Providers and the joining node never talk to each other directly. The chain itself is the
coordination point: providers write their votes as transactions, and the joining node reads
the attested record from state.

## 1. Deterministic snapshot format

Attestation only works if independent providers produce byte-identical snapshots (and
therefore identical hashes) for the same block. Wire's binary snapshot format guarantees
this: section data is written in a fixed canonical order, and a BLAKE3 root hash is computed
inline over the per-section hashes.

File layout (version 1):

```
[Header]   magic "WIRE" (0x57495245), version
[Section data]   raw packed rows, one block per section, canonical order
[Section index]  per section: name, data offset, data size, row count, BLAKE3 hash
[Footer]   section count, BLAKE3 root hash, index offset
```

Key points:

- The root hash is BLAKE3 over the concatenation of the per-section hashes in canonical
  order. Two providers that snapshot the same finalized block compute the same root hash.
- Within the KV sections, rows are in key order: `kv_object` by (code, table_id, key) and
  `kv_index_object` by (code, table_id, sec_key, pri_key), comparing keys as unsigned bytes
  with a shorter prefix first. Row order is therefore part of the hash but does not depend on
  insertion history or on the storage engine.
- The writer streams sequentially through a hashing buffer, so hashing adds essentially no
  overhead over a plain write. The reader memory-maps the file and uses the trailing index
  for direct section lookup.
- An "integrity hash" path computes the same root hash with no file I/O, for cheap periodic
  self-checks.
- BLAKE3 is provided by the LLVM build already linked into the tree; no new dependency.

The format, its design rationale, the rejected alternatives, and benchmark results are
documented in `docs/snapshot-benchmarks.md`.

## 2. On-chain attestation contract (`sysio.system`)

### Why an on-chain voting contract

Attestation is implemented as a small voting contract rather than a BLS aggregate-signature
and P2P vote-gossip layer. Its trust assumption is a quorum of producers that each held one of the
top 30 rank positions when it voted. The contract approach:

- adds no new P2P message types or off-chain vote accumulation,
- needs no BLS key sharing between a producer and its snapshot node,
- lets producers opt in; not every producer must run snapshot infrastructure,
- reuses the existing transaction, authorization, and table machinery end to end.

The actions live in `sysio.system` (not a standalone contract) so they have direct access
to the `producers` table and its rank index. They are organized as a `snapshot_attest`
sub-contract class -- a separate header and source file dispatched via `EXTRA_DISPATCH` --
following the same pattern as `peer_keys`.

Files:
- `contracts/sysio.system/include/sysio.system/snapshot_attest.hpp`
- `contracts/sysio.system/include/sysio.system/snapshot_attest_constants.hpp` -- constants shared with the node
- `contracts/sysio.system/src/snapshot_attest.cpp`

### Actions

| Action | Authority | Description |
|--------|-----------|-------------|
| `votesnaphash(producer, block_id, snapshot_hash)` | `producer`, normally its `snapshot` permission | Submit a hash vote for the block named by `block_id`. |
| `setsnpcfg(min_providers, block_spacing)` | `sysio` | Set quorum K and the positive block-height cadence. |
| `getsnaphash(block_num)` | read-only | Return the attested record for a block, if any. |

### Tables (all scoped to `sysio`)

Storage uses the KV table API (`sysio::kv::table` / `sysio::kv::global`).

| Table | Key | Contents | Secondary index |
|-------|-----|----------|-----------------|
| `snapconfig` | singleton | `{ min_providers, block_spacing }` | -- |
| `snapvotes` | auto-increment `id` | `{ id, block_num, block_id, snapshot_hash, voters[] }` | `byblocknum` |
| `snaprecords` | `block_num` | `{ block_num, block_id, snapshot_hash, attested_at_block }` | -- |

### Who can vote

Any producer holding one of the first `max_snap_provider_rank` (30) positions can vote; there is no
registration. Rank is position in the producer rank index, counting only producers with an active
producer row, an ACTIVE `OPERATOR_TYPE_PRODUCER` row in sysio.opreg whose bond clears the live
`req_prod_collat` (bootstrapped operators exempt), and an active finalizer key. Every vote reads the
rank live, so a producer that parks, loses its operator standing or finalizer key, or falls past
position 30 stops voting at once, and the producer that moves up into the band starts.

The bond is measured during the walk because `sysio.opreg::setconfig` re-evaluates no stored status,
and the rescore sweep it opens reaches the ranking only over later schedule rebuilds. A producer that
a raised minimum leaves short gives up its position at once, to the producer below it.

Votes are keyed by producer identity, so a producer leaving the band retracts nothing already
accepted, and the attestation credit earned in the open pay period stays until the period boundary
rolls it off.

#### The snapshot permission

The provider node votes as the producer. So that the producer's other keys never have to live on
that node, the producer gives it the key of a `snapshot` permission linked to `votesnaphash` and
nothing else:

```bash
clio set account permission myproducer1 snapshot <provider_node_public_key> active -p myproducer1@active
clio set action permission myproducer1 sysio votesnaphash snapshot -p myproducer1@active
```

That key can sign snapshot votes and nothing else: it cannot transfer funds, claim pay, or park the
producer, and it cannot link any other action, which still requires `active`. Rotating it is a native
`updateauth` on the permission and needs no contract call. Votes store producer identities, so a vote
signed by `active` and a retry signed by `snapshot` are the same voter.

Rotation alone does not revoke a compromised key. Native authorization lets the `snapshot` permission
manage itself: it can create permissions under itself and move the `votesnaphash` link onto one of
them, since it satisfies the current link. A delegate created that way keeps voting after the
`snapshot` key changes. To revoke the key fully, with `active`:

```bash
# list the permissions under snapshot and the action each one is linked to
clio get account myproducer1
# move the vote link back onto snapshot, then delete each permission under it, deepest first
clio set action permission myproducer1 sysio votesnaphash snapshot -p myproducer1@active
clio set account permission myproducer1 <delegate> null -p myproducer1@active
# then replace the key
clio set account permission myproducer1 snapshot <new_public_key> active -p myproducer1@active
```

The permission and its link are billed to the producer's own RAM (432 bytes for one fixed-size key).
A collateral-backed producer receives `regproducer_ram` (4,576 bytes) from sysio's pool the first time
`regproducer` creates its producer row, which covers this and the other permissions producers typically
add. Producer rows are never erased, so the grant reaches each account once.

### Voting and quorum

After computing a snapshot, a provider calls `votesnaphash(producer, block_id, snapshot_hash)`.
The contract checks that `producer` holds a top-30 rank position, counted among producers that clear
the live collateral minimum (bootstrapped producers exempt), derives `block_num` from
`block_id`, and accumulates the vote.

`min_providers` is the governance-set fixed K: every tuple finalizes after K distinct producer
votes. Its stored default is zero, which disables voting until governance chooses the launch value.
K does not scale with the number of producers that vote; governance explicitly owns that security
and liveness tradeoff. Configuration changes apply to pending heights, and retrying an existing
vote, from a producer that can still vote, finalizes its tuple if a newly lowered K is
already met.

`block_spacing` is the positive block-height distance between eligible snapshots, defaulting to
25,000. Governance may change it at any time; zero is rejected, with no minimum or maximum beyond
the uint32 type. Small values are intended for testing. Production should use much larger values
to avoid excessive snapshot overhead. Verification grace is `floor(min(block_spacing, 25000) / 2)`
blocks: small test values shorten it, but a large value cannot extend it beyond 12,500 blocks.

The contract applies changes immediately. Each node reads the fixed two-uint32 `snapconfig` KV row
once at startup through the controller's KV store, using the table identifier and eight-byte name key. If
there is no row, it uses 25,000. There is no ABI decode, queued read, timeout or retry for cadence.
Operators must restart provider and serving nodes after a change so they adopt the same interval;
snapshot bootstrap checks the loaded height against its own pre-replay configuration. The post-replay
value is then cached for the running node. Already-attested records remain discoverable after cadence changes.

A producer may retry a pending tuple idempotently while it can still vote (a vote from a top-30
producer on a finalized tuple is a no-op while its height remains eligible under the current cadence;
any other account is rejected), and may vote at multiple scheduled heights, but cannot submit two
different tuples at one height. Producer churn never removes an accepted vote.

Only positive exact multiples of the configured `block_spacing` are accepted. This bounds the
height space to the provider schedule and rejects manual/on-demand snapshots before they can create
pending rows. Scheduling remains based on block number, never elapsed time or block slots.

### Lifecycle and storage

- Votes accumulate monotonically per `(block_num, block_id, snapshot_hash)` at scheduled heights.
  A vote at a later height does not supersede or retract an earlier vote. Because snapshots are
  taken at finalized (irreversible) blocks, honest providers converge on one block ID and hash.
- When a tuple reaches quorum, the contract writes a `snap_record` to `snaprecords` and
  purges pending votes through that block height. Only compact attested records are retained,
  keeping RAM use negligible over time.
- Vote and record rows are billed to `sysio`. Only the last `max_pending_vote_heights` (2)
  scheduled heights at or below the current block take votes, so the vote table stays bounded when
  quorum never forms: an older height is rejected ("snapshot block is older than the pending vote
  window") and the next accepted vote purges its rows. Two periods are at least four times the
  verification grace, so a vote that lands in time for verifiers is never refused.
- A producer votes once per height, so an open height holds at most one row per distinct producer
  that ranked in the top 30 while it was open. Rank churn can push a height past
  `max_snap_providers` rows only by bringing further producers into the top 30, and those rows are
  purged with the rest of the height once it leaves the window.
- Once a newer height is attested, a purged unfinished historical height cannot be reopened.
  The latest retained `snaprecords` row acts as the monotonic finalized-height watermark.
- After a block is attested, any later vote carrying a different block ID or hash for that height
  is rejected with error code `snap_hash_disagreement_error` (9001). This is the on-chain
  signal a provider node uses to detect that its own snapshot diverged from the network.

The attested record is queryable by external clients and explorers via `get_table_rows` on
`snaprecords`, or via the read-only `getsnaphash` action.

## 3. Snapshot provider node

A snapshot provider is a syncing node -- not a producer and not a finalizer -- enabled with:

```ini
snapshot-producer-name = <producer>
```

The node holds only the key of the producer's `snapshot` permission (see "The snapshot permission"
above). When this option is set, `nodeop`:

1. Uses the startup `snapconfig.block_spacing` (25,000 if no row exists) to schedule snapshots at
   exact multiples. Startup removes provider-owned automatic requests with a stale schedule shape,
   reuses an identical request, and leaves manual requests alone. It does not refresh cadence on
   irreversible blocks; restart the node after governance changes the interval.
   Intervals above half the uint32 height range cannot form a recurring request. Startup logs the
   reason and schedules no automatic request for those intervals.
   The contract still accepts every positive uint32 interval.
2. On each finalized snapshot at an exact configured-cadence height, computes the deterministic
   BLAKE3 root hash and automatically builds, signs, and submits a `votesnaphash` transaction.
   Unscheduled manual/on-demand snapshots are retained locally but skipped by the vote callback.
   The transaction is authorized by
   `<producer>@snapshot`; the signing key is resolved through the signature provider manager,
   so only that permission's key needs to be present on this node.
3. If a submitted vote is rejected with error code 9001 (hash disagreement), logs a fatal
   error and shuts the node down -- the node's snapshot disagreed with the attested record
   and must not be trusted.

Role exclusivity is enforced at startup: `snapshot-producer-name` cannot be combined with
`producer-name`.

This auto-vote path is specific to `snapshot-producer-name` mode and to scheduled heights.
The contract independently enforces the same cadence, so a monitoring script cannot attest a
manual snapshot at an arbitrary height.

## 4. Snapshot verification on load

When a node starts with `--snapshot-endpoint`, `chain_plugin` records the loaded snapshot's block
number, block ID, and BLAKE3 root hash. Verification is not a single check at a fixed point: starting
with the first irreversible block past the snapshot height, the node attempts verification on every
finalized block until it reaches a terminal outcome. The `snaprecords` row for a height is created by
the providers' `votesnaphash` transactions, which land on-chain some blocks *after* that height, so
the record can legitimately be absent on early attempts and appear on a later one.

Manual `--snapshot` is an explicit operator-trusted escape hatch. It does not require the
attestation ABI or enabled attestation and never enters the post-sync verification path. Like
other startup modes, it still reads and validates any existing cadence configuration after replay.

For `--snapshot-endpoint`, a specific `/N` request must return metadata for exactly block N, and
the loaded snapshot head must match that metadata. Before replay begins, `snapconfig` is read
directly from the loaded snapshot and its height is checked against that cadence. An ineligible height
is rejected with guidance to delete the loaded chain state. After replay, the resulting configuration
is read once as the startup value for the running node.

Each attempt reads the on-chain `snaprecords` table (through the standard table-read path,
which performs ABI decoding) and resolves as follows:

| Condition | Result |
|-----------|--------|
| Irreversible record found; block ID and hash match the loaded snapshot tuple | Success; logged, verification complete. |
| Irreversible record found; block ID or hash differs from the loaded snapshot tuple | Fatal error; node halts. |
| No irreversible record; node still syncing | Pending; retry on the next finalized block. |
| No irreversible record; caught up; within the grace window | Pending; retry on the next finalized block. |
| Table read fails; syncing or within the grace window | Pending; retry on the next finalized block. |
| No irreversible record/read failure; caught up; grace exhausted | Fatal error; node halts. |
| Unexpected error during verification | Fatal error; node halts. |

"Caught up" means the latest finalized block's timestamp is within 30 seconds of wall-clock
time. The grace window is `floor(min(block_spacing, 25000) / 2)` finalized blocks past the snapshot height and exists
because a node bootstrapping from a fresh snapshot can reach the live tip before the providers'
votes for that height have landed. A missing record only becomes the terminal "never attested"
failure once the node is caught up *and* the finalized head has reached the end of that configured grace.
A record whose `attested_at_block` is above the latest finalized height follows the same policy;
even an unreachable claimed attestation height cannot extend the grace window.

Two implementation details worth knowing as an operator:

- Before replay begins, an auto-fetched snapshot's system ABI must declare compatible `snaprecords`
  and `snapconfig` table identifiers, keys and value schemas. A direct binary KV read before replay
  requires nonzero `min_providers`, positive `block_spacing`, and a loaded height at an exact multiple
  of that spacing. After replay, a second read captures the running node's startup value. No queued read,
  timeout or retry is involved. Any deferred irreversible attestation verification is caught up before
  dependent plugins can start serving. A rejection tells the operator to delete loaded chain state
  before restarting without `--snapshot-endpoint`.
- Verification cannot happen before the snapshot is loaded -- there is no chain state to
  query until then. The node loads the snapshot optimistically and verifies from synced
  on-chain state as it catches up.

The HTTP bootstrap path (`plugins/snapshot_api_plugin/README.md`) is strict because an auto-fetched
snapshot comes from an untrusted peer and must be backed by an attested record. A mismatch or an
unattested auto-fetched snapshot stops the node; recovery is to delete the derived chain state and
acquire a fresh snapshot from a trusted source before restarting.

## Operator deployment model

```
  [ Producer node ]                       [ Snapshot provider node ]
    producer-name = <account>               snapshot-producer-name = <account>
    (no snapshot-producer-name)             (no producer-name)
```

A producer in the top 30 needs no registration: it gives the provider node the key of its
`snapshot` permission. The provider node syncs the chain, generates snapshots on the configured
schedule, votes via `votesnaphash`, and may also serve snapshots over HTTP. The producer's block-signing,
finalizer and account keys stay off the provider node.

A minimal operator flow:

```bash
# a permission whose key the provider node holds, able to sign votes and nothing else
# (its votes count while the producer holds a top-30 rank position)
clio set account permission myproducer1 snapshot <provider_node_public_key> active -p myproducer1@active
clio set action permission myproducer1 sysio votesnaphash snapshot -p myproducer1@active

# initialize the network-wide cadence and fixed K (sysio authority)
clio push action sysio setsnpcfg \
  '{"min_providers":3,"block_spacing":25000}' -p sysio@active

# a vote (auto-submitted in snapshot-provider mode; shown here for reference)
clio push action sysio votesnaphash \
  '{"producer":"myproducer1","block_id":"<block_id>","snapshot_hash":"<root_hash>"}' \
  -p myproducer1@snapshot
```

## Trust and security model

- Trust reduces to: a quorum of producers, each holding a top-30 rank position when it voted,
  honestly computed the snapshot hash. Rank is read on every vote and counts only producers whose
  bond clears the live collateral minimum (bootstrapped producers exempt), so a producer that parks,
  loses its operator standing or finalizer key, falls short of a raised minimum, or falls past
  position 30 stops voting at once. Votes accepted earlier keep counting toward K.
- A compromised snapshot key can vote for its producer, and nothing else. It can also delegate that
  vote to a permission it creates, so revoking it means restoring the vote link and deleting those
  permissions, not only rotating the key (see "The snapshot permission").
- Determinism is what makes a quorum meaningful: if honest providers could compute different
  hashes for the same block, votes would never converge. The fixed snapshot format and
  canonical section ordering remove that ambiguity.
- A divergent snapshot is caught from both directions: a provider whose snapshot disagrees
  with an attested record is rejected on vote (error 9001) and self-halts; a joining node
  whose loaded snapshot disagrees with the attested record halts on verification.
- The attestation only certifies a hash. Confidentiality and availability of the snapshot
  files themselves are the distribution layer's concern (rate limiting, TLS, access control
  at a reverse proxy), described with the `snapshot_api_plugin`.

## Testing

- Contract tests (`contracts/tests/sysio.snapshot_attest_tests.cpp`) cover voting by any top-30
  producer and the live rank band, rejection of non-producers and parked producers, votes signed by
  the linked `snapshot` permission (unlinked, out-of-scope, rotated-key and delegated-key revocation
  cases included), fixed-K configuration, positive mutable cadence, adjacent-height rejection, per-vote
  rank and raised-collateral-minimum rejection, the band moving past a producer a raised minimum
  leaves short before the rescore sweep runs, monotonic votes across churn and heights,
  equivocation/disagreement rejection, finalization purging, and the `getsnaphash` query.
- Unit tests (`unittests/snapshot_attest_tests.cpp`) cover snapshot round-trip hash
  stability, a full chain whose snapshot hash matches its on-chain record, mismatch
  detection, the no-attestation case, and survival of attestation state across a snapshot
  load. The shared real-contract fixture also drives a chain-plugin auto-fetch test through
  download, early schema validation, retained-block replay, post-replay configuration validation,
  and terminal tuple verification. Chain-plugin tests additionally cover direct startup KV reads,
  replayed governance changes, deliberate replay termination, disabled configuration, endpoint-to-file
  block identity, and the trusted manual-snapshot bypass.
- The wall-clock integration test (`tests/snapshot_attest_test.py`) covers a ranked producer
  attesting a local snapshot, a provider node voting a scheduled snapshot with only the producer's
  linked `snapshot` key until it reaches `snaprecords`, startup cadence, provider restart/reuse and manual-request preservation, and proves an
  on-demand snapshot is rejected outside its configured cadence. C++
  fixtures configure a 250-block cadence to cover fixed-K voting, attestation-record persistence,
  snapshot round trips, and strict auto-fetch verification without manufacturing 25,000 blocks.

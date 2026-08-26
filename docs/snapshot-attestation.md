# Snapshot Attestation -- Design and Approach

## Overview

A node can join the network two ways: replay every block from genesis, or load a recent
snapshot of chain state and sync forward from there. Replaying is slow; loading a snapshot
is fast but raises a trust question -- how does the joining node know the snapshot it loaded
is the real chain state and not a corrupted or tampered copy?

Wire answers this with *on-chain snapshot attestation*. A set of registered snapshot
providers independently generate a snapshot at the same block height, compute a
deterministic hash of it, and record their agreement on-chain. Once a quorum of providers
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
- Establish snapshot authenticity through agreement of a quorum of registered providers,
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
  provider B  ---+--> votesnaphash(snap_account, block_id, hash) --> [ sysio.system ]
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
and P2P vote-gossip layer. Its trust assumption is a quorum of durable provider registrations
whose producers satisfied the active/rank eligibility checks when registered. The contract
approach:

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
- `contracts/sysio.system/src/snapshot_attest.cpp`

### Actions

| Action | Authority | Description |
|--------|-----------|-------------|
| `regsnapprov(producer, snap_account)` | `producer` | Create or rotate the producer's snapshot-provider delegation. |
| `votesnaphash(snap_account, block_id, snapshot_hash)` | `snap_account` | Submit a hash vote for the block named by `block_id`. |
| `setsnpcfg(min_providers)` | `sysio` | Set the fixed number K of producer votes required to attest. |
| `getsnaphash(block_num)` | read-only | Return the attested record for a block, if any. |

### Tables (all scoped to `sysio`)

Storage uses the KV table API (`sysio::kv::table` / `sysio::kv::global`).

| Table | Key | Contents | Secondary index |
|-------|-----|----------|-----------------|
| `snapconfig` | singleton | `{ min_providers }` | -- |
| `snapprovs` | `snap_account` | `{ snap_account, producer }` | `byproducer` |
| `snapvotes` | auto-increment `id` | `{ id, block_num, block_id, snapshot_hash, voters[] }` | `byblocknum` |
| `snaprecords` | `block_num` | `{ block_num, block_id, snapshot_hash, attested_at_block }` | -- |

### Registration

A producer calls `regsnapprov` to designate a separate `snap_account` as its snapshot
provider. The producer must be registered (via `regproducer`), active, and ranked at or below
`max_snap_provider_rank` (30) when the mapping is created. This producer-table check is the
registration trust gate; operator-registry status is deliberately not an additional dependency.
Eligibility is not rechecked while voting, so a provider that was valid when registered keeps a
stable delegation through ordinary producer churn.

The registration table is capped at 30. Normal producer lifecycle actions do no attestation work.
Only when a new registration encounters a full table does `regsnapprov` lazily remove mappings whose
producer is missing, inactive, or ranked above 30, print each eviction, then reapply the cap. All
uniqueness checks run before pruning, so a doomed registration cannot mutate unrelated mappings.
Pending votes are never retracted by this cleanup.
Delegating to a separate account decouples authority: the producer's keys never have to live on
the snapshot node -- only the snap_account's key does.

Calling `regsnapprov` again with the same pair is idempotent. Calling it with a new snap_account
atomically replaces that producer's old mapping. Votes store producer identities, so rotating the
signing account neither retracts an accepted vote nor allows the producer to vote twice.

### Voting and quorum

After computing a snapshot, a provider calls `votesnaphash(snap_account, block_id,
snapshot_hash)`. The contract checks that `snap_account` is a registered provider, derives
`block_num` from `block_id`, and accumulates the vote.

`min_providers` is the governance-set fixed K: every tuple finalizes after K distinct producer
votes. Its stored default is zero, which disables voting until governance chooses the launch value.
K does not scale with the number of registered mappings; governance explicitly owns that security
and liveness tradeoff. Configuration changes apply to pending heights, and retrying an existing
vote finalizes its tuple if a newly lowered K is already met.

A producer may retry the same tuple idempotently and may vote at multiple scheduled heights, but
cannot submit two different tuples at one height. Registration and producer-status churn never
removes an accepted vote.

Only exact multiples of 25,000 are accepted. This bounds the height space to the provider schedule
and rejects manual/on-demand snapshots before they can create pending rows.

### Lifecycle and storage

- Votes accumulate monotonically per `(block_num, block_id, snapshot_hash)` at scheduled heights.
  A vote at a later height does not supersede or retract an earlier vote. Because snapshots are
  taken at finalized (irreversible) blocks, honest providers converge on one block ID and hash.
- When a tuple reaches quorum, the contract writes a `snap_record` to `snaprecords` and
  purges pending votes through that block height. Only compact attested records are retained,
  keeping RAM use negligible over time.
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
snapshot-provider-account = <snap_account>
```

When this option is set, `nodeop`:

1. Auto-schedules a recurring snapshot every 25,000 blocks. The interval is fixed (not
   configurable) so that all providers snapshot at identical heights; the schedule is offset
   so snapshots land on exact multiples (blocks 25000, 50000, 75000, ...).
2. On each finalized snapshot at an exact 25,000-block cadence height, computes the deterministic
   BLAKE3 root hash and automatically builds, signs, and submits a `votesnaphash` transaction.
   Unscheduled manual/on-demand snapshots are retained locally but skipped by the vote callback.
   The transaction is authorized by
   `snap_account@active`; the signing key is resolved through the signature provider manager,
   so only the snap_account's key needs to be present on this node.
3. If a submitted vote is rejected with error code 9001 (hash disagreement), logs a fatal
   error and shuts the node down -- the node's snapshot disagreed with the attested record
   and must not be trusted.

Role exclusivity is enforced at startup: `snapshot-provider-account` cannot be combined with
`producer-name`.

This auto-vote path is specific to `snapshot-provider-account` mode and to scheduled heights.
The contract independently enforces the same cadence, so a monitoring script cannot attest a
manual snapshot at an arbitrary height.

## 4. Snapshot verification on load

When a node starts with `--snapshot-endpoint`, `chain_plugin` records the loaded snapshot's block
number, block ID, and BLAKE3 root hash. Verification is not a single check at a fixed point: starting
with the first irreversible block past the snapshot height, the node attempts verification on every
finalized block until it reaches a terminal outcome. The `snaprecords` row for a height is created by
the providers' `votesnaphash` transactions, which land on-chain some blocks *after* that height, so
the record can legitimately be absent on early attempts and appear on a later one.

Manual `--snapshot` is an explicit operator-trusted escape hatch. It loads without requiring the
attestation ABI or configuration and never enters the post-sync attestation verification path.

For `--snapshot-endpoint`, metadata at a non-cadence height is rejected before the snapshot
file is downloaded. A specific `/N` request must return metadata for exactly block N, and the
loaded snapshot head must match that metadata. Only exact 25,000-block cadence heights can ever
receive an attestation, so loading any other remote snapshot could never satisfy strict verification.

Each attempt reads the on-chain `snaprecords` table (through the standard table-read path,
which performs ABI decoding) and resolves as follows:

| Condition | Result |
|-----------|--------|
| Record found; block ID and hash match the loaded snapshot tuple | Success; logged, verification complete. |
| Record found; block ID or hash differs from the loaded snapshot tuple | Fatal error; node halts. |
| No record; node still syncing | Pending; retry on the next finalized block. |
| No record; caught up; within the grace window | Pending; retry on the next finalized block. |
| Table read fails; syncing or within the grace window | Pending; retry on the next finalized block. |
| No record/read failure; caught up; grace exhausted | Fatal error; node halts. |
| Unexpected error during verification | Fatal error; node halts. |

"Caught up" means the latest finalized block's timestamp is within 30 seconds of wall-clock
time. The grace window is 12,500 finalized blocks past the snapshot height -- half the fixed
25,000-block provider snapshot interval -- and exists because a node bootstrapping from a
fresh snapshot can reach the live tip before the providers' votes for that height have
landed. A missing record only becomes the terminal "never attested" failure once the node is
caught up *and* the finalized head is at least 12,500 blocks past the snapshot height.

Two implementation details worth knowing as an operator:

- Before replay begins, an auto-fetched snapshot's system ABI must declare compatible `snaprecords`
  and `snapconfig` physical table identifiers, keys, and value schemas. Its decoded `snapconfig`
  row must also contain a nonzero `min_providers`; bounded reads use a one-second cold-state
  budget per attempt. Otherwise startup fails immediately and tells the operator to delete the
  loaded chain state before restarting without `--snapshot-endpoint`.
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
    producer-name = <account>               snapshot-provider-account = <snap_account>
    (no snapshot-provider-account)          (no producer-name)
```

The producer registers the provider account once with `regsnapprov`. The provider node syncs
the chain, generates snapshots on the fixed schedule, votes via `votesnaphash`, and may also
serve snapshots over HTTP. No keys are shared between the two nodes.

A minimal operator flow:

```bash
# producer delegates a provider account (producer must be ranked <= 30)
clio push action sysio regsnapprov \
  '{"producer":"myproducer1","snap_account":"mysnapprov1"}' -p myproducer1@active

# set the network-wide fixed K (sysio authority)
clio push action sysio setsnpcfg \
  '{"min_providers":3}' -p sysio@active

# a vote (auto-submitted in snapshot-provider mode; shown here for reference)
clio push action sysio votesnaphash \
  '{"snap_account":"mysnapprov1","block_id":"<block_id>","snapshot_hash":"<root_hash>"}' \
  -p mysnapprov1@active
```

## Trust and security model

- Trust reduces to: a quorum of durable provider registrations honestly computed the snapshot
  hash. Each registration's producer must be active and ranked at or below 30 when the mapping
  is created; that eligibility is not continuously revalidated afterward.
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

- Contract tests (`contracts/tests/sysio.snapshot_attest_tests.cpp`) cover registration and
  rotation, side-effect-free uniqueness failures, traceable lazy full-table pruning, fixed-K
  configuration, scheduled-height rejection, monotonic votes across churn and heights,
  equivocation/disagreement rejection, finalization purging, and the `getsnaphash` query.
- Unit tests (`unittests/snapshot_attest_tests.cpp`) cover snapshot round-trip hash
  stability, a full chain whose snapshot hash matches its on-chain record, mismatch
  detection, the no-attestation case, and survival of attestation state across a snapshot
  load. The shared real-contract fixture also drives a chain-plugin auto-fetch test through
  download, early schema/configuration validation, retained-block replay, and terminal tuple
  verification. Chain-plugin tests additionally cover disabled configuration, endpoint-to-file
  block identity, and the trusted manual-snapshot bypass.
- The wall-clock integration test (`tests/snapshot_attest_test.py`) covers provider lifecycle
  actions and proves an on-demand snapshot is rejected outside the production cadence. C++ tests
  construct block 25,000 without hours of real-time production to cover fixed-K voting,
  attestation-record persistence, snapshot round trips, and strict auto-fetch verification.

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
system-contract state. Any node that loads a snapshot can then verify its hash against the
attested record after it has synced past the snapshot's block.

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
- Halt a node when its loaded snapshot hash contradicts the on-chain attestation, while
  staying permissive when no attestation exists for the loaded block.
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
and P2P vote-gossip layer. The trust model is equivalent -- in both cases you trust that a
quorum of the active schedule honestly computed the hash -- but the contract approach:

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
| `regsnapprov(producer, snap_account)` | `producer` | Delegate `snap_account` as the producer's snapshot provider. |
| `delsnapprov(account)` | `account` | Sever the relationship; accepts either the snap_account or the producer. |
| `votesnaphash(snap_account, block_id, snapshot_hash)` | `snap_account` | Submit a hash vote for the block named by `block_id`. |
| `setsnpcfg(min_providers, threshold_pct)` | `sysio` | Set the attestation quorum parameters. |
| `getsnaphash(block_num)` | read-only | Return the attested record for a block, if any. |

### Tables (all scoped to `sysio`)

Storage uses the KV table API (`sysio::kv::table` / `sysio::kv::global`).

| Table | Key | Contents | Secondary index |
|-------|-----|----------|-----------------|
| `snapconfig` | singleton | `{ min_providers, threshold_pct }` | -- |
| `snapprovs` | `snap_account` | `{ snap_account, producer }` | `byproducer` |
| `snapvotes` | auto-increment `id` | `{ id, block_num, block_id, snapshot_hash, voters[] }` | `byblocknum` |
| `snaprecords` | `block_num` | `{ block_num, block_id, snapshot_hash, attested_at_block }` | -- |

### Registration

A producer calls `regsnapprov` to designate a separate `snap_account` as its snapshot
provider. The producer must be registered (via `regproducer`) and ranked at or below
`max_snap_provider_rank` (30). Delegating to a separate account decouples authority: the
producer's keys never have to live on the snapshot node -- only the snap_account's key does.

Either party can call `delsnapprov` to end the relationship. The action looks the mapping up
in both directions, so it works whether invoked by the snap_account (deregistering itself) or
by the producer (removing its delegate).

### Voting and quorum

After computing a snapshot, a provider calls `votesnaphash(snap_account, block_id,
snapshot_hash)`. The contract checks that `snap_account` is a registered provider, derives
`block_num` from `block_id`, and accumulates the vote.

A hash is attested when the number of distinct providers voting for the same
`(block_num, block_id, snapshot_hash)` tuple reaches:

```
quorum = max(min_providers, ceil(registered_providers * threshold_pct / 100))
```

- `min_providers` is a hard floor (default 1; raised for mainnet) set by `setsnpcfg`.
- `threshold_pct` is the share of registered providers required (default 67, i.e.
  two-thirds).

The floor guarantees a minimum number of attestors even on a small network; the percentage
makes the quorum scale with participation.

### Lifecycle and storage

- Votes accumulate per `(block_num, block_id, snapshot_hash)`. Because snapshots are taken
  at finalized (irreversible) blocks, the `block_id` for a given `block_num` is the same
  across all honest providers, so honest votes converge on one tuple.
- When a tuple reaches quorum, the contract writes a `snap_record` to `snaprecords` and
  purges the pending votes for that block. Only the compact attested record is retained,
  keeping RAM use negligible over time.
- After a block is attested, any later vote carrying a different hash for that block is
  rejected with error code `snap_hash_disagreement_error` (9001). This is the on-chain
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
2. On each finalized snapshot, computes the deterministic BLAKE3 root hash and automatically
   builds, signs, and submits a `votesnaphash` transaction. The transaction is authorized by
   `snap_account@active`; the signing key is resolved through the signature provider manager,
   so only the snap_account's key needs to be present on this node.
3. If a submitted vote is rejected with error code 9001 (hash disagreement), logs a fatal
   error and shuts the node down -- the node's snapshot disagreed with the attested record
   and must not be trusted.

Role exclusivity is enforced at startup: `snapshot-provider-account` cannot be combined with
`producer-name`.

This auto-vote path is specific to `snapshot-provider-account` mode. Snapshots created
through the producer API (`create_snapshot` / `schedule_snapshot`) are not auto-voted; a
provider using that path submits `votesnaphash` itself (for example from a monitoring
script).

## 4. Snapshot verification on load

When a node starts with `--snapshot`, `chain_plugin` records the loaded snapshot's block
number and BLAKE3 root hash. Verification is not a single check at a fixed point: starting
with the first irreversible block past the snapshot height, the node attempts verification
on every finalized block until it reaches a terminal outcome. The `snaprecords` row for a
height is created by the providers' `votesnaphash` transactions, which land on-chain some
blocks *after* that height, so the record can legitimately be absent on early attempts and
appear on a later one.

Each attempt reads the on-chain `snaprecords` table (through the standard table-read path,
which performs ABI decoding) and resolves as follows:

| Condition | Result |
|-----------|--------|
| Record found; hash matches the loaded snapshot hash | Success; logged, verification complete. |
| Record found; hash differs from the loaded snapshot hash | Fatal error; node halts. |
| No record; node still syncing | Pending; retry on the next finalized block. |
| No record; caught up; system contract ABI has no `snaprecords` table | Warning (chain does not support attestation); verification skipped. |
| No record; caught up; within the grace window | Pending; retry on the next finalized block. |
| No record; caught up; grace window exhausted | Warning (height was never attested); verification skipped. |
| Unexpected error during verification | Fatal error; node halts. |

"Caught up" means the latest finalized block's timestamp is within 30 seconds of wall-clock
time. The grace window is 12,500 finalized blocks past the snapshot height -- half the fixed
25,000-block provider snapshot interval -- and exists because a node bootstrapping from a
fresh snapshot can reach the live tip before the providers' votes for that height have
landed. A missing record only becomes the terminal "never attested" warning once the node is
caught up *and* the finalized head is at least 12,500 blocks past the snapshot height.

Two implementation details worth knowing as an operator:

- The system contract's ABI is re-checked on every attempt, so a system-contract upgrade
  that adds attestation support while the node is still syncing is picked up and verification
  proceeds normally.
- Verification cannot happen before the snapshot is loaded -- there is no chain state to
  query until then. The node loads the snapshot optimistically and verifies from synced
  on-chain state as it catches up.

The permissive terminal outcomes (warn and continue when no table or no record exists) are
deliberate for manual `--snapshot` use: operators can load their own snapshots, and chains
whose system contract predates attestation still work. A hash *mismatch* is never permissive:
the node stops, and the recommended recovery is to delete the chain state derived from the
untrusted snapshot and acquire a fresh snapshot from a trusted source before restarting.
The HTTP bootstrap path (`plugins/snapshot_api_plugin/README.md`) applies strict enforcement
instead, because an auto-fetched snapshot comes from an untrusted peer and must be backed by
an attested record.

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

# set the network-wide quorum (sysio authority)
clio push action sysio setsnpcfg \
  '{"min_providers":3,"threshold_pct":67}' -p sysio@active

# a vote (auto-submitted in snapshot-provider mode; shown here for reference)
clio push action sysio votesnaphash \
  '{"snap_account":"mysnapprov1","block_id":"<block_id>","snapshot_hash":"<root_hash>"}' \
  -p mysnapprov1@active
```

## Trust and security model

- Trust reduces to: a quorum of registered providers (drawn from the ranked producer set)
  honestly computed the snapshot hash. This is the same assumption as trusting the active
  schedule.
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
  deregistration (including wrong-authority and rank-too-high rejection), config validation
  and authority, vote accumulation, quorum by floor and by percentage, post-attestation
  disagreement rejection, vote purging, and the `getsnaphash` query.
- Unit tests (`unittests/snapshot_attest_tests.cpp`) cover snapshot round-trip hash
  stability, a full chain whose snapshot hash matches its on-chain record, mismatch
  detection, the no-attestation case, and survival of attestation state across a snapshot
  load.
- An integration test (`tests/snapshot_attest_test.py`) drives the full flow: create a
  snapshot, vote to quorum, verify the `snaprecords` entry, restart a node from the attested
  snapshot, sync from peers, and confirm the attestation on the synced node.

# Snapshot Serving Implementation Plan

## Overview

This plan covers the implementation of a snapshot distribution system with on-chain attestation. Snapshots are generated deterministically by node operators, served via HTTP, and verified against on-chain records after a bootstrapping node syncs to head.

---

## Goals

- Allow new nodes to bootstrap quickly from a verified snapshot rather than replaying all blocks
- Ensure snapshot authenticity via on-chain attestation from a quorum of registered snapshot providers
- Deterministic snapshot generation so all providers produce identical root hashes for the same block
- Provide a simple HTTP endpoint for snapshot + peer list discovery
- Halt a node if its loaded snapshot hash does not match the on-chain attestation

---

## Phase 1 — Deterministic Snapshot Format (COMPLETE)

**Status:** Implemented in `feature/snapshot-v2`. See `docs/snapshot-benchmarks.md` for full design evolution and benchmark results.

### What Was Built

A new binary snapshot format (`v1`) with **sequential writes** and **buffered inline BLAKE3 hashing**. Every snapshot node that writes a snapshot at the same block produces an identical file with an identical root hash.

**File format:**

```
[Header]  (8 bytes)
  magic:        uint32_t  (0x57495245 "WIRE")
  version:      uint32_t  (1)

[Section Data]
  section 0 raw packed rows
  section 1 raw packed rows
  ...

[Section Index]  (sorted by section name)
  name:         null-terminated string
  data_offset:  uint64_t
  data_size:    uint64_t
  row_count:    uint64_t
  hash:         char[32]  (BLAKE3 per-section hash)

[Footer]  (44 bytes)
  num_sections: uint32_t
  root_hash:    char[32]  (BLAKE3 of concatenated section hashes in canonical order)
  index_offset: uint64_t
```

**Key implementation details:**

- **Writer (`threaded_snapshot_writer`):** Sequential single-threaded writes through a custom `hashing_streambuf` (1 MB buffer) that computes per-section BLAKE3 hashes inline. Section index written at end of file, enabling single-pass writes. Root hash computed by hashing concatenated per-section hashes in sorted order.
- **Reader (`threaded_snapshot_reader`):** Memory-mapped file. Reads footer to locate section index, then uses indexed lookup for O(num_sections) section access. Supports concurrent reads from multiple threads via thread-local datastream state. `madvise(MADV_DONTNEED)` releases pages after reading each section.
- **Integrity hash writer (`integrity_hash_snapshot_writer`):** Computes the same root hash as a full snapshot write but with no file I/O. Used for periodic verification.
- **BLAKE3 via LLVM:** Uses `llvm-c/blake3.h` bundled with LLVM (already linked). No new dependency. Internal 64 KB buffer in `blake3_encoder` coalesces small writes.
- **`load_index()` / `validate()` separation:** Lightweight `load_index()` parses header, footer, and section index without hash verification — used for quick metadata access (e.g. chain_id extraction). Full `validate()` re-hashes all sections from the memory map.

**Design decisions (explored and rejected alternatives):**

| Approach | Why Rejected |
|----------|-------------|
| Parallel writes with temp files | Extra I/O pass to assemble final file negates parallelism gains |
| Sharded contract tables | Chainbase inserts must be sequential per index type; no read benefit |
| Post-write parallel SHA-256 hash | SHA-256 too slow; ~20s overhead vs near-zero for inline BLAKE3 |
| Unbuffered inline BLAKE3 | Per-call overhead on tiny `fc::raw::pack` writes (1-8 bytes) |

**Performance (33 GB realistic EOS mainnet distribution, release build):**

| | Integrity Hash | Write | Read |
|---|---|---|---|
| **Spring (no hash)** | — | 70.0s (476 MB/s) | 254.4s (131 MB/s) |
| **Wire (inline BLAKE3)** | 34.1s | 63.3s (527 MB/s) | 226.4s (148 MB/s) |

Wire writes are 10% faster than Spring's no-hash baseline (buffer consolidation compensates for BLAKE3 cost). Reads are 11% faster (indexed section lookup eliminates linear scan).

---

## Phase 2 — On-Chain Snapshot Attestation (NEXT)

### Approach: On-Chain Voting Contract

Rather than building a parallel BLS signing and P2P voting layer, snapshot attestation uses a simple on-chain voting contract. Producers register delegate accounts ("snapshot providers") that compute snapshot hashes and vote on-chain. Once a quorum of registered providers agree on a hash for a given block, the record is marked as attested.

This approach was chosen over BLS aggregate signatures because:
- No new P2P message types or vote accumulation logic needed
- No requirement for BLS key sharing between producer and snapshot nodes
- Producers opt in — not all need to run snapshot infrastructure
- Uses existing transaction/action infrastructure entirely
- The trust model is equivalent: you're trusting that a quorum of the active schedule honestly computed the hash

### System Contract: `sysio.snapshot`

**Actions:**

```
regsnapprov(producer, snap_account)       // producer auth — register a snapshot provider
unregsnapprov(account)                    // account auth — works as snap_account or producer
votesnaphash(snap_account, block_id, snapshot_hash)  // snap_account auth
setconfig(min_providers, threshold_pct)   // sysio auth
getsnaphash(block_num)                    // read-only — returns attested record for a block number
```

**Registration:**
- A producer calls `regsnapprov` to designate a `snap_account` as their snapshot provider. This decouples authority — the producer's keys never need to be on the snapshot node.
- Either party can sever the relationship: the `snap_account` can unregister itself, or the producer can remove its delegate. `unregsnapprov` accepts either account and looks up the mapping in both directions.

**Voting:**
- After computing a snapshot at a scheduled interval, the snapshot provider calls `votesnaphash(snap_account, block_id, snapshot_hash)`.
- The contract verifies `snap_account` is a registered delegate of an active producer.
- Votes are accumulated per `block_id`.

**Attestation threshold:**
- A snapshot hash is marked "attested" when: `votes >= max(min_providers, ceil(registered_count * 2/3) + 1)`
- `min_providers` is a configurable floor (set via `setconfig` with `sysio` authority). Default: **1** for testnets, raised for mainnet.
- `threshold_pct` is the quorum percentage of registered providers (default: 67%).
- This ensures attestation scales with participation but never falls below the configured floor.

**Storage:**
- Votes accumulate per `(block_num, block_id, snapshot_hash)` tuple. The `block_num` is extracted from the `block_id`. Since snapshots are only produced at LIB (finalized) blocks, the `block_id` for a given `block_num` will always agree across honest providers. Attestation occurs when one tuple reaches quorum.
- Attested records: `snapshot_record { block_num, block_id, snapshot_hash, attested_at_block }`
- Once a `block_num` has an attested record, further votes for that `block_num` are no longer needed.
- **Disagreement detection:** A provider won't know their vote is bad until a quorum forms without them. On the provider's *next* `votesnaphash` call (for a later block), the contract checks whether any previously attested record has a `block_num` where this provider voted differently. If so, the action returns an error indicating the mismatch. The snapshot provider node must treat this as a fatal condition and shut down — its state is inconsistent with network consensus.
- **Purging:** Once a `block_num` is attested, vote records for previous (older) `block_num`s can be purged to free RAM. Only the attested `snapshot_record` needs to be retained. At ~200 bytes per record (72 bytes data + multi-index overhead) and ~2,523 snapshots per year (every 25,000 blocks at 0.5s block time), attested records consume ~500 KB/year — negligible. All attested records should be kept so any node can verify any historical snapshot.
- Queryable via `get_table_rows` on the `snaprecords` table, or via the `getsnaphash` read-only action for use by block explorers and external clients via read-only transactions.

### Snapshot Provider Node

A snapshot provider is a syncing node (not a producer, not a finalizer) that:
1. Generates snapshots at the configured schedule interval (every 25,000 blocks)
2. Computes the deterministic root hash
3. Submits `votesnaphash` transactions to the chain
4. Optionally serves snapshots over HTTP (Phase 3)

**Config:**

```ini
snapshot-provider = true    # enable snapshot generation + hash voting
```

Role exclusivity is enforced at startup: `snapshot-provider` cannot be enabled alongside `producer-name` or active finalizer configuration.

### Exit Criteria

- Contract deployed on devnet
- Multiple snapshot providers register, compute hashes, vote
- Attestation record created when quorum reached
- Bootstrapping node can query attested hash after syncing

---

## Phase 3 — HTTP Snapshot Server

**Files:** New `plugins/snapshot_plugin/`

**Tasks:**

- Create `snapshot_plugin` registering HTTP routes when `snapshot-provider = true`
- Endpoints:

```
GET /v1/snapshot/latest
    Response: { block_num, block_id, snapshot_root_hash, download_url }

GET /v1/snapshot/by_block/{block_num}
    Response: same structure for a specific block

GET /v1/snapshot/download/{block_num}
    Response: binary stream of snapshot file

GET /v1/snapshot/peers
    Response: { peers: ["host:port", ...] }
```

- Support `Range` header for resumable downloads
- Rate-limit the download endpoint

---

## Phase 4 — Bootstrap Node Verification & Integration

**Files:** `plugins/chain_plugin/`, `programs/nodeop/main.cpp`

**Tasks:**

- Add bootstrap config options:

```ini
snapshot-endpoint = https://snapshots.example.com   # HTTP snapshot provider base URL
snapshot-block-num =                                 # optional: request a specific block
```

- Bootstrap flow when `snapshot-endpoint` is set and no existing chain data is present:
    1. Fetch metadata from `/v1/snapshot/latest` (or `by_block` if specified)
    2. Download snapshot; compute `root_hash` locally and verify it matches the advertised hash — abort if mismatch
    3. Load the snapshot and begin p2p sync
    4. Once synced to head, query on-chain `snaprecords` table for the snapshot's `block_id`
    5. Compare locally computed hash against the on-chain attested hash
    6. **If mismatch:** log a fatal error and immediately shut down:

```
FATAL: Snapshot hash mismatch detected.
On-chain attested: <on_chain_hash>
Locally computed:  <computed_hash>
This node may be loading untrusted state. Shutting down immediately.
```

**Note:** Verification happens *after* sync, not before. The node loads the snapshot optimistically, syncs to head, then checks the on-chain record. This avoids a chicken-and-egg problem (can't query on-chain state before having chain state).

---

## Operator Deployment Model

```
[Producer Node]                    [Snapshot Provider Node]
  producer-name = <account>          snapshot-provider = true
  (no snapshot-provider)             (no producer-name)
```

The producer registers the snapshot provider's account via `regsnapprov`. The snapshot provider node syncs the chain, generates snapshots at scheduled intervals, votes on hashes, and optionally serves snapshots over HTTP. No shared keys between the two nodes.

---

## Testing Requirements

- **Unit:** Determinism test (two snapshots at same block produce identical root_hash), section ordering stability, startup role conflict detection
- **Contract:** Registration/unregistration, vote accumulation, threshold logic, duplicate rejection, quorum edge cases (min_providers vs percentage)
- **Integration:** Full cycle on local devnet — generate snapshot, vote hash, reach attestation, serve via HTTP, bootstrap new node, verify after sync
- **Adversarial:**
    - Tampered snapshot → computed hash doesn't match advertised hash → abort before loading
    - On-chain hash mismatch after sync → node halts with fatal error
    - Unregistered account votes → rejected
    - Vote after attestation with different hash → rejected
- **Performance:** Snapshot generation benchmarks (see Phase 1 results)

---

## Open Questions

**Resolved:**
- **Hash algorithm:** BLAKE3 — faster than SHA-256, uses LLVM's bundled implementation
- **Snapshot format:** Single-file binary with section index at end (not tar.zst — simpler, deterministic, already 10% faster than Spring)
- **Signing mechanism:** On-chain voting contract, not BLS aggregate signatures (simpler, equivalent trust model, no P2P changes needed)
- **Snapshot interval:** Every 25,000 blocks using the existing snapshot schedule mechanism
- **Attestation threshold:** `max(min_providers, ceil(registered_count * 2/3) + 1)` with configurable floor

**Still open:**
1. **Compression for distribution:** Add gzip or zstd compression layer for HTTP serving? Current format is uncompressed binary.
2. **Snapshot retention:** How many snapshots should a provider node retain? Prune policy?

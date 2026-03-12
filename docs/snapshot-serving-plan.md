# Snapshot Serving Implementation Plan

## Overview

This plan covers the implementation of a trusted, BLS-signed snapshot distribution system. Snapshots are generated deterministically by designated node operators ("snapshot nodeops"), signed by the active finalizer policy, and served via HTTP for bootstrapping new nodes. New nodes verify snapshot integrity against on-chain attestations before syncing.

---

## Goals

- Allow new nodes to bootstrap quickly from a verified snapshot rather than replaying all blocks
- Ensure snapshot authenticity via BLS aggregate signatures from the active finalizer policy
- Support deterministic, multi-threaded snapshot generation so all snapshot nodeops produce identical hashes
- Provide a simple HTTP endpoint for snapshot + peer list discovery
- Immediately halt a node if its loaded snapshot hash does not match the on-chain attestation

---

## Scope & Decisions

### Who Generates Snapshots?

- Introduce a dedicated **snapshot nodeop** role (configured via `config.ini` flag, e.g. `snapshot-provider = true`)
- Snapshot and integrity hash generation is triggered by the **existing snapshot schedule mechanism**, fired every **25,000 blocks** — no new interval config is needed; the snapshot plugin hooks into this existing callback
- Because snapshot signing uses the same BLS finalizer key as block finality, a node operator who is a finalizer must run **two separate nodeop processes** sharing the same finalizer key:
    - A **proposer/finalizer nodeop** — participates in block production and finality voting as normal
    - A **snapshot nodeop** — generates and signs snapshot hashes; serves snapshots over HTTP
- These roles are **mutually exclusive on a single node** and nodeop enforces this at startup:
    - A node configured with `snapshot-provider = true` (or `snapshot-hash-only = true`) **must not** be configured as a block producer or active finalizer; nodeop aborts startup if both are set
    - A node configured as a block producer or active finalizer **must not** have `snapshot-provider` or `snapshot-hash-only` enabled; nodeop aborts startup if both are set
- Non-finalizer node operators are not required to run a snapshot nodeop

### Snapshot Format

Since the system is pre-launch, **the snapshot format can be changed**:

- Replace the current single-file format with a **multi-section container format**
- Each logical section (accounts, contract tables, contract code, global state, producer schedule, finalizer policy) is written to a separate sub-file in parallel
- Sub-files are bundled into a **tar + Zstandard (`.tar.zst`) container** for distribution
- The container manifest records each section hash and the root hash derived from them in canonical order
- **Determinism requirement:** section ordering, serialization, and compression parameters must be fully deterministic across all nodeops (fixed Zstandard compression level, no wall-clock timestamps in tar headers, sorted map/set iteration)

### Integrity Hash

- Root hash is computed over the canonically serialized section hashes in manifest order (not over compressed bytes)
- Hash input includes: block number, block ID, snapshot format version, block timestamp (not wall clock)
- **Hash algorithm:** BLAKE3 preferred for speed; SHA-256 as fallback if ecosystem compatibility required

---

## Component Breakdown

### 1. Multi-threaded Snapshot & Hash Generation

**Files:** `libraries/chain/snapshot.cpp`, new `libraries/chain/snapshot_writer.cpp`

**Tasks:**

- Refactor snapshot serialization to split state into independent sections writable concurrently
- Use a configurable thread pool; join all threads before finalizing the container
- Implement per-section streaming hash; combine into root hash deterministically via manifest
- Ensure all iterated data structures are traversed in stable sorted order
- Add `snapshot_manifest` struct:

```cpp
struct snapshot_manifest {
    block_num_type             block_num;
    block_id_type              block_id;
    uint32_t                   format_version;
    block_timestamp_type       block_timestamp;
    vector<section_hash_entry> sections;   // { name, hash } in canonical order
    digest_type                root_hash;  // hash of canonically serialized section hashes
};
```

- Write unit tests confirming two independently generated snapshots at the same block produce identical `root_hash`

**New config options:**

```ini
snapshot-threads = 4       # worker threads for parallel section generation
snapshot-dir = snapshots/  # output directory for snapshot files
snapshot-provider = false  # set true to enable HTTP snapshot serving and hash voting
snapshot-hash-only = false # vote on snapshot hash only, without storing the snapshot file
```

Snapshot/hash generation interval is controlled by the **existing snapshot schedule**, configured at **every 25,000 blocks**. The snapshot plugin hooks into the existing schedule callback; no new interval option is introduced.

**Role exclusivity enforcement (startup validation):**

Add a startup check in `nodeop` / `chain_plugin` that aborts with a clear error if incompatible roles are configured together:

```
ERROR: Conflicting configuration detected.
'snapshot-provider' (or 'snapshot-hash-only') cannot be enabled on the same node as
block production ('producer-name') or active finalizer participation.
Run a separate nodeop instance with the same finalizer key for snapshot duties.
```

Conversely, if `snapshot-provider` or `snapshot-hash-only` is absent but a finalizer key is configured, no error is raised — the node operates normally as a proposer/finalizer. The two roles share the same BLS finalizer key but must run as separate processes.

---

### 2. Snapshot Hash Voting

**Files:** `plugins/net_plugin/`, alongside existing finalizer vote logic in `libraries/chain/hotstuff/`

**Tasks:**

- Define new P2P message type `snapshot_hash_message`:

```cpp
struct snapshot_hash_message {
    block_num_type   block_num;
    block_id_type    block_id;
    digest_type      snapshot_root_hash;
    bls_signature    finalizer_sig;       // sig over (block_num || block_id || root_hash)
    bls_public_key   finalizer_key;
};
```

- After computing a root hash, broadcast `snapshot_hash_message` to peers
- Receiving nodes: verify the finalizer key is in the active finalizer policy, verify the BLS signature, accumulate toward quorum weight **per the active finalizer policy for that block** (same weight threshold as block finality)
- Once quorum is reached, produce a `snapshot_certificate`:

```cpp
struct snapshot_certificate {
    block_num_type          block_num;
    block_id_type           block_id;
    digest_type             snapshot_root_hash;
    bls_aggregate_sig       aggregate_sig;
    vector<bls_public_key>  signers;
};
```

- Store certificate as a sidecar `.cert` file alongside the snapshot container

---

### 3. On-Chain Snapshot Attestation

**Files:** New `contracts/sysio.snapshot/` system contract

**Tasks:**

- Define action `attestsnap` callable by snapshot nodeops once a certificate is produced:

```
action attestsnap(block_num, block_id, snapshot_root_hash, snapshot_certificate)
```

- Contract verifies BLS aggregate signature against the active finalizer policy **for the attested block**; rejects if the quorum weight threshold defined by that policy is not met
- Stores immutable record: `snapshot_record { block_num, block_id, snapshot_root_hash, certified_at_block }`; first writer wins, duplicates rejected
- Expose read-only chain API endpoint `get_snapshot_record(block_num)`

---

### 4. HTTP Snapshot Server

**Files:** New `plugins/snapshot_plugin/`

**Tasks:**

- Create `snapshot_plugin` registering HTTP routes when `snapshot-provider = true`
- Endpoints:

```
GET /v1/snapshot/latest
    Response: { block_num, block_id, snapshot_root_hash, certificate, download_url }

GET /v1/snapshot/by_block/{block_num}
    Response: same structure for a specific block

GET /v1/snapshot/download/{block_num}
    Response: binary stream of .tar.zst snapshot container

GET /v1/snapshot/peers
    Response: { peers: ["host:port", ...] }
```

- Include full `snapshot_certificate` in metadata responses so clients can verify before downloading
- Support `Range` header for resumable downloads
- Rate-limit the download endpoint

---

### 5. Bootstrapping Node: Snapshot Load & Verification

**Files:** `plugins/chain_plugin/`, `programs/nodeop/main.cpp`

**Tasks:**

- Add bootstrap config options:

```ini
snapshot-endpoint = https://snapshots.example.com   # HTTP snapshot provider base URL
snapshot-block-num =                                 # optional: request a specific block
```

- Bootstrap flow when `snapshot-endpoint` is set and no existing chain data is present:
    1. Fetch metadata from `/v1/snapshot/latest` (or `by_block` if specified)
    2. Verify the `snapshot_certificate` BLS aggregate signature against the known finalizer policy — abort if invalid
    3. Download snapshot container; compute `root_hash` and verify it matches the certified hash — abort if mismatch
    4. Query on-chain `get_snapshot_record(block_num)` and compare against both certificate and computed hash
    5. **If any comparison fails:** log a fatal error and immediately shut down:

```
FATAL: Snapshot hash mismatch detected.
On-chain attested: <on_chain_hash>
Locally computed:  <computed_hash>
This node may be loading untrusted state. Shutting down immediately.
```

6. Load the verified snapshot and begin p2p sync using peers from `/v1/snapshot/peers`

---

## Implementation Phases

### Phase 1 — Deterministic Multi-threaded Snapshot Format
- Refactor snapshot writer into parallel independent sections
- Implement `snapshot_manifest` and root hash derivation
- **Exit criteria:** Determinism test passes; measurable generation speedup with 4 threads vs single thread

### Phase 2 — Hash Voting & Certificate Production
- Implement `snapshot_hash_message` P2P message and signature accumulation
- Reuse/extend existing BLS finalizer vote infrastructure
- **Exit criteria:** Certificate produced on devnet with >= 2/3 weight finalizers participating

### Phase 3 — On-Chain Attestation
- Deploy `sysio.snapshot` contract with `attestsnap` action and BLS verification
- **Exit criteria:** Attested hash queryable on-chain after certificate is produced

### Phase 4 — HTTP Snapshot Server
- Implement `snapshot_plugin` with all endpoints and resumable download support
- **Exit criteria:** Full snapshot downloadable; certificate passes standalone verification via curl

### Phase 5 — Bootstrap Node Verification & Integration
- Implement bootstrap flow in `chain_plugin` with hard shutdown on mismatch
- **Exit criteria:** Fresh node bootstraps from snapshot, verifies all hashes, joins devnet; tampered snapshot triggers clean fatal shutdown

---

## Operator Deployment Model

A finalizer node operator runs two nodeop processes on separate machines (or VMs), both configured with the same BLS finalizer key:

```
[Proposer/Finalizer Node]          [Snapshot Node]
  producer-name = <account>          snapshot-provider = true
  finalizer-key = <bls-pub>          finalizer-key = <same-bls-pub>
  (no snapshot-provider)             (no producer-name)
```

The snapshot node's finalizer key allows it to sign snapshot hashes that are recognized by the rest of the network as coming from a legitimate finalizer. It never participates in block production or block finality voting — its only use of the key is to sign `snapshot_hash_message` payloads.

---

## Testing Requirements

- **Unit:** Determinism test, BLS signature verify/aggregate, manifest hash computation, section ordering stability, startup role conflict detection (snapshot + producer config aborts, snapshot + finalizer config aborts)
- **Integration:** Full cycle on local devnet — generate → vote → certify → attest on-chain → serve → bootstrap
- **Adversarial:**
    - Tampered snapshot container → node halts with fatal error
    - Invalid BLS signature on certificate → rejected at download
    - On-chain hash mismatch after valid cert → node halts
    - Wrong block num → rejected
- **Performance:** Single-threaded vs. 4-thread generation time; target >= 2x speedup

---

## Open Questions

**Resolved:**
- **Snapshot interval:** Every 25,000 blocks using the existing snapshot schedule mechanism
- **Minimum signers / quorum threshold:** Determined by the active finalizer policy for the block being attested — same policy and weight rules as block finality

**Still open:**
1. **Hash algorithm:** BLAKE3 vs. SHA-256 — confirm based on existing codebase conventions
2. **Container format:** `.tar.zst` recommended (streaming-friendly); confirm Zstandard dependency is acceptable
3. **Who calls `attestsnap`?** Recommend: first snapshot nodeop to reach quorum calls it; contract deduplicates by block_num
4. **Hash-only nodeop incentives:** Leave participation optional for now; design message handling to support it from the start

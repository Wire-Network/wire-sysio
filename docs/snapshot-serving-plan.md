# Snapshot Serving Implementation Plan

## Overview

This plan covers the implementation of a snapshot distribution system with on-chain attestation. Snapshots are generated deterministically by node operators, served via HTTP, and verified against on-chain records after a bootstrapping node syncs to head.

---

## Goals

- Allow new nodes to bootstrap quickly from a verified snapshot rather than replaying all blocks
- Ensure snapshot authenticity via on-chain attestation from a quorum of registered snapshot providers
- Deterministic snapshot generation so all providers produce identical root hashes for the same block
- Provide a simple HTTP endpoint for snapshot + peer list discovery
- Halt a node if its loaded snapshot hash does not match the on-chain attestation (strict enforcement for auto-fetched snapshots; warnings for manual `--snapshot`)

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

## Phase 2 — On-Chain Snapshot Attestation (IMPLEMENTED)

**Status:** Implemented in `feature/snapshot-serving`. Contract actions in `sysio.system`, nodeop integration in `producer_plugin` and `chain_plugin`.

### Approach: On-Chain Voting Contract

Rather than building a parallel BLS signing and P2P voting layer, snapshot attestation uses an
on-chain voting contract. Producers register delegate accounts as candidates; atomic activation
creates a stable, versioned active roster. Once a quorum of that roster agrees on a hash for a
given block, the record is marked as attested.

This approach was chosen over BLS aggregate signatures because:
- No new P2P message types or vote accumulation logic needed
- No requirement for BLS key sharing between producer and snapshot nodes
- Producers opt in — not all need to run snapshot infrastructure
- Uses existing transaction/action infrastructure entirely
- The trust model is equivalent: you're trusting that a quorum of the active schedule honestly computed the hash

### System Contract Actions (in `sysio.system`)

Actions are implemented as a sub-contract class (`snapshot_attest`) following the `peer_keys` pattern — separate header and source file, dispatched via `EXTRA_DISPATCH` in CMakeLists.txt.

**Files:**
- `contracts/sysio.system/include/sysio.system/snapshot_attest.hpp`
- `contracts/sysio.system/src/snapshot_attest.cpp`

**Actions:**

```
regsnapprov(producer, snap_account)                         // producer auth — register a candidate
delsnapprov(account)                                        // candidate or producer auth
votesnaphash(snap_account, block_id, snapshot_hash)         // active-provider auth
evalsnapvote(vote_id, block_id, snapshot_hash, version)     // permissionless recovery
propsnaprost(members, expected_active_version)              // sysio auth — stage a shrink
cancsnaprost()                                              // sysio auth — cancel a shrink
setsnpcfg(min_providers, threshold_pct)                     // sysio auth
getsnaphash(block_num)                                      // read-only attested record
```

**Tables (all scoped to `sysio`):**

| Table | Primary Key | Description |
|-------|------------|-------------|
| `snapconfig` | singleton | `{ min_providers, threshold_pct }` |
| `snapregs` | `snap_account` | Bounded eligible candidates + `byproducer` secondary |
| `snaproster` | `snap_account` | Stable active members + `byproducer` secondary |
| `snaprstate` | singleton | Active/candidate counts and digests, version, and cleanup cursors |
| `snaprprop` | singleton | Governance-approved roster proposal |
| `snapvotes` | auto-increment `id` | Version-bound tuples and voters + `byblocknum`/`byblkroster` secondaries |
| `snaprecords` | `block_num` | Final tuple, attestation block, roster version, and roster digest |

**Registration and roster activation:**
- A producer calls `regsnapprov` to designate a `snap_account` as a candidate. The producer must be active and rank-eligible. The bounded candidate pool deterministically keeps the best 30 producer delegations.
- `delsnapprov` and producer lifecycle reconciliation update only the candidate pool. They cannot silently shrink the active quorum denominator.
- On the first vote for a new height, a sufficiently complete candidate set atomically becomes the next versioned active roster. An intentional shrink requires a staged `propsnaprost` proposal authorized by `sysio`.

**Voting:**
- After computing a snapshot, the snapshot provider calls `votesnaphash(snap_account, block_id, snapshot_hash)`.
- The contract verifies exact membership in the active roster and binds the vote to that roster version.
- Votes are accumulated per `(roster_version, block_num, block_id, snapshot_hash)` tuple. A producer can contribute to only one tuple per height and version.
- `evalsnapvote` lets any account re-evaluate one exact pending tuple without adding a vote, so finalization recovery does not depend on a particular provider retrying.

**Attestation threshold:**
- A snapshot hash is marked attested when `votes >= max(min_providers, ceil(active_roster_size * threshold_pct / 100), floor(active_roster_size / 3) + 1)`.
- `min_providers` is a configurable floor (set via `setsnpcfg` with `sysio` authority). Default: **1** for testnets, raised for mainnet.
- `threshold_pct` is the quorum percentage of the versioned active roster (default: 67%).
- The independent Byzantine floor prevents configuration from reducing quorum to one-third or less.

**Storage:**
- Attested records retain the exact roster version and digest that supplied quorum. A later conflicting vote for the same block is rejected.
- Pending cleanup is bounded to 30 rows per action and resumes from persistent cursors/high-water state; no attestation action performs an unbounded purge.
- Old-version votes never carry into a new roster, and finalized records remain compact permanent evidence.
- Queryable via `get_table_rows` on the `snaprecords` table, or via the `getsnaphash` read-only action for use by block explorers and external clients.

### Snapshot Provider Node

A snapshot provider is a syncing node (not a producer, not a finalizer) that:
1. Generates snapshots every 25,000 blocks (automatic, at exact multiples: block 25000, 50000, 75000, ...)
2. Computes the deterministic BLAKE3 root hash
3. Submits `votesnaphash` transactions to the chain
4. Optionally serves snapshots over HTTP (Phase 3)

**Config:**

```ini
snapshot-provider-account = <account>    # enables snapshot provider mode + hash voting
```

When `snapshot-provider-account` is set:
- A snapshot schedule is automatically created with `block_spacing = 25000` (constant, not configurable — all providers must use the same interval to produce snapshots at identical block heights).
- On each finalized snapshot, a `votesnaphash` transaction is automatically submitted.
- If the contract returns a disagreement error, the node retains the local tuple and shuts down only after the conflicting record becomes irreversible.
- Every 120 irreversible blocks, the node retries its latest tuple and evaluates a bounded page of pending votes with `evalsnapvote`.
- Recovery state and its cursor are persisted under `snapshots-dir`. An irreversible conflict sets a durable quarantine latch that prevents restart until the operator investigates and removes that state.

Role exclusivity is enforced at startup: `snapshot-provider-account` cannot be used alongside `producer-name`.

### Snapshot Verification on Load

When a node starts with `--snapshot`, it captures the snapshot's own block number and BLAKE3 root
hash before retained block-log replay. The snapshot ABI must declare `snaprecords`. Once the node
syncs and LIB advances past the snapshot block, it verifies the hash against the on-chain record:

- **No attestation table found:** Startup rejects the snapshot; pre-attestation system contracts are unsupported.
- **Record is still reversible:** Pending until `attested_at_block` is irreversible.
- **Table read fails while syncing:** Pending and retried on the next finalized block.
- **Table read fails after catch-up:** Fatal; the node stops rather than remaining unverified.
- **No record while syncing or during the 12,500-block grace window:** Pending and retried.
- **No record after grace:** Manual snapshots warn and continue; auto-fetched snapshots stop.
- **Irreversible hash match:** Success; snapshot integrity is confirmed.
- **Irreversible hash mismatch:** Fatal; the node stops.

Manual `--snapshot` remains permissive only when the current attestation table has no record for
the loaded height. Auto-fetch requires a matching record after the grace window.

### Exit Criteria

- ✅ Contract actions and host policies implemented and tested
- ✅ Multiple snapshot providers register, compute hashes, vote
- ✅ Attestation record created when quorum reached
- ✅ Bootstrapping node verifies snapshot hash after syncing
- ✅ Integration test validates full flow: create snapshot → attest → restart from snapshot → sync → verify

---

## Phase 3 — HTTP Snapshot Server (IMPLEMENTED)

**Status:** Implemented in `feature/snapshot-endpoint`. New `snapshot_api_plugin` with HTTP file serving extensions.

### Architecture

A new `snapshot_api_plugin` (separate from the existing `producer_api_plugin`) provides public read-only endpoints for snapshot discovery and download. A new `snapshot_ro` API category (`1 << 11`) allows operators to expose these endpoints publicly via `--http-category-address` while keeping the admin `snapshot` category private or loopback-only.

**Key design decisions:**

| Decision | Rationale |
|----------|-----------|
| Serve uncompressed via `file_body` (sendfile) | Zero-copy kernel sendfile maximizes throughput; compression breaks Range headers |
| No custom download rate limiting | HTTP plugin's existing `max_bytes_in_flight` and `max_requests_in_flight` provide sufficient back-pressure; operators use reverse proxies for finer control |
| POST with JSON body (not GET with path params) | Consistent with existing API pattern; avoids modifying HTTP dispatcher's exact-match routing |
| Separate plugin from `producer_api_plugin` | Public-facing vs operator-only separation; different API categories and trust models |

### Files

**New:**
- `plugins/snapshot_api_plugin/CMakeLists.txt`
- `plugins/snapshot_api_plugin/include/sysio/snapshot_api_plugin/snapshot_api_plugin.hpp`
- `plugins/snapshot_api_plugin/src/snapshot_api_plugin.cpp`
- `plugins/http_plugin/include/sysio/http_plugin/abstract_conn_fwd.hpp` — forward declaration header for `sysio::detail::abstract_conn` (avoids `sysio::detail` vs `sysio::chain::detail` namespace ambiguity)

**Modified:**
- `plugins/http_plugin/include/sysio/http_plugin/api_category.hpp` — added `snapshot_ro = 1 << 11`
- `plugins/http_plugin/include/sysio/http_plugin/common.hpp` — added `send_file_response()` and `get_request_header()` virtual methods to `abstract_conn`
- `plugins/http_plugin/include/sysio/http_plugin/http_plugin.hpp` — added `raw_url_handler` type and `add_raw_handler()` method
- `plugins/http_plugin/include/sysio/http_plugin/beast_http_session.hpp` — implemented `send_file_response()` using Beast's `http::file_body` with Range support
- `plugins/http_plugin/src/http_plugin.cpp` — `snapshot_ro` in category maps, `add_raw_handler()` implementation
- `libraries/chain/include/sysio/chain/snapshot_scheduler.hpp` — changed single callback to vector (`add_snapshot_finalized_callback`)
- `libraries/chain/snapshot_scheduler.cpp` — iterate callback vector
- `plugins/producer_plugin/include/sysio/producer_plugin/producer_plugin.hpp` — added `add_snapshot_finalized_callback()` and `get_snapshots_dir()`
- `plugins/producer_plugin/src/producer_plugin.cpp` — implemented new public methods
- `plugins/CMakeLists.txt` — added `add_subdirectory(snapshot_api_plugin)`
- `cmake/chain-tools.cmake` — added `snapshot_api_plugin` to link libraries
- `programs/nodeop/main.cpp` — registered `snapshot_api_plugin`

### Endpoints

All endpoints use `api_category::snapshot_ro` and are registered during `plugin_startup()`.

| Endpoint | Registration | Request | Response |
|----------|-------------|---------|----------|
| `POST /v1/snapshot/latest` | `add_api` (read_only queue) | no params | `{ block_num, block_id, block_time, root_hash }` or 404 |
| `POST /v1/snapshot/by_block` | `add_api` (read_only queue) | `{ block_num: N }` | same metadata or 404 |
| `POST /v1/snapshot/download` | `add_raw_handler` | `{ block_num: N }` | Binary file with `Content-Disposition: attachment`, supports `Range` header (206 Partial Content) |

### Implementation Details

- **Snapshot catalog:** `std::shared_mutex`-protected `std::map<uint32_t, snapshot_entry>` mapping block_num → metadata (block_num, block_id, block_time, root_hash, file_path, file_size).
- **Catalog init:** On startup, scans `snapshots_dir` for `snapshot-*.bin` files. Uses `threaded_snapshot_reader::load_index()` (fast — reads footer only) + `snapshot_info()` to extract block metadata.
- **Catalog update:** Registers callback via `producer_plugin::add_snapshot_finalized_callback()`. New snapshots are automatically added to the catalog when finalized.
- **File serving:** The download endpoint uses `conn->send_file_response()` which delegates to Beast's `http::file_body` for zero-copy file transfer. Range headers are parsed for resumable downloads.
- **Raw handler pattern:** `add_raw_handler()` receives the `abstract_conn_ptr` directly (bypassing the `url_response_callback` layer) to support binary/file responses.

### `/v1/snapshot/peers` — Deferred

The peers endpoint is a separate feature and was not implemented in this phase.

---

## Phase 4 — Bootstrap from Snapshot Endpoint (IMPLEMENTED)

**Status:** Implemented in `feature/snapshot-endpoint`. Bootstrap logic in `chain_plugin`, download support in `fc::http_client`.

### Configuration

The endpoint is CLI-only because bootstrap is a single-use operation:

```
--snapshot-endpoint URL    Fetch snapshot from URL and bootstrap.
                           URL formats:
                             https://snap.example.com          → fetches latest
                             https://snap.example.com/50000    → fetches block 50000
```

Snapshot bootstrap is an attended operation against an operator-selected endpoint. It reports connection/request phases
and, every five seconds during transfer, downloaded bytes, percentage, rate, and ETA when `Content-Length` is available.
SIGINT cancels pending resolver or socket work and retains atomic temporary-file cleanup. The existing
`chain-state-db-size-mb` setting bounds both fixed-length and chunked response bodies. No snapshot-specific resource
options are added.

The block number is encoded as a trailing path component of the URL. If the last path segment is a decimal number, it's treated as a specific block request (POST to `/v1/snapshot/by_block`); otherwise POST to `/v1/snapshot/latest`.

### Files

**Modified:**
- `libraries/libfc/include/fc/network/http/http_client.hpp` — added `post_to_file()` method
- `libraries/libfc/src/network/http/http_client.cpp` — implemented `post_to_file()` (POST with JSON body, write binary response to temp file, rename on completion)
- `plugins/chain_plugin/src/chain_plugin.cpp` — `--snapshot-endpoint` option, bootstrap flow, strict attestation verification

### Bootstrap Flow (in `chain_plugin::plugin_initialize`)

1. **Mutual exclusion:** `--snapshot-endpoint` is incompatible with `--snapshot` (error if both set).
2. **Existing data check:** If chain data exists (`shared_memory.bin` / `chain_head.dat`), error with message:
   ```
   Cannot bootstrap from snapshot endpoint with existing chain data.
   Rerun with --delete-all-blocks --snapshot-endpoint URL to remove
   existing blocks and state before bootstrapping.
   ```
   This works naturally with `--delete-all-blocks` which clears state before snapshot handling.
3. **Fetch metadata:** POST to `/v1/snapshot/latest` or `/v1/snapshot/by_block` depending on URL format.
4. **Download snapshot:** Uses bounded `fc::http_client::post_to_file()` streaming to POST to
   `/v1/snapshot/download`, enforce size and disk-headroom bounds, and atomically save the response to the local
   snapshots directory.
5. **Root hash verification:** Uses `threaded_snapshot_reader::load_index()` to read the footer and compare the stored root hash against the advertised `root_hash`. This is a fast metadata-only check that catches download corruption. Full integrity verification (re-hashing all sections) happens during snapshot loading, and on-chain attestation verification happens after syncing.
6. **Continue normal loading:** Sets `snapshot_path` to downloaded file and `snapshot_auto_fetched = true`. No `--genesis-json` needed — snapshot contains genesis.

The bootstrap logic is encapsulated in `chain_plugin_impl::fetch_snapshot_from_endpoint()`.

### Strict Attestation Verification

Manual and auto-fetched snapshots share the same retry and integrity policy; only the terminal
missing-record outcome differs:

| Condition | Manual `--snapshot` | Auto-fetched (`--snapshot-endpoint`) |
|-----------|-------------------|--------------------------------------|
| No attestation table | Startup rejected | Startup rejected |
| Missing record while syncing or within grace | Retry | Retry |
| Missing record after grace | Warning | **FATAL** — shutdown |
| Reversible record | Retry | Retry |
| Table read failure while syncing | Retry | Retry |
| Table read failure after catch-up | FATAL | FATAL |
| Irreversible hash mismatch | FATAL | FATAL |
| Irreversible hash match | Success | Success |

**Retry-based verification:** The attestation check (`verify_snapshot_attestation()`) runs on each irreversible block after the snapshot block. In the real-world flow, a snapshot is taken first, then providers independently generate their own snapshots (which can take minutes), submit votes, reach quorum, and the attestation becomes irreversible. The bootstrap node loads the snapshot, syncs forward, and eventually reaches the block containing the attestation record.

The check retries on each finalized block while the node is still catching up (finalized block time
more than ~30 seconds behind wall-clock). Both manual and auto-fetched snapshots then share a
12,500-finalized-block grace window past the snapshot height, because provider votes may still be
in flight after the bootstrap node reaches the tip. After grace, a missing record warns for manual
`--snapshot` and stops an auto-fetched bootstrap. A reversible record remains pending, while an
irreversible mismatch or a caught-up table-read failure stops either path.

**Note:** Verification happens *after* sync, not before. The node loads the snapshot optimistically, syncs to head, then checks the on-chain record. This avoids a chicken-and-egg problem (can't query on-chain state before having chain state).

---

## Operator Deployment Model

```
[Producer Node]                    [Snapshot Provider Node]
  producer-name = <account>          snapshot-provider-account = <snap_account>
  (no snapshot-provider-account)     (no producer-name)
```

The producer registers the snapshot provider's account via `regsnapprov`. The snapshot provider node syncs the chain, automatically generates snapshots every 25,000 blocks, votes on hashes via `votesnaphash`, and optionally serves snapshots over HTTP. No shared keys between the two nodes.

For a complete operator setup guide — including producer registration, provider account delegation, attestation quorum configuration, snapshot generation, attestation voting, and HTTP network configuration with `--http-category-address` — see [`plugins/snapshot_api_plugin/README.md`](../plugins/snapshot_api_plugin/README.md).

---

## Testing

### Implemented Tests

**Contract tests** (`contracts/tests/sysio.snapshot_attest_tests.cpp`) cover candidate
registration and reconciliation, deterministic capacity replacement, active-roster activation,
governance shrink proposals, version-bound voting, Byzantine and configured quorum floors,
permissionless finalization recovery, bounded cleanup, disagreement rejection, and record queries.

**Host tests** cover snapshot hash round trips and persistence
(`unittests/snapshot_attest_tests.cpp`), durable provider recovery and quarantine
(`plugins/producer_plugin/test/test_snapshot_attestation_recovery.cpp`), exact automatic schedule
boundaries (`plugins/producer_plugin/test/test_options.cpp`), and strict chain-plugin startup,
retry, grace, and table-read policy (`plugins/chain_plugin/test/plugin_config_test.cpp`).

**Integration tests** exercise end-to-end attestation/bootstrap and snapshots taken during the
Savanna transition while BIOS remains active with the real `snaprecords` table and `snap_record`
struct declarations merged into its ABI.

**Integration test** (`tests/snapshot_attest_test.py` — 5 tests):
- Snapshot creation + attestation vote with hash verification against `snaprecords` table
- Multiple provider votes reaching quorum
- `getsnaphash` read-only query
- Load attested snapshot → sync from peers → verify attestation records on synced node
- Provider deregistration via `delsnapprov`

**Integration test** (`tests/snapshot_api_test.py` — 9 tests):

Phase 3 (API endpoints):
- `/v1/snapshot/latest` returns 404 with empty catalog
- Create snapshot and verify `/v1/snapshot/latest` metadata
- `/v1/snapshot/by_block` returns correct metadata
- `/v1/snapshot/by_block` returns 404 for non-existent block
- `/v1/snapshot/download` serves binary file matching on-disk snapshot
- Range header support (206 Partial Content with correct `Content-Range`)
- Second snapshot updates catalog; first snapshot still accessible

Phase 4 (bootstrap):
- Bootstrap from latest snapshot endpoint — node syncs forward, finds attestation record in blocks after the snapshot
- Bootstrap with specific block number in URL — uses `addSwapFlags` to avoid flag duplication across relaunches

The Phase 4 tests use the realistic attestation flow: snapshot is taken first, then attested afterwards. The bootstrap node loads from the snapshot and syncs forward to find the attestation record, matching production behavior.

---

## Open Questions

**Resolved:**
- **Hash algorithm:** BLAKE3 — faster than SHA-256, uses LLVM's bundled implementation
- **Snapshot format:** Single-file binary with section index at end (not tar.zst — simpler, deterministic, already 10% faster than Spring)
- **Signing mechanism:** On-chain voting contract in `sysio.system`, not BLS aggregate signatures (simpler, equivalent trust model, no P2P changes needed)
- **Snapshot interval:** Every 25,000 blocks, constant (not configurable) — all providers must use the same interval
- **Attestation threshold:** `max(min_providers, ceil(active_roster_size * threshold_pct / 100), floor(active_roster_size / 3) + 1)` with configurable floor and percentage via `setsnpcfg`
- **Contract location:** Actions added to `sysio.system` as a sub-contract class (not a separate `sysio.snapshot` contract) for direct access to the producers table and rank index
- **Manual snapshot verification:** Startup requires the current attestation table. Both paths retry
  through catch-up and the 12,500-block grace window; afterward a missing record warns for manual
  `--snapshot`, while auto-fetch via `--snapshot-endpoint` requires a record.
- **Compression for distribution:** Serve uncompressed via `file_body` (kernel sendfile). Compression breaks Range headers, adds CPU cost per download, and operators already handle this via reverse proxies (nginx `gzip_static` / CDN). If needed later, generate `.bin.zst` alongside `.bin` during snapshot finalization and serve via `Accept-Encoding` negotiation.
- **Download rate limiting:** No custom limiting needed. HTTP plugin's existing `max_bytes_in_flight` and `max_requests_in_flight` provide sufficient back-pressure. Operators use reverse proxies for finer control.
- **API style for snapshot endpoints:** POST with JSON body (not GET with path params) for consistency with existing API pattern and to avoid modifying the HTTP dispatcher's exact-match routing.
- **Bootstrap config pattern:** Single CLI-only `--snapshot-endpoint` option with block number embedded in URL path (not separate `--snapshot-block-num` config option). CLI-only because bootstrap is a single-use operation.

**Still open:**
1. **Snapshot retention:** How many snapshots should a provider node retain? Prune policy?
2. **Peers endpoint:** `/v1/snapshot/peers` for peer discovery (deferred to separate feature).

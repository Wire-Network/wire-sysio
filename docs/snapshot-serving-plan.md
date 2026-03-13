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

Rather than building a parallel BLS signing and P2P voting layer, snapshot attestation uses a simple on-chain voting contract. Producers register delegate accounts ("snapshot providers") that compute snapshot hashes and vote on-chain. Once a quorum of registered providers agree on a hash for a given block, the record is marked as attested.

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
regsnapprov(producer, snap_account)       // producer auth — register a snapshot provider
delsnapprov(account)                      // account auth — works as snap_account or producer
votesnaphash(snap_account, block_id, snapshot_hash)  // snap_account auth
setsnpcfg(min_providers, threshold_pct)   // sysio auth
getsnaphash(block_num)                    // read-only — returns attested record
```

**Tables (all scoped to `sysio`):**

| Table | Primary Key | Description |
|-------|------------|-------------|
| `snapconfig` | singleton | `{ min_providers, threshold_pct }` |
| `snapprovs` | `snap_account` | `{ snap_account, producer }` + `byproducer` secondary |
| `snapvotes` | auto-increment `id` | `{ id, block_num, block_id, snapshot_hash, voters[] }` + `byblocknum` secondary |
| `snaprecords` | `block_num` | `{ block_num, block_id, snapshot_hash, attested_at_block }` |

**Registration:**
- A producer calls `regsnapprov` to designate a `snap_account` as their snapshot provider. The producer must be registered (via `regproducer`) with rank ≤ 30. This decouples authority — the producer's keys never need to be on the snapshot node.
- Either party can sever the relationship via `delsnapprov`: the `snap_account` can deregister itself, or the producer can remove its delegate. The action accepts either account and looks up the mapping in both directions.

**Voting:**
- After computing a snapshot, the snapshot provider calls `votesnaphash(snap_account, block_id, snapshot_hash)`.
- The contract verifies `snap_account` is a registered provider.
- Votes are accumulated per `(block_num, snapshot_hash)` tuple.

**Attestation threshold:**
- A snapshot hash is marked "attested" when: `votes >= max(min_providers, ceil(registered_count * threshold_pct / 100))`
- `min_providers` is a configurable floor (set via `setsnpcfg` with `sysio` authority). Default: **1** for testnets, raised for mainnet.
- `threshold_pct` is the quorum percentage of registered providers (default: 67%).
- This ensures attestation scales with participation but never falls below the configured floor.

**Storage:**
- Votes accumulate per `(block_num, block_id, snapshot_hash)` tuple. The `block_num` is extracted from the `block_id`. Since snapshots are only produced at LIB (finalized) blocks, the `block_id` for a given `block_num` will always agree across honest providers. Attestation occurs when one tuple reaches quorum.
- Attested records: `snap_record { block_num, block_id, snapshot_hash, attested_at_block }`
- Once a `block_num` has an attested record, any vote with a different hash is rejected ("disagrees with attested record").
- **Purging:** Once a `block_num` is attested, all vote records for `block_num <= attested` are purged to free RAM. Only the attested `snap_record` is retained. At ~200 bytes per record and ~2,523 snapshots per year (every 25,000 blocks at 0.5s block time), attested records consume ~500 KB/year — negligible.
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
- If the contract returns a disagreement error (hash mismatch with attested record), the node logs a fatal error and shuts down.

Role exclusivity is enforced at startup: `snapshot-provider-account` cannot be used alongside `producer-name`.

### Snapshot Verification on Load

When a node starts with `--snapshot`, it captures the snapshot's block number and BLAKE3 root hash. Once the node syncs and LIB advances past the snapshot block, it verifies the hash against on-chain `snaprecords`:

- **No attestation table found:** Warning only. Supports chains with system contracts that don't include snapshot attestation.
- **No record for this block number:** Warning only. Not every block height will have an attested snapshot.
- **Hash matches:** Success — snapshot integrity confirmed.
- **Hash mismatch:** Fatal error — snapshot may be corrupted or tampered with. Node shuts down.

This permissive approach for manual `--snapshot` allows operators to load their own snapshots without attestation. Future auto-fetch from peers (Phase 4) will enforce strict verification (fatal on missing attestation).

### Exit Criteria

- ✅ Contract actions implemented and tested (19 contract tests, 5 unit tests)
- ✅ Multiple snapshot providers register, compute hashes, vote
- ✅ Attestation record created when quorum reached
- ✅ Bootstrapping node verifies snapshot hash after syncing
- ✅ Integration test validates full flow: create snapshot → attest → restart from snapshot → sync → verify

---

## Phase 3 — HTTP Snapshot Server (IMPLEMENTED)

**Status:** Implemented in `feature/snapshot-endpoint`. New `snapshot_api_plugin` with HTTP file serving extensions.

### Architecture

A new `snapshot_api_plugin` (separate from the existing `producer_api_plugin`) provides public read-only endpoints for snapshot discovery and download. A new `snapshot_ro` API category (`1 << 11`) allows operators to expose these endpoints publicly via `--http-category-address` while keeping the admin `snapshot` category loopback-only.

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

A single CLI-only option (not config file — single-use bootstrap):

```
--snapshot-endpoint URL    Fetch snapshot from URL and bootstrap.
                           URL formats:
                             https://snap.example.com          → fetches latest
                             https://snap.example.com/50000    → fetches block 50000
```

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
4. **Download snapshot:** Uses `fc::http_client::post_to_file()` to POST to `/v1/snapshot/download` and save binary response to local snapshots directory.
5. **Root hash verification:** Uses `threaded_snapshot_reader::load_index()` to read the footer and compare the stored root hash against the advertised `root_hash`. This is a fast metadata-only check that catches download corruption. Full integrity verification (re-hashing all sections) happens during snapshot loading, and on-chain attestation verification happens after syncing.
6. **Continue normal loading:** Sets `snapshot_path` to downloaded file and `snapshot_auto_fetched = true`. No `--genesis-json` needed — snapshot contains genesis.

The bootstrap logic is encapsulated in `chain_plugin_impl::fetch_snapshot_from_endpoint()`.

### Strict Attestation Verification

When `snapshot_auto_fetched == true`, the attestation check in `verify_snapshot_attestation()` is upgraded from warnings to fatal errors:

| Condition | Manual `--snapshot` | Auto-fetched (`--snapshot-endpoint`) |
|-----------|-------------------|--------------------------------------|
| No attestation table | Warning | **FATAL** — shutdown |
| No record for block_num | Warning | **FATAL** — shutdown |
| Hash mismatch | FATAL | FATAL |
| Hash match | Success | Success |

**Retry-based verification:** The attestation check (`verify_snapshot_attestation()`) runs on each irreversible block after the snapshot block. In the real-world flow, a snapshot is taken first, then providers independently generate their own snapshots (which can take minutes), submit votes, reach quorum, and the attestation becomes irreversible. The bootstrap node loads the snapshot, syncs forward, and eventually reaches the block containing the attestation record.

The timeout is 12,500 blocks (~104 minutes at 0.5s block time), which is half the snapshot interval of 25,000 blocks. This provides ample time for providers to generate, vote, and reach quorum before the next snapshot is due.

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

**Contract tests** (`contracts/tests/sysio.snapshot_attest_tests.cpp` — 19 tests):
- Registration: basic, duplicate rejection, wrong auth, unregistered producer, rank too high
- Deregistration: by snap_account, by producer, not-found
- Config: `setsnpcfg` validation, sysio-only auth
- Voting: unregistered rejected, single vote no quorum, quorum reached, duplicate vote
- Threshold: min_providers floor, percentage calculation
- Disagreement: vote with different hash after attestation → rejected
- Purging: old vote records cleaned after attestation
- `getsnaphash`: returns attested record, not-found error

**Unit tests** (`unittests/snapshot_attest_tests.cpp` — 5 tests):
- `snapshot_hash_matches_attestation` — full chain with system contract, snapshot hash matches on-chain record
- `snapshot_roundtrip_preserves_hash` — write/read snapshot preserves BLAKE3 root hash
- `snapshot_hash_mismatch_detected` — wrong hash on-chain vs snapshot is detectable
- `snapshot_no_attestation_detected` — no vote → no record found
- `attestation_survives_snapshot_load` — attestation data survives snapshot round-trip in `snapshotted_tester`

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
- **Attestation threshold:** `max(min_providers, ceil(registered_count * threshold_pct / 100))` with configurable floor and percentage via `setsnpcfg`
- **Contract location:** Actions added to `sysio.system` as a sub-contract class (not a separate `sysio.snapshot` contract) for direct access to the producers table and rank index
- **Manual snapshot verification:** Warnings only for missing attestation table or record when using `--snapshot`. Strict enforcement reserved for auto-fetch via `--snapshot-endpoint`.
- **Compression for distribution:** Serve uncompressed via `file_body` (kernel sendfile). Compression breaks Range headers, adds CPU cost per download, and operators already handle this via reverse proxies (nginx `gzip_static` / CDN). If needed later, generate `.bin.zst` alongside `.bin` during snapshot finalization and serve via `Accept-Encoding` negotiation.
- **Download rate limiting:** No custom limiting needed. HTTP plugin's existing `max_bytes_in_flight` and `max_requests_in_flight` provide sufficient back-pressure. Operators use reverse proxies for finer control.
- **API style for snapshot endpoints:** POST with JSON body (not GET with path params) for consistency with existing API pattern and to avoid modifying the HTTP dispatcher's exact-match routing.
- **Bootstrap config pattern:** Single CLI-only `--snapshot-endpoint` option with block number embedded in URL path (not separate `--snapshot-block-num` config option). CLI-only because bootstrap is a single-use operation.

**Still open:**
1. **Snapshot retention:** How many snapshots should a provider node retain? Prune policy?
2. **Peers endpoint:** `/v1/snapshot/peers` for peer discovery (deferred to separate feature).

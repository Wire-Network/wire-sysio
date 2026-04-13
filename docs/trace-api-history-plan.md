# trace_api_plugin: History-Solution Upgrade Plan

Single PR, multiple commits. Each commit is independently reviewable and (where possible) independently testable.

## Goals

1. Fix `/v1/trace_api/get_transaction_trace` full-history scan.
2. Capture ABIs automatically (no operator-supplied `abi.json`), versioned correctly across mid-block / mid-trx setabi.
3. Refuse to start with a gap in recorded blocks (correctness foundation for #2).
4. Add exchange-friendly query endpoints over the existing trace data.

Non-goals: account-history index, cross-contract joins, streaming, public-internet hardening, ABI sidecar pruning.

## Storage shape (new sidecars, alongside existing slice files)

All new files live in the same slice dir as `trace_*.log`:

| File | Scope | Purpose |
|---|---|---|
| `trace_trx_idx_<range>.log` | per slice | mmap'd hash table: `trx_id_prefix64 -> block_num` |
| `abi_blobs.log` | global | ABI blobs, append-only |
| `abi_index.log` | global | append-only `(account, abi_global_seq, blob_offset, blob_len)` |

ABI sidecar is global (not per-slice) because ABI versions cross slice boundaries.

## Commits

### Commit 1: Continuity enforcement at startup

**Why:** Foundation. Without this, ABI lazy-fetch reasoning breaks (you can't claim "if a setabi happened between fresh-start and first-encounter we'd have seen it" if gaps are silently allowed).

**Changes:**
- `slice_directory`: add `last_recorded_block()` — scan highest index slice file front-to-back, return max `block_entry_v0.number`, or `nullopt` if no slice files exist.
- `store_provider`: expose `last_recorded_block()` delegating to `_slice_directory`.
- `chain_extraction_impl`: on first `signal_block_start` after startup, check:
  - empty dir (`nullopt`): log fresh-start at head, proceed.
  - non-empty dir: require `block_num == last_recorded_block + 1`, else invoke `except_handler` with a descriptive message ("delete slice dir or replay from snapshot covering blocks N..M"), which causes node shutdown.

**Tests:** unit tests for all continuity cases: fresh start, exact continuation, overlap (snapshot within existing data — ok), snapshot before data start (error), gap forward (error), check fires only once.

---

### Commit 2: trx_id index sidecar

**Why:** Replace O(N slices * slice scan) with O(slices) page-faults.

**File format:** `trace_trx_idx_<range>.log`
```
header { magic, version, bucket_count, bucket_size }
buckets[bucket_count] of { trx_id_prefix64, block_num, _pad }   // open addressing
```
Open-addressing hash, load factor ~0.5, linear probing. Sized at slice close from observed trx count. No bloom filter in v1 — hash probe is already O(1) per slice; revisit if not-found latency becomes a real complaint. Header has room to add a bloom section later without breaking old readers.

**Changes:**
- `trx_id_index_writer`: builds index from a closed `trace_trx_id_<range>.log`. Iterates entries, fills buckets, writes file.
- `trx_id_index_reader`: mmap's file, `lookup(trx_id) -> optional<block_num>`.
- `slice_directory`: `find_trx_id_index_slice(slice_num)`, parallels `find_trx_id_slice`.
- Maintenance thread: when a slice's trx_id file becomes stable (LIB-crossed or compressed), build the index. Idempotent (skip if exists, rebuild if header version mismatches).
- Startup: scan slice dir, build any missing indexes synchronously before serving requests (optional fast path: do it lazily on first miss with a one-time background build).
- Rewrite `store_provider::get_trx_block_number`:
  - iterate slices newest -> oldest
  - per slice: bucket probe
  - on hash hit, confirm by reading the actual `block_trxs_entry` to defeat 64-bit prefix collisions
  - fall back to current linear scan only if a slice's index file is missing/corrupt (log warning, schedule rebuild)

**Tests:** round-trip insert/lookup; collision handling (synthetic colliding ids); missing-index rebuild on startup; pruning when slice deleted (index file deleted alongside).

---

### Commit 3: ABI sidecar — storage layer

**Why:** Foundation for commits 4 and 5; pure data-layer with no chain hooks yet.

**Changes:**
- `abi_blob_store`: append-only `abi_blobs.log`. `store(bytes) -> {offset, len}`. No dedup — ABIs are small (few KB) and duplication across a node's lifetime is negligible.
- `abi_version_index`: in-memory `map<account, sorted_vector<{abi_global_seq, blob_offset, blob_len}>>`, persisted as append-only records to `abi_index.log`. Startup does a single pass over the file to populate the in-memory map.
- `lookup(account, action_global_seq) -> optional<bytes>`: `upper_bound` on per-account vector, step back one. `0` sentinel matches anything.
- `record(account, abi_global_seq, abi_bytes)`: append blob, append index entry, update in-memory map.

**Tests:** version lookup with `<=` semantics including `0` sentinel, restart replay produces identical in-memory map.

---

### Commit 4: ABI capture — live + lazy

**Why:** Wires sidecar to chain.

**Changes:**
- In `chain_extraction_impl::on_applied_transaction`, walk action_traces:
  - if `act.account == "sysio" && act.name == "setabi"`: decode payload `{account, abi: bytes}`, call `abi_version_index.record(account, act.receipt->global_sequence, abi)`. Invalidate decoder cache for that account.
  - for any other action: ensure ABI exists for `act.receiver` (the contract whose code ran). If `abi_version_index.lookup(receiver, 0)` is empty, fetch current ABI from `controller.db().find<account_metadata_object>(receiver)` and `record(receiver, 0, current_abi)`. (Sentinel 0 = "as of beginning of observation, exact block unknown.")
- Decoder helper `decode_action_data(account, action_global_seq, action_name, data) -> variant`:
  - `lookup` ABI bytes for that version
  - construct `abi_serializer` (cache by `(account, abi_global_seq)`)
  - on any throw: return raw bytes + flag

**Tests:** mid-trx setabi changes ABI for sibling inline action; lazy capture on first encounter; cache invalidation on setabi; corrupt ABI on chain falls back to raw.

---

### Commit 5: `get_actions` primitive endpoint

**Endpoint:** `POST /v1/trace_api/get_actions`

**Request:**
```json
{
  "contract": "sysio.token",            // required
  "action": "transfer",                  // optional
  "block_num": 12345,                    // OR
  "block_range": [12345, 12400],         // capped by trace-max-block-range
  "filters": {
    "from": "alice",                     // optional
    "to": "bob",                         // optional
    "authorizer": "alice"                // optional
  },
  "include_failed": false                // default false; when false, only "executed"
}
```

**Behavior:**
- Always traverses inline actions (no toggle).
- Match rule: `act.account == contract` (canonical action, not notification). Notifications surface as separate rows when `act.receiver == contract` but `act.account != contract` — excluded by default; reconsider only if needed.
- Range scan: iterate `block_num` in range, load block trace from existing slice infra, walk action_traces.
- Filters applied post-load.
- ABI decode via commit-4 helper; raw fallback on error.

**Response row:**
```json
{
  "block_num": 12345,
  "trx_id": "...",
  "action_ordinal": 3,
  "global_sequence": 9876543,
  "receiver": "sysio.token",
  "account": "sysio.token",
  "name": "transfer",
  "authorization": [...],
  "status": "executed",
  "irreversible": true,
  "data_decoded": {...},      // present if decode succeeded
  "data_raw": "..."           // present only if decode failed
}
```

**Config:**
- `trace-max-block-range` (default e.g. 1000; -1 = unlimited for local-only deployments).

**Tests:** filters; failed-trx exclusion/inclusion; inline traversal; range cap; decode-failure raw fallback.

---

### Commit 6: `get_token_transfers` convenience endpoint

**Endpoint:** `POST /v1/trace_api/get_token_transfers`

**Request:** same shape as `get_actions` minus `action`, plus:
```json
{
  "contract": "sysio.token",      // required, exact
  "account": "alice",             // optional, matches from OR to
  "memo_contains": "user_42",     // optional substring
  "block_num" | "block_range": ...,
  "include_failed": false
}
```

**Behavior:** thin wrapper over `get_actions` with `action="transfer"`, projects decoded data:
```json
{
  "block_num": ...,
  "trx_id": ...,
  "action_ordinal": ...,
  "global_sequence": ...,
  "from": "...",
  "to": "...",
  "quantity": "1.0000 SYS",
  "memo": "...",
  "status": "executed",
  "irreversible": true
}
```

If decode fails (ABI invalid), the row is **omitted** (not raw — exchanges can't use raw transfers; they'd see them via `get_actions` if needed). Log a warning with trx_id for diagnosis.

**Tests:** from/to OR semantics; memo substring; multiple transfers in one block; decode-failure omission.

---

## Risks / open items

- **Reorg of indexed data.** Trx_id index covers irreversible blocks only. With fast finality the reversible window is tiny (seconds, low single-digit blocks), and most operators are expected to run trace_api in read-mode=irreversible. Lookup path: index probe first; on miss, linear-scan the reversible window (cheap because the window is small) before returning not-found. No reorg fixup needed.
- **`abi_index.log` corruption.** Append-only with no checksums today. Acceptable v1; add CRC per record in a follow-up if it bites.
- **Continuity check + existing deployments.** Operators with existing slice dirs and gaps will fail to start after upgrade. Release note + `trace-allow-gap=true` escape hatch.

## Test plan

The trace_api_plugin has unusually strong test coverage today (see `plugins/trace_api_plugin/test/`: `test_trace_file`, `test_data_handlers`, `test_extraction`, `test_responses`, `test_compressed_file`, `test_configuration_utils`). Maintain that bar — every new component gets a dedicated test file with the same breadth.

### Commit 1 — continuity enforcement

New test file: `test/test_continuity.cpp`.

- Empty slice dir, chain head at block 1 -> starts, logs fresh-start.
- Empty slice dir, chain head at block 50M -> starts, logs fresh-start at 50M.
- Non-empty slice dir ending at block N, chain head at N+1 -> starts normally.
- Non-empty slice dir ending at block N, chain head at N+10 -> throws with operator-facing message naming the gap.
- Non-empty slice dir ending at block N, chain head at N-5 (replay from snapshot behind tip) -> starts normally once chain catches up; the first accepted_block after startup is N+1.
- `trace-allow-gap=true` config override -> legacy behavior, warns loudly.
- Slice dir contains only orphaned index file (no trace/trx_id) -> treat as empty (or throw — pick and test).
- Corrupted highest slice file -> clear error, does not proceed.

### Commit 2 — trx_id index

New test file: `test/test_trx_id_index.cpp`.

- Writer: build from synthetic `trace_trx_id_<range>.log`, verify bucket occupancy, linear probe chains terminate, load factor within spec.
- Reader: lookup hit, lookup miss, lookup with prefix collision (two trx_ids sharing the 64-bit prefix) -> second probe, correctness verified against underlying trx_id entry.
- Round-trip: random 10k trx_ids, every one found; 10k never-inserted trx_ids, all return not-found.
- File format: wrong magic -> error; wrong version -> error; truncated file -> error (not UB).
- mmap lifecycle: reader holds file, writer cannot overwrite (or does via new file + rename).
- Startup missing-index rebuild: delete one slice's index, start, verify it's rebuilt, lookup still works.
- Corrupt index file on startup: rebuilt from the trx_id log source-of-truth.

Extensions to existing tests:

- `test_extraction.cpp`: verify index is built when slice closes (on LIB cross).
- Integration test under `tests/`: run against a 100k-block replay dataset, measure and assert `get_transaction_trace` latency under a fixed budget (regression guard).
- Reversible window: lookup for a trx in the current reversible window hits via the fallback linear scan.

### Commit 3 — ABI sidecar storage

New test file: `test/test_abi_sidecar.cpp`.

- `abi_blob_store` append + read round-trip across many records.
- `abi_version_index`:
  - `lookup(acct, global_seq)` with `<=` semantics: boundary at exact match, just-before, just-after.
  - `0` sentinel entry matches any `global_seq`.
  - Multiple versions same account, different global_seqs -> returns correct one.
  - Lookup for unknown account -> empty.
  - Lookup with `global_seq` before the earliest recorded -> empty (not the `0` entry, unless a `0` entry exists).
- Restart replay: write N records across process lifetime, restart, in-memory map identical to pre-restart (compare via dump-to-vector).
- Corrupt `abi_index.log` tail: recover up to last valid record, log; do not crash.
- Mid-block multiple setabi for same account: both stored, lookup between them returns the earlier.

### Commit 4 — ABI capture

Extensions to `test_extraction.cpp` (add new cases; do not fork the file unless it grows unwieldy):

- Live capture: `setabi` action in applied_transaction -> sidecar has new record with correct `global_sequence`.
- Lazy capture: first action from never-before-seen account -> `lookup(account, 0)` populated from controller state.
- Cache invalidation: action -> lazy fetch; then setabi observed; next decode for that account uses the new ABI.
- Mid-trx setabi: trx has action A (account X), then setabi for X, then action B (account X). A decodes with old ABI, B decodes with new ABI. Verified via action_global_seq boundaries.
- Inline setabi: same as above but setabi is inline, not top-level.
- Plugin started at block 50M on a chain where X did setabi at block 40M and never again: first encounter lazy-fetches current ABI tagged at 0 — decode succeeds.
- Invalid ABI on chain (bytes that don't deserialize into an `abi_def`): sidecar records raw bytes; decoder returns raw on decode attempt; does not throw through to endpoint.
- Decoder cache hit counting (optional, via hook): verify cache actually reused across same `(account, abi_global_seq)`.

### Commit 5 — get_actions endpoint

Extensions to `test_responses.cpp`:

- Single block, single contract, single matching action -> returns one row with decoded data.
- Single block, no matching actions -> empty response, 200.
- Multiple matching actions across inline tree -> all returned in action_ordinal order.
- Filters combined: `contract + action`, `+from`, `+to`, `+authorizer`, mutually constraining.
- `include_failed=false` (default): `soft_fail`, `hard_fail`, `expired`, `delayed` excluded.
- `include_failed=true`: all statuses returned with correct `status` field.
- Block range: full range scanned, results in order.
- Range cap: request exceeding `trace-max-block-range` -> 400 with the cap value.
- `max_block_range=-1` (unlimited): large range works.
- ABI present, decode succeeds: `data_decoded` present, no `data_raw`.
- ABI present, decode fails (invalid bytes for the schema): `data_raw` present, `data_decoded` absent, with `decode_error`.
- Reversible block in range: included with `irreversible: false`.
- Contract the endpoint has never seen an ABI for: lazy fetch kicks in at query time or ingestion time (confirm which; test accordingly).
- Notifications vs canonical actions: only `act.account == contract` rows returned (notifications excluded).

### Commit 6 — get_token_transfers

New test file: `test/test_token_transfers.cpp`.

- Simple transfer: `from/to/quantity/memo` projected correctly.
- `account` filter = from OR to: matches either side.
- `memo_contains`: substring match, case-sensitive; non-match excluded; empty memo never matches a non-empty pattern.
- Multiple transfers in one block: all returned, ordered by `(block_num, action_ordinal)`.
- Inline-action transfer: returned.
- Failed transfer (hard_fail): excluded by default, included with `include_failed=true`.
- ABI decode failure: row omitted, warning logged, endpoint succeeds.
- Non-token contract with a `transfer` action of different schema: decode fails -> row omitted. (Caller was told to scope correctly; we just don't return garbage.)
- `contract` required -> 400 when omitted.
- Range semantics match `get_actions` (reuse shared test helpers).

### Cross-cutting integration tests (under `tests/`)

- End-to-end: launch a TestHarness cluster with trace_api enabled, deploy sysio.token, perform 1000 transfers with a mix of inline/top-level, setabi mid-run on the token contract changing the memo type, then query:
  - `get_transaction_trace` for each trx_id — all hit, latency asserted.
  - `get_token_transfers` for the token, each block in range, all transfers accounted for, old transfers decoded with old ABI, new ones with new ABI.
- Gap-refusal: kill the node, advance chain via a second node, restart first node with trace_api -> refuse to start.
- Restart idempotency: start, run 1000 blocks, stop, restart — sidecars/indexes intact, queries unchanged.
- Compressed slice interaction: force a slice compression, verify trx_id lookup still works against the `.clog` slice (via the existing compressed-slice read path).
- Retention pruning: set `minimum_irreversible_history_blocks` low, verify slice deletion also removes the matching `trace_trx_idx_*` file; verify ABI sidecar is untouched.

### Performance regression guard

Add a micro-benchmark (gated, not run in default ctest):

- 100k-block synthetic trace corpus, 10k random trx_id lookups: p50 and p99 latency must be within a configured budget (e.g., p99 < 5 ms on SSD). Fails the guard if we regress to linear-scan behavior.

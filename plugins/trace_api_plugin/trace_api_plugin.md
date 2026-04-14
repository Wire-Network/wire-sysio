# trace_api_plugin

Full-history action trace plugin for Wire nodeop. Captures every action
trace as blocks are applied, persists them in a structured on-disk store,
and exposes HTTP endpoints for querying traces, transactions, actions, and
token transfers.

---

## Table of contents

1. [Overview](#overview)
2. [Enabling the plugin](#enabling-the-plugin)
3. [Configuration options](#configuration-options)
4. [On-disk layout](#on-disk-layout)
   - [Slice files](#slice-files)
   - [Transaction-id index](#transaction-id-index)
   - [ABI store](#abi-store)
5. [Startup continuity check](#startup-continuity-check)
6. [ABI decoding](#abi-decoding)
7. [HTTP API reference](#http-api-reference)
   - [get_block](#get_block)
   - [get_transaction_trace](#get_transaction_trace)
   - [get_actions](#get_actions)
   - [get_token_transfers](#get_token_transfers)
8. [Pagination guide](#pagination-guide)
9. [Exchange / indexer integration guide](#exchange--indexer-integration-guide)
10. [Maintenance and retention](#maintenance-and-retention)
11. [Plugin variants](#plugin-variants)

---

## Overview

The trace_api_plugin writes a complete record of every action executed on
chain (including inline actions), alongside the ABI in effect at the time
each contract was called.  That data is kept on disk indefinitely (or for a
configurable retention window) and is served through a set of HTTP endpoints
without touching the chainbase database.

Key design points:

- **No chainbase dependency at query time** — responses are built entirely
  from the trace files on disk.
- **Inline actions included** — every entry in `chain::transaction_trace::
  action_traces` is stored, so inline notifications are captured alongside
  the originating action.
- **Versioned ABI decoding** — the ABI in effect at the moment each
  `setcode`/`setabi` transaction was applied is captured in `abi_store.log`.
  Queries decode `data` and `return_value` fields using the historically
  correct ABI, not the current on-chain ABI.
- **O(1) transaction lookup** — a per-slice hash index maps `trx_id` to
  `block_num` so `get_transaction_trace` does not scan the chain.

---

## Enabling the plugin

Add to `config.ini` or pass on the command line:

```ini
plugin = sysio::trace_api_plugin
trace-dir = traces
```

Or via CLI:

```bash
nodeop --plugin sysio::trace_api_plugin --trace-dir /var/lib/nodeop/traces
```

The plugin also requires `chain_plugin` and `http_plugin` (both loaded by
default).

---

## Configuration options

| Option | Default | Description |
|--------|---------|-------------|
| `trace-dir` | `traces` | Directory for trace files. Relative paths are resolved from the node's data directory. |
| `trace-slice-stride` | `10000` | Number of blocks per slice file. Larger values reduce file count but increase the amount of data re-scanned when a single slice is accessed. |
| `trace-minimum-irreversible-history-blocks` | `-1` | Blocks past LIB to retain before old slices can be auto-deleted. `-1` disables automatic deletion (keep forever). |
| `trace-minimum-uncompressed-irreversible-history-blocks` | `-1` | Blocks past LIB to keep uncompressed. Slices older than this threshold are transparently compressed. `-1` disables automatic compression. |
| `trace-max-query-limit` | `1000` | Maximum number of results a single `get_actions` or `get_token_transfers` request may return. Client-supplied `limit` values are clamped to this. Set to `-1` to remove the server-side cap entirely. |

### Recommended production settings

```ini
plugin = sysio::trace_api_plugin
trace-dir = /var/lib/nodeop/traces
trace-slice-stride = 10000
# Keep 2 weeks of uncompressed data (~2M blocks/day = ~28M blocks)
trace-minimum-uncompressed-irreversible-history-blocks = 28000000
# Keep 1 year total (compressed)
trace-minimum-irreversible-history-blocks = 365000000
```

For a full-history archive node omit or set both retention options to `-1`.

---

## On-disk layout

All files live inside `trace-dir`. The directory is monitored by
`resource_monitor_plugin` when that plugin is loaded.

### Slice files

Blocks are grouped into contiguous slices of `trace-slice-stride` blocks
each. Each slice is represented by four files that share a common range
suffix `<start>-<end>` (zero-padded to 10 digits):

| File | Description |
|------|-------------|
| `trace_<start>-<end>.log` | Serialized `block_trace_v0` records (action data). |
| `trace_index_<start>-<end>.log` | Append-only metadata log of `block_entry_v0` and `lib_entry_v0` records. Source of truth; used as a fallback for `get_block` and to track LIB advancement within the slice. |
| `trace_blk_idx_<start>-<end>.log` | Block-offset sidecar (see below).  Enables O(1) `get_block` lookups regardless of the block's position within the slice. |
| `trace_trx_idx_<start>-<end>.log` | Transaction-id hash index (see below). |

When a slice is compressed the trace file is replaced by:

| File | Description |
|------|-------------|
| `trace_<start>-<end>.clog` | zlib-compressed trace data with embedded seek points for random access. |

The index and trx_id index files are not compressed.

**Example** (10 000-block stride, blocks 0–29 999):

```
traces/
  trace_0000000000-0000010000.log
  trace_index_0000000000-0000010000.log
  trace_blk_idx_0000000000-0000010000.log
  trace_trx_idx_0000000000-0000010000.log
  trace_0000010000-0000020000.log
  trace_index_0000010000-0000020000.log
  trace_blk_idx_0000010000-0000020000.log
  trace_trx_idx_0000010000-0000020000.log
  trace_0000020000-0000030000.clog        <- compressed
  trace_index_0000020000-0000030000.log
  trace_blk_idx_0000020000-0000030000.log
  trace_trx_idx_0000020000-0000030000.log
  abi_store.log
```

### Block-offset index

`trace_blk_idx_<start>-<end>.log` is a flat fixed-size array of 64-bit
trace-log offsets, one entry per block in the slice, used by
`/v1/trace_api/get_block` for O(1) block lookups.

- **Header** (16 bytes): magic `BLIX`, version 1, slice width, reserved.
- **Slots** (8 bytes each): `offset + 1` into `trace_<start>-<end>.log`,
  or 0 when the slot is empty.  The `+1` encoding reserves 0 as an empty
  sentinel since a block's trace data can legitimately live at offset 0.

The sidecar is written synchronously alongside the metadata log as each
block is persisted.  Forks that re-apply the same block number overwrite
the slot naturally.  If the sidecar is missing or reports an empty slot,
`get_block` falls back to scanning the metadata log.

### Transaction-id index

`trace_trx_idx_<start>-<end>.log` is a compact open-addressing hash table
(load factor ≤ 0.5, linear probing) that maps a 64-bit prefix of a
transaction SHA-256 to the block number containing that transaction.

- **Header** (16 bytes): magic `TRIX`, version 1, bucket_count, reserved.
- **Buckets** (16 bytes each): `prefix64 (u64)` + `block_num (u32)` +
  `reserved (u32)`. Empty slots have `block_num == 0`.

The index is built once per slice when the slice's last block becomes
irreversible.  Queries against `/v1/trace_api/get_transaction_trace` use
this index for O(1) `trx_id → block_num` resolution instead of scanning
the chain.

### ABI store

`abi_store.log` is a single file that persists the ABI published by each
contract account across all `setabi` transactions observed since the node
started (or since the file was first written).

Format:

```
Header     (16 bytes): magic "ABIB", version 1, entry_count, reserved
Index      (entry_count × 24 bytes, sorted account ASC, global_seq ASC):
             account(u64) | global_seq(u64) | blob_offset(u32) | blob_size(u32)
Blob area  (variable): raw fc::raw-packed abi_def bytes in index order
```

To find the ABI for contract `A` in effect at `global_seq Q`: binary-search
the index for the last entry where `account == A && global_seq <= Q`, then
read the referenced blob.

The file is written atomically (write to `.tmp`, then rename) after each
block.  On node restart it is loaded into the writer so previously captured
ABIs survive across restarts.

---

## Startup continuity check

On plugin startup the trace store's recorded block range is compared against
the chain's current head.  Three outcomes are logged:

| Situation | Log level | Description |
|-----------|-----------|-------------|
| No prior trace data | `info` | Fresh start; tracing begins at the current head. |
| Snapshot restore detected | `warning` | The snapshot skips blocks already in the trace store, creating a gap. Trace data for the skipped range is inaccessible. |
| Normal restart | `info` | Store is contiguous with the chain head; tracing resumes normally. |

A continuity gap does not prevent the node from running, but `get_block` and
`get_transaction_trace` requests for block numbers inside the gap will return
404.

---

## ABI decoding

When serving any trace endpoint the plugin attempts to decode the raw `data`
and `return_value` bytes of each action using the ABI captured in
`abi_store.log`.

- The lookup key is `(account, global_sequence)` — the ABI that was in
  effect when that specific action executed is used, not the current ABI.
- If the ABI is unavailable (contract not yet captured, ABI store missing,
  or the action predates ABI capture), the fields are returned as raw hex
  strings.
- Decoding failures are soft: they are logged at `debug` level and the
  response falls back to raw hex instead of returning HTTP 500.  This
  prevents a malformed ABI in one contract from breaking queries for
  unrelated actions in the same block.

When decoded `params` and `return_data` are present they appear alongside the
raw `data` and `return_value` hex fields.

---

## HTTP API reference

All endpoints accept `POST` with a JSON body.  The base URL is
`/v1/trace_api/`.

---

### get_block

Retrieve the full action trace for a single block.

**Endpoint:** `POST /v1/trace_api/get_block`

**Request:**

```json
{ "block_num": 1000 }
```

**Response (200):**

```json
{
  "id": "000003e8...",
  "number": 1000,
  "previous_id": "000003e7...",
  "status": "irreversible",
  "timestamp": "2025-01-01T00:05:00.000Z",
  "producer": "bp.one",
  "transaction_mroot": "0000...0000",
  "finality_mroot":    "0000...0000",
  "transactions": [
    {
      "id": "abcd1234...",
      "block_num": 1000,
      "block_time": "2025-01-01T00:05:00.000Z",
      "producer_block_id": "000003e8...",
      "actions": [
        {
          "action_ordinal": 1,
          "creator_action_ordinal": 0,
          "closest_unnotified_ancestor_action_ordinal": 0,
          "global_sequence": 12345,
          "recv_sequence": 55,
          "auth_sequence": [["alice", 42]],
          "code_sequence": 3,
          "abi_sequence": 3,
          "receiver": "sysio.token",
          "account": "sysio.token",
          "name": "transfer",
          "authorization": [{ "actor": "alice", "permission": "active" }],
          "data": "0000000000855c34...",
          "return_value": "",
          "account_ram_deltas": [],
          "cpu_usage_us": 100,
          "net_usage": 16,
          "params": {
            "from": "alice",
            "to": "bob",
            "quantity": "1.0000 SYS",
            "memo": "payment"
          }
        }
      ],
      "cpu_usage_us": 200,
      "net_usage_words": 16,
      "signatures": ["SIG_K1_..."],
      "transaction_header": {
        "expiration": "2025-01-01T00:05:30",
        "ref_block_num": 999,
        "ref_block_prefix": 12345678,
        "max_net_usage_words": 0,
        "max_cpu_usage_ms": 0,
        "delay_sec": 0
      }
    }
  ]
}
```

**Error responses:**

| Code | Condition |
|------|-----------|
| 400 | `block_num` missing or not a number |
| 404 | Block not found in trace store |

---

### get_transaction_trace

Retrieve the trace for a single transaction by its ID.

**Endpoint:** `POST /v1/trace_api/get_transaction_trace`

**Request:**

```json
{ "id": "abcd1234ef567890abcd1234ef567890abcd1234ef567890abcd1234ef567890" }
```

The block number is resolved via the per-slice `trx_id` index (O(1)); no
block scanning is performed.

**Response (200):** A single transaction object in the same shape as one
element of `transactions` in the `get_block` response (includes `id`,
`block_num`, `block_time`, `actions`, `status`, etc.).

**Error responses:**

| Code | Condition |
|------|-----------|
| 400 | `id` missing or malformed |
| 404 | Transaction not found in index, or block trace not found |

---

### get_actions

Paginated search over action traces in a block range, with optional filters
on receiver, account (contract code), and action name.

**Endpoint:** `POST /v1/trace_api/get_actions`

**Request fields:**

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `block_num_start` | uint32 | `0` | First block to scan (inclusive). |
| `block_num_end` | uint32 | `UINT32_MAX` | Last block to scan (inclusive). |
| `receiver` | string | *(any)* | Filter: only actions where `receiver` matches. |
| `account` | string | *(any)* | Filter: only actions where `account` (code account) matches. |
| `action` | string | *(any)* | Filter: only actions where the action name matches. |
| `after_global_seq` | uint64 | `0` | Pagination cursor — skip actions with `global_sequence <= this`. |
| `limit` | uint32 | `100` | Maximum results to return (clamped to `trace-max-query-limit`, default 1000). |

**Request example:**

```json
{
  "block_num_start": 1,
  "block_num_end": 10000,
  "account": "sysio.token",
  "action": "transfer",
  "limit": 50
}
```

**Response (200):**

```json
{
  "actions": [
    {
      "action_ordinal": 1,
      "creator_action_ordinal": 0,
      "closest_unnotified_ancestor_action_ordinal": 0,
      "global_sequence": 101,
      "recv_sequence": 55,
      "auth_sequence": [["alice", 42]],
      "code_sequence": 3,
      "abi_sequence": 3,
      "receiver": "sysio.token",
      "account": "sysio.token",
      "name": "transfer",
      "authorization": [{"actor": "alice", "permission": "active"}],
      "data": "0000000000855c34...",
      "return_value": "",
      "account_ram_deltas": [],
      "cpu_usage_us": 100,
      "net_usage": 16,
      "params": {
        "from": "alice",
        "to": "bob",
        "quantity": "1.0000 SYS",
        "memo": "payment"
      },
      "trx_id": "abcd1234...",
      "block_num": 1000,
      "block_time": "2025-01-01T00:05:00.000Z",
      "producer_block_id": "000003e8..."
    }
  ],
  "more": true,
  "last_global_seq": 101
}
```

**Response fields:**

| Field | Description |
|-------|-------------|
| `actions` | Array of matching action objects. |
| `more` | `true` if there are more results beyond `limit`; use `last_global_seq` as the cursor for the next page. |
| `last_global_seq` | `global_sequence` of the last returned action; pass as `after_global_seq` on the next request. |

**Action object fields:**

| Field | Description |
|-------|-------------|
| `action_ordinal` | Position of this action in the transaction's execution tree (1-based). |
| `creator_action_ordinal` | Ordinal of the action that created this one (0 for top-level actions). |
| `closest_unnotified_ancestor_action_ordinal` | Ordinal of the nearest ancestor whose receiver has not already been notified. |
| `global_sequence` | Monotonically increasing sequence number across the entire chain. |
| `recv_sequence` | Per-receiver sequence number. |
| `auth_sequence` | Array of `[actor, sequence]` pairs, one per authorizing account. |
| `code_sequence` | Number of times the contract's code has been updated up to this action. |
| `abi_sequence` | Number of times the contract's ABI has been updated up to this action. |
| `receiver` | The account that received (and may have processed) the action. |
| `account` | The contract account whose code was executed. |
| `name` | The action name. |
| `authorization` | Array of `{actor, permission}` objects. |
| `data` | Raw action payload as hex. |
| `return_value` | Raw return value as hex (empty string when none). |
| `account_ram_deltas` | Array of `{account, delta}` objects capturing RAM allocation changes. |
| `cpu_usage_us` | Producer-set CPU in microseconds (present only for input/top-level actions). |
| `net_usage` | Producer-set NET usage in bytes (present only for input/top-level actions). |
| `params` | ABI-decoded action payload (omitted when ABI unavailable). |
| `return_data` | ABI-decoded return value (omitted when ABI unavailable or no return type defined). |
| `trx_id` | ID of the transaction that contains this action. |
| `block_num` | Block number. |
| `block_time` | Block timestamp (ISO-8601). |
| `producer_block_id` | Block ID as reported by the producer (null for pending blocks). |

**Error responses:**

| Code | Condition |
|------|-----------|
| 400 | Malformed request body, or `block_num_start > block_num_end`. |

#### Receiver vs account

Every SYSIO action has two account fields:

- **`account`** — the contract whose code is executed (always the contract
  that defines the action).
- **`receiver`** — the account receiving the notification. For the
  originating action `receiver == account`. For inline notifications sent to
  other accounts, `receiver != account`.

A `sysio.token::transfer` produces two action traces in the store:

| global_seq | receiver | account |
|-----------|----------|---------|
| N | `sysio.token` | `sysio.token` | ← original execution |
| N+1 | `bob` | `sysio.token` | ← inline notification to recipient |

Filter by `receiver="sysio.token"` to get exactly one entry per transfer.
Filter by `account="sysio.token"` to get both.

---

### get_token_transfers

Convenience wrapper around `get_actions` preset to return only token
`transfer` actions for a given contract.  Uses
`receiver=account=token_contract, action=transfer` so exactly one entry per
transfer is returned (the canonical execution; inline notification copies
to recipients are excluded).

**Endpoint:** `POST /v1/trace_api/get_token_transfers`

**Request fields:**

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `token_contract` | string | `sysio.token` | Contract account to filter on. |
| `block_num_start` | uint32 | `0` | First block to scan (inclusive). |
| `block_num_end` | uint32 | `UINT32_MAX` | Last block to scan (inclusive). |
| `after_global_seq` | uint64 | `0` | Pagination cursor. |
| `limit` | uint32 | `100` | Maximum results (clamped to 1000). |

**Request example:**

```json
{
  "token_contract": "sysio.token",
  "block_num_start": 1,
  "block_num_end": 50000,
  "limit": 100
}
```

**Response (200):**

```json
{
  "transfers": [
    {
      "global_sequence": 101,
      "receiver": "sysio.token",
      "account": "sysio.token",
      "name": "transfer",
      "authorization": [{"actor": "alice", "permission": "active"}],
      "data": "...",
      "return_value": "",
      "params": {
        "from": "alice",
        "to": "bob",
        "quantity": "1.0000 SYS",
        "memo": "payment"
      },
      "trx_id": "abcd1234...",
      "block_num": 1000,
      "block_time": "2025-01-01T00:05:00.000Z",
      "producer_block_id": "000003e8..."
    }
  ],
  "more": false,
  "last_global_seq": 101
}
```

The response uses `"transfers"` as the array key instead of `"actions"`.
`get_token_transfers` returns a **slim subset** of the fields that `get_actions` returns — it omits execution-tree ordinals
(`action_ordinal`, `creator_action_ordinal`, `closest_unnotified_ancestor_action_ordinal`), per-receipt sequence numbers
(`recv_sequence`, `auth_sequence`, `code_sequence`, `abi_sequence`), `account_ram_deltas`, and the resource usage fields
(`cpu_usage_us`, `net_usage`). These are rarely useful for token-transfer exchange/indexer workflows. If you need them,
call `get_actions` with `receiver=account=<token_contract>, action=transfer` instead.

**Error responses:**

| Code | Condition |
|------|-----------|
| 400 | Malformed request body, or `block_num_start > block_num_end`. |

---

## Pagination guide

All scan endpoints (`get_actions`, `get_token_transfers`) use a forward
cursor based on `global_sequence`:

```
# Page 1
POST /v1/trace_api/get_actions
{ "account": "sysio.token", "action": "transfer",
  "block_num_start": 1, "block_num_end": 1000000,
  "limit": 100 }

# Page 2 — use last_global_seq from page 1 response
POST /v1/trace_api/get_actions
{ "account": "sysio.token", "action": "transfer",
  "block_num_start": 1, "block_num_end": 1000000,
  "after_global_seq": <last_global_seq from page 1>,
  "limit": 100 }
```

Continue until `"more": false`.

Notes:
- `block_num_start` and `block_num_end` must remain the same across pages
  of the same scan.
- The cursor is global_sequence-based, not block-based, so pages never
  overlap or skip actions even when multiple actions share a block.
- An empty `actions` array with `"more": false` means no matching actions
  exist in the range.

---

## Exchange / indexer integration guide

### Removing the per-request limit

Exchanges running their own private nodeop should set
`trace-max-query-limit = -1` in `config.ini` to remove the server-side cap
entirely.  This allows a single request to return all transfers in a block
range without pagination, which simplifies indexing pipelines that process
blocks in bulk.

```ini
# config.ini — safe on a private/trusted node
trace-max-query-limit = -1
```

With this setting, the client controls page size through the `limit` field
(or omits it to get all matching results).  Do **not** set this on a public
RPC node — unbounded scans over large block ranges can exhaust server memory.

### Detecting deposits

To find all incoming transfers to your account (`exchange1111`) on
`sysio.token`:

```bash
# Scan the last 10000 blocks
curl -s -X POST http://127.0.0.1:8888/v1/trace_api/get_token_transfers \
  -H 'Content-Type: application/json' \
  -d '{
    "block_num_start": 100000,
    "block_num_end":   110000,
    "limit": 1000
  }' | jq '.transfers[] | select(.params.to == "exchange1111")'
```

Using `get_token_transfers` with no additional filter returns one entry per
transfer across all accounts.  Filter `params.to` client-side or scan with
`get_actions` and post-filter as needed.

The `receiver="sysio.token"` preset guarantees that each on-chain transfer
appears exactly once regardless of how many accounts were notified inline.

### Non-system token contracts

```bash
curl -s -X POST http://127.0.0.1:8888/v1/trace_api/get_token_transfers \
  -H 'Content-Type: application/json' \
  -d '{ "token_contract": "mytoken1111", "block_num_start": 1, "block_num_end": 9999999 }'
```

### Watching for smart-contract activity

```bash
# All actions executed by a DEX contract in blocks 5000–6000
curl -s -X POST http://127.0.0.1:8888/v1/trace_api/get_actions \
  -H 'Content-Type: application/json' \
  -d '{ "account": "my.dex", "block_num_start": 5000, "block_num_end": 6000 }'
```

### Inline actions

Inline actions (e.g. `eosio.token::transfer` called from inside another
contract) appear as separate entries in `get_actions` results with their own
`global_sequence` values.  The parent and child share the same `trx_id`.
Use `get_block` or `get_transaction_trace` if you need the full causal tree.

---

## Maintenance and retention

### Automatic retention

Set `trace-minimum-irreversible-history-blocks` to the number of blocks you
want to retain past LIB.  Slices that fall entirely before
`LIB - retention_blocks` are eligible for deletion.

Set `trace-minimum-uncompressed-irreversible-history-blocks` similarly to
control the compression boundary.  Slices are still accessible when
compressed but random-access reads may be slightly slower.

### Manual deletion

Stop nodeop before deleting trace files manually.  Delete the full set of
slice files for a given range (`trace_*`, `trace_index_*`,
`trace_blk_idx_*`, `trace_trx_idx_*`).  Partial deletion (e.g. deleting
only the trace file but not the index) will cause `bad_data_exception`
errors on the next startup.

### Snapshot restores

After restoring from a snapshot the trace store's recorded range may not
match the chain's new head.  The plugin logs a warning at startup and
continues. If the gap is large, consider either:

1. Copying the trace directory from a full-history node that has the missing
   range, or
2. Deleting the trace directory entirely and re-syncing from genesis (only
   practical for newer chains).

### abi_store.log

`abi_store.log` is rewritten after every block. If it is lost or corrupted,
delete it and restart nodeop.  The plugin will rebuild it as new `setabi`
transactions are applied; historical ABI lookup for events before the loss
will fall back to raw hex.

---

## Plugin variants

Two plugin classes are registered:

| Class | Purpose |
|-------|---------|
| `trace_api_plugin` | Full plugin: captures trace data AND exposes HTTP endpoints. Use this in production. |
| `trace_api_rpc_plugin` | HTTP-only: exposes endpoints against a trace directory written by another node. Use when separating the writer node from the query node. |

Both accept the same configuration options.

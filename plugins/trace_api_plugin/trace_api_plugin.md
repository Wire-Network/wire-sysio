# trace_api_plugin

Full-history action trace plugin for Wire nodeop. Captures every action
trace as blocks are applied, persists them in a structured on-disk store,
and exposes HTTP endpoints for querying traces, transactions, actions, and
token transfers.

---

## Table of contents

1. [Overview](#overview)
2. [Quick start](#quick-start)
3. [Configuration options](#configuration-options)
4. [HTTP API reference](#http-api-reference)
   - [get_block](#get_block)
   - [get_transaction_trace](#get_transaction_trace)
   - [get_actions](#get_actions)
   - [get_token_transfers](#get_token_transfers)
5. [Pagination guide](#pagination-guide)
6. [Exchange / indexer integration guide](#exchange--indexer-integration-guide)
7. [ABI decoding](#abi-decoding)
8. [Operations](#operations)
   - [Maintenance and retention](#maintenance-and-retention)
   - [Startup continuity check](#startup-continuity-check)
   - [Plugin variants](#plugin-variants)
9. [Implementation details](#implementation-details)
   - [On-disk layout](#on-disk-layout)
   - [ABI capture mechanics](#abi-capture-mechanics)

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
  `setabi` transaction was applied is captured in `abi_log.log`.  Queries
  decode `data` and `return_value` fields using the historically correct
  ABI, not the current on-chain ABI.
- **O(1) transaction lookup** — a per-slice hash index maps `trx_id` to
  `block_num` so `get_transaction_trace` does not scan the chain.

---

## Quick start

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
| `trace-slice-stride` | `10000` | Number of blocks per slice file. Must be in `[1, 1000000]`. Larger values reduce file count but bloat the block-offset sidecar's per-slice pre-allocation (`stride * 8` bytes, sparse) and stress the per-slice trx_id hash index (rejected if it would need more than 2^28 buckets). |
| `trace-minimum-irreversible-history-blocks` | `-1` | Blocks past LIB to retain before old slices can be auto-deleted. `-1` disables automatic deletion (keep forever). |
| `trace-minimum-uncompressed-irreversible-history-blocks` | `-1` | Blocks past LIB to keep uncompressed. Slices older than this threshold are transparently compressed. `-1` disables automatic compression. |
| `trace-max-block-range` | `1000` | Maximum number of blocks scanned by a single `get_actions` or `get_token_transfers` request. Must be in `[1, 10000]`. `block_num_end` is silently clamped to `block_num_start + trace-max-block-range - 1` when a request asks for more. The response envelope always reports the actual range scanned. |

### Recommended production settings

```ini
plugin = sysio::trace_api_plugin
trace-dir = /var/lib/nodeop/traces
trace-slice-stride = 10000
# Keep 2 weeks of uncompressed data (~2M blocks/day = ~28M blocks)
trace-minimum-uncompressed-irreversible-history-blocks = 28000000
# Keep 1 year total (compressed)
trace-minimum-irreversible-history-blocks = 365000000
# Widen per-request scan window for private/trusted nodes
trace-max-block-range = 5000
```

For a full-history archive node omit or set both retention options to `-1`.

---

## HTTP API reference

All endpoints accept `POST` with a JSON body. The base URL is
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
| 500 | Internal error reading the trace store |

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
element of `transactions` in the `get_block` response (`id`, `block_num`,
`block_time`, `producer_block_id`, `actions`, `cpu_usage_us`,
`net_usage_words`, `signatures`, `transaction_header`).

**Error responses:**

| Code | Condition |
|------|-----------|
| 400 | `id` missing or malformed |
| 404 | Transaction not found in index, or block trace not found |
| 500 | Internal error reading the trace store |

---

### get_actions

Paginated search over action traces in a block range, with optional filters
on receiver, account (contract code), and action name.

**Endpoint:** `POST /v1/trace_api/get_actions`

**Request fields:**

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `block_num_start` | uint32 | `0` | First block to scan (inclusive). |
| `block_num_end` | uint32 | `UINT32_MAX` | Last block to scan (inclusive). Silently clamped server-side to `block_num_start + trace-max-block-range - 1`. The response reports the actual range scanned. |
| `receiver` | string | *(any)* | Filter: match `act.receiver`. |
| `account` | string | *(any)* | Filter: match `act.account` (the contract whose code ran). |
| `action` | string | *(any)* | Filter: match action name. |
| `include_notifications` | bool | `false` | Notification handling (see below). |

**Notifications (`include_notifications`):**

- `false` (default): when exactly one of `receiver` / `account` is
  specified, the other is implicitly constrained to the same value — you
  get the canonical execution only (`act.account == act.receiver == filter_value`).
- `true`: only the explicitly-specified filters apply. Notifications where
  `act.account != act.receiver` are included.

When both `receiver` and `account` are explicitly specified, the flag has
no effect — both filters apply literally.

Returned actions within a transaction are sorted by `global_sequence`
(execution order), matching the behavior of `get_block` and chain_plugin's
`push_transaction` response. See the [Pagination guide](#pagination-guide)
for the cursor pattern.

**Request example:**

```json
{
  "block_num_start": 1,
  "block_num_end": 10000,
  "account": "sysio.token",
  "action": "transfer"
}
```

**Response (200):**

```json
{
  "block_num_start": 1,
  "block_num_end": 1000,
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
  ]
}
```

`block_num_start` and `block_num_end` on the response reflect the actual
range scanned (after clamping), so a client can detect a clamp and
resume pagination from `block_num_end + 1`.

**Response fields:**

| Field | Description |
|-------|-------------|
| `block_num_start` | First block number actually scanned. |
| `block_num_end` | Last block number actually scanned (after clamping). |
| `actions` | Array of matching action objects, ordered by `(block_num, global_sequence)`. |

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
| `params` | ABI-decoded action payload (omitted when ABI unavailable or decode failed). |
| `return_data` | ABI-decoded return value (omitted when ABI unavailable or no return type defined). |
| `decode_error` | Error message; present only when ABI decoding failed and the response falls back to raw hex. |
| `trx_id` | ID of the transaction that contains this action. |
| `block_num` | Block number. |
| `block_time` | Block timestamp (ISO-8601). |
| `producer_block_id` | Block ID as reported by the producer (null for pending blocks). |

**Error responses:**

| Code | Condition |
|------|-----------|
| 400 | Malformed request body, or `block_num_start > block_num_end`. |
| 500 | Internal error reading the trace store. |

#### Receiver vs account

Every SYSIO action has two account fields:

- **`account`** — the contract whose code is executed (always the contract
  that defines the action).
- **`receiver`** — the account receiving the notification. For the
  originating action `receiver == account`. For inline notifications sent
  to other accounts, `receiver != account`.

A `sysio.token::transfer` from alice to bob produces three action traces
in the store:

| global_seq | account | receiver | role |
|-----------|---------|----------|------|
| N | `sysio.token` | `sysio.token` | Canonical execution |
| N+1 | `sysio.token` | `alice` | Notification to sender |
| N+2 | `sysio.token` | `bob` | Notification to recipient |

The default query (no `include_notifications`) implicitly constrains
`receiver == account` when you specify one of them, returning only the
canonical row. To see notifications, set `include_notifications: true`.

---

### get_token_transfers

Convenience wrapper around `get_actions` preset to return only token
`transfer` actions for a given contract. Uses
`receiver = account = token_contract, action = "transfer"` so exactly one
entry per transfer is returned (the canonical execution; inline
notifications are excluded).

**Endpoint:** `POST /v1/trace_api/get_token_transfers`

**Request fields:**

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `token_contract` | string | `sysio.token` | Contract account to filter on. |
| `block_num_start` | uint32 | `0` | First block to scan (inclusive). |
| `block_num_end` | uint32 | `UINT32_MAX` | Last block to scan (inclusive). Silently clamped to `block_num_start + trace-max-block-range - 1`. |

**Request example:**

```json
{
  "token_contract": "sysio.token",
  "block_num_start": 1,
  "block_num_end": 50000
}
```

**Response (200):**

```json
{
  "block_num_start": 1,
  "block_num_end": 1000,
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
  ]
}
```

The response uses `"transfers"` as the array key instead of `"actions"`.
`get_token_transfers` returns a **slim subset** of the fields that
`get_actions` returns — it omits execution-tree ordinals
(`action_ordinal`, `creator_action_ordinal`,
`closest_unnotified_ancestor_action_ordinal`), per-receipt sequence
numbers (`recv_sequence`, `auth_sequence`, `code_sequence`,
`abi_sequence`), `account_ram_deltas`, and the resource usage fields
(`cpu_usage_us`, `net_usage`). These are rarely useful for token-transfer
exchange/indexer workflows. If you need them, call `get_actions` with
`receiver = account = <token_contract>, action = "transfer"` instead.

**Error responses:**

| Code | Condition |
|------|-----------|
| 400 | Malformed request body, or `block_num_start > block_num_end`. |
| 500 | Internal error reading the trace store. |

---

## Pagination guide

`get_actions` and `get_token_transfers` cap the per-request scan window at
`trace-max-block-range` blocks (default 1000). `block_num_end` is silently
clamped to `block_num_start + trace-max-block-range - 1` if the client asks
for more. The response always includes the actual `block_num_start` and
`block_num_end` scanned so the client can page reliably.

Within that window, ALL matching actions are returned — there is no
per-result limit and no in-window cursor.

To page across a wide range, advance `block_num_start` by the response's
`block_num_end + 1` each call:

```
# Page 1: blocks 1..1000 (assuming trace-max-block-range = 1000)
POST /v1/trace_api/get_actions
{ "account": "sysio.token", "action": "transfer",
  "block_num_start": 1, "block_num_end": 1000000 }

# Response: { "block_num_start": 1, "block_num_end": 1000, "actions": [...] }

# Page 2: blocks 1001..2000
POST /v1/trace_api/get_actions
{ "account": "sysio.token", "action": "transfer",
  "block_num_start": 1001, "block_num_end": 1000000 }
```

Continue until `block_num_end` returned by the server equals the
requested `block_num_end` (no clamp happened), or until you catch up to
the chain head. Use `get_block` or out-of-band head-block knowledge to
know when to stop.

Notes:
- Within each transaction, actions are sorted by `global_sequence`
  (execution order, not schedule order). See "Receiver vs account" for
  why this matters when an action queues both inlines and notifications.
- The maximum supported `trace-max-block-range` is 10,000. Raise it via
  `config.ini` on private/trusted nodes; public nodes should typically
  leave it at the default.

---

## Exchange / indexer integration guide

### Widening the per-request scan window

Exchanges running their own private nodeop can raise
`trace-max-block-range` in `config.ini` to reduce the number of round
trips per backfill:

```ini
# config.ini — safe on a private/trusted node
trace-max-block-range = 5000
```

Values up to 10,000 are accepted. Larger windows produce larger responses
(which hit `http-max-response-time-ms` and `http-max-body-size` limits)
and tie up an HTTP thread for longer on busy contracts. Pick the largest
window that still returns in a reasonable time for your typical contract
activity.

### Detecting deposits

To find all incoming transfers to your account (`exchange1111`) on
`sysio.token`:

```bash
curl -s -X POST http://127.0.0.1:8888/v1/trace_api/get_token_transfers \
  -H 'Content-Type: application/json' \
  -d '{
    "block_num_start": 100000,
    "block_num_end":   100999
  }' | jq '.transfers[] | select(.params.to == "exchange1111")'
```

`get_token_transfers` with no additional filter returns one entry per
transfer across all accounts. Filter `params.to` client-side, or scan
with `get_actions` and post-filter as needed.

The `receiver = account` preset guarantees that each on-chain transfer
appears exactly once regardless of how many accounts were notified
inline.

### Non-system token contracts

```bash
curl -s -X POST http://127.0.0.1:8888/v1/trace_api/get_token_transfers \
  -H 'Content-Type: application/json' \
  -d '{ "token_contract": "mytoken1111", "block_num_start": 1, "block_num_end": 9999 }'
```

### Watching for smart-contract activity

```bash
# All canonical actions executed by a DEX contract in blocks 5000–6000
curl -s -X POST http://127.0.0.1:8888/v1/trace_api/get_actions \
  -H 'Content-Type: application/json' \
  -d '{ "account": "my.dex", "block_num_start": 5000, "block_num_end": 6000 }'

# Same, but including inline notifications the DEX sent to other accounts
curl -s -X POST http://127.0.0.1:8888/v1/trace_api/get_actions \
  -H 'Content-Type: application/json' \
  -d '{ "account": "my.dex", "block_num_start": 5000, "block_num_end": 6000, "include_notifications": true }'
```

### Inline actions

Inline actions (e.g. `sysio.token::transfer` called from inside another
contract) appear as separate entries in `get_actions` results with their
own `global_sequence` values. The parent and child share the same
`trx_id`. Use `get_block` or `get_transaction_trace` if you need the full
causal tree.

---

## ABI decoding

When serving any trace endpoint the plugin attempts to decode the raw
`data` and `return_value` bytes of each action using the ABI captured in
`abi_log.log` at the point that action executed.

- The ABI in effect when an action ran is used — not the current on-chain
  ABI. This matters when a contract calls `setabi` mid-history: older
  actions decode against the older schema.
- If the ABI is unavailable (contract never captured, ABI log missing, or
  the action predates ABI capture on this node), `data` and
  `return_value` are returned as raw hex.
- If ABI decoding throws (e.g., a schema/data mismatch in one contract),
  the response includes a `decode_error` field and falls back to raw hex
  for that action only. Unrelated actions in the same block are
  unaffected.

When decoded, `params` and `return_data` appear alongside the raw `data`
and `return_value` hex fields.

See [ABI capture mechanics](#abi-capture-mechanics) for how the ABI log
is populated and the edge cases around same-transaction `setabi`.

---

## Operations

### Maintenance and retention

#### Automatic retention

Set `trace-minimum-irreversible-history-blocks` to the number of blocks
you want to retain past LIB. Slices that fall entirely before
`LIB - retention_blocks` are eligible for deletion.

Set `trace-minimum-uncompressed-irreversible-history-blocks` similarly to
control the compression boundary. Slices are still accessible when
compressed but random-access reads may be slightly slower.

#### Manual deletion

Stop nodeop before deleting trace files manually. Delete the full set of
slice files for a given range (`trace_*`, `trace_index_*`,
`trace_blk_idx_*`, `trace_trx_idx_*`). Partial deletion (e.g. removing
only the trace file but not the index) will cause `bad_data_exception`
errors on the next startup.

#### Snapshot restores

After restoring from a snapshot, the trace store's recorded range may
not match the chain's new head. If chain head is within the recorded
range, replay overlaps existing slices silently. If there's a gap, the
startup continuity check aborts (see below).

#### abi_log.log

`abi_log.log` is append-only. If it is lost or corrupted, delete it and
restart nodeop. The plugin will rebuild it as new `setabi` transactions
are applied or previously-unseen contracts are touched (lazy current-ABI
fetch). Historical ABI lookup for actions before the loss will fall back
to raw hex.

---

### Startup continuity check

On the first `block_start` signal after plugin startup, the trace
store's recorded block range is compared against the chain's current
head, and the plugin chooses one of four outcomes:

| Situation | Behavior |
|-----------|----------|
| No prior trace data (empty slice dir) | `info` log, fresh start; tracing begins at the current head. |
| Chain head is within `[first_recorded, last_recorded + 1]` (exact continuation OR overlap from a snapshot replay) | Silent — re-applied blocks naturally overwrite existing slice entries. |
| Chain head is **before** the first recorded block | Plugin throws; `error` log, **node shuts down**. |
| Chain head is **after** `last_recorded + 1` (forward gap) | Plugin throws; `error` log, **node shuts down**. |

The shutdown is intentional: a gap means the trace store is no longer a
faithful continuous record of chain history, and silently accepting it
would let `get_block` / `get_transaction_trace` return inconsistent data
for blocks on either side of the gap.

To recover, pick one:

- **Load a snapshot whose chain head is within the existing recorded
  range (or one block past it).** Replay covers the existing slice
  entries; tracing continues with no gap.
- **Copy the missing slice files from another node** that has the
  missing range, then restart.
- **Delete the trace directory and start fresh.** Tracing resumes from
  the current chain head; old block traces are lost.

The check fires only on the first `block_start` after the plugin loads,
so recovery actions take effect on the next startup.

---

### Plugin variants

Two plugin classes are registered:

| Class | Purpose |
|-------|---------|
| `trace_api_plugin` | Full plugin: captures trace data AND exposes HTTP endpoints. Use this in production. |
| `trace_api_rpc_plugin` | HTTP-only: exposes endpoints against a trace directory written by another node. Use when separating the writer node from the query node. |

Both accept the same configuration options.

---

## Implementation details

### On-disk layout

All files live inside `trace-dir`. The directory is monitored by
`resource_monitor_plugin` when that plugin is loaded.

#### Slice files

Blocks are grouped into contiguous slices of `trace-slice-stride` blocks
each. Each slice is represented by four files that share a common range
suffix `<start>-<end>` (zero-padded to 10 digits):

| File | Description |
|------|-------------|
| `trace_<start>-<end>.log` | Serialized `block_trace_v0` records (action data). |
| `trace_index_<start>-<end>.log` | Append-only metadata log of `block_entry_v0` and `lib_entry_v0` records. Source of truth; used as a fallback for `get_block` and to track LIB advancement within the slice. |
| `trace_blk_idx_<start>-<end>.log` | Block-offset sidecar. Enables O(1) `get_block` lookups regardless of the block's position within the slice. |
| `trace_trx_idx_<start>-<end>.log` | Transaction-id hash index. |

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
  abi_log.log
```

#### Block-offset index

`trace_blk_idx_<start>-<end>.log` is a flat fixed-size array of 64-bit
trace-log offsets, one entry per block in the slice, used by
`/v1/trace_api/get_block` for O(1) block lookups.

- **Header** (16 bytes): magic `BLIX`, version 1, slice width, reserved.
- **Slots** (8 bytes each): `offset + 1` into `trace_<start>-<end>.log`,
  or 0 when the slot is empty. The `+1` encoding reserves 0 as an empty
  sentinel since a block's trace data can legitimately live at offset 0.

The sidecar is written synchronously alongside the metadata log as each
block is persisted. Forks that re-apply the same block number overwrite
the slot naturally. If the sidecar is missing or reports an empty slot,
`get_block` falls back to scanning the metadata log.

#### Transaction-id index

`trace_trx_idx_<start>-<end>.log` is a compact open-addressing hash
table (load factor ≤ 0.5, linear probing) that maps a 64-bit prefix of
a transaction SHA-256 to the block number containing that transaction.

- **Header** (16 bytes): magic `TRIX`, version 1, bucket_count, reserved.
- **Buckets** (16 bytes each): `prefix64 (u64)` + `block_num (u32)` +
  `reserved (u32)`. Empty slots have `block_num == 0`.

On hash hit, the candidate block is scanned for a full trx_id match to
defeat 64-bit prefix collisions (a miss on the full compare re-probes
the hash table). The index is built once per slice when the slice's
last block becomes irreversible. Queries against
`/v1/trace_api/get_transaction_trace` use this index for O(1)
`trx_id → block_num` resolution.

#### ABI log

`abi_log.log` is an append-only file that persists the ABI published by
each contract account across all `setabi` transactions observed since
the node started (or since the file was first written).

Format:

```
Header     (16 bytes): magic "ABIL" (u32), version 1 (u32), reserved (u64)
Records (repeated until EOF):
             account    (u64)
             global_seq (u64)
             blob_size  (u64)
             blob_bytes (blob_size bytes)
             crc32      (u32) over (account, global_seq, blob_size, blob_bytes)
```

An in-memory index keyed by `(account, global_sequence)` is built at
startup by walking the file record-by-record and validating each CRC.
Runtime lookups go through the index; the matching blob is then read
from the file via `pread()`. Appends stream new records to the end of
the file under a mutex, with no rewrite of existing records.

Writes are not fsync'd; the on-disk tail may lose the last few records
on a kernel crash. On startup the recovery scan detects torn or
CRC-mismatched records and truncates the file at the first bad one —
any lost records are rebuilt the next time their contract is touched
(via an observed `setabi` or the lazy current-ABI fetch).

---

### ABI capture mechanics

ABI records enter `abi_log.log` through two paths:

1. **`setabi` observation** — every time a transaction contains a
   `sysio::setabi` action, the plugin decodes the action's payload and
   records `(target_account, setabi.global_sequence, abi_bytes)`. This
   gives exact ABI-version boundaries at the granularity of
   global_sequence.

2. **Lazy current-ABI fetch** — on the first action observed for an
   account that has no prior `abi_log` entry, the plugin reads the
   account's current ABI from the chain DB and records it at
   `global_sequence = 0`. The `0` is a sentinel meaning "ABI as of
   first observation; exact recorded sequence unknown." Lookups step
   back from the query `global_sequence` to the largest recorded entry
   ≤ query, so a 0 sentinel matches any action of that account that
   predates the first recorded real setabi.

#### Same-transaction `setabi` caveat

If the plugin observes a contract for the *first* time via a transaction
that *also* contains a `setabi` for that same contract, actions on that
contract which executed *before* the setabi within that transaction
cannot be decoded and are returned as raw hex. The pre-setabi ABI is no
longer reachable from the post-apply chain state (by the time the
applied_transaction signal fires, the chain DB already reflects the new
ABI), so the plugin deliberately does not record the post-apply (new)
ABI as the contract's pre-observation baseline — doing so would decode
pre-setabi actions with the wrong schema.

Once the contract has been observed at least once (via any earlier
transaction, or via a setabi-free transaction), later same-trx setabis
do not have this limitation: pre-setabi actions decode correctly with
the previously-recorded ABI.

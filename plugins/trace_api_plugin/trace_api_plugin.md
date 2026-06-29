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
| `trace-slice-stride` | `10000` | Number of blocks per slice file. Must be in `[1, 1000000]`. Larger values reduce file count but bloat the block-offset sidecar's per-slice pre-allocation (`stride * 8` bytes, sparse) and stress the per-slice trx_id hash index (rejected if it would need more than 2^28 buckets). Also bounds the worst-case scan cost of `get_actions` on a positive bloom probe - smaller strides mean less work per hit-slice at the cost of more sidecar files (see [Slice stride vs. query latency](#slice-stride-vs-query-latency)). Setting takes effect on nodeop restart; existing slices retain their old naming. |
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
| `block_num_end` | uint32 | `UINT32_MAX` | Last block to scan (inclusive). Silently clamped server-side to `block_num_start + trace-max-block-range - 1` AND to the last block actually recorded by this node, so the reported range never includes blocks that do not exist yet. The response reports the actual range scanned. |
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
      "producer_block_id": "000003e8...",
      "block_status": "irreversible",
      "trx_cpu_usage_us": 200,
      "trx_net_usage_words": 16
    }
  ]
}
```

`block_num_start` and `block_num_end` on the response reflect the actual
range scanned (after clamping), so a client can detect a clamp and
resume pagination from `block_num_end + 1`.  The reported `block_num_end`
is also reduced when a response reaches the `max_actions_per_response`
ceiling (see the [Pagination guide](#pagination-guide)); the resume rule
is identical.  Because the server also clamps to its last recorded block,
resuming at `block_num_end + 1` can never skip blocks that had not been
produced (or recorded) at the time of the request.  When nothing in the
requested window has been recorded yet, the response carries
`block_num_end == block_num_start - 1`
("nothing scanned") — retry the same `block_num_start` later.

**Response fields:**

| Field | Description |
|-------|-------------|
| `block_num_start` | First block number actually scanned. |
| `block_num_end` | Last block number actually scanned. Reduced below the requested end by the `trace-max-block-range` window, the last recorded block, or the `max_actions_per_response` (10,000) action ceiling — whichever stops the scan first. Resume the next page at `block_num_end + 1`. |
| `actions` | Array of matching action objects, ordered by `(block_num, global_sequence)`. Capped per response by `max_actions_per_response` (see the Pagination guide). |

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
| `cpu_usage_us` | Producer-set CPU in microseconds for this action (present only for input/top-level actions). |
| `net_usage` | Producer-set NET usage in bytes for this action (present only for input/top-level actions). |
| `params` | ABI-decoded action payload (omitted when ABI unavailable or decode failed). |
| `return_data` | ABI-decoded return value (omitted when ABI unavailable or no return type defined). |
| `decode_error` | Error message; present only when ABI decoding failed. The raw hex `data`/`return_value` fields are always emitted regardless of decode outcome. |
| `trx_id` | ID of the transaction that contains this action. |
| `block_num` | Block number. |
| `block_time` | Block timestamp (ISO-8601). |
| `producer_block_id` | Block ID as reported by the producer (null for pending blocks). |
| `block_status` | Finality of this action's block: `"irreversible"` once the block is at or before LIB, `"pending"` otherwise. Mirrors the `status` field on `get_block`. A pending block can later be promoted to irreversible as LIB advances; consumers that gate on finality must re-poll. Operators that only want to serve already-final data can run nodeop with `read-mode = irreversible`, which causes every block returned by trace_api to carry `"irreversible"`. |
| `trx_cpu_usage_us` | Parent transaction's total CPU in microseconds. |
| `trx_net_usage_words` | Parent transaction's total NET usage in words (`ceil(net_usage / 8)`). |

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
| `block_num_end` | uint32 | `UINT32_MAX` | Last block to scan (inclusive). Silently clamped to `block_num_start + trace-max-block-range - 1` and to the last recorded block (same semantics as `get_actions`). |

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
      "producer_block_id": "000003e8...",
      "block_status": "irreversible"
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
(action-level `cpu_usage_us` / `net_usage` and trx-level
`trx_cpu_usage_us` / `trx_net_usage_words`). These are rarely useful for token-transfer
exchange/indexer workflows. If you need them, call `get_actions` with
`receiver = account = <token_contract>, action = "transfer"` instead.

`block_status` IS retained -- exchanges crediting transfers need finality
just as much as general action consumers. See the `get_actions` field
table above for its semantics, including the irreversible-mode operator
note.

Pagination is identical to `get_actions`: the same `trace-max-block-range`
window and `max_actions_per_response` (10,000) ceiling apply, with matched
transfers counted toward the ceiling, and the response's `block_num_end`
drives the `block_num_end + 1` resume cursor.  See the
[Pagination guide](#pagination-guide).

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

Within that window the number of actions returned in one response is
bounded by a hard ceiling, `max_actions_per_response` (10,000). This
complements the block-window cap: the window bounds how many blocks are
scanned, the action ceiling bounds how many matching actions are decoded,
serialized, and held in memory, so a broad or unfiltered query over a
busy range cannot materialize an unbounded result set. The ceiling is a
hard-coded constant, not a nodeop setting — it cannot be raised or
disabled, so the bound holds on every node.

The ceiling is enforced at block boundaries: the scan stops once a fully
scanned block brings the running action total to the ceiling, and the
response's `block_num_end` is set to that last fully-scanned block. A
response is never split in the middle of a block, so it may exceed the
ceiling by at most one block's worth of matching actions (itself bounded
by consensus block limits). Resume at `block_num_end + 1` exactly as for
the block-window clamp — block-boundary truncation means resuming neither
skips nor duplicates actions.

The server therefore reduces the reported `block_num_end` below the
requested end for any of three reasons: the `trace-max-block-range`
window, the last block it has actually recorded, or the
`max_actions_per_response` ceiling.  Resuming at `block_num_end + 1` is
safe in all three cases: blocks that had not been produced (or had not
reached this node) when you asked are never reported as scanned, so they
cannot be skipped, and a truncated block is always reported either as
fully scanned or not at all.

To page across a wide range, advance `block_num_start` to the response's
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
requested `block_num_end` (no truncation happened).  The same loop
absorbs the action ceiling transparently: over a busy range a single
window may take several requests to cross, each resuming at the previous
`block_num_end + 1`.  When you catch up to the chain head, the server's
recorded-block clamp shrinks the reported window for you: a response with
`block_num_end < block_num_start` means nothing new is scannable yet —
wait and retry the same `block_num_start`.

Notes:
- Within each transaction, actions are sorted by `global_sequence`
  (execution order, not schedule order). See "Receiver vs account" for
  why this matters when an action queues both inlines and notifications.
- The maximum supported `trace-max-block-range` is 10,000. Raise it via
  `config.ini` on private/trusted nodes; public nodes should typically
  leave it at the default.
- A response carries at most `max_actions_per_response` (10,000) matching
  actions plus the overflow from the final block, then stops at that
  block boundary (reported via `block_num_end`).  This caps result volume
  independently of the block-window cap, so a busy receiver over a wide
  window is paged across multiple responses instead of returned all at
  once.
- Retention pruning (`trace-minimum-irreversible-history-blocks`) deletes
  old slices.  A query into a pruned range returns an empty result
  indistinguishable from "no matches" — indexers backfilling history must
  start within the node's retained range.

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

#### abi_log.log / abi_log.journal

`abi_log.log` is append-only. If it is lost or corrupted, delete it and
restart nodeop. The plugin will rebuild it as new `setabi` transactions
are applied or previously-unseen contracts are touched (lazy current-ABI
fetch). Historical ABI lookup for actions before the loss will fall back
to raw hex.

`abi_log.journal` durably mirrors the in-memory reversible overlay
(records for blocks above LIB) so they survive a restart; it is replayed
and then compacted at startup. If it is lost or corrupted, only the
not-yet-irreversible ABI records are affected (they fall back to raw hex
until their contract is touched again); the irreversible history in
`abi_log.log` is unaffected. It is safe to delete alongside `abi_log.log`.

**Operational note.** trace_api is a history/query plugin; run it only on
dedicated query nodes. **Never enable it on a block-producing node** (test
environments aside). A failed ABI write is fatal (the node shuts down
rather than record incomplete history — see [ABI log](#abi-log)), so a
node's liveness is tied to the writability of its `trace-dir`: a query node
that fills or loses its trace storage exits cleanly and resumes without
data loss once restarted with space — appropriate for a query node, and a
further reason never to couple this plugin to block production. (This
matches the existing startup continuity check, which also shuts the node
down on unrecoverable trace-data gaps.)

---

### Startup continuity check

On the first `block_start` signal after plugin startup, the trace
store's recorded block range is compared against the chain's current
head, and the plugin chooses one of five outcomes:

| Situation | Behavior |
|-----------|----------|
| No prior trace data (empty slice dir) | `info` log, fresh start; tracing begins at the current head. |
| Chain head is within `[first_recorded, last_recorded + 1]` (exact continuation OR overlap from a snapshot replay) | Silent — re-applied blocks naturally overwrite existing slice entries. |
| Chain head is **before** the first recorded block | Plugin throws; `error` log, **node shuts down**. |
| Chain head is **after** `last_recorded + 1` (forward gap) | Plugin throws; `error` log, **node shuts down**. |
| Index slice files are missing in the **middle** of the recorded range (deleted or partially copied) | Plugin throws; `error` log, **node shuts down**. Detected by filename contiguity, so it catches removed slices, not corrupted-in-place ones. |

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
each. Each slice is represented by five files that share a common range
suffix `<start>-<end>` (zero-padded to 10 digits):

| File | Description |
|------|-------------|
| `trace_<start>-<end>.log` | Serialized `block_trace_v0` records (action data). |
| `trace_index_<start>-<end>.log` | Append-only metadata log of `block_entry_v0` and `lib_entry_v0` records. Source of truth; used as a fallback for `get_block` and to track LIB advancement within the slice. |
| `trace_blk_idx_<start>-<end>.log` | Block-offset sidecar. Enables O(1) `get_block` lookups regardless of the block's position within the slice. |
| `trace_trx_idx_<start>-<end>.log` | Transaction-id hash index. |
| `trace_recv_bloom_<start>-<end>.log` | Per-slice bloom filter over action receivers and (receiver, action) pairs. `get_actions` consults it to skip slices that cannot contain the requested filter value. |

When a slice is compressed the trace file is replaced by:

| File | Description |
|------|-------------|
| `trace_<start>-<end>.clog` | zlib-compressed trace data with embedded seek points for random access. |

The metadata, block-offset, trx-id, and receiver-bloom sidecars are not compressed - they are already compact and need random access.

**Example** (10 000-block stride, blocks 0–29 999):

```
traces/
  trace_0000000000-0000010000.log
  trace_index_0000000000-0000010000.log
  trace_blk_idx_0000000000-0000010000.log
  trace_trx_idx_0000000000-0000010000.log
  trace_recv_bloom_0000000000-0000010000.log
  trace_0000010000-0000020000.log
  trace_index_0000010000-0000020000.log
  trace_blk_idx_0000010000-0000020000.log
  trace_trx_idx_0000010000-0000020000.log
  trace_recv_bloom_0000010000-0000020000.log
  trace_0000020000-0000030000.clog        <- compressed
  trace_index_0000020000-0000030000.log
  trace_blk_idx_0000020000-0000030000.log
  trace_trx_idx_0000020000-0000030000.log
  trace_recv_bloom_0000020000-0000030000.log
  abi_log.log
  abi_log.journal
```

`abi_log.log` and `abi_log.journal` are global (not per-slice): the
append-only ABI log and its durable reversible-overlay journal.

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

#### Receiver bloom sidecar

`trace_recv_bloom_<start>-<end>.log` is a per-slice pair of bloom
filters used by `/v1/trace_api/get_actions` to skip slices that cannot
contain the requested filter value. Without it, a request for a
rarely-active receiver across a wide block range would have to fetch
and scan every block in every slice; with it, slices that do not
contain the receiver are dismissed by a single O(1) probe and the
scanner advances `block_num` to the next slice boundary.

Contents:

- **Receiver filter** - `boost::bloom::filter<uint64_t, 7>` over
  `action_trace_v0::receiver` (name stored as its 64-bit value).
- **Composite filter** - `boost::bloom::filter<uint64_t, 7>` over a
  deterministic pack of `(receiver, action)` pairs. Probed when the
  caller supplies both a `receiver` and an `action` filter, giving an
  extra selectivity win for `get_token_transfers`-style lookups.

Format:

```
Header (40 bytes):
             magic         (u32) = 0x42524957 ("WIRB" on little-endian)
             version       (u32) = 1
             k_hashes      (u32) = 7
             n_recv        (u32)  - distinct receivers inserted
             n_recv_action (u32)  - distinct (receiver, action) pairs inserted
             reserved      (u32)
             recv_capacity_bits         (u64) - filter bit count
             recv_action_capacity_bits  (u64)
Body:
             receiver bloom bits       (recv_capacity_bits / 8 bytes)
             (receiver, action) bits   (recv_action_capacity_bits / 8 bytes)
             crc32 (u32) over header + body
```

Filters are sized for a 1% false-positive rate with a minimum floor of
32 items to avoid degenerate tiny bit arrays on sparse slices.  Total
sidecar size is dominated by the number of distinct receivers seen in
the slice (the composite filter scales with (receiver, action) pairs,
typically 2-3x the receiver count).  A busy mainnet slice sits around
10 KB; an empty slice produces a minimal always-miss file.

Build model: the bloom is built by `slice_directory::build_recv_bloom`
on the same schedule as the trx_id index - when the slice becomes
fully irreversible (its last block is below LIB), the maintenance pass
opens the slice's uncompressed data log, streams through each
`block_trace_v0` record in order, and inserts every action's receiver
(and `(receiver, action)` pair) into two
`boost::unordered_flat_set<uint64_t>` accumulators.  The filters are
then sized, populated, and written (temp + rename).  Deferring the
write to irreversibility means forks cannot corrupt an already-written
sidecar: a fork cannot reach back across LIB, so the slice's data log
is final by the time the bloom is built.  Fork re-writes leave stale
`block_trace_v0` records in the data log (the blk_offset sidecar
points only to the canonical offset); the stream-scan visits those
stale records too, so the bloom ends up as a superset of the canonical
receivers.  That is safe - bloom allows false positives, and a
forked-out receiver probing as present just means the query scan
visits that slice and finds no canonical match.

Query model: `get_actions` probes the bloom once per distinct slice in
the queried block range.  A negative probe is authoritative and the
entire slice is skipped with no `get_block` call.  A positive probe
(or a missing/corrupt sidecar) falls through to the existing scan.
Unfiltered queries (no `receiver`, no `account`, no `action`) do not
consult the bloom.

Retention: `slice_directory::run_maintenance_tasks` removes the bloom
sidecar alongside the slice's other files when the slice ages out of
`minimum_irreversible_history_blocks`.

##### Slice stride vs. query latency

The bloom is per-slice, so on a **positive** probe the scanner still
reads every block in the slice before returning.  That cost is bounded
by `trace-slice-stride`: larger strides mean more work per hit-slice,
smaller strides mean finer skip granularity at the cost of more
sidecar files.

Rough per-slice scan cost on SSD with slices in the OS page cache
(dominated by deserializing `block_trace_v0` records):

| `trace-slice-stride` | Scan after bloom hit (warm) | Scan (compressed, cold) | Files per slice |
|----------------------|-----------------------------|--------------------------|-----------------|
| 10000 (default)      | ~50-100 ms                  | ~200-500 ms              | 5 (+ .clog)     |
| 2500                 | ~15-25 ms                   | ~50-125 ms               | 5 (+ .clog)     |
| 1000                 | ~5-10 ms                    | ~20-50 ms                | 5 (+ .clog)     |

Smaller strides approximate a finer-grained bloom skip (bigger
stride-shrink == more precise "miss" resolution) at the cost of
linearly more files on disk.  At stride 1000 a year of busy-chain
history lands around 380 000 slice files total, which modern
filesystems handle but makes directory listings slow.

Queries that cluster near head (the common case) visit only a handful
of slices regardless of stride, so stride mostly affects deep-history
lookups on sparse accounts.  For nodes that primarily serve recent
queries the default is fine; nodes that expect frequent deep scans can
benefit from dropping to 2500 or lower.

`trace-slice-stride` takes effect on nodeop restart.  Existing slices
keep their old `<first>-<last>` naming; new slices written after the
restart use the new stride.  Nothing is migrated - query paths read
whatever sidecars exist for the slice covering a given block.

#### ABI log

`abi_log.log` is an append-only file that persists the ABI published by
each contract account across all `setabi` transactions observed since
the node started (or since the file was first written).

The file only ever contains records for **irreversible** blocks — a
record is appended exactly when LIB passes the block that committed it,
the same finality boundary the other trace files key off (e.g. the bloom
sidecar is only built for fully irreversible slices). A fork can
therefore never invalidate a written record, so the file needs no block
tagging, tombstones, or rewrite. Records for blocks above LIB live in an
in-memory reversible overlay (see
[ABI capture mechanics](#abi-capture-mechanics)) that is durably mirrored
to `abi_log.journal`, so the overlay is restored exactly on restart.

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
Runtime lookups resolve over the union of this index and the reversible
overlay; a disk-resolved blob is then read from the file via `pread()`.
Appends stream new records to the end of the file under a mutex, with no
rewrite of existing records.

Writes are not fsync'd; the on-disk tail may lose the last few records
on a kernel crash. On startup the recovery scan detects torn or
CRC-mismatched records and truncates the file at the first bad one —
any lost records are rebuilt the next time their contract is touched
(via an observed `setabi` or the lazy current-ABI fetch).

A write failure when persisting a record here (the flush at LIB, e.g.
disk full) is **fatal**, uniform with the journal: the node shuts down
cleanly rather than continuing without recording. No data is lost — the
record is still in the reversible overlay and its journal entry, so a
restart with storage available restores it from the journal and
re-persists it on the next LIB advance.

##### Reversible journal (`abi_log.journal`)

The in-memory reversible overlay is durably mirrored to a sibling
append-only journal so a restart restores it exactly. This cannot be
re-derived from the recorded block traces: a `setabi` leaves an action
trace, but a lazy `global_seq = 0` record is read from chain state and
leaves none — and once a later `setabi` supersedes the account, chain
state no longer holds the pre-`setabi` ABI, so those lazy bytes are
unrecoverable unless persisted before the restart.

```
Header  (16 bytes): magic "ABIJ" (u32), version 1 (u32), reserved (u64)
Records (repeated until EOF):
          body_len (u32)
          body     (body_len bytes): op (u8) + fc::raw fields
                     op = put      -> block_num (u32), account (u64),
                                       global_seq (u64), blob (raw bytes)
                     op = rollback -> block_num (u32)
          crc32    (u32) over (body_len, body)
```

`append_reversible` writes a `put`; `rollback_reversible` writes a
`rollback` (only when it actually discards something, so steady-state
`block_start` adds nothing); `flush_irreversible` writes nothing (a
flushed record already lives in `abi_log.log`). On startup the journal
is replayed in order into the overlay — dropping any `(account,
global_seq)` already on disk — then compacted (rewritten via a temp file
and atomic rename to contain only the still-reversible records). Torn or
CRC-mismatched tails are truncated like the main log. A journal write
failure is **fatal**: recording history is the point of a trace_api
node, so the node shuts down cleanly rather than continuing with
incomplete or inconsistent history. A failed rollback first rewrites the
journal to the post-rollback overlay before shutting down — dropping only
the orphaned forked records while keeping the canonical ones — so a
restart neither resurrects a forked-out ABI nor loses the still-reversible
window; if even that rewrite fails (e.g. disk full) it truncates the
journal as a safe last resort (the current window then degrades to raw
hex). On restart the chain re-applies the rolled-back block, so the
aborted work is re-recorded.

The journal is bounded so it cannot grow without limit on a node that
runs for months without a restart. A flushed record's `put` stays in the
file until reclaimed, so `flush_irreversible` reclaims it two ways: when
the overlay fully drains it truncates the file back to its header (the
common case on a healthy node, between capture bursts), and as a backstop
for a busy node whose overlay rarely hits exactly empty, once the file
passes a size threshold it is compacted down to just the live overlay.
The journal size is therefore bounded by that threshold regardless of
uptime.

---

### ABI capture mechanics

ABI records enter the ABI store through two paths:

1. **`setabi` observation** — every time a transaction contains a
   `sysio::setabi` action, the plugin decodes the action's payload and
   records `(target_account, setabi.global_sequence, abi_bytes)`. This
   gives exact ABI-version boundaries at the granularity of
   global_sequence.

2. **Lazy current-ABI fetch** — on the first action observed for an
   account that has no prior ABI record, the plugin reads the
   account's current ABI from the chain DB and records it at
   `global_sequence = 0`. The `0` is a sentinel meaning "ABI as of
   first observation; exact recorded sequence unknown." Lookups step
   back from the query `global_sequence` to the largest recorded entry
   ≤ query, so a 0 sentinel matches any action of that account that
   predates the first recorded real setabi.

#### Fork handling and irreversibility

A captured record moves through three phases, mirroring how the trace
slices treat blocks:

1. **Collect** — candidates are gathered per transaction at execution
   time; nothing is recorded for executions that never land in an
   accepted block (speculative relays, aborted production rounds).
2. **Commit (reversible)** — when a block is accepted, its
   transactions' candidates enter an in-memory reversible overlay,
   tagged with the accepting block's number. Overlay records
   participate in lookups immediately, so actions in `pending` blocks
   decode normally.
3. **Flush (irreversible)** — when the chain's irreversible-block
   signal passes a record's block, the record is appended to
   `abi_log.log` and leaves the overlay. Only this phase writes to
   disk.

If a fork replaces an accepted block, the replacing block's
`block_start` discards every overlay record tagged with an equal or
higher block number — before the replacing block's transactions
execute, so the lazy fetch's "first encounter" decision also sees the
canonical state. A `setabi` that only ever existed on a forked-out
branch therefore never reaches the file and stops resolving the moment
the fork switch begins.

**Restarts.** The overlay is memory-only, and the accepted-block
signals for blocks between LIB and head do not re-fire on a clean
restart. The overlay is instead restored from the durable reversible
journal (`abi_log.journal`, see
[ABI log](#abi-log)), which mirrors every overlay mutation in order.
This preserves **both** kinds of reversible record — including lazy
`global_seq = 0` captures, which leave no action trace and could never
be recovered by scanning recorded traces. (An earlier design rebuilt
only `setabi` records from traces and dropped the lazy bytes; a later
`setabi` then suppressed any re-capture, so pre-`setabi` actions
degraded to raw permanently. Journaling the overlay removes that loss.)

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

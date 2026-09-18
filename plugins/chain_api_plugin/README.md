# chain_api_plugin

`chain_api_plugin` exposes the chain's read and write RPC surface over HTTP: block and account queries, ABI
and contract code retrieval, contract table reads, and transaction submission. It is a thin registration
layer — it holds no state and registers no options; every handler forwards to `chain_plugin`'s
`chain_apis::read_only` and `chain_apis::read_write` API objects. Enable it with
`plugin = sysio::chain_api_plugin` on any node that should answer `/v1/chain/*` requests; it is not loaded by
a bare `nodeop` run. Naming it also pulls in `chain_plugin` and `http_plugin` through
`APPBASE_PLUGIN_REQUIRES`.

## How it works

The plugin does all of its work in `plugin_startup`: it obtains the read-only and read-write API objects from
`chain_plugin`, both constructed with `http_plugin`'s `http-max-response-time-ms` as their per-call deadline,
and registers one handler per endpoint with `http_plugin`. `set_program_options`, `plugin_initialize`, and
`plugin_shutdown` are empty.

```
chain_api_plugin::plugin_startup
  |
  |-- chain_plugin::get_read_only_api(max_response_time)   -> chain_apis::read_only   (chain_ro, node)
  |-- chain_plugin::get_read_write_api(max_response_time)  -> chain_apis::read_write  (chain_rw)
  |
  '-- http_plugin::add_api / add_async_api  (one entry per /v1/chain/<call>)
```

Three things are decided at registration time and visible to operators:

- **Which thread runs a call.** Most endpoints are registered with `add_api(..., exec_queue::read_only)`, so
  the call goes on the read-only queue, whose tasks run in parallel on the read-only thread pool as well as on
  the main application thread, for as long as nothing from the read-write queue is executing.
  `get_info`, `get_accounts_by_authorizers`, `send_read_only_transaction`, `get_raw_block`, and
  `get_block_header` are registered with `add_async_api` and run on an `http_plugin` worker thread.
  `get_block`, `get_account`, and `get_table_rows` run on the app thread but return a function that is posted
  back onto the HTTP thread pool to do the final serialization, keeping ABI conversion off the app thread.
  Transaction calls run asynchronously: keys are recovered in parallel, then the work is posted to the
  `trx_read_write` queue — a separate app-thread queue that runs only when no `read_write` task is pending —
  or, for `send_read_only_transaction`, to the read-exclusive queue.
- **Which category a call belongs to.** `/v1/chain/get_info` is registered under the `node` category, so it is
  reachable on every listener, exactly like `/v1/node/get_supported_apis`. Read calls are `chain_ro`; the four
  transaction-submission calls are `chain_rw`. With `--http-category-address` this is what lets a node publish
  reads publicly while keeping writes on loopback.
- **Which calls exist at all.** `get_accounts_by_authorizers` is registered only when `chain_plugin` reports
  account queries enabled (`enable-account-queries = true`), and `get_transaction_status` only when the
  transaction finality status feature is enabled
  (`transaction-finality-status-max-storage-size-gb` above 0). When the feature is off the path is simply
  absent and returns the standard 404.

Two request bodies get specialized parsing so a malformed body produces a precise message rather than a
generic parse error: `get_transaction_status` reports `Invalid transaction id`, and `get_transaction_id`
requires un-exploded hex `data` on each action, preferring `hex_data` when both are present.

The plugin also relays two `http_plugin` facts back into the API objects: ABI errors are shortened unless
`verbose-http-errors` is set, and the read-only API is told whether the `chain_ro` category is enabled at all,
so `tracked_votes` can skip work on a node that serves no chain reads.

## Enabling / configuration

`chain_api_plugin` registers no options of its own. Everything that shapes its behavior comes from
`chain_plugin` (which calls are available, deadlines, ABI serialization time) and `http_plugin` (addresses,
threads, limits).

Two further settings decide whether a registered call works rather than whether it exists:

- `send_read_only_transaction` fails with `read-only transactions execution not enabled on API node` unless
  `read-only-threads` is greater than 0. A producing node cannot set that option, so the call is unavailable
  there.
- Every `chain_rw` call is refused with `Not allowed, node has api-accept-transactions = false` when
  `api-accept-transactions = false`: each handler validates that flag before it computes its deadline.

```ini
plugin = sysio::chain_api_plugin

# http_plugin -- where the endpoints are served.
http-server-address = 127.0.0.1:8888

# chain_plugin -- the two options that add endpoints.
enable-account-queries = true
transaction-finality-status-max-storage-size-gb = 1

# http_plugin -- per-call deadline for every chain API handler.
http-max-response-time-ms = 15
```

The equivalent command line:

```bash
nodeop --plugin sysio::chain_api_plugin \
       --http-server-address 127.0.0.1:8888 \
       --enable-account-queries true \
       --transaction-finality-status-max-storage-size-gb 1
```

Publishing reads while keeping writes private uses `http_plugin`'s category binding:

```bash
nodeop --plugin sysio::chain_api_plugin \
       --http-server-address http-category-address \
       --http-category-address chain_ro,0.0.0.0:8888 \
       --http-category-address chain_rw,127.0.0.1:8889
```

## Options

The plugin's `set_program_options` is empty — it registers no options.

## HTTP API

Every endpoint takes a JSON body by POST. "Params" below names the fields of the call's `_params` struct;
`none` means the body must be empty or `{}`, and `optional` means an empty body is also accepted.

### Node-global

| URL | Params | Code | Purpose |
|---|---|---|---|
| `/v1/chain/get_info` | none | 200 | Chain identity and head state: `server_version`, `chain_id`, head and last-irreversible block number, id and time, `head_block_producer`, virtual and per-block CPU/NET limits, fork-db head, total CPU/NET weight, earliest available block. Served on every listener; answered from `chain_plugin`'s per-block cache on an HTTP thread. |

### Read-only (`chain_ro`)

| URL | Params | Code | Purpose |
|---|---|---|---|
| `/v1/chain/get_account` | `account_name`, `expected_core_symbol` (optional) | 200 | Account state: privilege, last code update, core liquid balance, RAM quota and usage, NET/CPU weights and limits, permissions, and linked actions. |
| `/v1/chain/get_code` | `account_name`, `code_as_wasm` | 200 | Deployed contract code, its hash, and ABI. |
| `/v1/chain/get_code_hash` | `account_name` | 200 | Just the deployed code hash. |
| `/v1/chain/get_abi` | `account_name` | 200 | The account's ABI as JSON. |
| `/v1/chain/get_raw_code_and_abi` | `account_name` | 200 | Code and ABI as raw blobs. |
| `/v1/chain/get_raw_abi` | `account_name`, `abi_hash` (optional) | 200 | Raw ABI blob plus code and ABI hashes; supplying `abi_hash` lets the caller skip the body when unchanged. |
| `/v1/chain/get_block` | `block_num_or_id` | 200 | A full block, ABI-decoded. Serialization runs on the HTTP thread pool. |
| `/v1/chain/get_raw_block` | `block_num_or_id` | 200 | The same block without ABI decoding. Runs on an HTTP thread. |
| `/v1/chain/get_block_info` | `block_num` | 200 | Block header summary for one block number. |
| `/v1/chain/get_block_header` | `block_num_or_id`, `include_extensions` | 200 | Block id and signed header; extensions are read off disk only when requested. Runs on an HTTP thread. |
| `/v1/chain/get_block_header_state` | `block_num_or_id` | 200 | Block identity and header, taken from the fork database (the best branch when looked up by number, any branch when looked up by id) and otherwise from the block log, so the block need not be reversible. The response carries `block_num`, `id`, and `header` only. |
| `/v1/chain/get_table_rows` | `code`, `table`, `scope`, `json`, `find`, `index_name`, `lower_bound`, `upper_bound`, `limit`, `reverse`, `show_payer`, `values_only`, `time_limit_ms` | 200 | Paged contract table scan. The result carries `more` and `next_key` for pagination; the page is also bounded by the call deadline. |
| `/v1/chain/get_table_by_scope` | `code`, `table`, `lower_bound`, `upper_bound`, `limit`, `reverse`, `time_limit_ms` | 200 | Enumerate the scopes a contract's tables occupy. |
| `/v1/chain/get_currency_balance` | `code`, `account`, `symbol` (optional) | 200 | Token balances held by an account in a token contract. |
| `/v1/chain/get_currency_stats` | `code`, `symbol` | 200 | Supply, max supply, and issuer for one token symbol. |
| `/v1/chain/get_producers` | `json`, `lower_bound`, `limit`, `time_limit_ms` (all ignored) | 200 | One row per producer in the active schedule, always in a single response: none of the four params is read, `more` is never set, and `total_producer_vote_weight` is always 0 because the rows come from the schedule rather than from a vote tally. |
| `/v1/chain/get_producer_schedule` | none | 200 | Active and pending producer schedules. |
| `/v1/chain/get_finalizer_info` | none | 200 | Active and pending finalizer policies, plus the last tracked vote for each finalizer in them. |
| `/v1/chain/get_activated_protocol_features` | `lower_bound`, `upper_bound`, `search_by_block_num`, `reverse` (all optional) | 200 | Every protocol feature activated on this chain within the requested bounds, in one response. It is not paged: `limit` and `time_limit_ms` are ignored and `more` is never set. |
| `/v1/chain/get_consensus_parameters` | none | 200 | Current chain configuration and WASM configuration. |
| `/v1/chain/get_required_keys` | `transaction`, `available_keys` | 200 | Subset of the supplied keys that is required to authorize the transaction. |
| `/v1/chain/get_transaction_id` | a transaction object | 200 | Transaction id computed from the supplied transaction. Action `data` must be un-exploded hex. |
| `/v1/chain/compute_transaction` | `transaction` | 200 | Execute a transaction speculatively and return its trace without submitting it. |
| `/v1/chain/send_read_only_transaction` | `transaction` | 200 | Execute a read-only transaction. The handler runs on an HTTP thread and posts the execution onto the read-exclusive queue. **Fails unless `read-only-threads` is greater than 0**, which a producing node cannot configure. |
| `/v1/chain/get_accounts_by_authorizers` | `accounts`, `keys` | 200 | Accounts whose permissions are authorized by the supplied accounts or keys. **Registered only when `enable-account-queries = true`.** |
| `/v1/chain/get_transaction_status` | `id` | 200 | Where a transaction stands relative to head and LIB, with the earliest block still tracked. **Registered only when the transaction finality status feature is enabled.** |

### Read-write (`chain_rw`)

All four calls are refused with `Not allowed, node has api-accept-transactions = false` when
`api-accept-transactions = false`.

| URL | Params | Code | Purpose |
|---|---|---|---|
| `/v1/chain/push_transaction` | a packed transaction object | 202 | Submit a transaction and return its id and trace. |
| `/v1/chain/push_transactions` | array of packed transaction objects | 202 | Submit several transactions in one request. |
| `/v1/chain/send_transaction` | a packed transaction object | 202 | Submit a transaction. |
| `/v1/chain/send_transaction2` | `transaction`, `return_failure_trace`, `retry_trx`, `retry_trx_num_blocks` | 202 | Submit a transaction with control over failure traces and over retry, which requires `chain_plugin`'s transaction retry feature. |

## Diagnostics

The plugin has no logger of its own; its few lines go to the default logger, and per-request diagnostics come
from `http_plugin`'s `http_plugin` logger.

- `starting chain_api_plugin` at debug level, once, at startup.
- One `add chain_ro api url: /v1/chain/<call> ...` line per registered endpoint from `http_plugin`, including
  `disabled for category address not configured` when no listener carries that category. This is the
  authoritative list of what this node actually serves — and the place to confirm that
  `get_accounts_by_authorizers` or `get_transaction_status` was registered.
- Handler exceptions are logged and converted by `http_plugin`'s shared mapping; the log line names
  `chain.<call>`.

## Tests

The plugin has no `test/` directory, and no C++ test target exercises it: `plugin_test` does not link it and
nothing under `unittests` references it. Its endpoints are covered end to end by
`tests/plugin_http_api_test.py` (`test_ChainApi`), which drives a running node over HTTP. The API
implementations it forwards to are covered by `plugin_test` (`tests/get_table_tests.cpp`,
`tests/get_producers_tests.cpp`, and their siblings).

## Related plugins

- [`chain_plugin`](../chain_plugin/README.md) — owns the controller and implements every call registered here.
- [`http_plugin`](../http_plugin/README.md) — serves the endpoints, and supplies the per-call deadline and the
  category-to-address binding.
- [`net_api_plugin`](../net_api_plugin/README.md) — the same pattern for `/v1/net/*`.
- `producer_api_plugin` — the same pattern for `/v1/producer/*`.

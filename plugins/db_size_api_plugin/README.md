# db_size_api_plugin

`db_size_api_plugin` exposes a single read-only HTTP endpoint, `/v1/db_size/get`, that reports how much of the
chain state database (the chainbase shared-memory segment) is in use, together with a per-index row count. It is
the endpoint an operator polls to answer "how close is this node to `--chain-state-db-size-mb`?" and "which index
is growing?". The plugin is opt-in: `nodeop` registers it, but it is only initialized when
`plugin = sysio::db_size_api_plugin` names it, and it pulls in `http_plugin` and `chain_plugin` through
`APPBASE_PLUGIN_REQUIRES`.

## How it works

The plugin starts no threads, connects to no signals, and holds no state of its own. In `plugin_startup` it
registers one handler with `http_plugin` under the `db_size` API category, dispatched on the
`appbase::exec_queue::read_only` queue. The handler calls `chain_plugin::chain().db()` and reads four values
straight off the chainbase segment manager plus the per-index row-count multiset:

| Reported field | Source |
|---|---|
| `free_bytes` | `db.get_segment_manager()->get_free_memory()` |
| `size` | `db.get_segment_manager()->get_size()` |
| `used_bytes` | `size` minus `free_bytes` |
| `reclaimable_bytes` | `db.get_reclaimable_memory()` |
| `indices` | `db.row_count_per_index()`, one `{index, row_count}` entry per chainbase index |

`size` is the segment size the node was started with, so it tracks `chain_plugin`'s `--chain-state-db-size-mb`
option. The `read_only` queue is not a guarantee that the read stays off the main thread: it runs on a
read-only thread only when `--read-only-threads` is greater than 0, and during the write window the main
application thread executes `read_only` tasks alongside `read_write` work -- with `read-only-threads = 0` the
main thread is the only place they run at all.

## Enabling / configuration

`config.ini`:

```ini
plugin = sysio::db_size_api_plugin
plugin = sysio::chain_api_plugin

# db_size is an administrative category -- keep it on loopback.
http-server-address = http-category-address
http-category-address = db_size,127.0.0.1:8888
http-category-address = chain_ro,127.0.0.1:8888
```

Every category bound with `http-category-address` needs the plugin that owns it named in a `plugin` option, so
the `chain_ro` line above requires `plugin = sysio::chain_api_plugin`; without it startup fails with
`--plugin=sysio::chain_api_plugin is required for --http-category-address=chain_ro,127.0.0.1:8888`.

Command line:

```bash
nodeop --plugin sysio::db_size_api_plugin \
       --http-server-address http-category-address \
       --http-category-address db_size,127.0.0.1:8888
```

With the plain `--http-server-address <ip>:<port>` form (default `127.0.0.1:8888`), the endpoint is served on
that one listener alongside every other enabled API category.

## Options

The plugin registers no program options of its own -- its `set_program_options` is empty. The endpoint's address
and limits come from `http_plugin` (`--http-server-address`, `--http-category-address`,
`--http-max-response-time-ms`, ...), and the segment size it reports comes from `chain_plugin`'s
`--chain-state-db-size-mb`.

## HTTP API

| URL | Method / params | API category | Purpose |
|---|---|---|---|
| `/v1/db_size/get` | Any verb with an empty body (`no_params`); a non-empty body is rejected with `no parameter should be given`. Returns `200` and `application/json`. | `db_size` | Chain-state segment usage plus per-index row counts |

Response body:

```json
{
  "free_bytes": 912680550,
  "used_bytes": 160931898,
  "reclaimable_bytes": 0,
  "size": 1073612448,
  "indices": [
    { "index": "sysio::chain::account_object", "row_count": 412 },
    { "index": "sysio::chain::account_metadata_object", "row_count": 412 }
  ]
}
```

`index` is the demangled C++ name of the chainbase object type the index stores, and `indices` carries one
entry per index the running node has instantiated -- the set depends on the protocol features and contracts in
play, so treat it as a list to iterate rather than a fixed schema. `row_count_per_index()` returns a multiset
keyed on `(row_count, type_name)`, so the array arrives sorted by ascending row count.

## Diagnostics

The plugin declares no logger and emits no log lines of its own. A failed request is turned into an HTTP error
by `http_plugin::handle_exception("db_size", "get", ...)`, so any diagnostics appear under `http_plugin`'s
logging with `db_size`/`get` in the message.

## Tests

The plugin has no `test/` directory and therefore no dedicated target. Its coverage is `test_DbSizeApi` in
`tests/plugin_http_api_test.py`, which drives `/v1/db_size/get` against a running node on the `db_size`
listener: an empty request and an empty-content request must both return a payload carrying `free_bytes`,
`used_bytes`, `size`, and `indices`, and a request with a parameter must return `400`.

## Related plugins

- [`http_plugin`](../http_plugin) -- serves the endpoint and owns the `db_size` category binding.
- [`chain_plugin`](../chain_plugin) -- owns the chainbase database this plugin measures and the
  `--chain-state-db-size-mb` option that sets `size`.
- [`prometheus_plugin`](../prometheus_plugin) -- scrape-oriented metrics; use it for time series, use this for a
  point-in-time index breakdown.
- [`resource_monitor_plugin`](../resource_monitor_plugin) -- watches *file system* space, which is a different
  exhaustion mode from the chain-state segment this plugin reports.

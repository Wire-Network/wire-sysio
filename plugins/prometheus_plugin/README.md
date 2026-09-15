# prometheus_plugin

`prometheus_plugin` exposes `nodeop`'s internal counters and gauges in Prometheus text-exposition format at
`/v1/prometheus/metrics`, so a Prometheus server can scrape block production, p2p connectivity, HTTP request,
and outbound-transport metrics directly off the node. It is opt-in -- `nodeop` registers it, but it is only
initialized when `plugin = sysio::prometheus_plugin` names it -- and through `APPBASE_PLUGIN_REQUIRES` it pulls
in `http_plugin`, `chain_plugin`, `producer_plugin`, and `net_plugin`, all four of which it registers callbacks
with. It registers no program options of its own.

## How it works

```
http_plugin      --register_update_metrics------------> |
net_plugin       --register_update_p2p_connection_metrics->|
                 --register_increment_failed_p2p_connections->|   prometheus
                 --register_increment_dropped_trxs-------->|   strand   ---> prometheus::Registry
producer_plugin  --register_update_speculative_block_metrics->|  (1 thread, "prom" pool)
controller       --register_update_produced_block_metrics-->|
                 --register_update_incoming_block_metrics-->|

GET /v1/prometheus/metrics --(http_plugin async api)--> post to strand --> TextSerializer --> text/plain
```

In `plugin_initialize` the plugin registers its update handlers with each dependency and then adds one
asynchronous handler to `http_plugin` via `add_async_api(..., http_content_type::plaintext)` in the `prometheus`
API category. Every callback except the two thread-safe `Counter::Increment` hooks (failed p2p connections and
dropped transactions) posts its work onto a single `boost::asio::io_context::strand`, so the registry is only
ever mutated from one thread. The plugin also caches `http_plugin::get_max_response_time()` at initialization;
its handler's `start()` derives a per-request deadline from it.

`plugin_startup` starts the one-thread `prom` pool that backs the strand and then publishes the static `nodeop`
info series (server version, chain id, version strings, earliest available block). `plugin_shutdown` stops the
pool.

A scrape posts onto the strand and serializes the whole registry with `prometheus::TextSerializer`. Before
serializing, the report folds the process-wide outbound HTTP transport snapshot (`fc::http::get_metrics_snapshot()`)
into its counters as deltas since the previous scrape, so `nodeop_outbound_http_*` only advances when a scrape
happens. The report then increments `exposer_scrapes_total` and adds the response size to
`exposer_transferred_bytes_total`.

**Per-connection p2p series are bounded.** Each peer in the `net_plugin` snapshot gets gauge series labelled
`{remote_ip, connection_id}` -- the real socket peer address and the node's own monotonic connection counter,
both observed locally, never values a peer supplies in its handshake. When a connection leaves the snapshot its
series are removed from every p2p family, so connection churn cannot grow the registry without bound. The
outbound-HTTP failure family likewise has one series per `fc::http::failure_kind` enumerator and no more.

## Enabling / configuration

`config.ini`:

```ini
plugin = sysio::prometheus_plugin

# Keep the scrape endpoint on a private interface; give it its own listener
# so the chain APIs stay on loopback.
http-server-address = http-category-address
http-category-address = chain_ro,127.0.0.1:8888
http-category-address = prometheus,127.0.0.1:9101
```

Command line:

```bash
nodeop --plugin sysio::prometheus_plugin \
       --http-server-address http-category-address \
       --http-category-address prometheus,127.0.0.1:9101
```

With the plain `--http-server-address <ip>:<port>` form (default `127.0.0.1:8888`), the metrics endpoint is
served on that one listener alongside every other enabled API category.

A matching scrape job:

```yaml
scrape_configs:
  - job_name: nodeop
    metrics_path: /v1/prometheus/metrics
    static_configs:
      - targets: ["<your-host>:9101"]
```

## Options

The plugin registers no program options -- its `set_program_options` body is empty. Everything that governs the
endpoint comes from `http_plugin`: `--http-server-address` / `--http-category-address` for where it is served,
and `--http-max-response-time-ms` (default `15`), whose value the plugin caches as its response-deadline base.

## HTTP API

| URL | Method / params | API category | Purpose |
|---|---|---|---|
| `/v1/prometheus/metrics` | Any verb with an empty body (`no_params`); a non-empty body is rejected with `no parameter should be given`. Returns `200` with `Content-Type: text/plain`. | `prometheus` | Prometheus text exposition of the whole registry |

A Prometheus scraper's plain `GET` works: `http_plugin` dispatches on the URL path and only special-cases
`OPTIONS`, and `no_params` requires the body to be empty.

### Metric families

| Family | Type | Labels | Meaning |
|---|---|---|---|
| `nodeop` | Info | `server_version`, `chain_id`, `server_version_string`, `server_full_version_string`, `earliest_available_block_num` | Static information about the server |
| `nodeop_http_requests_total` | Counter | `handler` | Number of HTTP requests, per handler target |
| `nodeop_outbound_http_requests_total` | Counter | -- | Total shared outbound HTTP transport requests |
| `nodeop_outbound_http_successes_total` | Counter | -- | Total successful shared outbound HTTP transport requests |
| `nodeop_outbound_http_request_bytes_total` | Counter | -- | Process-wide complete request-body bytes uploaded by the shared outbound HTTP transport, including completed retry attempts |
| `nodeop_outbound_http_response_bytes_total` | Counter | -- | Total response-body bytes accepted by the shared outbound HTTP transport |
| `nodeop_outbound_http_failures_total` | Counter | `category` | Shared outbound HTTP transport failures, one series per fixed `fc::http::failure_kind` |
| `nodeop_p2p_failed_connections` | Counter | -- | Total number of failed outgoing p2p connections |
| `nodeop_p2p_dropped_trxs_total` | Counter | -- | Total number of transactions dropped by `net_plugin` |
| `nodeop_p2p_peers` / `nodeop_p2p_clients` | Gauge | -- | Current connected outgoing peers / incoming clients |
| `nodeop_p2p_port`, `nodeop_p2p_connection_number`, `nodeop_p2p_accepting_blocks`, `nodeop_p2p_last_received_block`, `nodeop_p2p_first_available_block`, `nodeop_p2p_last_available_block`, `nodeop_p2p_unique_first_block_count`, `nodeop_p2p_latency`, `nodeop_p2p_bytes_received`, `nodeop_p2p_last_bytes_received`, `nodeop_p2p_bytes_sent`, `nodeop_p2p_last_bytes_sent`, `nodeop_p2p_block_sync_bytes_received`, `nodeop_p2p_block_sync_bytes_sent`, `nodeop_p2p_block_sync_throttling`, `nodeop_p2p_connection_start_time`, `nodeop_p2p_peer_score` | Gauge | `remote_ip`, `connection_id` | One series per live p2p connection; removed when the connection leaves the snapshot |
| `nodeop_cpu_usage_us_total` / `nodeop_net_usage_us_total` | Counter | `block_type` (`produced`, `incoming`) | Total cpu / net usage in microseconds for blocks |
| `nodeop_last_irreversible` / `nodeop_head_block_num` | Gauge | -- | Last irreversible block number / head block number |
| `nodeop_block_num` | Gauge | -- | Current block number |
| `nodeop_blocks_produced` | Counter | -- | Number of blocks produced |
| `nodeop_blocks_speculative_num` | Counter | -- | Number of speculative blocks created |
| `nodeop_blocks_incoming` | Counter | -- | Number of incoming blocks |
| `nodeop_trxs_produced_total` / `nodeop_trxs_incoming_total` | Counter | -- | Transactions produced / incoming |
| `nodeop_unapplied_transactions_total` | Counter | -- | Unapplied transactions from produced blocks |
| `nodeop_subjective_bill_account_size_total` | Counter | -- | Subjective bill account size from produced blocks |
| `nodeop_scheduled_trxs_total` | Counter | -- | Scheduled transactions from produced blocks |
| `nodeop_produced_elapsed_us_total` / `nodeop_produced_us_total` | Counter | -- | Total produced-block elapsed time / total time |
| `nodeop_incoming_elapsed_us_total` / `nodeop_incoming_us_total` | Counter | -- | Total incoming-block elapsed time / total time |
| `nodeop_incoming_us_block_latency` | Counter | -- | Total incoming-block latency |
| `nodeop_total_time_us_<produced\|speculative>_block`, `nodeop_idle_time_us_…`, `nodeop_other_time_us_…` | Counter | -- | Per-block time accounting, one family per block type |
| `nodeop_num_success_trx_…`, `nodeop_success_trx_time_us_…`, `nodeop_num_failed_trx_…`, `nodeop_fail_trx_time_us_…`, `nodeop_num_transient_trx_…`, `nodeop_transient_trx_time_us_…` | Counter | -- | Per-block transaction counts and times, suffixed `_produced_block` or `_speculative_block` |
| `exposer_scrapes_total` | Counter | -- | Total number of prometheus scrape requests received |
| `exposer_transferred_bytes_total` | Counter | -- | Total number of bytes for responses to prometheus scrape requests |

Speculative-block metrics are also updated from produced blocks, so the `_speculative_block` families include
blocks this node produced.

## Diagnostics

The plugin uses the default logger and declares no logger category of its own. It is nearly silent:

| Line | Level | When |
|---|---|---|
| `Prometheus plugin started.` | debug | End of `plugin_startup`. |
| `Prometheus plugin shutdown.` | debug | End of `plugin_shutdown`. |
| `Prometheus exception <detail>` | error | An `fc::exception` escaped onto the `prom` thread pool. |

Nothing is logged per scrape. Scrape volume is visible in the metrics themselves via `exposer_scrapes_total`
and `exposer_transferred_bytes_total`, and request errors surface through `http_plugin`'s handling as
`prometheus`/`metrics`.

## Tests

The test directory has no `CMakeLists.txt`, so `plugin_target` builds the default target from its sources:

```bash
ninja -C build/debug test_prometheus_plugin
./build/debug/plugins/prometheus_plugin/test_prometheus_plugin
```

The `prometheus_p2p_metrics` suite drives `catalog_type` directly with synthetic `net_plugin` snapshots and
pins down three properties: per-connection series are keyed by the locally observed `{remote_ip,
connection_id}` and never by peer-supplied handshake values, series for connections that leave the snapshot are
removed, and the outbound-HTTP failure family has fixed label cardinality.

## Related plugins

- [`http_plugin`](../http_plugin) -- serves the endpoint, owns the `prometheus` category binding and the
  response-time limit, and feeds `nodeop_http_requests_total`.
- [`net_plugin`](../net_plugin) -- source of every `nodeop_p2p_*` series.
- [`producer_plugin`](../producer_plugin) -- source of the speculative-block metrics.
- [`chain_plugin`](../chain_plugin) -- its controller is the source of the produced- and incoming-block metrics
  and of the chain id in the `nodeop` info series.
- [`http_client_plugin`](../http_client_plugin) -- registers the outbound transport options whose traffic the
  `nodeop_outbound_http_*` counters measure.
- [`status_monitor_plugin`](../status_monitor_plugin) -- pushes `get_info` documents to OpenSearch/Elasticsearch;
  a push-model complement to this plugin's scrape model.

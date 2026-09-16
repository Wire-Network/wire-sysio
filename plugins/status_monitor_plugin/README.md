# status_monitor_plugin

`status_monitor_plugin` writes one `/v1/chain/get_info` snapshot per LIB advance straight to an
OpenSearch or Elasticsearch `_bulk` endpoint. Each snapshot is rendered through an operator-supplied JSON
document template, so the shape of the document is entirely under the operator's control, and the delivery
path never touches the logging framework: dashboards get a steady, low-volume stream of chain-status
documents without a log pipeline in between.

The plugin is opt-in (`plugin = sysio::status_monitor_plugin`) and stays disabled until
`--status-monitor-target-url` is set. Every knob is a `status-monitor-*` option, accepted both in
`config.ini` and on the `nodeop` command line, and is fixed at startup.

## How it works

```
channels::irreversible_block  (application thread, after the block commits)
        |
        |  1. is the node synced?  (controller::is_synced(): LIB time within 5 s of wall-clock time)
        |  2. has LIB advanced since the last document?
        |  3. copy the get_info snapshot + wall-clock time, queue it   (cheap; never blocks)
        v
render worker  (one thread)      -> renders the JSON template into one NDJSON line
        |                           drops the document if it exceeds the per-document byte cap
        v
delivery worker  (one thread)    -> assembles up to --status-monitor-max-items-per-task documents
        |                           per _bulk request, pairing each with the index action line
        v
es client io thread               -> POST <target-url>/_bulk with retries and backoff
```

Three threads do the work; the application thread only copies a snapshot and enqueues it. The
get_info snapshot comes from `chain_plugin`'s per-block cache, which `chain_plugin` refreshes on every
accepted and irreversible block whenever `sysio::status_monitor_plugin` is named in the `plugin` option
(the same mechanism `chain_api_plugin` uses). Because the snapshot is the same `fc::variant` the HTTP API
serializes, the nested `status_monitor` object in each document is byte-identical to a `/v1/chain/get_info`
response body.

Two checks keep the stream meaningful:

- **Liveness.** A document is emitted only while `controller::is_synced()` holds: the last irreversible
  block's time is within 5 s of wall-clock time. While the node syncs, while finality lags, or on a host whose
  clock runs more than that ahead of the chain, nothing is sent; the pause is warned about at most once a
  minute and the resumption is logged once. Both checks run as LIB advances, so a halted LIB sends and logs
  nothing.
- **One document per LIB.** A delivery that finds the last irreversible block unchanged since the last
  document sends nothing, so several blocks becoming irreversible at once yield one document for the newest.

Steady-state volume is one document per LIB advance, normally one per block: about two per second at the
500 ms block interval.

## Enabling the plugin

### `config.ini`

```ini
plugin = sysio::status_monitor_plugin

# Required to activate; without it the plugin logs "disabled" and does nothing.
status-monitor-target-url = https://opensearch.example.com
# Index or write alias the documents are written to. Required when the target URL is set.
status-monitor-target-index = <your-index-or-write-alias>
# JSON object whose string values may carry ${token} placeholders. Required when the target URL is set.
status-monitor-target-template-file = /etc/wire/status-monitor-template.json

# Optional HTTP basic auth; both or neither.
status-monitor-username = status-writer
status-monitor-password = <secret>

# Delivery tuning (defaults shown).
status-monitor-max-items-per-task = 100
status-monitor-max-pending-documents = 256
status-monitor-connect-timeout-ms = 5000
status-monitor-request-timeout-ms = 10000
status-monitor-max-retries = 3
status-monitor-retry-backoff-ms = 250
```

### Command line

```bash
nodeop --plugin sysio::status_monitor_plugin \
       --status-monitor-target-url https://opensearch.example.com \
       --status-monitor-target-index <your-index-or-write-alias> \
       --status-monitor-target-template-file /etc/wire/status-monitor-template.json \
       --status-monitor-username status-writer \
       --status-monitor-password '<secret>'
```

`nodeop --help` lists every option with its current default; the template help text enumerates the exact
placeholder names the plugin supplies.

### Options

| Option | Default | Meaning |
|---|---|---|
| `status-monitor-target-url` | unset (plugin disabled) | Base URL of the OpenSearch/Elasticsearch endpoint; `http` or `https`. `/_bulk` is appended by the plugin. |
| `status-monitor-target-index` | required when active | Index or write alias named in each bulk action line. |
| `status-monitor-target-template-file` | required when active | Path of the JSON document template (see below). |
| `status-monitor-username` | unset | HTTP basic auth user; requires the password. |
| `status-monitor-password` | unset | HTTP basic auth password; requires the username. |
| `status-monitor-max-items-per-task` | 100 (1 to 10000) | Maximum documents per `_bulk` request. The delivery worker sends whatever is queued, up to this many; it never waits to fill a batch. |
| `status-monitor-max-pending-documents` | 256 (1 to 65536) | Rendered documents allowed to wait behind the active request; the newest is dropped when full. |
| `status-monitor-connect-timeout-ms` | 5000 | Connect timeout per request. |
| `status-monitor-request-timeout-ms` | 10000 | Header, read, idle, and total timeout per request. |
| `status-monitor-max-retries` | 3 (0 to 10) | Additional attempts after the first for a 5xx, 429, timeout, or connection failure, so four attempts per batch at the default. |
| `status-monitor-retry-backoff-ms` | 250 (greater than 0) | Initial backoff between attempts; doubles per attempt, capped at 2000 ms. |
| `status-monitor-additional-ca-file` | unset (`outbound-http-additional-ca-file` applies) | PEM CA bundle added to the system trust store for requests to the endpoint, the startup check included. |
| `status-monitor-additional-ca-path` | unset (`outbound-http-additional-ca-path` applies) | Hashed CA directory added to the system trust store for requests to the endpoint, the startup check included. |
| `status-monitor-proxy` | unset (`outbound-http-proxy` applies) | Explicit proxy URL for requests to the endpoint, the startup check included. |

An invalid active configuration (bad URL scheme, empty index, missing or unparseable template, a template
that references a token the plugin does not supply, a value outside its range, auth with only one half set)
fails startup with `plugin_config_exception` naming the option.

## The document template

The template is a JSON object. Its string values may contain `${token}` placeholders; every other member is
emitted exactly as written, and no member is required. Object keys are never tokenized. A JSON file has no
comment syntax, so a `"_comment"` member would land in every document.

Two placeholder forms exist:

- **Whole-value** (`"field": "${data}"`): the value is replaced by the token's typed JSON value, so
  `${data}` becomes a nested object, `${line}` a bare integer, `${epoch_millis}` a bare integer.
- **Mixed string** (`"field": "${file}:${line} ${func}"`): each placeholder is substituted as text inside
  the string, JSON-escaped.

`$${` writes a literal `${`. Modifiers: `${timestamp:iso8601}` (the default) or `${timestamp:epoch_millis}`,
and `${level:upper}` or `${level:lower}`. Timestamps and level names render exactly as the logging layouts
do, so a status document and a log document can share one index mapping.

Tokens the plugin supplies, and what each renders:

| Token | Value |
|---|---|
| `timestamp`, `iso8601` | Wall-clock time the snapshot was taken, as `YYYY-MM-DDTHH:MM:SS.ffffffZ` |
| `epoch_millis` | The same instant as milliseconds since the epoch (bare integer) |
| `level` | `info` |
| `message`, `logger` | `status_monitor` |
| `file`, `line`, `func` | Source location of the renderer (the absolute source path the build compiled, the line, `render_document`), matching the shape the es log sink emits |
| `thread` | `status_monitor` |
| `data` | An object with one member, `status_monitor`, holding the full `/v1/chain/get_info` body |

Referencing any other name fails startup, and the error names both the offending token and the supplied
set.

The source tree carries a sample at `etc/status_monitor/document-template.json`:

```json
{
  "@timestamp": "${epoch_millis}",
  "level": "${level:upper}",
  "message": "${message}",
  "category": "${logger}",
  "sourceLocation": "${file}:${line} ${func}",
  "data": "${data}",
  "env": "local",
  "app": "nodeop",
  "principal": "node-a",
  "logStream": "nodeop/node-a"
}
```

Rendered with that template, one document looks like this (the `status_monitor` object is the complete
get_info body; only a few of its fields are shown):

```json
{"@timestamp":1789432200123,"level":"INFO","message":"status_monitor","category":"status_monitor",
 "sourceLocation":"/build/src/plugins/status_monitor_plugin/src/status_monitor.cpp:178 render_document",
 "data":{"status_monitor":{"server_version":"4b599551","chain_id":"...","head_block_num":123456,
   "last_irreversible_block_num":123454,"last_irreversible_block_id":"...","head_block_time":"2026-09-15T14:30:00.000",
   "head_block_producer":"prod.a", "...":"..."}},
 "env":"local","app":"nodeop","principal":"node-a","logStream":"nodeop/node-a"}
```

Members such as `env`, `app`, `principal`, and `logStream` are literals in the template: set them per node.
On the OpenSearch side, map `@timestamp` as a `date` field with the `epoch_millis` format if the template
uses `${epoch_millis}`, or keep `${timestamp}` for an ISO 8601 string.

## Delivery semantics

- At startup the plugin checks the endpoint once: a single `GET` of the target URL's root, carrying the same
  credentials and timeouts a bulk request would, and no retry. A 2xx passes, and so does a `403`: the root is
  the cluster-info path, which OpenSearch fine-grained access control gates behind the cluster `monitor/main`
  permission, so a least-privilege credential that may only write to `_bulk` is answered with a `403` there
  while delivery works -- the endpoint answered and accepted the credential, which is what the check asks.
  Everything else -- a `401` (the credential itself rejected), a `404`, another status, a connection failure,
  or a timeout -- fails `nodeop` startup with the probed URL and the reason. An endpoint that cannot be
  reached would otherwise drop every document for as long as the node runs.
- Each document is one NDJSON line, paired with an `{"index":{"_index":"<index>"}}` action line; a request
  body holds up to `max-items-per-task` pairs and stays under the 1 MiB batch cap. A single document larger
  than 256 KiB is dropped and counted rather than sent.
- A 5xx, a 429, a timeout, or a connection failure is retried up to `max-retries` more times with doubling
  backoff (capped at 2 s), each attempt bounded by `request-timeout-ms`. Any other 4xx is terminal for that
  batch. A 2xx is never retried: a partial bulk response (`"errors":true`) credits the acknowledged documents
  and counts the rejected ones, and a 2xx that is not a bulk response is counted as a failed batch.
- Once the batch is given up on its documents are dropped -- nothing is re-queued -- and the drop is
  logged (see Diagnostics).
- A batch's documents are consumed as they are sent; nothing is re-rendered.
- Basic auth is sent as an `Authorization: Basic` header when configured. TLS uses the system trust store
  plus any additional CA file or path, and requests go through the proxy when one is set; each
  `status-monitor-*` transport option falls back to its node-wide `outbound-http-*` counterpart.
- Shutdown unsubscribes from the irreversible_block channel, cancels an in-flight request, joins both
  workers, and logs the final counters. Documents still waiting are discarded, not flushed.

## Backpressure

The pipeline never blocks block application. The render queue holds at most 32 snapshots and the delivery
queue at most `max-pending-documents` rendered documents; when either is full, the newest item is dropped
and counted. Every drop and failure is visible in the counters below.

## Diagnostics

All diagnostics go through the `status_monitor` logger (configure it like any other logger in
`logging.json`; `SIGHUP` re-binds it, but never re-reads the plugin options). Nothing is logged per block.

- Startup: one line reporting the endpoint (credentials stripped) reachable and naming the index, the
  template's tokens, and the batching limits; or `no --status-monitor-target-url provided, disabled`. An
  unreachable endpoint logs one error naming the probed URL and the reason, and fails startup. A `403` from
  the root is not a failure (see Delivery semantics): startup continues and logs the reachable line.
- Liveness: one warning when documents pause because the irreversible block is behind wall-clock time, at
  most once a minute while paused, and one line when they resume.
- Delivery, per failed batch: one warning for the first bulk request that did not fully index, then at most
  one a minute counting the failed batches in between, naming the attempts it took, the failure detail (for
  example `HTTP 503 from https://opensearch.example.com`), and what became of its documents -- `N document(s)
  dropped` once the batch is given up on, `N of M document(s) rejected` for a partial bulk response, `N
  document(s) canceled` at shutdown. This line fires only on failure, so the per-block data path stays
  log-free.
- Delivery, summarized: when a bulk request fails or something is dropped, one warning carrying every counter
  and the last failure detail; when delivery succeeds again, one recovery line. These two share one rate
  limit of at most one line per minute between them, so an endpoint that alternates between failing and
  succeeding cannot log a line per block.
- Shutdown: one line with the final `documents_indexed`, `documents_failed`, `batches_indexed`,
  `batches_failed`, `snapshots_dropped_queue_full`, and `documents_dropped_queue_full` counters.

Counters carried by the delivery warning:

| Counter | Meaning |
|---|---|
| `batches_indexed` / `batches_failed` | Bulk requests fully acknowledged / partial, rejected, unavailable, or canceled |
| `documents_indexed` / `documents_failed` | Documents acknowledged by the endpoint / rejected or never delivered |
| `snapshots_dropped_queue_full` | Snapshots rejected by a full render queue |
| `documents_dropped_queue_full` | Rendered documents rejected by a full delivery queue |
| `documents_dropped_oversize` | Documents larger than the per-document cap |
| `render_failures` | Template renders that threw (never expected) |

## Relationship to the logging es sink

The plugin and the logging framework's `es_sink` share one delivery client, `fc::network::es::es_client`,
and one template engine, `fc::json_template`. The sink ships log records; this plugin ships chain status.
They can target the same endpoint, and because the token vocabulary and the timestamp/level rendering are
shared, a template written for one produces the same field shapes in the other.

## Tests

```bash
ninja -C build/debug test_status_monitor_plugin
./build/debug/plugins/status_monitor_plugin/test/test_status_monitor_plugin
```

The suite covers option registration and validation, rendering of the shipped sample template and of every
supplied token, bulk-body assembly and splitting, the two-stage pipeline with an injected
sender, drop and failure accounting, progress classification, and the plugin lifecycle without a running
node. No test dials a real endpoint.

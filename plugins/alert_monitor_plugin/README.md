# alert_monitor_plugin

`alert_monitor_plugin` writes one `/v1/chain/get_info` snapshot per LIB advance straight to an
OpenSearch or Elasticsearch `_bulk` endpoint. Each snapshot is rendered through an operator-supplied JSON
document template, so the shape of the document is entirely under the operator's control, and the delivery
path never touches the logging framework: dashboards get a steady, low-volume stream of chain-status
documents without a log pipeline in between.

The plugin can also run WireQL queries against the node's own state, each at its own block interval, and send
each result as a document through the same template, delivery queue, and endpoint (see [Queries](#queries)).

The plugin is opt-in (`plugin = sysio::alert_monitor_plugin`) and stays disabled until
`--alert-monitor-target-url` is set. Every knob is a `alert-monitor-*` option, accepted both in
`config.ini` and on the `nodeop` command line, and is fixed at startup.

## How it works

```
channels::irreversible_block  (application thread, after the block commits)
        |
        |  1. is the node synced?  (controller::is_synced(): LIB time within 5 s of wall-clock time)
        |  2. has LIB advanced since the last document?
        |  3. copy the get_info snapshot + wall-clock time, queue it   (cheap; never blocks)
        |  4. dispatch each query due at this LIB to its caller        (cheap; never blocks)
        |
        +-----------------------------------+
        v                                   v
render worker  (one thread)         query callers  (one thread per configured query)
  renders the JSON template into      run the query (1 s deadline), render its result through
  one NDJSON line                     the same template, then submit_document()
        |                                   |
        +-----------------+-----------------+
                          |  drops a document that exceeds the per-document byte cap
                          v
delivery worker  (one thread)    -> assembles up to --alert-monitor-max-items-per-task documents
        |                           per _bulk request, pairing each with the index action line
        v
es client io thread               -> POST <target-url>/_bulk with retries and backoff
```

Three threads do the work, plus one per configured query; the application thread only copies a snapshot,
enqueues it, and dispatches the queries that are due. The get_info snapshot comes from `chain_plugin`'s
per-block cache, which `chain_plugin` refreshes on every
accepted and irreversible block whenever `sysio::alert_monitor_plugin` is named in the `plugin` option
(the same mechanism `chain_api_plugin` uses). Because the snapshot is the same `fc::variant` the HTTP API
serializes, the `status` member of each document's `alert_monitor` object has the same fields as a
`/v1/chain/get_info` response body, with every integer as a JSON number (the HTTP API quotes those above
0xffffffff).

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
plugin = sysio::alert_monitor_plugin

# Required to activate; without it the plugin logs "disabled" and does nothing, except that
# alert-monitor-query requires it (a query without a target URL fails startup).
alert-monitor-target-url = https://opensearch.example.com
# Index or write alias the documents are written to. Required when the target URL is set.
alert-monitor-target-index = <your-index-or-write-alias>
# JSON object whose string values may carry ${token} placeholders. Required when the target URL is set.
# A relative path is resolved against the config directory (--config-dir).
alert-monitor-target-template-file = /etc/wire/alert-monitor-template.json

# Optional HTTP basic auth; both or neither.
alert-monitor-username = status-writer
alert-monitor-password = <secret>

# Delivery tuning (defaults shown).
alert-monitor-max-items-per-task = 100
alert-monitor-max-pending-documents = 256
alert-monitor-connect-timeout-ms = 5000
alert-monitor-request-timeout-ms = 10000
alert-monitor-max-retries = 3
alert-monitor-retry-backoff-ms = 250
```

### Command line

```bash
nodeop --plugin sysio::alert_monitor_plugin \
       --alert-monitor-target-url https://opensearch.example.com \
       --alert-monitor-target-index <your-index-or-write-alias> \
       --alert-monitor-target-template-file /etc/wire/alert-monitor-template.json \
       --alert-monitor-username status-writer \
       --alert-monitor-password '<secret>'
```

`nodeop --help` lists every option with its current default; the template help text enumerates the exact
placeholder names the plugin supplies.

### Options

| Option | Default | Meaning |
|---|---|---|
| `alert-monitor-target-url` | unset (plugin disabled) | Base URL of the OpenSearch/Elasticsearch endpoint; `http` or `https`. `/_bulk` is appended by the plugin. Without it the plugin logs "disabled" and does nothing, except that `alert-monitor-query` requires it. |
| `alert-monitor-target-index` | required when active | Index or write alias named in each bulk action line. |
| `alert-monitor-target-template-file` | required when active | Path of the JSON document template (see below); a relative path is resolved against the config directory, so a node directory stays relocatable. |
| `alert-monitor-query` | none (repeatable) | One query per value: `<snake_case_name>,<block_interval>,<query...>`. See [Queries](#queries). |
| `alert-monitor-username` | unset | HTTP basic auth user; requires the password. |
| `alert-monitor-password` | unset | HTTP basic auth password; requires the username. |
| `alert-monitor-max-items-per-task` | 100 (1 to 10000) | Maximum documents per `_bulk` request. The delivery worker sends whatever is queued, up to this many; it never waits to fill a batch. |
| `alert-monitor-max-pending-documents` | 256 (1 to 65536) | Rendered documents allowed to wait behind the active request; the newest is dropped when full. |
| `alert-monitor-connect-timeout-ms` | 5000 | Connect timeout per request. |
| `alert-monitor-request-timeout-ms` | 10000 | Header, read, idle, and total timeout per request. |
| `alert-monitor-max-retries` | 3 (0 to 10) | Additional attempts after the first for a 5xx, 429, timeout, or connection failure, so four attempts per batch at the default. |
| `alert-monitor-retry-backoff-ms` | 250 (greater than 0) | Initial backoff between attempts; doubles per attempt, capped at 2000 ms. |
| `alert-monitor-additional-ca-file` | unset (`outbound-http-additional-ca-file` applies) | PEM CA bundle added to the system trust store for requests to the endpoint, the startup check included. |
| `alert-monitor-additional-ca-path` | unset (`outbound-http-additional-ca-path` applies) | Hashed CA directory added to the system trust store for requests to the endpoint, the startup check included. |
| `alert-monitor-proxy` | unset (`outbound-http-proxy` applies) | Explicit proxy URL for requests to the endpoint, the startup check included. |

An invalid active configuration (bad URL scheme, empty index, missing or unparseable template, a template
that references a token the plugin does not supply, a value outside its range, auth with only one half set,
an invalid query spec, a query the query engine's parser rejects, two queries with one name, a query without a
target URL, a query with a template that has no `${data}`, or a query without
`plugin = sysio::query_engine_plugin`) fails startup with `plugin_config_exception` naming the option.

## Queries

With `plugin = sysio::query_engine_plugin` also enabled, each `alert-monitor-query` value is one WireQL query
(the query engine's SQL dialect; see `plugins/query_engine_plugin/README.md`) that the plugin runs against the
node's own state at its own block interval, sending each result as one document through the same template,
delivery queue, and endpoint as the status documents.

### Format

A value is a spec in the style of `--signature-provider`:

```
<snake_case_name>,<block_interval>,<query...>
```

- `<snake_case_name>` matches `[a-z][a-z0-9_]*` over the whole string and is unique among the queries. It is
  the `${data}` key of the query's documents and therefore a stable OpenSearch field path, `data.<name>`. The
  names `alert_monitor` (the status documents' key) and `thread` (which the logging framework's es sink writes
  as a string at `data.thread` in the same index) are reserved.
- `<block_interval>` is an integer from 1 to 4294967295: the query runs once each time LIB enters a new
  bucket of that many blocks (`lib / block_interval`). With intervals 1 and 10, both run at LIB 10 and only the
  first at LIB 11. Bucket 0 never runs, so a query first runs once LIB reaches its interval. Missed buckets are
  never caught up: after a pause or a jump only the current bucket runs.
- `<query...>` is everything after the second comma, commas included, and must not be blank.

Repeat the key in `config.ini`, or the flag on the command line, once per query:

```ini
plugin = sysio::query_engine_plugin
plugin = sysio::alert_monitor_plugin
alert-monitor-target-url = https://opensearch.example.com
alert-monitor-target-index = <your-index-or-write-alias>
alert-monitor-target-template-file = alert-monitor-document-template.json
alert-monitor-query = pending_underwrites,10,SELECT COUNT(*) AS n FROM uwreqs OWNER 'sysio.uwrit'
alert-monitor-query = top_positions,100,SELECT beneficiary, SUM(amount) AS total FROM sample.positions GROUP BY beneficiary ORDER BY total DESC LIMIT 5
```

```bash
nodeop --plugin sysio::query_engine_plugin --plugin sysio::alert_monitor_plugin \
       ... \
       --alert-monitor-query "pending_underwrites,10,SELECT COUNT(*) AS n FROM uwreqs OWNER 'sysio.uwrit'"
```

A query given in config.ini can contain neither `#` nor a newline, and loses trailing spaces and tabs:
`config.ini` cuts a line at its first `#`, reads one line per value, and trims spaces, tabs, carriage returns,
and newlines from both ends of the value. On the command line they are fine, but the option takes multiple
tokens, so the spec must be quoted there.

An invalid spec fails startup with `plugin_config_exception` naming the option, the value's position among the
option's values (command-line values come first), the format, and either the expected and actual field counts or
the failing field's index and label (`0 name`, `1 block_interval`, `2 query`). An error never reproduces the
spec's text -- a missing field shifts query text into the name and interval positions -- except a name that is
already valid, which the interval, query, and duplicate-name errors include so the entry can be found.

Each query is then parsed with the query engine's own parser and limits, under the same 1 s deadline it runs
with, so a query that does not parse, or that exceeds a parse limit (a query cut short at a `#` in config.ini
included), fails startup too. The error names the query, the error kind, the limit or the line and column when the
error has one, and the engine's message, never the query text. Unknown tables or columns and planning errors still
surface only when the query runs.

### Requirements and limits

- **The query engine.** `plugin = sysio::query_engine_plugin` must be enabled whenever a query is configured;
  startup fails otherwise. The engine can be configured on any node, but it runs chain reads only on read-only
  threads: on a node without `read-only-threads` (every producing node) each query fails with `STATE_UNAVAILABLE`
  and is counted. The engine refuses the `speculative` read mode and fails startup with its own message.
  `alert_monitor_plugin` itself does not require the engine and runs with status documents alone.
- **A fixed 1 s deadline.** Every execution has a fixed 1 s overall deadline, the engine's own default, in place
  of the node's `query-timeout-ms`, so queries must stay small. Each chain read within it is still bounded by
  `query-max-capture-ms`, which by default is the smaller of `query-timeout-ms` and the read-only transaction
  time, so a `query-timeout-ms` below the read-only transaction time still cuts alert queries short. The deadline
  also bounds shutdown.
- **`${data}` must be a whole value.** A query document's `${data}` is an object keyed by the query's name, so
  the template should write `"data": "${data}"`; a mixed string such as `"data": "x ${data}"` renders it as text.
  Startup fails when the template has no `${data}` at all; where it is placed is not checked.
- **One run at a time.** Each query has its own caller thread. A query that becomes due while its previous run
  is still active is skipped and counted.
- **Engine capacity.** Each running query holds one of the engine's `query-max-in-flight` slots (default 4) and
  runs on one of its `query-worker-threads` (default 2), both shared with HTTP queries. Queries whose intervals
  share a boundary come due at the same LIB, so size both for the queries that can be due together plus the
  expected HTTP load. An execution the engine refuses as `QUERY_BUSY` is retried every 10 ms until its 1 s
  deadline runs out, then counted and logged as `QUERY_BUSY`.
- **The sync gate.** Queries pause and resume with the status documents.
- **Size.** A document above the 256 KiB per-document cap is dropped and counted, never sent.
- **The delivery queue.** Query documents share the delivery queue with status documents, so a burst of query
  documents against a slow endpoint can fill it and drop status documents too.
- **Logging.** The plugin never logs a query's text; its diagnostics name the query by its name and position, and
  nodeop's "Non-default options" line masks the text. Elsewhere it can still appear: nodeop's own startup error
  echoes a malformed config.ini line or a mistyped option name, a query given on the command line is visible in
  the process list, and every result carries the table, owners, and column names the query read.
- **Absent documents.** A failed run sends no document, only the throttled warning. An alert rule that must notice
  a failing query watches for its documents stopping, not for an error document.

### Payload

A query document renders through the same template and tokens as a status document; only `${data}` (and
`${func}`, `render_query_document`) differ. `${data}` holds one member, keyed by the query's name:

```json
{"data":{"pending_underwrites":{"record":"query","version":1,
  "query":{"name":"pending_underwrites","ordinal":0,"block_interval":10},
  "trigger":{"lib_num":123450,"lib_id":"...","lib_time":"..."},
  "observed_at":"...",
  "result":{"schema_version":"1.1","complete":true,"source":{"owners":[],"table":"..."},
            "state":{"...":"captured block"},"columns":[],"rows":[],"stats":{},"page":{}}}}}
```

| Member | Value |
|---|---|
| `record` | `query` |
| `version` | `1`: the layout of this object |
| `query` | The entry's `name`, its `ordinal` (position among the option's values), and its `block_interval`; never the query text |
| `trigger` | The LIB that made the query due: `lib_num`, `lib_id`, and `lib_time` (null when get_info has none) |
| `observed_at` | Wall-clock time the result was rendered; the timestamp tokens carry the same instant |
| `result` | The query engine's result, unchanged: `schema_version`, `complete`, `source`, `state`, `columns`, `rows`, `stats`, `page`. `result.state` is the state the engine actually captured, not relabelled as the trigger block |

A status document's `${data}` holds one member, `alert_monitor`, with the same `record`/`version` header:
`{"alert_monitor":{"record":"status","version":1,"status":{...}}}`, where `status` is the get_info body.

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
| `message`, `logger` | `alert_monitor` |
| `file`, `line`, `func` | Source location of the renderer (the absolute source path the build compiled, the line, `render_document`), matching the shape the es log sink emits |
| `thread` | `alert_monitor` |
| `data` | An object with one member. A status document's is `alert_monitor`: `record` (`status`), `version` (`1`, the layout of this object), and `status`, the full `/v1/chain/get_info` body. A query document's is the query's name (see [Payload](#payload)) |

Referencing any other name fails startup, and the error names both the offending token and the supplied
set.

The source tree carries a sample at `etc/alert_monitor/document-template.json`:

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

Rendered with that template, one document looks like this (`alert_monitor.status` is the complete get_info
body; only a few of its fields are shown):

```json
{"@timestamp":1789432200123,"level":"INFO","message":"alert_monitor","category":"alert_monitor",
 "sourceLocation":"/build/src/plugins/alert_monitor_plugin/src/alert_monitor.cpp:<line> render_document",
 "data":{"alert_monitor":{"record":"status","version":1,
   "status":{"server_version":"4b599551","chain_id":"...","head_block_num":123456,
     "last_irreversible_block_num":123454,"last_irreversible_block_id":"...",
     "head_block_time":"2026-09-15T14:30:00.000","head_block_producer":"prod.a", "...":"..."}}},
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
  or a timeout -- is one warning naming the probed URL and the reason, and startup continues: a node never
  fails to start over its monitoring endpoint. Until the endpoint answers, each batch retries, then is dropped
  and counted. Configuration errors still fail startup.
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
  `alert-monitor-*` transport option falls back to its node-wide `outbound-http-*` counterpart.
- Shutdown unsubscribes from the irreversible_block channel, joins the query callers (each returns by its 1 s
  deadline), cancels an in-flight request, joins both workers, and logs the final counters. Documents and
  query runs still waiting are discarded, not flushed.

## Backpressure

The pipeline never blocks block application. The render queue holds at most 32 snapshots and the delivery
queue at most `max-pending-documents` rendered documents; when either is full, the newest item is dropped
and counted. Every drop and failure is visible in the counters below.

## Diagnostics

All diagnostics go through the `alert_monitor` logger (configure it like any other logger in
`logging.json`; `SIGHUP` re-binds it, but never re-reads the plugin options). Nothing is logged per block.

- Startup: one line reporting the endpoint (credentials stripped) reachable, or not reachable yet, and naming
  the index, the template's tokens, the batching limits, and the configured query names; or
  `no --alert-monitor-target-url provided, disabled`. An unreachable endpoint first logs one warning,
  `endpoint ... not reachable at startup; batches retry, then drop and count: <reason>`, and startup continues.
  A `403` from the root is not a failure (see Delivery semantics).
- Queries, per query: one warning for the first failed run, then at most one a minute for that query counting
  the failures in between, naming the query's name and position (never its text) and what failed: the query
  engine's error kind and message (with the limit it hit, or the line and column it names), `query service unavailable`,
  `document dropped: dropped_oversize`, `document dropped: dropped_queue_full`, or `unexpected error`.
- Liveness: one warning when documents pause because the irreversible block is behind wall-clock time, at
  most once a minute while paused, and one line when they resume.
- Delivery, per failed batch: one warning for the first bulk request that did not fully index, then at most
  one a minute counting the failed batches in between, naming the attempts it took, the failure detail (for
  example `HTTP 503 from https://opensearch.example.com`), and what became of its documents -- `N document(s)
  dropped` once the batch is given up on, `N of M document(s) rejected` for a partial bulk response, `N
  document(s) canceled` at shutdown. This line fires only on failure, so the per-block data path stays
  log-free.
- Delivery, summarized: when a bulk request fails or a status document is dropped, one warning carrying every
  counter (the query counters too, when queries are configured) and the last failure detail; when delivery
  succeeds again, one recovery line. These two share one rate limit of at most one line per minute between
  them, so an endpoint that alternates between failing and succeeding cannot log a line per block. Query
  failures and dropped query documents do not open this line; the per-query lines report them.
- Shutdown: one line with the final `documents_indexed`, `documents_failed`, `batches_indexed`,
  `batches_failed`, `snapshots_dropped_queue_full`, and `documents_dropped_queue_full` counters, plus the query
  counters when queries are configured.

Counters carried by the delivery warning:

| Counter | Meaning |
|---|---|
| `batches_indexed` / `batches_failed` | Bulk requests fully acknowledged / partial, rejected, unavailable, or canceled |
| `documents_indexed` / `documents_failed` | Documents acknowledged by the endpoint / rejected or never delivered |
| `snapshots_dropped_queue_full` | Snapshots rejected by a full render queue |
| `documents_dropped_queue_full` | Rendered status documents rejected by a full delivery queue |
| `documents_dropped_oversize` | Status documents larger than the per-document cap |
| `render_failures` | Template renders that threw (never expected) |

Query counters, carried by the delivery warning and the shutdown line when queries are configured:

| Counter | Meaning |
|---|---|
| `queries_succeeded` | Runs whose document was queued for delivery |
| `queries_skipped_busy` | Runs skipped because the same query's previous run was still active |
| `queries_failed` | Failed runs by query engine error kind, only the kinds that occurred (e.g. `[QUERY_TIMEOUT=2]`) |
| `queries_failed_other` | Runs that failed otherwise, including runs with no query service (before the engine starts or after it stops) |
| `query_documents_queued` | Query documents admitted to the delivery queue |
| `query_documents_dropped_oversize` | Query documents larger than the per-document cap |
| `query_documents_dropped_queue_full` | Query documents rejected by a full delivery queue |

## Relationship to the logging es sink

The plugin and the logging framework's `es_sink` share one delivery client, `fc::network::es::es_client`,
and one template engine, `fc::json_template`. The sink ships log records; this plugin ships chain status and
query results.
They can target the same endpoint, and because the token vocabulary and the timestamp/level rendering are
shared, a template written for one produces the same field shapes in the other.

## Tests

```bash
ninja -C build/debug test_alert_monitor_plugin
./build/debug/plugins/alert_monitor_plugin/test/test_alert_monitor_plugin
```

The suite covers option registration and validation, rendering of the shipped sample template and of every
supplied token, bulk-body assembly and splitting, the two-stage pipeline with an injected
sender, drop and failure accounting, progress classification, and the plugin lifecycle without a running
node. For queries it covers spec parsing (repeated on the command line and in `config.ini`, every rejected form,
and errors that never reproduce query text), the startup parse under the engine's limits (including a query cut at
a config.ini `#`), the `${data}` requirement, the template path relative to the config directory, the due rule,
and the query runner over a scripted query service: one caller per query, skips while a run is active, failures by
kind, a missing service, dropped documents, the fixed deadline on every execution, `QUERY_BUSY` retries within it,
and bounded shutdown and destruction. One case runs a query end to end: a real query engine over the query engine
plugin's own chain fixture (the `query_fixture` test contract, so the build needs `BUILD_TEST_CONTRACTS`), through
the pipeline and a real es client to a loopback capture server. No test dials an external endpoint.

# Query engine plugin

`sysio::query_engine_plugin` exposes a shared C++ query service and, when HTTP is enabled,
`POST /v1/query/execute`. The plugin class lives in `sysio`; every other plugin type, including the generated parser, lives in `sysio::query_engine`. All ABI and table access is in process. HTTP carries client requests and
responses; no table HTTP client, external database, historical snapshots or retained cursors are used.

## Enablement

The plugin is linked into `nodeop` and is **opt-in**. Add to the query node's existing `config.ini`:

```ini
plugin = sysio::query_engine_plugin
read-mode = head
```

The required plugins are `chain_plugin` and `producer_plugin`. HTTP is optional: enable
`sysio::http_plugin` separately to expose the route on its existing `chain_ro` listener. A plain
`http-server-address` serves every category, so the route needs no `chain_api_plugin`; a node that
routes categories with `http-category-address chain_ro,...` must also load `chain_api_plugin`,
because `http_plugin` accepts a category address only for a configured plugin. If HTTP is absent,
registered but not initialized, or has no enabled `chain_ro` listener, the C++ service still starts.
The query plugin adds no listener or port option.

Every chain read runs on the executor's **read-exclusive** queue, which only `producer_plugin`'s
read-only threads drain, inside a read window the application thread never enters. Startup
therefore requires `read-only-threads` to be greater than zero: `producer_plugin` defaults it to 3
only when `chain_api_plugin` is configured, so a node without `chain_api_plugin` sets it explicitly.
It rejects configured `producer-name` values and speculative read mode. `irreversible` read mode is
also supported. P2P synchronization remains the node's responsibility.

## C++ service

Retrieve the service after plugin startup and call it from a worker thread:

```cpp
using namespace sysio::query_engine;
auto service = appbase::app().get_plugin<sysio::query_engine_plugin>().get_query_service();
query_result result = service->execute(
   "SELECT beneficiary, SUM(amount) AS total FROM sample.positions GROUP BY beneficiary ORDER BY total DESC",
   query_options{.timeout = constants::no_deadline, .limit = 20, .offset = 0});
for (const fc::variant_object& row : result.rows) {
   // Consume owned row values.
}
```

The public interface is `query_result execute(const std::string& query,
const std::optional<query_options>& options = std::nullopt)`. `query_result` carries `schema_version`,
`complete`, `source`, `state`, `columns`, `stats`, and `std::vector<fc::variant_object> rows`.
Failures throw `query_error`; C++ execution does not create an RPC envelope.

`query_options.timeout` is an optional `std::chrono::milliseconds`. Unset, it applies the configured
`query-timeout-ms`, because `execute` blocks its caller; `constants::no_deadline` is the explicit
opt-out; zero expires immediately and negative values are invalid. `limit` is optional and `offset`
defaults to zero. Offset skips
completed, ordered output rows; the smaller of an explicit SQL LIMIT and the per-call limit then
applies. Pagination never reduces the input used for aggregation. Configured scan, memory, group,
capture-duration and result-row caps still apply. SQL OWNER specifies accounts.

`execute` enqueues a `query_task` on `fc::parallel::worker_task_queue` and blocks its caller. The
query worker parses, plans and evaluates, calling `query_read_api` for internally synchronized reads.
The read API schedules only raw ABI/row capture through the controller's read-exclusive queue and
waits for owned copies; ABI hashing and decoding, descriptor compilation and row decoding all run
on the query worker. Do not call blocking `execute` from a chain executor callback; application-thread
calls are rejected before admission. HTTP has its own bounded `worker_task_queue`, whose workers
invoke `execute`, so HTTP waiters cannot occupy the query workers they depend on.

`get_query_service()` returns null before startup and after shutdown. A retained service remains
valid after shutdown and rejects execution with `QUERY_CANCELLED`.

## Query language

```sql
SELECT beneficiary, COUNT(*) AS records, SUM(amount) AS total
FROM positions OWNER 'sample', 'other'
WHERE amount >= 10
GROUP BY beneficiary
HAVING total > 100
ORDER BY total DESC
LIMIT 20
```

`OWNER` identifies contract accounts and combines rows of the **same table** under those accounts.
Every selected owner's ABI must declare matching key/value field names, ABI type names and shapes. Up to 64 distinct owners
are accepted. There is no account discovery. An unqualified table requires `OWNER`; a single owner
can instead be supplied as `FROM sample.positions`. Combining a qualified table with `OWNER` is an
error. Owners belong in SQL, not JSON-RPC parameters. This syntax does not perform a JOIN.

Keywords are case insensitive. Identifiers preserve case; double quotes permit dots in names
(`FROM "sample.one".positions`) and are how a field or table whose name is a keyword is written
(`SELECT "count" FROM sponsorcount OWNER 'sysio.roa'`; the output column is still `count`). Strings
use single quotes with doubled quote escaping. There are
no comments or multiple statements. Fields resolve against row values; `value.nested.score` is
explicit value access and `key.id` selects the primary key. `SELECT *` projects the value fields and
must stand alone.

Supported clauses, in order: SELECT, FROM, OWNER, WHERE, GROUP BY, HAVING, ORDER BY, LIMIT.
Supported predicates: `=`, `!=`, `<>`, `<`, `<=`, `>`, `>=`, `IS [NOT] NULL`, `NOT`, `AND`, `OR`,
and parentheses. Precedence is comparison, NOT, AND, OR. Literals are signed decimal integers,
decimal fractions with at most 18 places, strings, TRUE, FALSE and NULL. Exponents are unsupported.

Aggregates are `COUNT(*)`, `COUNT(field)`, `SUM`, `AVG`, `MIN`, and `MAX`. Each selected aggregate
requires `AS`. Selected nonaggregate fields must be grouped. HAVING can use grouped fields,
aggregate calls (including unselected aggregates), and aggregate aliases. Ambiguous aliases are
rejected. WHERE cannot use aggregates or aliases. Output names must be unique.

ORDER BY uses selected output names only. NULL sorts last in either direction. Ties and unordered
detail results follow owner chain-name order, then encoded primary-key order; grouped results use
canonical typed group-key order. LIMIT follows complete grouping, HAVING and ordering. LIMIT 0 is
valid. Exceeding a server cap fails the query; it never silently truncates totals.

No JOIN, subquery, UNION, DISTINCT, SQL OFFSET, arbitrary arithmetic/functions, writes or DDL.
C++ pagination uses `query_options.offset`; HTTP params remain exactly `{query}`.
Primary-key equality, leading composite-key prefix and range predicates can narrow capture. Other
predicates use a bounded scan and residual evaluation. Secondary-index optimization is not included.

## Values and response

Send a JSON-RPC 2.0 object with method `query.execute` and exactly one named parameter, `query`:

```json
{"jsonrpc":"2.0","id":"totals","method":"query.execute","params":{"query":"SELECT SUM(amount) AS total FROM sample.positions"}}
```

IDs may be null, strings of at most 128 Unicode characters, or integral JSON numbers in
[-4294967295, 4294967295]. Use string IDs outside this range. Valid requests return HTTP 200 with
either `result` or `error`. Notifications omit `id` and return HTTP 204 with an empty body, including
invocation failures. Malformed envelopes and unsupported batch arrays return errors with null ID.
HTTP limits enforced before dispatch retain the HTTP plugin's normal transport behavior.

Every successful result contains `schema_version`, `complete: true`, `source`, `state`, `columns`,
`rows`, and `stats`. `source.owners` and `state.abis` identify every selected owner. `state.block_id`
identifies the applied state captured by all internal pages; metadata also includes read mode,
irreversible height, chain ID, capture time and local sync status. `synced: false` does not prevent
reads. A head state may subsequently be forked out; this is a current-state read, not a finality claim.

All numeric cells, including nested numbers and asset precision, are decimal **strings**. Integers
through 128 bits are exact; aggregation uses checked 256-bit integers and checked 512-bit comparison
temporaries. AVG is an exact rational during predicates/ordering, rendered to 18 decimal places with
round-half-even and trailing zeros removed. Assets return `{amount, symbol, precision}`; extended
assets also return `contract`. Asset aggregation and ordering require matching denomination; `=` and
`!=` between different denominations are simply unequal.

Booleans remain JSON booleans. Optional fields and absent binary extensions are null. Comparisons
with null are UNKNOWN; WHERE/HAVING retain only TRUE. COUNT(field) ignores null; other aggregates
ignore null and yield null on empty input. An ungrouped empty aggregate produces one row, with
COUNT zero; a grouped empty query produces no groups.

Structs, arrays and variants support lossless projection, with scalar paths through structs. Whole
containers cannot be compared, grouped or ordered. ABI floats are projected as `0x` plus their raw
little-endian IEEE bytes; they cannot participate in predicates, ordering or aggregates. No host
floating-point arithmetic is used. Malformed bytes in a referenced field, or a row whose length
disagrees with its ABI layout, fail with ROW_DECODE_ERROR for a query that references any value
field; the bytes of fields a query never references are skipped by structure, not validated, and
`COUNT(*)` or a key-only query reads no value bytes at all.

ABI enums have logical type `enumeration`: cells carry the member name (the decimal value for an
unlisted value), like `get_table_rows`, while comparisons, GROUP BY, ORDER BY, MIN and MAX use the
underlying integer. A string literal that spells an integer binds that value; any other string binds
by exact member name, then by the unique member whose name ends in `_` plus the literal (`'READY'`
for `MESSAGE_STATUS_READY`), the member-name rules `abi_serializer` applies to action data (where,
unlike here, a numeric string is not an integer); an unknown or ambiguous name fails with
VALUE_ERROR. Integer literals compare directly. SUM and AVG reject enums.

Timestamps render as UTC ISO 8601 with microsecond precision, omitting a zero fraction, for every
stored value: years outside 0000..9999 use the expanded form with an explicit sign
(`+10000-01-01T00:00:00`), so no stored instant makes a table unqueryable. Timestamp literals accept
the same form. Checksum and `bytes` literals must be hex of the exact width and are lowercased;
`public_key` and `signature` literals are re-rendered through the chain codec, so a range bound
and the residual predicate always agree with the decoded text.

Only the fields a query references are decoded; every other ABI node is skipped, so `COUNT(*)`
reads no row bytes and a projected scan is charged for one decoded row at a time.

See [schema/README.md](schema/README.md), the normative schemas, and [examples/request.json](examples/request.json).

## Resource limits

All options are immutable, positive integers available through CLI or config.ini.

| Option | Default | Meaning |
|---|---:|---|
| query-worker-threads | 2 | Query workers; the optional HTTP adapter has the same number of separate workers |
| query-max-in-flight | 4 | Engine admissions including retained reads; also the independent HTTP ingress cap |
| query-max-query-bytes | 16384 | SQL bytes before ANTLR |
| query-timeout-ms | 1000 | Request deadline, including ingress/queue time; C++ callers may opt out per call |
| query-max-capture-ms | producer_plugin's read-only transaction time | Each chain read callback (the ABI copy and the coherent data capture); unset, the smaller of that time and `query-timeout-ms`; a configured value may not exceed it |
| query-max-abi-bytes | 1048576 | ABI blob size per selected owner, checked before copy |
| query-max-scan-rows | 100000 | Total candidate rows across owners |
| query-max-raw-bytes | 67108864 | Copied ABI, raw rows, continuation and capture overhead |
| query-max-memory-bytes | 134217728 | Conservative allocation charges per request; a decoded row's charges are released once it is folded in |
| query-max-groups | 10000 | Aggregate groups |
| query-max-result-rows | 10000 | Output rows after offset and SQL/per-call limit |
| query-max-response-bytes | 8388608 | Encoded success envelope bytes |

Additional bounds are 4096 tokens, depth 64, 2048 AST/ABI descriptor nodes, 64 owners and 512 rows
per internal page; `error.data.limit` names them `sql-tokens`, `sql-depth`, `sql-nodes`,
`sql-owners`, `abi-depth` and `abi-nodes`, distinct from the option names. Pages do not yield to
another chain state. The capture bound is producer_plugin's read-only transaction time — the
smaller of `max-transaction-time` and the effective read-only read window less its minimum — and
every check inside a read callback also honors the current read window's own end, so a capture that
starts late in a window stops at the window's end instead of running its full budget past it and
delaying the next write window; the read is then queued again for the next window, with the cut
attempt's rows and bytes uncharged, while the request deadline allows. Only that deadline fails it,
and a `QUERY_TIMEOUT` raised while a read the window cut short is waiting for, or running in, the
next window names `read-only-read-window-time-us` as its limit; the `query` logger's shutdown
counters include how many reads were cut. A configured
`query-max-capture-ms` may only tighten the budget and cannot exceed the request timeout; unset, the
built-in default is clamped to the request timeout. Raw memory cannot exceed total memory; aggregate
admitted-memory multiplication is checked.
Default engine admission permits 512 MiB of accounted execution memory. When HTTP is enabled,
its independent ingress/response admission can retain another 512 MiB; each HTTP request shares
one budget across ingress, execution and serialization. C++ callers own their returned results
and are responsible for how many completed results they retain. This is not a process RSS ceiling: the node,
HTTP buffers, bounded ANTLR allocations and allocator overhead are separate. Lower or narrower
limits fail explicitly with no partial response; expensive queries need narrower criteria.

The read API synchronizes two captures internally (raw ABI bytes, then all data pages); both
callbacks copy bytes and nothing else, so each holds the read window for time proportional to the
bytes copied. Workers hash and decode the ABI, compile descriptors, and decode and evaluate owned
row copies after capture. ABI changes before capture fail with SCHEMA_CHANGED.
Workers never hold database iterators. Deadline/shutdown cancellation completes once; cancelled
callbacks retain their admission token until drained. Shutdown joins workers without waiting on
future app-thread work. Client disconnect does not provide immediate cancellation through the
existing callback API; the configured HTTP deadline and shutdown still bound the work.

The `query` logger reports startup limits, throttled unexpected failures, and shutdown counters.
Engine counters count C++ execution outcomes and produced rows; HTTP envelope-size failures and
HTTP ingress rejections belong to the adapter boundary. Normal logs do not include SQL or row values. SIGHUP rebinds the logger only.

## Errors

| Code | Kind |
|---:|---|
| -32700 | PARSE_ERROR |
| -32600 | INVALID_REQUEST |
| -32601 | METHOD_NOT_FOUND |
| -32602 | INVALID_PARAMS |
| -32010 | QUERY_SYNTAX |
| -32011 | QUERY_SEMANTICS |
| -32012 | QUERY_LIMIT |
| -32013 | QUERY_TIMEOUT |
| -32014 | QUERY_BUSY |
| -32015 | SCHEMA_CHANGED |
| -32016 | ROW_DECODE_ERROR |
| -32017 | VALUE_ERROR |
| -32018 | STATE_UNAVAILABLE |
| -32019 | QUERY_CANCELLED |
| -32603 | INTERNAL_ERROR |

Error data always contains `kind`, `retryable`, `line`, `column`, and `limit`. Source positions are
1-based when present. Retryability is advisory; the server never automatically retries. BUSY,
SCHEMA_CHANGED, STATE_UNAVAILABLE, cancellation and timeout are retryable; unchanged syntax,
semantic, value and limit failures are not.

## Build and validation

`vcpkg.json` pins the `antlr4` runtime to 4.13.2 in its overrides and adds the `antlr4-tools` host
dependency. The latter is a
`wire-vcpkg-registry` port that installs the checksum-pinned official generator JAR under
`tools/antlr4`; the upstream runtime port supplies no JAR. CMake locates it through vcpkg's host
tool paths and passes it to the runtime port's `antlr4-generator` module, which generates the
parser into the build tree (`<build>/plugins/query_engine_plugin/generated/WireQuery`) whenever
`grammar/WireQuery.g4` changes. Nothing generated is committed, and the generator and runtime must
both match the pinned version.

Generation needs a Java runtime, which is a build requirement: on Ubuntu 24.04 install
`openjdk-21-jre-headless` from the default repositories; on macOS `brew install openjdk` and put
`$(brew --prefix openjdk)/bin` on `PATH` (Homebrew keeps it keg-only). The Docker build image and
CI install it the same way; the devcontainer image does not include a JRE yet, and a matching
`wire-devcontainer` change adds `openjdk-21-jre-headless`. CMake checks for a Java 11 runtime before
the generator module runs and names the package to install when it is missing.

Build the default all target in the configured build directory. See [test/README.md](test/README.md)
for real-controller, HTTP, scheduling, workload and shared-reader regression tests. Schema, examples
and README files install under `${CMAKE_INSTALL_DATAROOTDIR}/wire/query_engine_plugin`
(`share/wire/query_engine_plugin` by default).

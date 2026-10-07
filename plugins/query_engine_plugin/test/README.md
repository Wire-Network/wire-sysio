# Query engine validation

The tests use the production ANTLR parser, planner, decoder, evaluator, source, synchronized read API, engine, public service and HTTP handler. A CDT
`query_fixture` WASM contract supplies real ABI tables and signed actions, replicated into an
actual validating controller. No external database, cluster or network-table mock is required.

From the repository root, after the normal full build with tests and test contracts enabled:

```sh
build/release/plugins/query_engine_plugin/test/test_query_engine_plugin --log_level=test_suite
build/release/plugins/query_engine_plugin/test/test_query_engine_plugin_http --log_level=test_suite
build/release/tests/plugin_test --run_test=get_table_tests
build/release/plugins/status_monitor_plugin/test/test_status_monitor_plugin
```

Substitute the configured build directory. CTest registers the first query binary as parallelizable
and the application/HTTP binary as nonparallelizable because it uses appbase process globals.
HTTP response compatibility cases live in the existing HTTP-plugin test executable.

The HTTP fixture starts the actual plugin/dependency closure through `chain::application`, applies
signed blocks through the producer plugin's scheduled incoming-block path, and uses a temporary
Unix socket with TCP explicitly disabled. The plugin is enabled through `--plugin`; a separate test
verifies the route is absent when omitted. Every actual query response and checked-in JSON example
is validated against the shipped schema plus dynamic column/cell invariants.

Lifecycle tests use deterministic stage barriers and injected clocks to exercise queue deadlines,
admission, cancellation and exactly-once completion. Executor tests pause after the first 512-row
page and queue a signed block update, with one and with two read-only threads, on the
read-exclusive queue the plugin schedules its reads on. The update must remain excluded through
capture; worker evaluation of owned bytes then returns the old state and the next query returns the
new state. A deterministic capture timeout must leave subsequent block application live. Startup
guards reject zero read-only threads and a `query-max-capture-ms` above producer_plugin's read-only
transaction time.

`query_application_representative_workload` reports native capture microseconds, copied bytes,
accounted capture memory, configured admitted memory, total concurrent HTTP latency, scanned rows
and continued block progress for a bounded 1027-row/four-request workload. This is workload-specific
evidence, not a universal throughput guarantee. Use `--log_level=message` to retain the measurements.

The parser is generated into the build tree from `grammar/WireQuery.g4` by the vcpkg-installed
pinned ANTLR JAR and the system Java runtime on every build that changes the grammar; nothing
generated is committed, and a configure without Java fails at the generator module.
Build fixtures through CMake/CDT and copy the generated ABI/WASM into the source fixture directory.
Do not regenerate unrelated chain snapshot/reference fixtures.

`query_execute` covers owned typed results/metadata, the public virtual execute interface, worker
execution, the configured default deadline and the explicit no-deadline opt-out, timeout
validation, per-call limit/offset, aggregate pagination, SQL LIMIT composition, configured caps and
stopped services. Lifecycle tests exercise the blocking API with deterministic queued reads and
stage barriers, plus HTTP queue saturation/deadlines while its only worker is blocked in execute.
The application tests also execute queries with HTTP registered but disabled, and with HTTP enabled
without a listener, using one/two read threads. The plugin requires chain_plugin and
producer_plugin. Integration tests cover ABI enum names in results and literals, columns named
`key`/`value`, decode accounting over a 16k-row scan at the default limits, timestamps outside the
four-digit-year range, case-insensitive checksum literals, every primary-key range predicate against
a forced full scan, which limit each budget case reports, arrays of extension-only structs skipped
element by element, several `fixed_bytes<32>` fields in one row type, retained memory that does not
scale with scanned rows, and the `fasp` table whose id 0xFFFF sits at the partition boundary
(forward query and reverse page). `query_rpc` pins the error envelope (code, retryability, position,
limit name, echoed ID) and the capture-bound clamp for a short timeout; `query_language` pins the
token bound below the node bound and quoted keywords as identifiers; lifecycle tests stop the HTTP
handler with requests in flight and re-queue a read the window cut short, restoring the cut attempt's
charges (rows and bytes against an uncut reference run, every counter at unit level), naming the
window in a later deadline failure over both the C++ and the HTTP path until a retry succeeds, and
never retrying a capture that overran its own budget; `query_execute` pins the caller-owned budget
contract (required, single use). Integration tests also pin that a text extreme,
finalized aggregates and the engine's peak statistic track live state. The application tests pin the
read-exclusive queue (a write window longer than the deadline times a query out), the capture budget
derived from the read window, and that the plugin binds the window on every read (the capture
deadline a budget carries after an engine run, and the plugin's window function on a read thread).

Every implementation/parser namespace is `sysio::query_engine`; the appbase plugin name is
`sysio::query_engine_plugin`.

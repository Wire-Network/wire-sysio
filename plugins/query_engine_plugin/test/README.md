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
page and queue a signed block update, with both zero and two read-only threads. The update must
remain excluded through capture; worker evaluation of owned bytes then returns the old state and
the next query returns the new state. A deterministic capture timeout must leave subsequent block
application live.

`query_application_representative_workload` reports native capture microseconds, copied bytes,
accounted capture memory, configured admitted memory, total concurrent HTTP latency, scanned rows
and continued block progress for a bounded 1027-row/four-request workload. This is workload-specific
evidence, not a universal throughput guarantee. Use `--log_level=message` to retain the measurements.

Generated grammar sources must match the `check_query_parser` CMake target using the vcpkg-installed
pinned ANTLR JAR and system JVM. Configure must repopulate absent, empty or incomplete generated
sources, fail clearly without a JVM when generation is needed, and accept complete sources without Java.
Build fixtures through CMake/CDT and copy the generated ABI/WASM into the source fixture directory.
Do not regenerate unrelated chain snapshot/reference fixtures.

`query_execute` covers owned typed results/metadata, the public virtual execute interface, worker
execution, default unlimited timeout, timeout validation, per-call limit/offset, aggregate pagination,
SQL LIMIT composition, configured caps and stopped services. Lifecycle tests exercise the blocking
API with deterministic queued reads and stage barriers, plus HTTP queue saturation/deadlines while
its only worker is blocked in execute. The application tests also execute queries
with HTTP registered but disabled, and with HTTP enabled without a listener, using zero/two read
threads. The plugin's only required dependency is chain_plugin.

Every implementation/parser namespace is `sysio::query_engine`; the appbase plugin name is
`sysio::query_engine_plugin`.

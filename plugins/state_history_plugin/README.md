# state_history_plugin

`state_history_plugin` (SHiP) records per-block transaction traces, chain-state deltas, and finality data into
append-only log files on disk, and serves them to clients over a binary websocket protocol. It is what
downstream indexers, explorers, and history APIs consume instead of replaying the chain themselves. The plugin
is opt-in -- `nodeop` registers it, but it is only initialized when `plugin = sysio::state_history_plugin` names
it -- and it depends on `chain_plugin` through `APPBASE_PLUGIN_REQUIRES`. Enabling the plugin alone records
nothing: at least one of `--trace-history`, `--chain-state-history`, or `--finality-data-history` must also be
set.

## How it works

### Recording

The plugin connects three `chain_plugin` controller signals during `plugin_initialize`:

| Signal | Handler | Effect |
|---|---|---|
| `block_start` | `on_block_start` | Clears the trace converter's cached traces for the new block. |
| `applied_transaction` | `on_applied_transaction` | Accumulates the trace and its packed transaction (only when the trace log is enabled). |
| `accepted_block` | `on_accepted_block` | Writes one entry into each enabled log, then notifies every connected session that a block was applied. |

Up to three independent logs are maintained, each created only when its option is set, and each a `log_catalog`
rooted at the state-history directory:

| Option | Log base name | Content |
|---|---|---|
| `--trace-history` | `trace_history` | Per-block transaction traces, packed by the trace converter. |
| `--chain-state-history` | `chain_state_history` | Per-block chainbase deltas. The first entry written into an empty log is a full state image. |
| `--finality-data-history` | `finality_data_history` | `controller::head_finality_data()` per block. |

If any of those writes throws, the plugin calls `appbase::app().quit()` **and** rethrows
`state_history_write_exception`; both are required, because the throw is what keeps the block from being
committed while the quit is what actually stops the node. That fail-stop behavior is the default;
`--state-history-force-write` replaces it (see below).

Two side effects an operator should expect at initialization:

- When `--chain-state-history` is on and `--disable-replay-opts` was not set, the plugin forces
  `disable_replay_opts = true` on the controller and logs the fact. Delta recording is incompatible with the
  replay optimization.
- If `resource_monitor_plugin` is loaded (looked up with `find_plugin`, so it is optional), the state-history
  directory is registered with it for free-space monitoring.

`--delete-state-history` removes the entire state-history directory during initialization, before the logs are
opened. The directory is then recreated.

### Serving

`plugin_startup` computes `first_available_block` -- the minimum of the controller's earliest available block
and the first block of each enabled log -- logs it, opens the listeners, and starts a one-thread `ship` pool. If
the chain has a non-zero head block and the chain-state log is empty, the initial full state is written first,
bracketed by two log lines, and this can take a considerable amount of time.

Two listeners are possible and both may be active at once: a TCP listener on `--state-history-endpoint` and a
unix-domain listener on `--state-history-unix-socket-path`. Both run on the `ship` pool's executor, so stopping
the pool at shutdown also stops accepting. Each accepted socket is handed to the main application thread, which
constructs a `session` and stores it in the connection set; the set is only ever touched by the main thread
because `on_accepted_block` iterates it.

A session runs two coroutines on its own strand -- a read loop and a write loop. Each dispatches the parts that
touch controller state onto the main application thread and then resumes on the strand, so per-connection
socket work never runs on the main thread and controller reads never run off it. Sockets get `TCP_NODELAY`, a
1 MiB send buffer, and a 512 KiB websocket write buffer.

## Websocket protocol

A client connects to `--state-history-endpoint` (or the unix socket) and performs a normal websocket handshake;
the server's response carries `Server: state_history/<nodeop version>`.

1. **ABI frame.** Immediately after `async_accept`, the server writes one **text** message containing the
   state-history ABI JSON (the `state_history_plugin_abi` blob in `libraries/state_history/abi.cpp`). A client
   uses it to decode everything that follows. After this frame the stream switches to binary.
2. **Requests** (client to server) are the `state_request` variant, packed with `fc::raw`. The variant index is
   part of the wire format and new messages are only ever appended, so the indices below are fixed:

   | Index | Request | Fields |
   |---|---|---|
   | 0 | `get_status_request_v0` | (none) |
   | 1 | `get_blocks_request_v0` | `start_block_num`, `end_block_num`, `max_messages_in_flight`, `have_positions`, `irreversible_only`, `fetch_block`, `fetch_traces`, `fetch_deltas` |
   | 2 | `get_blocks_ack_request_v0` | `num_messages` |
   | 3 | `get_blocks_request_v1` | everything in v0 plus `fetch_finality_data` |
   | 4 | `get_status_request_v1` | (none) |

3. **Results** (server to client) are the `state_result` variant:

   | Index | Result | Fields |
   |---|---|---|
   | 0 | `get_status_result_v0` | `head`, `last_irreversible`, `trace_begin_block`, `trace_end_block`, `chain_state_begin_block`, `chain_state_end_block`, `chain_id` |
   | 1 | `get_blocks_result_v0` | `head`, `last_irreversible`, `this_block`, `prev_block`, `block`, `traces`, `deltas` |
   | 2 | `get_blocks_result_v1` | everything in v0 plus `finality_data` |
   | 3 | `get_status_result_v1` | everything in v0 plus `finality_data_begin_block`, `finality_data_end_block` |

   A `get_blocks_request_v1` is answered with `get_blocks_result_v1`, a v0 request with `get_blocks_result_v0`.
   `*_begin_block` / `*_end_block` come from each log's block range, and are zero for a log that is not enabled.

**Flow control.** `max_messages_in_flight` on the blocks request is a credit counter. Each block result the
server sends consumes one credit; `get_blocks_ack_request_v0.num_messages` adds that many back. With zero
credits the write loop sends nothing and sleeps. Status responses are not credited and are written before the
block in the same pass.

**Cursor and forks.** The server streams from `start_block_num` towards `end_block_num`, advancing one block per
result and never past the head block -- or past the last irreversible block when `irreversible_only` is set. The
client's `have_positions` are consulted once per blocks request: for each position whose block id does not match
the server's id for that block number, `start_block_num` is pulled back to that block number. While streaming,
if a block is applied whose number is lower than the current cursor (a fork being applied over already-sent
blocks), the cursor rewinds to it.

**Status request limit.** Pending status requests are counted per session and capped at 1024
(`max_status_request_queue_depth`). A client that exceeds it has its session failed with
`state history status request queue limit exceeded`.

**Entry framing.** Trace, delta, and finality payloads are written as continuation frames: a boolean saying
whether the entry is present, then, when present, the uncompressed size followed by the decompressed bytes
streamed in 1 MiB chunks. The whole block result is one websocket message terminated by a final empty frame.

## Log files on disk

Everything lives under the state-history directory -- `--state-history-dir`, default `state-history`, resolved
relative to the node's data dir when the value is relative. Each enabled log is a pair of files named for the
log's base name:

```
<state-history-dir>/trace_history.log          <state-history-dir>/trace_history.index
<state-history-dir>/chain_state_history.log    <state-history-dir>/chain_state_history.index
<state-history-dir>/finality_data_history.log  <state-history-dir>/finality_data_history.index
```

The `.log` holds `<entry><position of entry>` repeated; the `.index` holds just the positions, and can be
regenerated from the `.log` if it is removed. Each entry is a `state_history_log_header` followed by the
payload.

Three retention shapes are mutually exclusive, selected by which options are set:

- **Flat (default).** One `.log`/`.index` pair per enabled log, growing without bound.
- **Pruned**, when `--state-history-log-retain-blocks` is set. The log keeps only that many most recent blocks;
  the value must be at least 1000 so that newly applied forks can still be delivered to clients before being
  pruned out. This option cannot be combined with any partition option.
- **Partitioned**, when any of `--state-history-retained-dir`, `--state-history-archive-dir`,
  `--state-history-stride`, or `--max-retained-history-files` is set. On reaching a stride boundary the current
  pair is renamed `<log name>-<start num>-<end num>.log` / `.index` and moved into the retained directory, and a
  new current pair is started; the retained files together form an extended history that is still served.
  Setting any one of the four turns partitioning on and the others take their defaults: retained dir `retained`,
  archive dir `archive`, stride `1000000`, and an unbounded retained-file count. When the retained count exceeds
  `--max-retained-history-files`, the oldest group is moved to the archive directory, or deleted if the archive
  directory is the empty string. **Retained log files must not be manipulated by hand**; files placed in the
  archive directory are entirely the operator's, and `nodeop` never touches them again.

`--state-history-force-write` is the emergency-recovery escape hatch. Without it, a log that fails its startup
checks or cannot accept the next block is fatal. With it, such a bundle is moved aside to
`<stem>-corrupt-<n>.log` / `.index` -- kept on disk, never deleted -- and writing continues into a fresh log, so
the node stays up. The cost is completeness: blocks held only in the set-aside logs, plus any the node skips
while recovering, are missing from the history this node serves. `sys-util`'s `ship-log` subcommand inspects,
repairs, and trims the set-aside logs, and can merge them back once the `-corrupt-` infix is renamed off.

## Enabling / configuration

`config.ini`:

```ini
plugin = sysio::state_history_plugin

# Nothing is recorded unless at least one of these is enabled.
trace-history = true
chain-state-history = true
finality-data-history = true

state-history-dir = state-history
# Caution: only expose this port to your internal network.
state-history-endpoint = 127.0.0.1:8080

# Partitioned retention: roll every 1,000,000 blocks, keep 10 groups,
# move older groups into the archive directory.
state-history-stride = 1000000
max-retained-history-files = 10
state-history-retained-dir = retained
state-history-archive-dir = archive
```

Command line:

```bash
nodeop --plugin sysio::state_history_plugin \
       --trace-history \
       --chain-state-history \
       --finality-data-history \
       --state-history-dir state-history \
       --state-history-endpoint 127.0.0.1:8080 \
       --state-history-stride 1000000 \
       --max-retained-history-files 10
```

A pruned, single-pair deployment instead (the two shapes cannot be combined):

```bash
nodeop --plugin sysio::state_history_plugin --trace-history \
       --state-history-log-retain-blocks 100000
```

To start a node's history from scratch, add the command-line-only `--delete-state-history`.

## Options

Every option is registered into the `config.ini` options description -- and so is accepted in `config.ini` and on
the command line -- except `delete-state-history`, which is registered on the command line only.

| Option | Default | Meaning |
|---|---|---|
| `state-history-dir` | `state-history` | Location of the state-history directory; absolute, or relative to the application data dir. |
| `state-history-retained-dir` | unset (`retained` once partitioning is on) | Location of the state-history retained directory; absolute, or relative to the state-history dir. |
| `state-history-archive-dir` | unset (`archive` once partitioning is on) | Location of the state-history archive directory; absolute, or relative to the state-history dir. An empty string means blocks files beyond the retained limit are deleted instead of archived. Files in the archive dir are entirely under the operator's control -- `nodeop` does not access them again. |
| `state-history-stride` | unset (`1000000` once partitioning is on) | Split the state-history log files when the block number is a multiple of the stride. The current log and index are renamed `*-history-<start num>-<end num>.log`/`.index` and a new current pair is created; all files following that format are used to construct an extended history log. |
| `max-retained-history-files` | unset (unbounded once partitioning is on) | Maximum number of history file *groups* to retain so the blocks in them can still be queried. Beyond it the oldest group is moved to the archive dir, or deleted if the archive dir is empty. Retained history log files should not be manipulated by users. |
| `delete-state-history` | `false` (command line only) | Clear state history files. |
| `trace-history` | `false` | Enable trace history. |
| `chain-state-history` | `false` | Enable chain state history. |
| `finality-data-history` | `false` | Enable finality data history. |
| `state-history-endpoint` | `127.0.0.1:8080` | The endpoint upon which to listen for incoming connections. Caution: only expose this port to your internal network. |
| `state-history-unix-socket-path` | unset | Path (relative to data-dir) at which to create a unix socket to listen for incoming connections. |
| `trace-history-debug-mode` | `false` | Enable debug mode for trace history. |
| `state-history-log-retain-blocks` | unset | If set, periodically prune the state history files to store only this many most recent blocks. Must be at least 1000, and cannot be used together with `state-history-retained-dir`, `state-history-archive-dir`, `state-history-stride`, or `max-retained-history-files`. |
| `state-history-force-write` | `false` | EMERGENCY RECOVERY option: never let damaged or inconsistent state history logs prevent the node from running. A log that fails its startup checks or cannot accept the next block is moved aside (kept on disk, never deleted) and writing continues into a fresh log. Without this option such conditions are fatal. |

Violations (`state-history-log-retain-blocks` below 1000, or mixing it with the partition options) throw
`plugin_exception` during initialization and stop startup.

## HTTP API

None. The plugin serves its own websocket listeners and registers nothing with `http_plugin`.

## Diagnostics

The plugin logs through a dedicated `state_history` logger, configurable like any other category in
`logging.json`. It implements `handle_sighup`, so `SIGHUP` re-binds the logger (it does not re-read plugin
options). A handful of initialization lines go to the default logger instead.

| Line | Logger / level | When |
|---|---|---|
| `Setting disable-replay-opts=true required by state_history_plugin chain-state-history=true option` | default, info | Initialization, whenever delta recording forces the setting. |
| `state-history-force-write is set (emergency recovery): ...` | default, warn | Initialization, whenever the force-write option is on. |
| `Deleting state history` | `state_history`, info | `--delete-state-history` was given. |
| `Storing initial state on startup, this can take a considerable amount of time` / `Done storing initial state on startup` | `state_history`, info | Startup with a non-empty chain and an empty chain-state log. |
| `Placing initial state in block <n>` | `state_history`, info | The first entry written into an empty chain-state log. |
| `First available block for SHiP <n>` | `state_history`, info | Startup, after the logs are opened. |
| `incoming state history connection from <endpoint>` | `state_history`, info | A client connected. Unix-socket clients show as `UNIX socket`. |
| `state history connection from <endpoint> failed: <reason>` | `state_history`, info | The session ended on an error; logged at most once per session. |
| `Exception in SHiP thread pool, exiting: <detail>` | `state_history`, error | Fatal on the `ship` thread; the application quits. |
| `Failed to write block <n> to <log>.log (...); state-history-force-write is set: moving the head log aside ...` | default, error | Force-write recovery took over for a failed write. |
| `Moving <log>.log aside to <log>-corrupt-<n>.log` | default, warn | A bundle was set aside by force-write recovery. |
| `retained log file <log>.log has block range A-B but ... which results in a hole` | default, error | A gap was detected across retained logs at startup; fatal unless force-write is set. |

A node whose history is not being served usually shows up as either no `incoming state history connection` line
at all (the client cannot reach the listener) or repeated `state history connection from ... failed` lines.

## Tests

The plugin directory has no `test/` subdirectory, so there is no `test_state_history_plugin` target. Coverage
comes from two places.

The log-file machinery is compiled into the `plugin_test` binary (suites `ship_file_tests` and
`ship_log_utils_tests`):

```bash
ninja -C build/debug plugin_test
./build/debug/tests/plugin_test -t ship_file_tests
./build/debug/tests/plugin_test -t ship_log_utils_tests
```

End-to-end protocol behavior is covered by the Python integration tests in `tests/`, which drive real nodes with
the `ship_client` and `ship_streamer` helper executables. `ship_test`, `ship_test_unix`, `ship_restart_test`,
`ship_kv_delta_test`, `ship_reqs_across_svnn_test`, `ship_log_util_test`, and `ship_kill_client_test` carry the
`nonparallelizable_tests` label; `ship_streamer_test` and `ship_streamer_if_fetch_finality_data_test` carry
`long_running_tests`:

```bash
ninja -C build/debug ship_client ship_streamer
ctest --test-dir build/debug -R '^ship_' -L nonparallelizable_tests
```

## Related plugins

- [`chain_plugin`](../chain_plugin) -- the source of all three signals and the controller state that is
  recorded; also owns `--disable-replay-opts`, which this plugin forces on for delta recording.
- [`resource_monitor_plugin`](../resource_monitor_plugin) -- monitors free space on the state-history directory
  when it is loaded; state-history logs are usually the largest thing a node writes.
- [`trace_api_plugin`](../trace_api_plugin) -- a separate, HTTP-served trace store with its own retention; SHiP
  is the websocket streaming path.
- [`snapshot_api_plugin`](../snapshot_api_plugin) -- serves state snapshots for bootstrap, the usual companion to
  a history node.

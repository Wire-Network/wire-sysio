# resource_monitor_plugin

`resource_monitor_plugin` watches the free space of every file system that holds a directory `nodeop` writes to,
and gracefully shuts the node down before that space runs out. It exists because running a file system to zero is
not a clean failure: a full `data/blocks` volume leaves the block log missing irreversible blocks, and a full
`data/state` volume kills `nodeop` with `SIGBUS` when the state file cannot map new pages. The plugin is **always
active** -- `nodeop` passes it as one of the four template plugins to `application::init<>`, so it is initialized
whether or not any `plugin =` line names it -- and it has no dependencies of its own
(`APPBASE_PLUGIN_REQUIRES()` is empty).

## How it works

Directories are not discovered; they are *registered* by the plugins that own them. Each caller looks the monitor
up with `app().find_plugin<resource_monitor_plugin>()` during its own `plugin_initialize`, so registration is
optional and silently skipped if the monitor is absent:

| Registering plugin | Directory |
|---|---|
| `chain_plugin` | `blocks_dir` and `state_dir` |
| `producer_plugin` | the snapshots directory |
| `trace_api_plugin` | the trace directory |
| `state_history_plugin` | the state-history directory |

At `plugin_startup` the handler walks the registered list and, for each path, adds the path itself **plus each of
its immediate subdirectories** -- a directory such as `data` can have subdirectories mounted on different file
systems. File systems are de-duplicated by device id (`stat`'s `st_dev`), so two registered paths on one volume
are monitored once. A path that cannot be `stat`ed, or whose space cannot be read at registration time, fails
startup with `plugin_config_exception`.

Thresholds are converted to a **minimum-available-bytes** value per file system at registration time, so the
periodic check only has to call `space()` and compare:

- **Percentage mode** (default): `shutdown_available = (100 - threshold) * capacity / 100`, with the warning band
  at `threshold - 5` percentage points.
- **Absolute mode** (`--resource-monitor-space-absolute-gb`): `shutdown_available` is exactly the configured
  value, and the warning value is `v + 1` GiB when `v < 10`, otherwise `v + v/10` GiB. Absolute mode overrides the
  percentage threshold and applies the same byte figure to every monitored file system regardless of its capacity.

One thread (a `resmon` named thread pool of size 1) runs `space_monitor_loop` on a `boost::asio::steady_timer`
rearmed every `--resource-monitor-interval-seconds`. Each pass:

1. Reads `space()` for every monitored file system. A read error is logged as a warning and that file system is
   skipped for this pass -- the monitor never takes the node down because it could not measure.
2. If available bytes are below `shutdown_available` on any file system, logs an error line naming the path,
   threshold, available, capacity, and shutdown-available figures, and -- unless
   `--resource-monitor-not-shutdown-on-threshold-exceeded` is set -- calls `appbase::app().quit()` to begin a
   graceful shutdown. The loop does not rearm the timer in that case.
3. Otherwise, if available bytes are below the warning value, logs a warning (and a second line naming the
   shutdown threshold when shutdown is enabled).
4. Advances the warning-interval counter, which gates warning output to one pass in every
   `--resource-monitor-warning-interval` passes. Exceeded-threshold errors are not suppressed by the counter when
   shutdown is enabled.

An unhandled exception on the monitor thread logs `Exception in resource monitor plugin thread pool, exiting` and
quits the application.

## Enabling / configuration

No `plugin =` line is required; the plugin is always initialized. Every option is registered into the
`config.ini` options description, so all of them are accepted in `config.ini` and on the command line.

`config.ini`:

```ini
# Percentage mode (the default shape) -- warn at 80% used, shut down at 85%.
resource-monitor-interval-seconds = 2
resource-monitor-space-threshold = 85
resource-monitor-warning-interval = 30
```

```ini
# Absolute mode -- shut down with less than 20 GiB free on any monitored file
# system (warning at 22 GiB). Overrides resource-monitor-space-threshold.
resource-monitor-space-absolute-gb = 20
```

Command line:

```bash
nodeop --resource-monitor-interval-seconds 2 \
       --resource-monitor-space-threshold 85 \
       --resource-monitor-warning-interval 30
```

To keep the node running and only log (for example on a host where an external agent handles capacity), add the
flag:

```bash
nodeop --resource-monitor-space-threshold 85 --resource-monitor-not-shutdown-on-threshold-exceeded
```

## Options

| Option | Default | Meaning |
|---|---|---|
| `resource-monitor-interval-seconds` | `2` | Time in seconds between two consecutive checks of resource usage. Must be between 1 and 300. |
| `resource-monitor-space-threshold` | `90` | Threshold as a percentage of used space vs total space. Above (threshold - 5%) a warning is generated; above the threshold a graceful shutdown is initiated unless `resource-monitor-not-shutdown-on-threshold-exceeded` is set. Must be between 6 and 99. |
| `resource-monitor-space-absolute-gb` | unset | Absolute threshold in gibibytes of *remaining* space, applied to each monitored directory. If remaining space is less than the value for any monitored directory the threshold is considered exceeded. Overrides `resource-monitor-space-threshold`. Must be greater than 0 GiB and less than the maximum 64-bit value. |
| `resource-monitor-not-shutdown-on-threshold-exceeded` | not set (flag) | Indicates `nodeop` will not shut down when the threshold is exceeded. |
| `resource-monitor-warning-interval` | `30` | Number of resource-monitor intervals between two consecutive warnings while the threshold is hit. Must be between 1 and 450. |

Every range check is enforced in `plugin_initialize` and a violation throws `plugin_config_exception` naming the
option, so a bad value fails startup rather than being clamped. With the default 2-second interval, a warning
interval of 30 means at most one warning per minute and the 450 maximum is 15 minutes.

## HTTP API

None. The plugin registers no handlers and does not depend on `http_plugin`; it reports exclusively through the
log and through shutdown.

## Diagnostics

The plugin uses the default logger (`ilog`/`wlog`/`elog`/`dlog`); it declares no logger category of its own and
does not implement `handle_sighup`. Its worker thread is named `resmon`.

At initialization, one line per resolved setting:

```
Monitoring interval set to 2
Space usage threshold set to 85%                 # or: Space usage absolute threshold set to 20 GiB, warning set to 22
Shutdown flag when threshold exceeded set to true
Warning interval set to 30
```

At startup, two lines per file system as it is registered:

```
/var/lib/wire/data/blocks's file system to be monitored
/var/lib/wire/data/blocks's file system monitored. shutdown_available: 96 GiB, capacity: 640 GiB, threshold: 85%
```

While running:

| Line | Level | Meaning |
|---|---|---|
| `Space usage warning: <path>'s file system approaching threshold. available: N GiB, warning_available: M GiB` | warn | Inside the warning band; rate-limited by `resource-monitor-warning-interval`. |
| `nodeop will shutdown when space usage exceeds threshold <t>` | warn | Follows the line above when shutdown is enabled. |
| `Space usage warning: <path>'s file system exceeded threshold <t>, available: N GiB, Capacity: C GiB, shutdown_available: M GiB` | error | The threshold has been crossed. |
| `Gracefully shutting down, exceeded file system configured threshold.` | error | Immediately precedes `app().quit()`. |
| `Unable to get space info for <path>: [code: N] <message>. Ignore this failure.` | warn | The `space()` call failed; this file system is skipped for this pass only. |
| `Exit due to error: N, message: <message>` | warn | The monitor timer failed; the loop stops. |
| `Exception in resource monitor plugin thread pool, exiting: <detail>` | error | Fatal on the monitor thread; the application quits. |

`threshold` renders as `<n>%` in percentage mode and `<n> GiB` in absolute mode, so the log always states which
mode is in force.

## Tests

The directory carries its own `test/CMakeLists.txt`, which builds `test_resmon_plugin` and registers it as an
ordinary ctest (no non-parallelizable or long-running label):

```bash
ninja -C build/debug test_resmon_plugin
./build/debug/plugins/resource_monitor_plugin/test/test_resmon_plugin
```

Three Boost.Test suites, all driven through a mock space provider so no real disk is filled:
`resmon_plugin_tests` (plugin lifecycle and option handling), `space_handler_tests` (`add_file_system`,
including the device-id de-duplication), and `threshold_tests` (percentage and absolute threshold evaluation,
warning band, and warning-interval throttling).

## Related plugins

- [`chain_plugin`](../chain_plugin) -- registers `blocks_dir` and `state_dir`, the two directories whose
  exhaustion this plugin was written to prevent.
- [`producer_plugin`](../producer_plugin) -- registers the snapshots directory.
- [`trace_api_plugin`](../trace_api_plugin) -- registers the trace directory.
- [`state_history_plugin`](../state_history_plugin) -- registers the state-history directory.
- [`db_size_api_plugin`](../db_size_api_plugin) -- reports chain-state *segment* usage, a different exhaustion
  mode from the file-system space this plugin watches.

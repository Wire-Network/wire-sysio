# cron_plugin

`cron_plugin` provides `nodeop` with an in-process cron scheduler: other plugins register functions against
cron-style schedules and the plugin fires them on a worker pool. It is infrastructure, not an operator-facing
feature -- it exposes no HTTP endpoint and does nothing on its own. Operators rarely name it: both
`batch_operator_plugin` and `underwriter_plugin` declare it in `APPBASE_PLUGIN_REQUIRES`, so enabling either one
loads it automatically. It has no dependencies of its own (`APPBASE_PLUGIN_REQUIRES()` is empty).

## How it works

`plugin_initialize` constructs one `sysio::services::cron_service` with `name = "cron_plugin"`,
`num_threads = --cron-threads`, and `autostart = false`; `plugin_startup` then starts it, and `plugin_shutdown`
stops it. A running service owns two things:

- **One scheduler thread**, named `cron-sched`. It holds no jobs open -- it sleeps on a condition variable until
  the earliest upcoming trigger across all jobs, or indefinitely when no job has one.
- **One `boost::asio::thread_pool`** of `--cron-threads` workers, which is where job functions actually run.

Each job carries a schedule, a `std::function<void()>`, and optional metadata. A schedule has six fields, and an
**empty field is a wildcard** matching every value in its range:

| Field | Range |
|---|---|
| `milliseconds` | 0..59999 (milliseconds within the minute) |
| `minutes` | 0..59 |
| `hours` | 0..23 |
| `day_of_month` | 1..31 |
| `month` | 1..12 |
| `day_of_week` | 0..7 (0 and 7 both mean Sunday) |

Each field is a set of three value kinds, mirroring crontab syntax: `exact_value` (a single value, crontab
`30`), `step_value` (every N, crontab `*/15`), and `range_value` (crontab `1-5`). Values outside the field's
range are discarded during expansion, and a `step_value` of 0 contributes nothing.

Rather than re-deriving the next fire time on every wake, each job keeps up to **8** pre-computed upcoming
trigger times (`cron_service::schedule_trigger_count`). The scheduler loop is:

```
wait until the earliest upcoming trigger (or until woken by add/cancel/stop)
  -> dispatch_ready_jobs()   post every job whose front trigger is now due onto the worker pool
  -> replenish_triggers()    top each job back up to 8 upcoming triggers
  -> repeat
```

Two behaviors matter to anyone reading the log:

- **`one_at_a_time` metadata.** When set, a trigger is *skipped* while a previous invocation of the same job is
  still running; the job does not queue up behind itself. When unset, invocations may overlap.
- **A throwing job never stops the scheduler.** Every invocation is wrapped in a catch-all that logs the failure
  against the job id and moves on.

Cancellation is safe to call concurrently with running callbacks: `cancel` marks the job cancelled (already
posted work returns immediately), and `cancel_all` does that for every job. Stopping the service stops the
scheduler thread, cancels all jobs, then stops and joins the worker pool. The destructor stops the service, so
a service is never left running behind a dropped owner.

### API surface for other plugins

`cron_plugin` forwards a small API to its service: `add_job(schedule, fn, metadata)` returning a job id,
`update_job_metadata(id, metadata)`, `list_jobs(queries)` (queries match a job id or a metadata tag),
`cancel_job(id)`, `cancel_all_jobs()`, and `cron_service()` for direct access.

A plugin that needs its own isolated pool does not have to share this one. `cron_service::create(options)`
builds a standalone service, and `cron_service_manager` manages a named set of them
(`create`, `get_or_create`, `get`, `contains`, `stop`, `stop_all`, `all`, `all_names`, `size`, `empty`).
`batch_operator_plugin` uses this: it creates a private `cron_service` sized from the number of outposts it
discovered, so its OPP polling jobs do not contend with anything else on the shared pool.

## Enabling / configuration

No `plugin =` line is normally needed -- `batch_operator_plugin` and `underwriter_plugin` both require it.
Naming it explicitly is harmless.

`config.ini`:

```ini
plugin = sysio::batch_operator_plugin   # pulls in cron_plugin automatically

# Worker threads for the shared cron pool. Raise this only if jobs on the
# shared service are long enough to queue behind each other.
cron-threads = 2
```

Command line:

```bash
nodeop --plugin sysio::batch_operator_plugin --cron-threads 2
```

Note that `--cron-threads` sizes only this plugin's shared service. A private service created by another plugin
(as `batch_operator_plugin` does) sizes itself and is unaffected by this option.

## Options

| Option | Default | Meaning |
|---|---|---|
| `cron-threads` | `1` | Number of worker threads to use for cron job processing. |

The option is registered into the `config.ini` options description, so it is accepted both in `config.ini` and
on the command line. It is read once in `plugin_initialize`; changing it requires a restart.

## HTTP API

None. The plugin registers no handlers and does not depend on `http_plugin`.

## Diagnostics

The plugin uses the default logger and declares no logger category of its own. Its scheduler thread is named
`cron-sched`, which is what shows up in per-thread log lines and in a debugger.

| Line | Level | When |
|---|---|---|
| `Starting cron plugin` | info | `plugin_startup`. |
| `Shutdown cron plugin` | info | `plugin_shutdown`. |
| `cron_service starting` | info | The service object is constructed. |
| `cron_service stop` | info | `stop()` was called, before it checks whether the service was running. |
| `cron_service already started` / `cron_service already stopped` | warn | A redundant `start()` / `stop()`; the call is a no-op. |
| `step value is <= 10ms, this may cause excessive CPU usage` | warn | A schedule field expanded a `step_value` of 10 or less. Emitted during field expansion, so it can repeat as triggers are recomputed. |
| `JOB_ID(<id>) FAILED: <detail>` | error | A job function threw. One variant per caught type (`fc::exception`, `boost::exception`, `std::runtime_error`, `std::exception`). |
| `JOB_ID(<id>) unknown exception` | error | A job function threw something none of the above matched. |

Because the same `cron_service` code backs private services created by other plugins, these lines can also come
from those services; the job id in a `JOB_ID(...)` line is unique only within one service.

## Tests

The test directory has no `CMakeLists.txt`, so `plugin_target` builds the default target from its sources:

```bash
ninja -C build/debug test_cron_plugin
./build/debug/plugins/cron_plugin/test_cron_plugin
```

The `cron_service` suite covers the service end to end without a running node: construction with options and
basic `add`, millisecond-field triggers firing within the same second, `cancel_all` across multiple jobs, the
destructor stopping the service cleanly, independent jobs progressing in parallel, `list` filtering by id and
tag, `expand_field` for exact/step/range/wildcard, step and range schedules, the `next_fire_time` algorithm,
trigger pre-computation, the scheduler waking when a new job is added, and day-of-week schedules.

## Related plugins

- [`batch_operator_plugin`](../batch_operator_plugin) -- requires this plugin, and additionally creates its own
  private `cron_service` sized from the discovered outpost count for OPP polling jobs.
- [`underwriter_plugin`](../underwriter_plugin) -- requires this plugin and schedules its scan job on the shared
  service.
- [`resource_monitor_plugin`](../resource_monitor_plugin) -- a plugin with its own dedicated timer thread rather
  than a cron job; the two are independent.

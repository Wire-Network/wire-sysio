# producer_plugin

`producer_plugin` drives block production and block application. It schedules the production loop for the
producers this node is configured for, signs blocks and finalizer votes through
`signature_provider_manager_plugin`, applies blocks received from the network, holds the unapplied-transaction
queue and subjective billing, runs the read-only transaction window, and owns snapshot creation and
scheduling. `nodeop` initializes it on every run alongside `resource_monitor_plugin`, `chain_plugin`, and
`net_plugin`, so there is no `plugin =` line to add — a node with no `producer-name` still needs it to apply
incoming blocks and to take snapshots. It requires `chain_plugin` and `signature_provider_manager_plugin`
through `APPBASE_PLUGIN_REQUIRES`, and registers no HTTP endpoints of its own.

## How it works

```
plugin_initialize
  |  load producer-name accounts, and the signing providers for their keys
  |  size and validate the read-only window (read-only-threads / windows)
  |  register the incoming::methods::transaction_async provider
  |  apply greylist accounts and limit, subjective-billing exclusions
  |  init the implicit production-pause vote tracker
  |  point the snapshot scheduler at snapshots-dir
  |  snapshot-provider-account: register the snapshot-finalized vote callback
  |
plugin_startup
  |  connect controller signals: accepted_block, accepted_block_header,
  |                              irreversible_block, block_start
  |  producers configured -> also aggregated_vote and voted_block
  |  provider mode -> auto-schedule snapshots every 25000 blocks
  |  read-only-threads > 0 -> start the read-only pool, begin the write window
  |  start the 1-thread production timer
  |  post schedule_production_loop (runs after net_plugin has started)
  |
plugin_shutdown
     stop the timer thread, stop the read-only pool, clear the unapplied queue
```

### Threads

- **Production timer** — one thread, always started. It fires the production loop at block boundaries.
- **Read-only execution pool** — `read-only-threads` threads, started only when the size is above 0. Each
  thread initializes its own chain thread-local data and timer.

### Windows

When `read-only-threads` is above 0 the node alternates two windows: a **write window** of
`read-only-write-window-time-us`, during which blocks and normal transactions are applied, and a **read
window** of `read-only-read-window-time-us`, during which the read-only pool executes read-only transactions
in parallel. The read window must be longer than the 10,000 µs minimum. The maximum read-only transaction time
is clamped at startup to fit inside the effective read window minus that minimum, and both the clamp and the
resulting value are logged.

### Production, pausing, and the vote timeout

Block production runs only for accounts given with `producer-name`, and the node must be in `validation-mode =
full`, must accept transactions from at least one of the API or P2P paths, and must not be in `read-mode =
irreversible` — each is asserted at startup with a message naming the conflict.

Beyond the explicit `pause-on-startup` switch and the `/v1/producer/pause` and `/v1/producer/pause_at_block`
endpoints, production pauses implicitly. `production-pause-vote-timeout-ms` sets how long the node waits for
votes — from its own `producer-name` finalizers or from others — before pausing production; 0 disables the
check. `max-irreversible-block-age` bounds how old the irreversible block may be for a chain this node will
produce on, and `max-reversible-blocks` pauses production once reversible blocks run too far beyond
irreversible.

### Snapshots

`producer_plugin` owns the snapshot scheduler and the `snapshots-dir` it writes into, and it exposes snapshot
creation, scheduling, and a finalization callback that `snapshot_api_plugin` uses to keep its serving catalog
current.

Setting `snapshot-provider-account` turns on **provider mode**, which cannot be combined with
`producer-name`. In provider mode the plugin auto-schedules snapshots on the shared 25,000-block cadence
(25000, 50000, 75000, ...) so every provider snapshots at identical heights, and submits a `votesnaphash`
attestation from the configured account each time a scheduled snapshot finalizes. The auto-schedule is
persisted to `snapshot-schedule.json` in the snapshots directory and reused across restarts rather than
re-created.

### Transactions

The plugin registers the `incoming::methods::transaction_async` provider, so every transaction arriving from
the API or from P2P lands here. Results are published on the `transaction_ack` channel, which `net_plugin`
subscribes to in order to decide whether to relay.

Subjective billing bounds how much CPU a failing account can consume:
`subjective-account-max-failures` per `subjective-account-max-failures-window-size` blocks,
`subjective-account-cpu-allowed-us` of extra CPU above an account's own quota, decaying over
`subjective-account-decay-time-minutes`. Three switches turn the whole mechanism off for a class of traffic —
contract payers (`disable-subjective-payer-billing`, on by default), P2P transactions, and API transactions —
and `disable-subjective-account-billing` excludes named accounts.

## Enabling / configuration

`producer_plugin` is always initialized, so `config.ini` carries only its settings. A producing node:

```ini
producer-name = <your-account>
signature-provider = <your-public-key>=KEY:<your-private-key>

# Production timing.
produce-block-offset-ms = 450
max-transaction-time = 499
max-irreversible-block-age = -1
max-reversible-blocks = 3600
production-pause-vote-timeout-ms = 6000

# Subjective billing.
subjective-account-max-failures = 3
subjective-account-cpu-allowed-us = 300000
```

`signature-provider` is registered by `signature_provider_manager_plugin`, not by this plugin.

A non-producing node that serves read-only transactions and takes snapshots:

```ini
read-only-threads = 3
read-only-write-window-time-us = 200000
read-only-read-window-time-us = 60000
snapshots-dir = snapshots
```

A snapshot provider — note that provider mode and `producer-name` are mutually exclusive:

```ini
snapshot-provider-account = <your-account>
snapshots-dir = snapshots
```

The equivalent command lines:

```bash
nodeop --producer-name <your-account> \
       --produce-block-offset-ms 450 \
       --max-transaction-time 499 \
       --production-pause-vote-timeout-ms 6000

nodeop --snapshot-provider-account <your-account> --snapshots-dir snapshots

nodeop --pause-on-startup   # -x; start with production paused
```

## Options

Every `producer_plugin` option is registered as a config-file option, so each is equally valid in
`config.ini` and as a `nodeop --<name>` argument. Three carry short forms: `-p` for `producer-name`, `-e`
for `enable-stale-production`, and `-x` for `pause-on-startup`.

### Production

| Option | Default | Meaning |
|---|---|---|
| `producer-name`, `-p` | unset | Account whose blocks this node produces; may be repeated. |
| `enable-stale-production`, `-e` | `false` | Produce blocks even when the chain is stale. |
| `pause-on-startup`, `-x` | `false` | Start the node with production paused. |
| `produce-block-offset-ms` | `450` | Minimum time reserved at the end of a production round for blocks to propagate to the next producer. |
| `max-transaction-time` | `499` | Restrict allowed transaction execution time, in milliseconds, to a value potentially lower than the on-chain `max_transaction_cpu_usage`. |
| `max-irreversible-block-age` | `-1` | Maximum age in seconds of the irreversible block for a chain this node will produce on. A negative value means unlimited. |
| `max-reversible-blocks` | `3600` | Maximum reversible blocks beyond irreversible before production is paused. 0 disables the check. |
| `production-pause-vote-timeout-ms` | `6000` | Vote timeout. Production pauses when no vote arrives from this node's `producer-name` finalizers or from other finalizers within this window. 0 disables. |
| `max-block-cpu-usage-threshold-us` | `5000` | When within this many microseconds of `max-block-cpu-usage`, the block counts as full and can be produced immediately. |
| `max-block-net-usage-threshold-bytes` | `1024` | When within this many bytes of `max-block-net-usage`, the block counts as full and can be produced immediately. |

### Transactions and subjective billing

| Option | Default | Meaning |
|---|---|---|
| `incoming-transaction-queue-size-mb` | `1024` | Maximum size in MiB of the incoming transaction queue. Beyond it, transactions are subjectively dropped with resource exhaustion. |
| `subjective-cpu-leeway-us` | `31000` | Microseconds allowed for a transaction that starts with insufficient CPU quota to complete and cover its CPU usage. |
| `subjective-account-max-failures` | `3` | Maximum failures allowed for one account per window. |
| `subjective-account-max-failures-window-size` | `1` | Window size, in blocks, for `subjective-account-max-failures`. |
| `subjective-account-decay-time-minutes` | `1440` | Time over which an account returns to full subjective CPU. |
| `subjective-account-cpu-allowed-us` | `300000` | Maximum CPU, above the account's own CPU, that one authorizing account may use within `subjective-account-decay-time-minutes`. |
| `disable-subjective-account-billing` | unset | Account excluded from subjective CPU billing; may be repeated. |
| `disable-subjective-payer-billing` | `true` | Disable subjective CPU billing for all contract payer accounts. On by default: under contract-pays the payer is the called contract, so billing it for its callers' failures charges the party provisioned to absorb traffic rather than the account causing it. |
| `disable-subjective-p2p-billing` | `false` | Disable subjective CPU billing for P2P transactions. |
| `disable-subjective-api-billing` | `false` | Disable subjective CPU billing for API transactions. |
| `greylist-account` | unset | Account that may not reach extended CPU/NET virtual resources; may be repeated. |
| `greylist-limit` | `1000` | Limit, between 1 and 1000, on the multiple by which CPU/NET virtual resources may extend during low usage. Enforced subjectively only; 1000 enforces no limit. |

### Read-only execution

| Option | Default | Meaning |
|---|---|---|
| `read-only-threads` | 0 on a node with `producer-name`, otherwise 3 | Worker threads in the read-only execution pool. Maximum 128. |
| `read-only-write-window-time-us` | `200000` | Microseconds the write window lasts. |
| `read-only-read-window-time-us` | `60000` | Microseconds the read window lasts. Must be greater than the 10,000 µs minimum. |

### Snapshots

| Option | Default | Meaning |
|---|---|---|
| `snapshots-dir` | `snapshots` | Location of the snapshots directory (absolute, or relative to the data dir). |
| `snapshot-provider-account` | empty | Account used to sign and submit `votesnaphash` transactions. Setting it enables snapshot provider mode. Cannot be used alongside `producer-name`. |

## HTTP API

`producer_plugin` registers no HTTP handlers. `producer_api_plugin` publishes its runtime controls as
`/v1/producer/*` under the `producer_ro`, `producer_rw`, and `snapshot` categories — including pause and
resume, `pause_at_block`, runtime option updates, greylist and whitelist/blacklist management, the integrity
hash, protocol feature activation scheduling, unapplied transaction inspection, and manual snapshot creation
and scheduling.

## Diagnostics

Eight named loggers, all re-read on `SIGHUP`. Configure them independently in `logging.json`; the transaction
tracing loggers are the volume-heavy ones.

| Logger | Carries |
|---|---|
| `producer_plugin` | The plugin's own lifecycle and production messages. |
| `transaction_success_tracing` | Successful transaction traces. |
| `transaction_failure_tracing` | Failed transaction traces. |
| `transaction_trace_success` | Full successful transaction traces. |
| `transaction_trace_failure` | Full failed transaction traces. |
| `transaction` | Transaction-level messages. |
| `transient_trx_success_tracing` | Successful traces for transactions marked transient. |
| `transient_trx_failure_tracing` | Failed traces for transactions marked transient. |

Lines an operator should recognize:

- `Launching block production for <n> producers at <time>.` — production is configured and starting. A fresh
  chain also prints the new-chain banner.
- `read-only-threads <n>, max read-only trx time to be enforced: <n> us` and `Read-only max transaction time
  <n>us set to fit in the effective read-only window <n>us.` — the read window sizing that was actually
  applied.
- `Snapshot provider mode: auto-scheduled snapshots every 25000 blocks (request id <n>)`, or
  `... reusing persisted auto-schedule ...` on a restart.
- `Producer paused.` on an explicit pause.
- `Exception in read-only thread pool, exiting` and `Exception in producer timer thread, exiting` — either
  quits the node.
- `Exception during snapshot execution: ...` — quits the node.
- Startup assertion failures name the conflicting option directly: a node cannot have `producer-name`
  configured with `validation-mode` other than `full`, with neither API nor P2P transactions accepted, or with
  `read-mode = irreversible`; finalizers cannot be configured in `read-mode = irreversible`; and
  `snapshot-provider-account cannot be used alongside producer-name`.
- Shutdown: `exit shutdown` at debug level.

## Tests

```bash
ninja -C build/debug test_producer_plugin
./build/debug/plugins/producer_plugin/test/test_producer_plugin
```

The suite covers the implicit production-pause vote tracker, full transaction handling, option parsing and
validation, block timing calculations, rejection of delayed transactions, and the mockable timers the
production loop is built on.

## Related plugins

- [`chain_plugin`](../chain_plugin/README.md) — required dependency; owns the controller this plugin produces
  blocks into and applies blocks against.
- `signature_provider_manager_plugin` — required dependency; supplies the block and finalizer signing
  providers selected by `signature-provider`.
- `producer_api_plugin` — publishes this plugin's runtime controls as `/v1/producer/*`.
- [`net_plugin`](../net_plugin/README.md) — subscribes to the `transaction_ack` channel this plugin publishes,
  and depends on this plugin for its producer-key checks.
- [`snapshot_api_plugin`](../snapshot_api_plugin/README.md) — serves the snapshots this plugin creates, via
  the snapshot-finalized callback.

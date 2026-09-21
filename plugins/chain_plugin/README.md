# chain_plugin

`chain_plugin` owns the blockchain itself. It constructs and configures the `controller` — block log, chain
state database, fork database, WASM runtime, protocol features, whitelists and blacklists — brings it up from
genesis, a snapshot, or existing state, and relays the controller's signals onto the appbase channels
declared in `chain_interface`. Those channels have few subscribers: `batch_operator_plugin` and
`underwriter_plugin` take `irreversible_block` as their sync gate, and nothing else subscribes to any of
them — `net_plugin` connects to the controller's signals directly instead. `nodeop` initializes it on every
run alongside `resource_monitor_plugin`, `net_plugin`, and `producer_plugin`, so there is no `plugin =` line
to add; it pulls in `signature_provider_manager_plugin` through `APPBASE_PLUGIN_REQUIRES`, which in turn
pulls in `http_client_plugin`. It registers no HTTP endpoints of its own — `chain_api_plugin` publishes the
`chain_apis::read_only` and `chain_apis::read_write` objects this plugin hands out.

## How it works

```
plugin_initialize
  |  register default signature providers: with no `wire` provider configured, a key is generated and
  |     saved to <config-dir>/default_signature_providers.json; the same happens for a `wire_bls` key
  |     when producer-name is set
  |  read every option into controller::config (dirs, limits, runtime, read/validation mode, lists)
  |  --snapshot-endpoint: download + hash-check the snapshot, to be loaded like --snapshot
  |  resolve the chain id and, with no state, the genesis to start from
  |  construct the controller and its chain id
  |  construct the optional side databases:
  |     trx_finality_status_processing   transaction-finality-status-max-storage-size-gb > 0
  |     trx_retry_db                     transaction-retry-max-storage-size-gb > 0
  |     tracked_votes                    always
  |     get_info_db                      refreshes per block only for an in-process consumer
  |  register method providers: get_block_by_id, get_head_block_id
  |  connect controller signals -> appbase channels
  |  add chain indices
  |
plugin_startup
  |  controller::startup( from snapshot | from genesis | from existing state )
  |  account_query_db constructed here (enable-account-queries)
  |
runtime
     controller signal            side-database work                 published channel (priority)
     accepted_block_header   ->                                   -> accepted_block_header (medium)
     accepted_block          ->  account_query_db::commit_block    -> accepted_block        (high)
                                 trx_retry_db::on_accepted_block
                                 trx_finality_status::signal_accepted_block
                                 tracked_votes::on_accepted_block
                                 get_info_db::on_accepted_block
     irreversible_block      ->  trx_retry_db::on_irreversible_block-> irreversible_block   (low)
                                 trx_finality_status::signal_irreversible_block
                                 get_info_db::on_irreversible_block
                                 snapshot attestation verification
     applied_transaction     ->  account_query_db::cache_transaction_trace -> applied_transaction (low)
                                 trx_retry_db::on_applied_transaction
                                 trx_finality_status::signal_applied_transaction
     block_start             ->  trx_retry_db::on_block_start
                                 trx_finality_status::signal_block_start
```

### Threads

`chain_plugin` starts no thread of its own; it sizes the controller's pools:

- `chain-threads` — the controller thread pool, used for signature recovery and other parallel chain work.
- `vote-threads` — the vote processor pool. On a node with `producer-name`, voting cannot be turned off: both
  an unset `vote-threads` and an explicit `vote-threads = 0` are replaced by 4, and the choice is logged.
  Without `producer-name`, an unset `vote-threads` leaves the pool at 0, and a node with no vote threads
  accepts no votes — `net_plugin` drops every vote a peer sends and propagates none. Give such a node an
  explicit `vote-threads` above 0 to have it carry votes.
- `sys-vm-oc-compile-threads` — tier-up compilation threads, on builds with the SYS VM OC runtime.

### Startup sources

The CLI options select where the node comes up from:

- **Existing state** — the default.
- **Genesis** — what a node with no chain state falls back to. The genesis state comes from `blocks.log` when
  it holds one, otherwise from `--genesis-json` (optionally retimed with `--genesis-timestamp`), otherwise
  from the built-in default genesis. The default genesis is built from this node's own keys, so on a node
  with `producer-name` it needs a `wire_bls` finalizer key and startup fails when none is configured.
- **A snapshot file** — `--snapshot <path>`. Operator-trusted; no attestation is verified.
- **A snapshot endpoint** — `--snapshot-endpoint <url>`, which downloads and hash-checks a snapshot from a
  serving node and then loads it. This path is not trusted the same way, and it carries hard constraints. The
  endpoint must serve a snapshot taken at a block number that is a multiple of 25000, because only those
  heights are attested on chain, and the snapshot must contain readable, enabled on-chain attestation state.
  After the node syncs past the snapshot's height, the loaded block id and root hash are checked against the
  on-chain attestation record on each finalized block: a record that does not match the loaded snapshot stops
  the node immediately, with no grace, while a record that has not appeared yet keeps verification pending.
  Verification only concludes once the node has caught up to the live tip: until then every finalized block
  simply retries, with no limit. Past that point a missing record is tolerated for a grace window of 12,500
  finalized blocks beyond the snapshot height before the node is stopped. The option requires an empty
  database; use `--delete-all-blocks` to clear existing data.

Both snapshot paths share the chain-state size setting as their download and state ceiling
(`--chain-state-db-size-mb`), and the endpoint path accepts per-caller HTTPS transport overrides
(`--snapshot-endpoint-additional-ca-file`, `--snapshot-endpoint-additional-ca-path`,
`--snapshot-endpoint-proxy`) that take precedence over the process-wide `--outbound-http-*` fallbacks
registered by `http_client_plugin`.

### The `get_info` cache

`get_info_db` refreshes its cached `/v1/chain/get_info` result on every irreversible block, and on every
accepted block outside `read-mode = irreversible` — but only when an in-process consumer is configured.
"Configured" means a `plugin` option value contains `sysio::chain_api_plugin` or
`sysio::status_monitor_plugin`. Without one, the cache is filled lazily on first use and not refreshed. The
flag is fixed when `chain_plugin` initializes, because appbase runs that before any dependent plugin.

### Features that add work only when enabled

| Feature | Enabled by | Effect |
|---|---|---|
| Account queries | `enable-account-queries = true` | Builds `account_query_db` at startup and lets `chain_api_plugin` register `/v1/chain/get_accounts_by_authorizers`. A failure to build it is logged and the feature is left off; the node still starts. |
| Transaction retry | `transaction-retry-max-storage-size-gb` above 0 | Re-sends a transaction to the network if it is not seen in a block. Only transactions submitted through `send_transaction2` with `retry_trx: true` are tracked, not every incoming transaction. Setting the option on a node with `producer-name` fails startup. |
| Transaction finality status | `transaction-finality-status-max-storage-size-gb` above 0 | Tracks where a transaction stands relative to head and LIB, and lets `chain_api_plugin` register `/v1/chain/get_transaction_status`. |
| Deep mind logging | `deep-mind = true` | Emits the deep-mind trace stream on unbuffered `stdout`. Requires both `api-accept-transactions = false` and `p2p-accept-transactions = false`. |

## Enabling / configuration

`chain_plugin` is always initialized, so `config.ini` carries only its settings:

```ini
# Storage layout (relative paths resolve against the data dir).
blocks-dir = blocks
state-dir = state
finalizers-dir = finalizers

# Sizing.
chain-state-db-size-mb = 65536
chain-state-db-guard-size-mb = 128
chain-threads = 4

# Execution and validation posture.
wasm-runtime = sys-vm-jit
read-mode = head
validation-mode = full
api-accept-transactions = true
abi-serializer-max-time-ms = 15

# Optional features.
enable-account-queries = true
transaction-retry-max-storage-size-gb = 1
transaction-finality-status-max-storage-size-gb = 1
```

Bootstrapping from a snapshot-serving node is command line only, because the startup options are CLI options:

```bash
nodeop --delete-all-blocks \
       --snapshot-endpoint http://<your-host>:9090 \
       --p2p-peer-address <your-host>:9876
```

Replaying from the block log, and stopping at a chosen height:

```bash
nodeop --replay-blockchain
nodeop --terminate-at-block 1000000 --truncate-at-block 1000000
```

## Options

`chain_plugin` registers two groups. The **config-file options** below are accepted both in `config.ini` and
as `nodeop --<name>` arguments. The **command-line options** are accepted only on the command line; most of
them change how the node starts or make it print something and exit.

### Storage locations

| Option | Default | Meaning |
|---|---|---|
| `blocks-dir` | `blocks` | Location of the blocks directory (absolute, or relative to the data dir). |
| `state-dir` | `state` | Location of the state directory (absolute, or relative to the data dir). |
| `finalizers-dir` | `finalizers` | Location of the finalizer safety data directory (absolute, or relative to the data dir). |
| `protocol-features-dir` | `protocol_features` | Location of the protocol features directory (absolute, or relative to the config dir). |
| `blocks-retained-dir` | unset | Location of the retained blocks directory (absolute, or relative to `blocks-dir`). Empty means the blocks dir itself. |
| `blocks-archive-dir` | `archive`, once any split option is set | Location of the blocks archive directory (absolute, or relative to `blocks-dir`). Leaving it unset does not mean deletion: as soon as any of `blocks-retained-dir`, `blocks-log-stride`, or `max-retained-block-files` is set, the archive dir defaults to `archive` and files past the retained limit are moved there. Only an explicitly empty value makes them be deleted instead. Files here are entirely under the operator's control and are not accessed by `nodeop` again. |

### Block log retention

| Option | Default | Meaning |
|---|---|---|
| `blocks-log-stride` | unset | Split the block log when the head block number is a multiple of the stride. The current log and index are renamed `<blocks-retained-dir>/blocks-<start>-<end>.log/index` and a new current pair is created. |
| `max-retained-block-files` | unset | Maximum number of block files to retain so their blocks remain queryable. Past the limit the oldest file is moved to the archive dir, which defaults to `archive`; it is deleted only when `blocks-archive-dir` is set to an explicitly empty value. Retained files must not be manipulated by hand. |
| `block-log-retain-blocks` | unset | When greater than 0, periodically prune the block log to the configured number of most recent blocks; a value above 0 requires a file system that supports hole punching, and startup fails when it does not. When 0, no blocks are written to the block log and the file is removed after startup. Cannot be combined with `blocks-retained-dir`, `blocks-archive-dir`, `blocks-log-stride`, or `max-retained-block-files` — startup fails when it is. |

### Sizing and threads

| Option | Default | Meaning |
|---|---|---|
| `chain-state-db-size-mb` | `1024` | Maximum size in MiB of the chain state database. Also the maximum accepted snapshot download size. |
| `chain-state-db-guard-size-mb` | `128` | Shut the node down safely when free space in the chain state database drops below this size in MiB. |
| `chain-threads` | `4` | Number of worker threads in the controller thread pool. Must be greater than 0. |
| `vote-threads` | unset | Number of worker threads in the vote processor pool. On a node with `producer-name` both an unset value and an explicit 0 are replaced by 4, so voting cannot be disabled there. Elsewhere, unset leaves the pool at 0 and the node neither accepts nor propagates votes on P2P. |
| `abi-serializer-max-time-ms` | `15` | Maximum ABI serialization time allowed, in milliseconds. |
| `signature-cpu-billable-pct` | `50` | Percentage of actual signature-recovery CPU to bill, as a whole number. |
| `maximum-variable-signature-length` | `16384` | Subjective limit, in bytes, on the variable components of a variable-length signature. |

### Execution runtime

| Option | Default | Meaning |
|---|---|---|
| `wasm-runtime` | `sys-vm-jit` on builds with the JIT runtime, otherwise `sys-vm` | Override the default WASM runtime. `sys-vm-jit` compiles WebAssembly to native x86 ahead of execution; `sys-vm` is an interpreter. Which values exist depends on the runtimes the build enabled. |
| `profile-account` | unset | Name of an account whose code will be profiled; may be repeated. |
| `contracts-console` | `false` | Print contract output to the console. |
| `native-contract` | unset | `account:/path/to/contract_native.so` — route a contract's execution through a native shared object for debugger support; may be repeated. State data is copied into `.native-debug/` directories to protect the originals. Registered only on builds with the native module runtime. |
| `sys-vm-oc-cache-size-mb` | `1024` | Maximum size in MiB of the SYS VM OC code cache. Registered only on builds with the OC runtime. |
| `sys-vm-oc-compile-threads` | `1` | Threads used for SYS VM OC tier-up. Must be non-zero. Registered only on builds with the OC runtime. |
| `sys-vm-oc-enable` | `auto` | SYS VM OC tier-up: `auto` always uses OC for `sysio.*` accounts and for the `sys-vm-oc-whitelist` suffixes; every other account gets OC only when a non-producing node applies a block, or inside a read-only transaction. Speculative execution, `compute_transaction`, and block building stay on the baseline runtime for those accounts. `all` enables it for all contract execution; `none` disables it. Registered only on builds with the OC runtime. |
| `sys-vm-oc-whitelist` | `wire` | Account suffixes tiered up under `sys-vm-oc-enable = auto`; may be repeated. Registered only on builds with the OC runtime. |

### Read, validation, and transaction acceptance

| Option | Default | Meaning |
|---|---|---|
| `read-mode` | `head` | `head`: state contains changes up to the head block, and received transactions are relayed if valid. `irreversible`: state contains changes up to the last irreversible block, and received transactions are executed speculatively and relayed if valid. `speculative`: state contains blockchain changes up to head plus some transactions not yet in a block. |
| `validation-mode` | `full` | `full` fully validates every incoming block. `light` fully validates incoming block headers and trusts the transactions inside those validated blocks. |
| `api-accept-transactions` | `true` | Allow transactions arriving over the API to be evaluated and relayed if valid. |
| `trusted-producer` | unset | A producer whose signed block headers are fully validated while the transactions in those blocks are trusted; may be repeated. |
| `disable-ram-billing-notify-checks` | `false` | Disable the check that subjectively fails a transaction when a contract bills RAM to another account inside a notification handler. |
| `disable-replay-opts` | `false` | Disable optimizations that specifically target replay. |
| `database-map-mode` | `mapped` | `mapped` memory-maps the database as a file; `mapped_private` maps it privately with no disk writeback until exit; `heap` preloads it into swappable memory; `locked` preloads and locks it in memory. `heap` and `locked` use huge pages when available and are not offered on Windows. |
| `checkpoint` | unset | `[BLOCK_NUM,BLOCK_ID]` pair enforced as a checkpoint; may be repeated. |

### Whitelists and blacklists

| Option | Default | Meaning |
|---|---|---|
| `actor-whitelist` | unset | Account added to the actor whitelist; may be repeated. |
| `actor-blacklist` | unset | Account added to the actor blacklist; may be repeated. |
| `contract-whitelist` | unset | Contract account added to the contract whitelist; may be repeated. |
| `contract-blacklist` | unset | Contract account added to the contract blacklist; may be repeated. |
| `action-blacklist` | unset | Action, in `code::action` form, added to the action blacklist; may be repeated. |
| `key-blacklist` | unset | Public key that must not appear in authorities; may be repeated. |
| `sender-bypass-whiteblacklist` | unset | Deferred transactions sent by these accounts skip every subjective whitelist and blacklist check; may be repeated. |

### Optional features

| Option | Default | Meaning |
|---|---|---|
| `enable-account-queries` | `false` | Enable queries that find accounts by various metadata, backing `/v1/chain/get_accounts_by_authorizers`. |
| `transaction-retry-max-storage-size-gb` | unset | Maximum size in GiB allocated to transaction retry. Any value above 0 enables the feature, and only transactions sent through `send_transaction2` with `retry_trx: true` are tracked. Setting the option at all on a node with `producer-name` fails startup. |
| `transaction-retry-interval-sec` | `20` | How often, in seconds, to resend an incoming transaction that has not been seen in a block. Must be at least twice `p2p-dedup-cache-expire-time-sec`. |
| `transaction-retry-max-expiration-sec` | `120` | Maximum transaction expiration eligible for retry. Must be larger than `transaction-retry-interval-sec`. |
| `transaction-finality-status-max-storage-size-gb` | unset | Maximum size in GiB allocated to transaction finality status. Any value above 0 enables the feature. |
| `transaction-finality-status-success-duration-sec` | `180` | How long, in seconds, a successful transaction's finality status stays available after it is first identified. |
| `transaction-finality-status-failure-duration-sec` | `180` | How long, in seconds, a failed transaction's finality status stays available after it is first identified. |
| `deep-mind` | `false` | Print deeper information about chain operations. Requires `api-accept-transactions = false` and `p2p-accept-transactions = false`. |
| `integrity-hash-on-start` | `false` | Log the state integrity hash on startup. |
| `integrity-hash-on-stop` | `false` | Log the state integrity hash on shutdown. |

### Snapshot endpoint transport

These three are config-file options registered alongside the CLI `--snapshot-endpoint`. Each overrides the
process-wide `outbound-http-*` fallback that `http_client_plugin` registers.

| Option | Default | Meaning |
|---|---|---|
| `snapshot-endpoint-additional-ca-file` | unset | PEM CA bundle added to the system trust store for snapshot-endpoint HTTPS requests. |
| `snapshot-endpoint-additional-ca-path` | unset | Hashed CA directory added to the system trust store for snapshot-endpoint HTTPS requests. |
| `snapshot-endpoint-proxy` | unset | Explicit proxy URL for snapshot-endpoint HTTP requests. |

### Command-line only

| Option | Default | Meaning |
|---|---|---|
| `genesis-json` | unset | File to read the genesis state from. |
| `genesis-timestamp` | unset | Override the initial timestamp in the genesis state file. |
| `print-genesis-json` | `false` | Extract the genesis state from `blocks.log` as JSON, print it, and exit. |
| `extract-genesis-json` | unset | Extract the genesis state from `blocks.log` as JSON into the named file, and exit. |
| `print-build-info` | `false` | Print build environment information as JSON and exit. |
| `extract-build-info` | unset | Write build environment information as JSON into the named file and exit. |
| `force-all-checks` | `false` | Skip no validation check while replaying blocks. Use when replaying blocks from an untrusted source. |
| `replay-blockchain` | `false` | Clear the chain state database and replay all blocks. |
| `hard-replay-blockchain` | `false` | Clear the chain state database, recover as many blocks as possible from the block log, then replay them. |
| `delete-all-blocks` | `false` | Clear the chain state database and the block log. |
| `truncate-at-block` | `0` | Stop hard replay or block-log recovery at this block number when non-zero. Combined with `terminate-at-block`, prunes received blocks from the fork database on exit. |
| `terminate-at-block` | `0` | Stop the node after reaching this block number when non-zero. To pause at a block instead, use `/v1/producer/pause_at_block`. |
| `snapshot` | unset | File to read the snapshot state from. |
| `snapshot-endpoint` | unset | Fetch a snapshot from a URL and bootstrap. `http(s)://host:port` fetches the latest snapshot; `http(s)://host:port/50000` fetches the snapshot at block 50000. The served snapshot's block number must be a multiple of 25000, since only those heights are attested on chain. Requires an empty database. |

## HTTP API

`chain_plugin` registers no HTTP handlers. It exposes `chain_apis::read_only` and `chain_apis::read_write`,
which [`chain_api_plugin`](../chain_api_plugin/README.md) publishes as `/v1/chain/*`.

## Diagnostics

`chain_plugin` writes most of its output through the default logger; two named loggers are re-read on
`SIGHUP`:

| Logger | Carries |
|---|---|
| `vote` | Finalizer vote decisions — monotony and liveness check failures, QC integration. Mostly at debug level. |
| `dmlog` | The deep-mind trace stream, when `deep-mind = true`. |

Lines an operator should recognize:

- Startup: `starting chain in read/write mode`, then `Blockchain started; head block is #<n>` — with
  `genesis timestamp is <t>` appended when the node started from genesis.
- Snapshot load: one line naming the loaded block number, and for `--snapshot-endpoint` its root hash.
- Snapshot attestation, on success: `Snapshot attestation verified successfully for block #<n>`. The same
  check has four ways to stop the node instead, each logged at FATAL: `Could not read the on-chain snapshot
  attestation for block #<n> after syncing <k> blocks past it.`, `No attested snapshot record found for
  block #<n> after syncing <k> blocks past it.`, `Snapshot attestation mismatch for block #<n>!`, and `Error
  verifying snapshot attestation for block #<n>`. Each says the node has been stopped rather than continue
  with an unverified snapshot. A configuration-check failure instead tells the operator to delete chain state
  before restarting without `--snapshot-endpoint`.
- `Setting vote-threads to 4 on producing node`, when `producer-name` is set and `vote-threads` is unset or 0.
- `Unable to enable account queries` — logged and dropped; the node starts with the feature off.
- Native contract debugging, when configured: `Native debug: <account> (code_hash=...) -> <path>` per contract
  and a count at the end.
- Shutdown: `shutdown` at debug level.

Guard exceptions from the chain state database (free space below `chain-state-db-guard-size-mb`) are logged by
a dedicated handler and shut the node down; the fix is more space or a larger `chain-state-db-size-mb`.

## Tests

```bash
ninja -C build/debug test_chain_plugin
./build/debug/plugins/chain_plugin/test/test_chain_plugin
```

The suite is registered as one CTest case per Boost test case, so a single area can be run on its own:

```bash
./build/debug/plugins/chain_plugin/test/test_chain_plugin --run_test=get_info_consumer_plugin_policy
```

It covers the snapshot attestation policies (configuration preflight, required schemas, record tuple, block
identity, table-read failure, timeout, and retry), snapshot-endpoint option registration and validation,
outbound HTTP global option registration, the `get_info` consumer policy, the default option set and the SYS
VM OC whitelist on builds that have it, and the three side databases: `account_query_db`,
`trx_finality_status_processing`, and `trx_retry_db`.

## Related plugins

- [`chain_api_plugin`](../chain_api_plugin/README.md) — publishes this plugin's read and write APIs as
  `/v1/chain/*`.
- [`producer_plugin`](../producer_plugin/README.md) — produces and applies blocks against this controller, and
  owns snapshot creation and scheduling.
- [`net_plugin`](../net_plugin/README.md) — propagates blocks, transactions, and votes. It connects to this
  plugin's controller signals directly rather than through the appbase channels, and subscribes only to
  `transaction_ack`.
- `signature_provider_manager_plugin` — required dependency; supplies signing providers, and brings in
  `http_client_plugin`, which registers the process-wide `outbound-http-*` options.
- [`snapshot_api_plugin`](../snapshot_api_plugin/README.md) — serves the snapshots that `--snapshot-endpoint`
  downloads.
- [`status_monitor_plugin`](../status_monitor_plugin/README.md) — the second in-process consumer of the
  `get_info` cache.

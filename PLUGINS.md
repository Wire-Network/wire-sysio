# nodeop plugins

`nodeop` is assembled from appbase plugins. It always initializes `resource_monitor_plugin`, `chain_plugin`,
`net_plugin`, and `producer_plugin`, so those four need no `plugin =` line. Every other plugin is enabled with
`plugin = sysio::<name>` in `config.ini` or `--plugin sysio::<name>` on the command line; a plugin named in
`APPBASE_PLUGIN_REQUIRES` by another enabled plugin is loaded automatically. The options of the plugins
`nodeop` links are `nodeop --help` output, with two exceptions: `wallet_plugin` and `wallet_api_plugin` are
linked and initialized only by `kiod`, so their options are `kiod --help` output. Each plugin's README below
documents what it does, how it works, and how to configure it. The plugin lifecycle
(registration, initialize, startup, shutdown) is described in [plugins/usage_pattern.md](plugins/usage_pattern.md);
[plugins/template_plugin](plugins/template_plugin/README.md) is the scaffold a new plugin starts from.

| Plugin | Description | Docs |
|---|---|---|
| `batch_operator_plugin` | Cranks depot and outpost contracts, ferrying OPP message chains between the WIRE chain and the external blockchains (Ethereum, Solana) on the epoch schedule | [README](plugins/batch_operator_plugin/README.md) |
| `chain_api_plugin` | Publishes chain_plugin's read and write APIs as the `/v1/chain/*` HTTP endpoints | [README](plugins/chain_api_plugin/README.md) |
| `chain_plugin` | Owns the controller (block log, chain state, fork database, WASM runtime) and relays its signals onto the appbase channels declared in `chain_interface` | [README](plugins/chain_plugin/README.md) |
| `cron_plugin` | Provides an in-process cron scheduler that other plugins use to run functions on cron-style schedules | [README](plugins/cron_plugin/README.md) |
| `db_size_api_plugin` | Serves one read-only endpoint reporting chain-state segment usage and per-index row counts | [README](plugins/db_size_api_plugin/README.md) |
| `external_debugging_plugin` | Forwards every OPP envelope the batch operator computes to an external debugging server over JSON-RPC 2.0 | [README](plugins/external_debugging_plugin/README.md) |
| `http_client_plugin` | Owns the shared outbound HTTP client used for KIOD signing and registers the process-wide outbound-http transport options | [README](plugins/http_client_plugin/README.md) |
| `http_plugin` | The node's HTTP/RPC server: listeners, worker threads, admission control, and the API-category-to-address binding every API plugin registers against | [README](plugins/http_plugin/README.md) |
| `net_api_plugin` | Publishes net_plugin's peer management as the `/v1/net/*` HTTP endpoints | [README](plugins/net_api_plugin/README.md) |
| `net_plugin` | The peer-to-peer layer: listeners, peer discovery and authentication, block synchronization, and relay of blocks, transactions, and votes | [README](plugins/net_plugin/README.md) |
| `outpost_client_plugin` | Base plugin for the outpost client family, exporting the chain-agnostic `outpost_client` OPP interface and the bounded JSON-RPC policy both chain clients build on | [README](plugins/outpost_client_plugin/README.md) |
| `outpost_ethereum_client_plugin` | Owns this node's EVM connections: policy-enforcing signing clients with verified chain ids, loaded contract ABIs, and the chunked OPP envelope relay to the Ethereum outpost contracts | [README](plugins/outpost_ethereum_client_plugin/README.md) |
| `outpost_solana_client_plugin` | Owns this node's Solana JSON-RPC clients and their signature providers, relaying OPP envelopes to the Solana outpost program | [README](plugins/outpost_solana_client_plugin/README.md) |
| `producer_api_plugin` | HTTP endpoints for inspecting and administering a producing node (`/v1/producer/*`) | [README](plugins/producer_api_plugin/README.md) |
| `producer_plugin` | Produces and applies blocks, signs finalizer votes, runs the read-only transaction window, and owns snapshot creation and scheduling | [README](plugins/producer_plugin/README.md) |
| `prometheus_plugin` | Exposes nodeop's block, p2p, HTTP, and outbound-transport metrics in Prometheus text format at `/v1/prometheus/metrics` | [README](plugins/prometheus_plugin/README.md) |
| `query_engine_plugin` | Opt-in SQL queries over the node's current chain state, answered by an in-process C++ service and, when HTTP is enabled, `POST /v1/query/execute`; runs only on a non-producing node in head or irreversible read mode | [README](plugins/query_engine_plugin/README.md) |
| `resource_monitor_plugin` | Watches free space on every file system nodeop writes to and gracefully shuts the node down before it runs out | [README](plugins/resource_monitor_plugin/README.md) |
| `signature_provider_kms_plugin` | Adds the `KMS:` signature-provider scheme, signing Ethereum keys remotely through AWS KMS so the key never leaves AWS | [README](plugins/signature_provider_kms_plugin/README.md) |
| `signature_provider_manager_plugin` | Owns every signing key the node uses, parsing each `--signature-provider` spec and publishing the providers to the chain, producer, net, outpost, and underwriter plugins | [README](plugins/signature_provider_manager_plugin/README.md) |
| `signature_provider_ssm_plugin` | Adds the `SSM:` signature-provider scheme, fetching a private key once from an AWS SSM Parameter Store SecureString and signing locally thereafter | [README](plugins/signature_provider_ssm_plugin/README.md) |
| `snapshot_api_plugin` | Public, read-only HTTP endpoints for snapshot discovery and download, so nodes can bootstrap from each other | [README](plugins/snapshot_api_plugin/README.md) |
| `state_history_plugin` | Records per-block traces, chain-state deltas, and finality data to append-only logs and streams them to clients over a binary websocket protocol | [README](plugins/state_history_plugin/README.md) |
| `status_monitor_plugin` | Ships one `/v1/chain/get_info` snapshot per irreversible block to an OpenSearch/Elasticsearch `_bulk` endpoint, rendered through an operator JSON template | [README](plugins/status_monitor_plugin/README.md) |
| `test_control_api_plugin` | HTTP surface for `test_control_plugin`, registering the `/v1/test_control/*` write endpoints in their own `test_control` API category | [README](plugins/test_control_api_plugin/README.md) |
| `test_control_plugin` | Test-only plugin that arms deliberate node misbehavior: shut down inside a named producer's round, throw from a controller signal handler, or republish the next block with one action swapped | [README](plugins/test_control_plugin/README.md) |
| `trace_api_plugin` | Full-history action trace store with HTTP endpoints for querying traces, transactions, actions, and token transfers | [trace_api_plugin.md](plugins/trace_api_plugin/trace_api_plugin.md) |
| `underwriter_plugin` | Autonomous underwriter daemon that polls pending swaps and submits signed underwrite-intent commits for the outpost legs that still lack one | [README](plugins/underwriter_plugin/README.md) |
| `wallet_api_plugin` | Publishes the `kiod` key store as the `/v1/wallet/*` HTTP endpoints that `clio` and a `KIOD:` signature provider call. Linked and initialized by `kiod`, not `nodeop` | [README](plugins/wallet_api_plugin/README.md) |
| `wallet_plugin` | The `kiod` key store: a locked directory of AES-encrypted wallet files with inactivity auto-lock and transaction and digest signing. Linked and initialized by `kiod`, not `nodeop` | [README](plugins/wallet_plugin/README.md) |

## Support code under `plugins/`

| Directory | Description | Docs |
|---|---|---|
| `chain_interface` | Not a plugin: the header-only appbase channel and method declarations that carry block and transaction events between plugins | [README](plugins/chain_interface/README.md) |
| `template_plugin` | Not built into nodeop: the minimal plugin scaffold (plugin_target, pimpl, lifecycle) a new plugin is copied from | [README](plugins/template_plugin/README.md) |

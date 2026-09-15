# external_debugging_plugin

`external_debugging_plugin` forwards every OPP envelope that `batch_operator_plugin` computes -- in either
direction, for every outpost -- to an external debugging server over JSON-RPC 2.0, so the envelopes a node
actually produced and consumed can be inspected off-box. It is a diagnostic tool for cross-chain work, not a
production requirement. `nodeop` registers it and it declares `batch_operator_plugin` in
`APPBASE_PLUGIN_REQUIRES`, but it stays completely inert until `--ext-debugging-server` supplies a URL: without
it, startup logs `no --ext-debugging-server provided, disabled` and nothing else happens.

## How it works

```
batch_operator_plugin::debugging_opp_envelope()   (signal, fires on the plugin's own threads)
        |
        |  slot retains a shared_ptr to the sink, so a concurrent disconnect is safe
        v
debug_envelope_event_sink   bounded worker queue: 1 thread,
        |                   max_pending_items = --ext-debugging-max-pending-envelopes
        |                   full -> event dropped and counted, never blocks the emitter
        v
worker callback   parse bytes as opp::Envelope (skip on failure)
        |         -> PutEnvelopeRequest -> JSON-RPC "Envelope" -> POST <server>/api/opp
        v
PutEnvelopeResponse { key, data_existed }  -> logged
```

`plugin_initialize` reads the options, validates that both `--ext-debugging-max-pending-envelopes` and
`--ext-debugging-request-timeout-ms` are greater than zero (each throws `plugin_config_exception` naming the
option otherwise), overlays the `ext-debugging-*` transport options on the process-wide `outbound-http-*`
fallbacks, and records whether a server URL was supplied. No network work happens here.

`plugin_startup`, when enabled, does three things under one deadline scope:

1. **Builds the RPC client** against `<server>/api/opp` with `endpoint_refresh_policy::never`, so DNS is
   resolved once at startup and a later connection failure does not invalidate that result. The client's request
   policy caps both the request and the response body at 1 MiB and sets *every* timeout -- connect, header,
   read, idle, and total -- to `--ext-debugging-request-timeout-ms`.
2. **Probes the server** with `GET /api/ping`. The probe's deadline nests inside the startup deadline, so DNS
   resolution and the ping share the one configured request budget. A failure fails startup with
   `External debugging server not reachable at <endpoint>` -- an unreachable debugging server stops the node
   rather than running silently without diagnostics.
3. **Starts the sink and connects the signal.** The sink is a single-threaded bounded worker queue; the slot
   handed to the signal holds a `shared_ptr` to it so shutdown and a concurrent emission cannot race.

Each event is the tuple `(epoch_index, endpoints_type, batch_op_name, envelope_data)`. The worker first parses
`envelope_data` as an `opp::Envelope`; if that fails the event is logged and skipped rather than sent. Otherwise
it builds a `PutEnvelopeRequest` (`batch_op_name`, `endpoints_type`, `envelope_data`), renders it to JSON with
protobuf's own serializer (enums as integers), and issues it as a JSON-RPC 2.0 call whose `method` is
`Envelope`. `json_rpc_client::call` is POST and single-attempt -- it is never retried, even if the base request
policy would otherwise allow it -- so a failed delivery is a dropped diagnostic, not a stall. The response's
`key` and `data_existed` are logged.

`endpoints_type` is the `DebugOutpostEndpointsType` proto enum, which names the direction of the envelope:
`DEBUG_OUTPOST_ENDPOINTS_TYPE_OUTPOST_ETHEREUM_DEPOT`, `..._OUTPOST_SOLANA_DEPOT`,
`..._DEPOT_OUTPOST_ETHEREUM`, `..._DEPOT_OUTPOST_SOLANA`.

**Backpressure is by dropping, never by blocking.** The signal fires on `batch_operator_plugin`'s threads, so
the sink admits an event only if the queue has room; otherwise it increments a drop counter and logs a warning
carrying the epoch, direction, batch operator, current pending depth, capacity, and running total. Shutdown
disconnects the signal first, then stops the sink (joining the active callback) and discards anything still
queued, logging the discarded count.

## Enabling / configuration

The plugin needs `batch_operator_plugin` to be running -- that is where the envelopes come from -- and a server
URL.

`config.ini`:

```ini
plugin = sysio::batch_operator_plugin
plugin = sysio::external_debugging_plugin

# Without this line the plugin loads and does nothing.
ext-debugging-server = http://<your-host>:9876

# Backpressure and per-request budget (defaults shown).
ext-debugging-max-pending-envelopes = 16
ext-debugging-request-timeout-ms = 5000
```

Command line:

```bash
nodeop --plugin sysio::batch_operator_plugin \
       --plugin sysio::external_debugging_plugin \
       --ext-debugging-server http://<your-host>:9876 \
       --ext-debugging-max-pending-envelopes 16 \
       --ext-debugging-request-timeout-ms 5000
```

For an HTTPS server behind a private CA, or reached through a proxy:

```bash
nodeop --plugin sysio::external_debugging_plugin \
       --ext-debugging-server https://<your-host> \
       --ext-debugging-additional-ca-file /etc/wire/ca/debugging-ca.pem \
       --ext-debugging-proxy http://<your-host>:3128
```

## Options

All options are registered into the `config.ini` options description, so all are accepted in `config.ini` and on
the command line.

| Option | Default | Meaning |
|---|---|---|
| `ext-debugging-server` | unset (plugin disabled) | URL of the external debugging server (e.g. `http://localhost:9876`). If not provided, OPP tracking is disabled. |
| `ext-debugging-max-pending-envelopes` | `16` | Maximum debugging envelopes waiting behind the active server request. Must be greater than 0. |
| `ext-debugging-request-timeout-ms` | `5000` | Maximum time in milliseconds for each debugging-server request. Applied as the connect, header, read, idle, and total timeout, and as the startup ping budget. Must be greater than 0. |
| `ext-debugging-additional-ca-file` | unset (falls back to `outbound-http-additional-ca-file`) | PEM CA bundle added to system trust for external-debugging HTTPS requests. |
| `ext-debugging-additional-ca-path` | unset (falls back to `outbound-http-additional-ca-path`) | Hashed CA directory added to system trust for external-debugging HTTPS requests. |
| `ext-debugging-proxy` | unset (falls back to `outbound-http-proxy`) | Explicit proxy URL for external-debugging HTTP requests. |

The three transport options are the plugin's caller-scoped triple over the process-wide `outbound-http-*`
options registered by `http_client_plugin`; a value set here always wins over the global fallback, and an unset
value inherits it.

## HTTP API

The plugin registers **no** handler with `http_plugin` -- it is an HTTP client. What it calls outbound:

| Call | When | Purpose |
|---|---|---|
| `GET <ext-debugging-server>/api/ping` | Once, during `plugin_startup` | Reachability probe; failure aborts startup. |
| `POST <ext-debugging-server>/api/opp`, JSON-RPC 2.0 `{"jsonrpc":"2.0","method":"Envelope","params":<PutEnvelopeRequest as JSON>,"id":N}` | Once per admitted envelope event | Delivers one envelope. The `result` is a `PutEnvelopeResponse` carrying `key`, `data_existed`, and `batch_op_names`. |

Both calls are bounded at 1 MiB request body and 1 MiB response body.

## Diagnostics

The plugin uses the default logger and declares no logger category of its own.

| Line | Level | When |
|---|---|---|
| `external_debugging_plugin: no --ext-debugging-server provided, disabled` | info | Startup without a server URL; the plugin then does nothing. |
| `external_debugging_plugin: connected to batch_operator_plugin, forwarding to <endpoint> with pending_capacity=N request_timeout_ms=M` | info | Startup succeeded; the endpoint is rendered credential- and path-free. |
| `opp_tracking: server validation failed at <endpoint>: <detail>` | error | The startup ping failed; immediately followed by the `External debugging server not reachable at <endpoint>` assertion that aborts startup. |
| `external_debugging_plugin: send_envelope: delivered epoch=E endpoints=D batch_op=B (N bytes) -> key=K existed=X` | info | One envelope was accepted by the server. One line per delivered envelope. |
| `external_debugging_plugin: failed to parse envelope data for event from batch_op=B, skipping` | error | The event's bytes were not a decodable `opp::Envelope`; nothing is sent. |
| `external_debugging_plugin: error sending envelope to <endpoint> (epoch=E, batch_op=B)` | error | The RPC threw; the exception detail is appended and the event is dropped. |
| `external_debugging_plugin: event not admitted; dropping epoch=E endpoints=D batch_op=B pending=P capacity=C total_dropped=T` | warn | The bounded queue was full. `total_dropped` is the running count. |
| `external_debugging_plugin: discarding N pending envelopes during shutdown; total_dropped=T` | warn | Shutdown dropped queued work. |
| `external_debugging_plugin: shutdown complete` | info | End of `plugin_shutdown`. |
| `rpc_client::execute: method=... request=... res=...` | info | Emitted by the shared typed-RPC helper around each call; these are verbose and carry the full request and response JSON. |

A `total_dropped` that keeps climbing means the debugging server cannot keep up with envelope production; raise
`--ext-debugging-max-pending-envelopes`, lower `--ext-debugging-request-timeout-ms` so failing requests clear
faster, or fix the server. Note that a rising drop count never affects the batch operator itself -- the sink
drops rather than blocking it.

## Tests

The test directory has no `CMakeLists.txt`, so `plugin_target` builds the default target from its sources:

```bash
ninja -C build/debug test_external_debugging_plugin
./build/debug/plugins/external_debugging_plugin/test_external_debugging_plugin
```

The `external_debugging_plugin_tests` suite covers option registration, the bounded defaults, rejection of a
zero pending capacity and a zero request timeout, acceptance of a hostname-form server URL, the sink bounding
pending envelopes and counting drops, and the signal slot keeping a stopped sink alive after its owner releases
it. No test dials a real server.

## Related plugins

- [`batch_operator_plugin`](../batch_operator_plugin) -- required by this plugin; owns the
  `debugging_opp_envelope()` signal and the `DebugEnvelopeEvent` type that carries each envelope.
- [`http_client_plugin`](../http_client_plugin) -- registers the process-wide `outbound-http-*` fallbacks this
  plugin's `ext-debugging-*` triple layers over.
- [`prometheus_plugin`](../prometheus_plugin) -- its `nodeop_outbound_http_*` counters include this plugin's
  outbound traffic, since all outbound callers share one transport.
- [`underwriter_plugin`](../underwriter_plugin) -- the other OPP-side plugin; it has its own
  `/v1/underwriter/*` diagnostics rather than an external sink.

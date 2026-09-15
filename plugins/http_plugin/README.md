# http_plugin

`http_plugin` is the node's HTTP/RPC server. It owns the listening sockets, the HTTP worker thread pool,
request admission control, and the mapping from API category to listen address; it registers no endpoints of
its own beyond `/v1/node/get_supported_apis`. Every `*_api_plugin` names it in `APPBASE_PLUGIN_REQUIRES`, so
enabling `sysio::chain_api_plugin`, `sysio::net_api_plugin`, `sysio::producer_api_plugin`, or any other API
plugin loads it automatically — an operator rarely writes `plugin = sysio::http_plugin` directly. It is not
loaded by a bare `nodeop` run, because `nodeop` initializes only `resource_monitor_plugin`, `chain_plugin`,
`net_plugin`, and `producer_plugin` by default.

## How it works

`nodeop` calls `http_plugin::set_defaults` before initialization, fixing three per-executable values: the
default unix socket path (empty, so unix socket support is off unless `unix-socket-path` is set), the default
HTTP port (`8888`, which makes the `http-server-address` default `127.0.0.1:8888`), and the `Server` response
header (`<executable name>/<version string>`).

```
plugin_initialize                       plugin_startup (posted to the app thread, high priority)
  build categories_by_address             start http thread pool (--http-threads)
    http-server-address  -> node            one Beast listener per address in categories_by_address
    unix-socket-path     -> node            listening() flips true
    http-category-address-> one category
```

Each listener is created for one address and the set of API categories bound to that address. A request is
matched by exact path against the registered handlers; the handler runs only if its `api_category` is in that
listener's set, otherwise the request falls through to the not-found path. This is how one process serves, for
example, `chain_ro` publicly and `producer_rw` on loopback only.

Handlers reach the plugin through three registration paths, and the path decides which thread runs them:

| Registration | Thread | Used for |
|---|---|---|
| `add_handler` / `add_api(api, queue, priority)` | posted to the appbase executor, runs on the main application thread in the given `exec_queue` at the given priority | calls that must read chain state under the executor's read window |
| `add_async_handler` / `add_async_api` | runs inline on an HTTP worker thread | calls that do not touch the controller, or that do their own posting |
| `add_raw_handler` | inline on an HTTP worker thread, handler owns the connection | binary and file responses (`snapshot_api_plugin` downloads) |

Admission control runs before any work is queued. A request is rejected with a 503 "Busy" response when it
would push the in-flight byte total past `http-max-bytes-in-flight-mb` or the in-flight request count past
`http-max-in-flight-requests`. For app-thread handlers the request body is additionally reserved against the
in-flight byte budget for as long as it sits in the executor queue, and released exactly once when the posted
work runs, throws, returns early on shutdown, or is discarded.

`/v1/node/get_supported_apis` is answered by the HTTP connection handler itself rather than by a registered
handler: it returns the paths of every handler whose category is enabled on the listener that received the
request, so it is reachable on every listener and its answer differs per listener. An `OPTIONS` request is
answered with `{}` plus the configured CORS headers; any other method reaches the handler, and the API
plugins' calls take a JSON body by POST.

Exceptions thrown by handlers are converted to a JSON `error_results` body by a single shared mapping:

| Thrown | Status | `message` |
|---|---|---|
| `chain::unknown_block_exception` | 400 | Unknown Block |
| `chain::invalid_http_request` | 400 | Invalid Request |
| `chain::account_query_exception` | 400 | Account lookup |
| `chain::unsatisfied_authorization` | 401 | UnAuthorized |
| `chain::tx_duplicate` | 409 | Conflict |
| `fc::eof_exception` | 422 | Unprocessable Entity |
| any other `fc::exception`, `std::exception`, or unknown | 500 | Internal Service Error |

`verbose-http-errors` controls how much of the exception log is appended to the body: one detail entry when
off, up to ten when on.

`plugin_shutdown` stops the HTTP thread pool. `SIGHUP` re-reads the `http_plugin` logger configuration.

### API categories

`--http-category-address` binds one category to one address. Each category requires the plugin that owns it to
be named in a `plugin` option, or startup fails with `plugin_config_exception`.

| Category | Owning plugin |
|---|---|
| `chain_ro`, `chain_rw` | `sysio::chain_api_plugin` |
| `net_ro`, `net_rw` | `sysio::net_api_plugin` |
| `producer_ro`, `producer_rw`, `snapshot` | `sysio::producer_api_plugin` |
| `snapshot_ro` | `sysio::snapshot_api_plugin` |
| `db_size` | `sysio::db_size_api_plugin` |
| `trace_api` | `sysio::trace_api_plugin` |
| `prometheus` | `sysio::prometheus_plugin` |
| `test_control` | `sysio::test_control_plugin` |
| `underwriter` | `sysio::underwriter_plugin` |

`node` is the category of endpoints served on every listener; it is what `http-server-address` and
`unix-socket-path` bind, and it is not a value accepted by `--http-category-address`.

## Enabling / configuration

`http_plugin` arrives with any API plugin, so the usual `config.ini` names the API plugins and tunes the
server:

```ini
plugin = sysio::chain_api_plugin
plugin = sysio::net_api_plugin

# One address for every enabled category (the default).
http-server-address = 127.0.0.1:8888

http-threads = 2
max-body-size = 2097152
http-max-bytes-in-flight-mb = 500
http-max-in-flight-requests = -1
http-max-response-time-ms = 15
http-validate-host = true
http-keep-alive = true
```

Per-category binding replaces `http-server-address` with the literal string `http-category-address`, and
`unix-socket-path` must then be left unset:

```ini
plugin = sysio::chain_api_plugin
plugin = sysio::net_api_plugin

http-server-address = http-category-address
http-category-address = chain_ro,0.0.0.0:8888
http-category-address = chain_rw,127.0.0.1:8889
http-category-address = net_ro,127.0.0.1:8889
http-category-address = net_rw,127.0.0.1:8889
```

The equivalent command line:

```bash
nodeop --plugin sysio::chain_api_plugin \
       --plugin sysio::net_api_plugin \
       --http-server-address http-category-address \
       --http-category-address chain_ro,0.0.0.0:8888 \
       --http-category-address chain_rw,127.0.0.1:8889 \
       --http-category-address net_ro,127.0.0.1:8889 \
       --http-category-address net_rw,127.0.0.1:8889 \
       --http-threads 4
```

Two `http-category-address` entries that share a port but spell the host differently are a configuration
error, even if both resolve to the same addresses. A unix socket path must start with `/`, `./`, or `../`; a
relative path is resolved against the data directory.

## Options

All of `http_plugin`'s options are config-file options, so each is equally valid in `config.ini` and as a
`nodeop --<name>` argument.

### Listeners

| Option | Default | Meaning |
|---|---|---|
| `http-server-address` | `127.0.0.1:8888` | Local IP and port to listen on for incoming HTTP connections. Set to the literal `http-category-address` to enable the `http-category-address` option; leave blank to disable. |
| `unix-socket-path` | unset | Filename, relative to the data dir, of a unix socket for HTTP RPC; blank disables it. Must not be set when `http-category-address` is used. |
| `http-category-address` | unset | `category,address` pair binding one API category to one listen address; may be repeated. The address is `<hostname>:port`, `<ipaddress>:port`, or a unix socket path starting with `/`, `./`, or `../`. Valid categories are `chain_ro`, `chain_rw`, `db_size`, `net_ro`, `net_rw`, `producer_ro`, `producer_rw`, `snapshot`, `trace_api`, `prometheus`, `test_control`, `snapshot_ro`, and `underwriter`. |

### Limits and threads

| Option | Default | Meaning |
|---|---|---|
| `http-threads` | `2` | Number of worker threads in the HTTP thread pool. Must be greater than 0. |
| `max-body-size` | `2097152` | Maximum body size in bytes accepted for an incoming RPC request. |
| `http-max-bytes-in-flight-mb` | `500` | Maximum megabytes `http_plugin` uses for in-flight request processing; `-1` for unlimited. A 503 is returned when exceeded. |
| `http-max-in-flight-requests` | `-1` | Maximum number of concurrently processed requests; `-1` for unlimited. A 503 is returned when exceeded. |
| `http-max-response-time-ms` | `15` | Maximum time on the main thread for processing a request; `-1` for unlimited. API plugins read this value as their per-call deadline. |
| `http-keep-alive` | `true` | When false, connections are not kept alive even if the client requests it. |

### Request validation and errors

| Option | Default | Meaning |
|---|---|---|
| `http-validate-host` | `true` | When false, any incoming `Host` header is accepted. |
| `http-alias` | unset | Additional acceptable `Host` header values; may be repeated. The configured server addresses are included by default. |
| `verbose-http-errors` | `false` | Append the error log to HTTP responses. Also relayed to `chain_api_plugin`, which shortens ABI errors when this is off. |

### CORS

| Option | Default | Meaning |
|---|---|---|
| `access-control-allow-origin` | unset | Value returned in the `Access-Control-Allow-Origin` header of each response. |
| `access-control-allow-headers` | unset | Value returned in the `Access-Control-Allow-Headers` header of each response. |
| `access-control-max-age` | unset | Value returned in the `Access-Control-Max-Age` header of each response. |
| `access-control-allow-credentials` | `false` | When set, `Access-Control-Allow-Credentials: true` is returned on each response. |

## HTTP API

`http_plugin` registers one endpoint itself; everything else under `/v1/` comes from the API plugins.

| URL | Method / params | Purpose |
|---|---|---|
| `/v1/node/get_supported_apis` | no body | Returns `{"apis": [...]}`, the paths of every handler whose category is enabled on the listener that received the request. Served on every listener, including one bound to a single category. |

## Diagnostics

All of the plugin's own output goes through the `http_plugin` logger, which `SIGHUP` re-reads.

- Startup, per registered handler: `add <category> api url: <path> on <addresses>`, or
  `add <category> api url: <path> disabled for category address not configured` when no listener carries that
  category — the quickest way to confirm an endpoint is actually reachable.
- Startup, per CORS option supplied: one line echoing the configured value.
- Listener creation failures log `http service failed to start for <address>` and abort startup.
- A fatal exception on an HTTP worker thread logs `Exception in http thread pool, exiting` and quits the node.
- At debug level: `Request: <remote endpoint> <request>` per request, `404 - not found: <path>` for an
  unmatched path or a path whose category is not enabled on that listener, and one line per handled exception
  naming the API and call.
- Shutdown: `exit shutdown` at debug level.

## Tests

```bash
ninja -C build/debug http_plugin_unit_tests
./build/debug/plugins/http_plugin/test/http_plugin_unit_tests
```

The suite covers body trimming and empty-content detection, `parse_params` for each `http_params_types` mode,
rejection of invalid `http-category-address` specifications, category-to-address binding and per-listener
endpoint visibility, loopback detection, and the in-flight byte and request accounting including the
request-body reservation.

## Related plugins

- [`chain_api_plugin`](../chain_api_plugin/README.md) — registers the `chain_ro` / `chain_rw` endpoints.
- [`net_api_plugin`](../net_api_plugin/README.md) — registers the `net_ro` / `net_rw` endpoints.
- `producer_api_plugin` — registers the `producer_ro`, `producer_rw`, and `snapshot` endpoints.
- [`snapshot_api_plugin`](../snapshot_api_plugin/README.md) — registers the `snapshot_ro` endpoints, and is
  the consumer of `add_raw_handler` for file downloads.
- `db_size_api_plugin`, `trace_api_plugin`, `prometheus_plugin`, `test_control_api_plugin`,
  `underwriter_plugin` — the remaining category owners.

# net_api_plugin

`net_api_plugin` exposes `net_plugin`'s peer management over HTTP: inspect current connections, read the
gossiped block-producer peer list, and connect or disconnect a peer at runtime. It holds no state and
registers no options — each handler forwards directly to a `net_plugin` method. Enable it with
`plugin = sysio::net_api_plugin` on a node whose peering you want to manage or observe without a restart;
naming it also pulls in `net_plugin` and `http_plugin` through `APPBASE_PLUGIN_REQUIRES`.

The write half of this API (`connect`, `disconnect`) changes the node's peering at runtime, so it is
registered under the separate `net_rw` category and the plugin warns at startup when that category is
reachable off loopback.

## How it works

`plugin_initialize` does one thing: it asks `http_plugin` whether the `net_rw` category is bound only to
loopback addresses and, if not, logs a boxed security warning. It does not refuse the configuration — a node
can legitimately serve `net_rw` on a closed management network.

`plugin_startup` takes a reference to the running `net_plugin` and registers all five endpoints with
`http_plugin::add_async_api`, so every call runs inline on an HTTP worker thread rather than being posted to
the main application thread. `plugin_shutdown` is empty.

```
net_api_plugin::plugin_initialize -> http_plugin::is_on_loopback(net_rw) ? ok : SECURITY WARNING
net_api_plugin::plugin_startup    -> http_plugin::add_async_api(5 handlers -> net_plugin methods)
```

The split across categories is the operationally significant part:

- `net_ro` — `status`, `connections`, `bp_gossip_peers`. Read-only inspection.
- `net_rw` — `connect`, `disconnect`. Mutates the node's peer set for the life of the process.

With `--http-category-address` the two can be bound to different addresses, which is the supported way to
expose peer inspection while keeping peer mutation on loopback.

## Enabling / configuration

`net_api_plugin` registers no options of its own; it is configured entirely through `http_plugin` addresses
and `net_plugin` peering options.

```ini
plugin = sysio::net_api_plugin

# Default: every enabled category on one loopback address.
http-server-address = 127.0.0.1:8888
```

Splitting read from write:

```ini
plugin = sysio::net_api_plugin

http-server-address = http-category-address
http-category-address = net_ro,0.0.0.0:8888
http-category-address = net_rw,127.0.0.1:8889
```

The equivalent command line:

```bash
nodeop --plugin sysio::net_api_plugin \
       --http-server-address http-category-address \
       --http-category-address net_ro,0.0.0.0:8888 \
       --http-category-address net_rw,127.0.0.1:8889
```

Calling the endpoints:

```bash
curl -X POST http://127.0.0.1:8888/v1/net/connections -d '{}'
curl -X POST http://127.0.0.1:8888/v1/net/status      -d '"<your-host>:9876"'
curl -X POST http://127.0.0.1:8889/v1/net/connect     -d '"<your-host>:9876"'
curl -X POST http://127.0.0.1:8889/v1/net/disconnect  -d '"<your-host>:9876"'
```

## Options

The plugin's `set_program_options` is empty — it registers no options.

## HTTP API

All five endpoints take a JSON body by POST, run on an HTTP worker thread, and answer with HTTP 201.
`connect`, `disconnect`, and `status` take a bare JSON string naming a peer endpoint (`host:port`);
`connections` and `bp_gossip_peers` take no body, or `{}`.

| URL | Category | Params | Code | Purpose |
|---|---|---|---|---|
| `/v1/net/connections` | `net_ro` | none | 201 | Every current connection: peer address, remote IP and port, connection id, peer score, connecting/syncing flags, whether the peer is a BP or gossip peer, whether the socket is open, blocks-only or transactions-only, last vote received, and the peer's last handshake message. |
| `/v1/net/status` | `net_ro` | `"host:port"` | 201 | The same connection record for one peer endpoint. |
| `/v1/net/bp_gossip_peers` | `net_ro` | none | 201 | The gossiped block-producer peer list: producer name, externally reachable server endpoint, outbound IP address, and expiration. |
| `/v1/net/connect` | `net_rw` | `"host:port"` | 201 | Add the endpoint as a supplied peer and start connecting to it. Returns the resulting status string. |
| `/v1/net/disconnect` | `net_rw` | `"host:port"` | 201 | Drop the connection to the endpoint and remove it from the supplied peer set. Returns the resulting status string. |

`status` looks the peer up by an **exact string match** on the address `net_plugin` stored for the connection,
which is the configured peer address including any `:trx` or `:blk` suffix -- `p2p.example.com:9876:blk` is
found only by that full string, not by `p2p.example.com:9876`. Inbound connections have no stored address, so
they cannot be looked up at all and only appear in `connections`. A peer that is not found is not an error: the
call still answers 201, with the body being the plain string `"connection not found: <host>"` instead of a
connection record.

## Diagnostics

The plugin has no logger of its own; its lines go to the default logger, and per-request diagnostics come from
`http_plugin`'s `http_plugin` logger.

- `starting net_api_plugin` at debug level, once, at startup.
- A boxed warning at initialization when the `net_rw` category is bound to anything other than loopback:

  ```
  **********SECURITY WARNING**********
  *                                  *
  * --        Net RW API          -- *
  * - EXPOSED to the LOCAL NETWORK - *
  * - USE ONLY ON SECURE NETWORKS! - *
  *                                  *
  ************************************
  ```

  This is a warning, not a failure; the node starts. Treat a non-loopback `net_rw` bind as a deliberate
  private-management-network decision.
- One `add net_ro api url: /v1/net/<call> ...` or `add net_rw api url: ...` line per endpoint from
  `http_plugin`, including `disabled for category address not configured` when no listener carries that
  category.
- Handler exceptions are logged and converted by `http_plugin`'s shared mapping; the log line names
  `net.<call>`.

## Tests

The plugin has no `test/` directory, and no C++ test binary links it -- `plugin_test` does not, and
`test_net_plugin` covers `net_plugin` internals rather than the calls registered here (its BP-peering case runs
against a mock connection manager). The endpoints are covered by the Python integration tests:
`test_NetApi` in `tests/plugin_http_api_test.py` drives `connect`, `disconnect`, `status`, and `connections`
over HTTP, and `tests/auto_bp_gossip_peering_test.py` is the only test that exercises `bp_gossip_peers`, on a
running cluster.

## Related plugins

- [`net_plugin`](../net_plugin/README.md) — owns the peer connections and implements every call registered
  here.
- [`http_plugin`](../http_plugin/README.md) — serves the endpoints and supplies the `net_ro` / `net_rw`
  category binding and the loopback check behind the security warning.
- [`chain_api_plugin`](../chain_api_plugin/README.md) — the same pattern for `/v1/chain/*`.

# http_client_plugin

`http_client_plugin` owns the process's shared outbound `fc::http_client` -- the client the signing path uses to
reach a `kiod` wallet -- and it is the single registrar of the process-wide outbound HTTP transport options
(`--outbound-http-*`). Operators almost never name it directly: `signature_provider_manager_plugin` declares it
in `APPBASE_PLUGIN_REQUIRES`, so any node that resolves signature providers loads it automatically, and `nodeop`
registers it unconditionally so its options always parse. It has no dependencies of its own
(`APPBASE_PLUGIN_REQUIRES()` is empty) and serves no HTTP itself.

## How it works

The plugin constructs one `fc::http_client` in its constructor and hands it out through `get_client()`;
`signature_provider_manager_plugin` calls that accessor to make KIOD signing requests. Nothing else happens at
runtime -- `plugin_startup` and `plugin_shutdown` are empty, and no thread, timer, or signal connection is
created.

Its real responsibility is configuration. `sysio/http_client_plugin/http_client_options.hpp` defines the shared
outbound-HTTP option vocabulary used across the whole node:

- `add_global_transport_program_options()` registers the three process-wide fallbacks
  (`--outbound-http-additional-ca-file`, `--outbound-http-additional-ca-path`, `--outbound-http-proxy`). **This
  plugin is the one and only caller** -- the header's comment states the process-wide options have a single
  owner, so no other plugin may register them.
- `add_transport_program_options(names, caller)` registers one *caller's* three overrides under that caller's own
  prefix. Each outbound caller in the node registers its own triple: `http-client-*` here (scope
  `shared KIOD/signing`), plus `snapshot-endpoint-*`, `outpost-ethereum-*`, `outpost-solana-*`, and
  `ext-debugging-*` registered by their own plugins.
- `read_transport_options(options, names)` applies the globals first and then the caller's own names on top, so
  a caller-specific value always wins over the process-wide fallback and an unset caller value inherits it.

In `plugin_initialize` this plugin performs that overlay for the `http-client-*` triple and installs the result
on its `fc::http_client` via `set_transport_options`. The options are read once at initialization; there is no
re-read on `SIGHUP`.

```
--outbound-http-*        (registered here; applies to every outbound caller)
        |
        v  overlaid by, and overridden by, each caller's own triple
--http-client-*          -> this plugin's fc::http_client            (KIOD / signing)
--snapshot-endpoint-*    -> chain_plugin snapshot bootstrap download
--outpost-ethereum-*     -> outpost_ethereum_client_plugin
--outpost-solana-*       -> outpost_solana_client_plugin
--ext-debugging-*        -> external_debugging_plugin
```

## Enabling / configuration

The plugin needs no `plugin =` line in the common case: it is loaded as a dependency of
`signature_provider_manager_plugin`. Naming it explicitly is harmless.

`config.ini`:

```ini
# Process-wide fallbacks for every outbound HTTPS caller in the node.
outbound-http-additional-ca-file = /etc/wire/ca/internal-root.pem
outbound-http-proxy = http://<your-host>:3128

# Narrower override, applied only to the shared KIOD/signing client.
http-client-additional-ca-file = /etc/wire/ca/kiod-ca.pem
```

Command line:

```bash
nodeop --outbound-http-additional-ca-file /etc/wire/ca/internal-root.pem \
       --outbound-http-proxy http://<your-host>:3128 \
       --http-client-additional-ca-file /etc/wire/ca/kiod-ca.pem
```

In this example the KIOD/signing client trusts `kiod-ca.pem` (its own value wins) while still picking up the
global proxy, and every other outbound caller trusts `internal-root.pem`.

## Options

All six are registered into the `config.ini` options description, so they are accepted both in `config.ini` and
on the command line. All are unset by default, in which case the system trust store is used and no proxy is
configured.

### Process-wide fallbacks (registered here for the whole node)

| Option | Default | Meaning |
|---|---|---|
| `outbound-http-additional-ca-file` | unset | PEM CA bundle added to system trust for all outbound HTTPS callers unless a caller-specific override is set. |
| `outbound-http-additional-ca-path` | unset | Hashed CA directory added to system trust for all outbound HTTPS callers unless a caller-specific override is set. |
| `outbound-http-proxy` | unset | Explicit proxy URL used by all outbound HTTP callers unless a caller-specific override is set. |

### Shared KIOD/signing client overrides

| Option | Default | Meaning |
|---|---|---|
| `http-client-additional-ca-file` | unset (falls back to `outbound-http-additional-ca-file`) | PEM CA bundle added to system trust for shared KIOD/signing HTTPS requests. |
| `http-client-additional-ca-path` | unset (falls back to `outbound-http-additional-ca-path`) | Hashed CA directory added to system trust for shared KIOD/signing HTTPS requests. |
| `http-client-proxy` | unset (falls back to `outbound-http-proxy`) | Explicit proxy URL for shared KIOD/signing HTTP requests. |

## HTTP API

None. This plugin is an HTTP *client*; it registers no handler with `http_plugin` and does not depend on it.

## Diagnostics

The plugin emits no log lines of its own. Failures surface from whichever component is using the client -- for
the KIOD path that is `signature_provider_manager_plugin`'s logging, and TLS/proxy errors carry the message
produced by `fc::http_client` for the configured transport.

## Tests

The plugin has no `test/` directory and therefore no dedicated target. The shared option vocabulary it registers
is covered by `test_chain_plugin`'s `outbound_http_global_option_registration` case, which registers the global
triple plus every caller triple (including `http-client-*`) and asserts the local-over-global precedence of
`read_transport_options`:

```bash
ninja -C build/debug test_chain_plugin
./build/debug/plugins/chain_plugin/test/test_chain_plugin -t outbound_http_global_option_registration
```

## Related plugins

- [`signature_provider_manager_plugin`](../signature_provider_manager_plugin) -- the consumer that declares this
  plugin in `APPBASE_PLUGIN_REQUIRES` and calls `get_client()` for KIOD signing.
- [`chain_plugin`](../chain_plugin) -- registers the `snapshot-endpoint-*` caller triple for snapshot bootstrap
  downloads, layered over the same globals.
- [`external_debugging_plugin`](../external_debugging_plugin),
  [`outpost_ethereum_client_plugin`](../outpost_ethereum_client_plugin),
  [`outpost_solana_client_plugin`](../outpost_solana_client_plugin) -- the other outbound callers that register
  their own triples over these globals.
- [`prometheus_plugin`](../prometheus_plugin) -- exports the process-wide outbound transport counters
  (`nodeop_outbound_http_*`) that cover traffic sent through these options.

# outpost_client_plugin

`outpost_client_plugin` is the base of the outpost client family. It registers no options and owns no
connection of its own; what it contributes is the two headers every outpost client plugin builds on — the
chain-agnostic `outpost_client` service-provider interface (SPI) and the shared outbound JSON-RPC policy —
plus the appbase node in the dependency graph that guarantees they are initialized before any chain-specific
client. An operator never enables it by hand: `outpost_ethereum_client_plugin` and
`outpost_solana_client_plugin` both name it in `APPBASE_PLUGIN_REQUIRES`, so naming either of those loads
this one automatically.

## How it works

The plugin class (`sysio/outpost_client_plugin.hpp`) declares
`APPBASE_PLUGIN_REQUIRES((signature_provider_manager_plugin))` and implements the four lifecycle methods as
near no-ops: `set_program_options` registers nothing, `plugin_initialize` does nothing, and startup and
shutdown each write a single line. Its value is entirely in the two public headers it exports, and in the
ordering the `APPBASE_PLUGIN_REQUIRES` edge buys — the signature-provider manager has created every provider
before a chain-specific client tries to resolve one.

### `sysio/outpost_client/outpost_client.hpp` — the SPI

`outpost_client` is the abstract interface between the plugins that orchestrate OPP work
(`batch_operator_plugin`, `underwriter_plugin`) and the chain-specific concretes. An orchestrator holds an
`outpost_client_ptr` (a `std::shared_ptr<outpost_client>`) and calls only these virtuals; it never interprets
an EVM address, a Solana public key, a PDA, or a signature-provider format.

| Member | Purpose |
|---|---|
| `chain_kind()` | `sysio::opp::types::ChainKind` of the target chain — diagnostics and debug-endpoint selection |
| `chain_code()` | Slug-packed code of this chain's `sysio.chains` row — a `slug_name` over `[A-Z0-9_]`, at most 8 symbols, carried as the packed `uint64`. Rows are registered with `sysio.chains::regchain` |
| `chain_id()` | Numeric chain id on the target chain; Solana has none and reports `0` |
| `authenticated_caller_address()` | Raw chain-native address that authenticates this client's writes — 20 bytes on Ethereum, 32 on Solana |
| `to_string()` | `{chain_code}:{ChainKind_Name}:{chain_id}`. The default prints `chain_code()` as the raw packed integer, so the row registered as `ETH` against a local chain renders `23373212024832:CHAIN_KIND_EVM:31337`. Virtual, with a default derived from the three getters |
| `deliver_outbound_envelope(epoch_index, envelope_bytes, deadline)` | OPP outbound — submit one envelope to the remote chain; returns the chain-native transaction id |
| `read_inbound_envelope(epoch_index, deadline)` | OPP inbound — return the envelope the remote chain produced for this epoch, or an empty vector when the latest slot does not match |
| `uw_commit(uw_request_id, uic_bytes, deadline)` | Underwriter commit — relay a signed `UnderwriteIntentCommit` through the outpost; returns only after on-chain confirmation |

Two rules bind every implementation. Each RPC-bound call must enforce the `deadline` it is passed, so a hung
remote chain cannot starve a cron worker; the protected helper `throw_if_past_deadline(deadline_abs, op)`
throws `fc::timeout_exception` labelled with `to_string()` and is meant to be called before each blocking
RPC. And `read_inbound_envelope` must filter by `epoch_index` itself — both outposts expose a single
latest-outbound storage slot, so a poll can still observe the preceding epoch until the consensus-reaching
delivery overwrites it, and forwarding that stale envelope to `sysio.msgch::deliver` trips an
`envelope epoch_index mismatch` assertion.

The header also declares the cross-chain envelope cap:

```cpp
inline constexpr size_t OPP_MAX_ENVELOPE_BYTES = 32'768;
```

This value mirrors the Solana program's `MAX_ENVELOPE_BYTES`, the Ethereum `OPPCommon.MAX_ENVELOPE_BYTES`,
and the depot's `sysio.msgch` packing cap. Every derived guard in the concrete clients — hex bounds, JSON-RPC
response ceilings, Solana chunk counts — follows from this single declaration, so the sides must never
diverge.

### `sysio/outpost_client/rpc_options.hpp` — the shared RPC policy

`outpost_rpc::rpc_options(options, transport_names)` returns the bounded
`fc::network::json_rpc::client_options` that one configured outpost RPC client runs under. Both chain plugins
call it, so both inherit the same limits:

| Bound | Value |
|---|---|
| `rpc_max_request_bytes` | 1 MiB |
| `rpc_max_response_bytes` | 4 MiB |
| connect timeout | 10 s |
| header timeout | 30 s |
| read timeout | 30 s |
| idle timeout | 30 s |
| total timeout | 30 s |

None of these are operator-tunable; they are compiled in. The transport half of the policy is read from the
`variables_map` through `outbound_http::read_transport_options`, which applies the process-wide
`--outbound-http-additional-ca-file`, `--outbound-http-additional-ca-path`, and `--outbound-http-proxy`
options first and then lets the caller's own names override them. The caller's names are supplied by the
chain plugin (for example `--outpost-ethereum-proxy`), which is why the per-chain overrides always win over
the process-wide fallbacks.

## Enabling / configuration

There is nothing to enable and nothing to configure. `outpost_ethereum_client_plugin` and
`outpost_solana_client_plugin` each declare this plugin in `APPBASE_PLUGIN_REQUIRES`, so appbase initializes
it whenever one of them is named:

```ini
# Loading either chain client pulls outpost_client_plugin in; it needs no line of its own.
plugin = sysio::outpost_ethereum_client_plugin
plugin = sysio::outpost_solana_client_plugin
```

```bash
nodeop --plugin sysio::outpost_ethereum_client_plugin \
       --plugin sysio::outpost_solana_client_plugin
```

## Options

`set_program_options` registers no options. Every option an operator sets for an outpost connection belongs
to the chain-specific plugin (`--outpost-ethereum-*`, `--outpost-solana-*`) or to the process-wide
`--outbound-http-*` set owned by `http_client_plugin`.

## HTTP API

None. The plugin registers no endpoints and does not depend on `http_plugin`.

## Diagnostics

Two lines, both through fc's `default` logger:

- `Starting outpost client plugin` at startup.
- `Shutdown outpost client plugin` at shutdown.

Everything else an operator sees about an outpost connection is emitted by the chain-specific plugin or by
the concrete client, which prefixes its lines with the SPI's `to_string()` label
(`outpost_ethereum_client[23373212024832:CHAIN_KIND_EVM:31337]: ...`).

## Tests

```bash
ninja -C build/debug test_outpost_client_plugin
./build/debug/plugins/outpost_client_plugin/test_outpost_client_plugin
```

The binary holds two suites. `outpost_client_plugin` has a single case, `init_plugin`, which asserts a
constant and therefore only proves the plugin header compiles and links. `outpost_client_interface_tests`
exercises the SPI's getter and format semantics through a minimal subclass, over an EVM outpost on a local
chain id, an EVM outpost on mainnet, and an SVM outpost whose numeric chain id is `0`; it passes synthetic
small chain codes rather than packed slugs, and it overrides `to_string()` with its own copy of the format,
so the base class's default implementation is not under test. Higher-fidelity mocks live in
`batch_operator_plugin/test`, where they are consumed by the `outpost_opp_job` tests. No test dials a chain.

## Related plugins

- [`outpost_ethereum_client_plugin`](../outpost_ethereum_client_plugin/README.md) — the EVM concrete.
- [`outpost_solana_client_plugin`](../outpost_solana_client_plugin/README.md) — the Solana concrete.
- `signature_provider_manager_plugin` — required dependency; supplies the signers the concretes resolve by name.
- `http_client_plugin` — owns the process-wide `--outbound-http-*` transport options this plugin's RPC policy reads.
- `batch_operator_plugin`, `underwriter_plugin` — the SPI's consumers.

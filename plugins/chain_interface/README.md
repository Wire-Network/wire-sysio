# chain_interface

`chain_interface` is not a plugin — it is a header-only support library that sits under `plugins/` because
plugins are its only consumers. Its single header, `sysio/chain/plugin_interface.hpp`, declares the appbase
**channels** and **methods** that carry block and transaction events between plugins, so a plugin can consume
`chain_plugin`'s block stream or hand a transaction to `producer_plugin` without either side including the
other's header. There is nothing to enable and nothing to configure; a plugin picks it up by putting
`plugins/chain_interface/include` on its include path.

## How it works

The directory holds one file and no `CMakeLists.txt`, so no target is built and no entry exists in
`plugins/CMakeLists.txt`. It reaches consumers through two include-path additions:

- `plugins/producer_plugin/CMakeLists.txt` adds it as a **PUBLIC** include directory of `producer_plugin`.
  Because it is public, every target that links `producer_plugin` — directly or transitively, which is most
  of the plugin tree — inherits the path.
- `cmake/test-tools.cmake` adds it to the include directories of unit-test targets, so tests can drive the
  same channels and methods.

Everything lives in `namespace sysio::chain::plugin_interface`, which opens `sysio::chain` and `appbase`, so
the declarations read against the chain's own types. A forward-declared tag struct, `chain_plugin_interface`,
identifies the method group.

The two appbase primitives behave differently and the header uses both deliberately. A **channel**
(`channel_decl`) is one-to-many publish/subscribe: a publisher posts a value at a priority and every
subscriber sees it. A **method** (`method_decl`) is a call into a registered provider and returns a value; the
`transaction_async` declaration carries `first_provider_policy`, so the first registered provider handles the
call. A plugin reaches either through `app().get_channel<...>()` / `app().get_method<...>()` and holds the
returned `handle` for the lifetime of its subscription or registration.

### Channels

| Declaration | Payload | Published by | Priority |
|---|---|---|---|
| `channels::accepted_block_header` | `block_signal_params` | `chain_plugin`, relaying `controller::accepted_block_header` | `medium` |
| `channels::accepted_block` | `block_signal_params` | `chain_plugin`, relaying `controller::accepted_block` | `high` |
| `channels::irreversible_block` | `block_signal_params` | `chain_plugin`, relaying `controller::irreversible_block` | `low` |
| `channels::applied_transaction` | `transaction_trace_ptr` | `chain_plugin`, relaying `controller::applied_transaction` | `low` |
| `compat::channels::transaction_ack` | `std::pair<fc::exception_ptr, packed_transaction_ptr>` | `producer_plugin` after a speculative transaction is applied or rejected, and `chain_plugin`'s transaction-retry database | `low` |

`irreversible_block` is the one most non-chain plugins consume: `batch_operator_plugin` and
`underwriter_plugin` each subscribe to it as their sync gate, arming their work only once the node has caught
up. `net_plugin` subscribes to `compat::channels::transaction_ack` so it can relay or drop a peer's
transaction according to the result.

### Methods

| Declaration | Signature | Provider |
|---|---|---|
| `methods::get_block_by_id` | `signed_block_ptr(const block_id_type&)` | `chain_plugin` |
| `methods::get_head_block_id` | `block_id_type()` | `chain_plugin` |
| `incoming::methods::transaction_async` | `void(const packed_transaction_ptr&, bool, transaction_metadata::trx_type, bool, next_function<transaction_trace_ptr>)`, `first_provider_policy` | `producer_plugin` |

`transaction_async` is the inbound transaction path for every source, not only the HTTP API.
`chain_plugin` calls it for transactions arriving over the API, and again — through
`chain_plugin::accept_transaction` — for every transaction `net_plugin` accepts from a peer (it drops peer
transactions outright when `p2p-accept-transactions` is false or while it is syncing);
`producer_plugin` calls it directly to submit its own `votesnaphash` transactions. `producer_plugin`
registers the single provider that schedules all of them, replying through the `next_function`
continuation.

## Enabling / configuration

Not applicable — this is a header-only library, not a plugin. There is no `plugin = ...` line, no options,
and no lifecycle. To consume it from a plugin that does not already link `producer_plugin`, add the include
directory and include the header:

```cmake
target_include_directories(<your_plugin>
        PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../chain_interface/include")
```

```cpp
#include <sysio/chain/plugin_interface.hpp>

// held for as long as the subscription should live:
sync_gate_subscription =
   app().get_channel<chain::plugin_interface::channels::irreversible_block>().subscribe(
      [this](const chain::block_signal_params& p) { /* ... */ });
```

[`usage_pattern.md`](../usage_pattern.md) lists connecting to other plugins' signals and registering
provider methods among the typical `plugin_initialize` actions; the sync-gate consumers in this tree
(`batch_operator_plugin`, `underwriter_plugin`) subscribe from `plugin_startup` instead, after their own state
is built. Either way, keep the returned handle — dropping it cancels the subscription.

## Options

None. The library registers nothing with `boost::program_options`.

## HTTP API

None.

## Diagnostics

None. The header declares types only; every log line about a block or transaction event comes from the plugin
that publishes or consumes it.

## Tests

The directory has no test target of its own — there is no code to test, only declarations. The channels and
methods are exercised wherever they are used: `plugins/chain_plugin/test/test_trx_retry_db.cpp` runs the
application so `compat::channels::transaction_ack` works and subscribes to it, and
`plugins/producer_plugin/test/test_trx_full.cpp` calls `incoming::methods::transaction_async` and subscribes
to the ack channel. Both run under their own plugin's test target.

## Related plugins

- `chain_plugin` — publishes all four block and transaction channels and provides both block-lookup methods.
- `producer_plugin` — provides `transaction_async`, publishes `transaction_ack`, and is the target that makes
  this include path public to the rest of the tree.
- `net_plugin` — subscribes to `transaction_ack`, and feeds every transaction it accepts from a peer into
  `transaction_async` via `chain_plugin::accept_transaction`.
- `batch_operator_plugin`, `underwriter_plugin` — subscribe to `irreversible_block` as their sync gate.
- [`plugins/usage_pattern.md`](../usage_pattern.md) — when to connect, register, and release these handles
  across the appbase lifecycle.

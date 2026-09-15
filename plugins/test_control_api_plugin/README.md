# test_control_api_plugin

`test_control_api_plugin` is the HTTP surface of [`test_control_plugin`](../test_control_plugin/README.md).
It registers three write endpoints under `/v1/test_control/` that arm the test-only behaviors that plugin
implements — shut the node down inside a named producer's round, throw from a named controller signal
handler, and republish the next block with one action swapped for another. It is a test-only plugin and it is
opt-in; naming it is the normal way to get both plugins, because it declares `test_control_plugin` as a
dependency.

## How it works

The plugin declares `APPBASE_PLUGIN_REQUIRES((test_control_plugin)(chain_plugin)(http_plugin))`.
`set_program_options` and `plugin_initialize` are empty. All of its work is in `plugin_startup`: it takes the
read-write API handle from `test_control_plugin` via `get_read_write_api()` and registers three handlers with
`http_plugin`, each in the `api_category::test_control` category and each dispatched on the
`appbase::exec_queue::read_write` queue.

Each handler parses the request body into the matching parameter struct, calls the one API method, and
answers with the serialized result. The parameters are required: an empty body, an empty JSON object, or a
body that does not match the parameter struct is rejected by `http_plugin` with HTTP 400 and the
`invalid_http_request` chain error (`3200006`) before the API method is reached. Any exception raised inside
the handler is converted by `http_plugin::handle_exception`. The result type of all three calls is the empty
struct, so a successful response body is `{}` with status **202 Accepted** — the status says the behavior was
armed, not that it has fired.

Because the endpoints live in their own `test_control` API category, they can be bound to a separate listener
with `--http-category-address test_control,<address>` while the rest of the node's APIs stay elsewhere.
`nodeop` maps that category name back to `sysio::test_control_plugin` when it reports which plugin serves it.

## Enabling / configuration

```ini
plugin = sysio::test_control_api_plugin
# test_control_plugin, chain_plugin, and http_plugin are loaded automatically as dependencies.
```

```bash
nodeop --plugin sysio::test_control_api_plugin
```

These endpoints can stop the node and rewrite a block. Keep them on a loopback listener:

```bash
nodeop --plugin sysio::test_control_api_plugin \
       --http-server-address http-category-address \
       --http-category-address test_control,127.0.0.1:8888
```

## Options

`set_program_options` registers no options. Where the endpoints listen is controlled by `http_plugin`'s
`--http-server-address` and `--http-category-address` options.

## HTTP API

All three endpoints are POST with a JSON body, require their parameters, and answer `202` with `{}`.

| Endpoint | Params | Purpose |
|---|---|---|
| `/v1/test_control/kill_node_on_producer` | `producer` (account name), `where_in_sequence` (uint32), `based_on_lib` (bool) | Arm a shutdown once the named producer's round reaches the given slot. `based_on_lib` true tracks `irreversible_block`; false tracks `accepted_block`. |
| `/v1/test_control/throw_on` | `signal` (string), `exception` (string) | Arm a single throw from the named controller signal handler — one of `block_start`, `accepted_block_header`, `accepted_block`, `irreversible_block`, `applied_transaction`, `voted_block`, `aggregated_vote`. `exception` set to `controller_emit_signal_exception` throws that type; any other value throws `misc_exception`. Fires once, then disarms. |
| `/v1/test_control/swap_action` | `from` (action name), `to` (action name), `trx_priv_key` (private key), `blk_priv_key` (private key), `shutdown` (bool, default false) | Arm a one-shot rewrite of the next accepted block that contains an action named `from`, replacing it with `to`, re-signing the transaction and the block, and broadcasting the result. Requires Savanna to be active. `shutdown` true quits the node right after. |

## Diagnostics

The plugin itself logs nothing. Request-level logging comes from `http_plugin`, and every armed behavior and
its effect is logged by `test_control_plugin` — see that plugin's README for the line shapes (`received throw
on: ...`, `Swapped action ... to ...`, `shutting down`, and the rest).

## Tests

The directory has no `test/` subdirectory, so the build produces no per-plugin test target. The endpoints are
covered by the Python integration tests under `tests/`: `plugin_http_api_test.py` exercises
`kill_node_on_producer` for the empty, empty-object, invalid, and valid parameter cases and asserts the
`400`/`3200006` and `{}` responses, and lists `test_control` among the API categories the node reports;
`nodeop_signal_throw_test.py` drives `throw_on` across every signal; `interrupt_trx_test.py` drives
`swap_action`; and the `TestHarness` `Node` helper calls `kill_node_on_producer` for the fork, snapshot, and
LIB-advance tests.

## Related plugins

- [`test_control_plugin`](../test_control_plugin/README.md) — required dependency; implements every behavior these endpoints arm.
- `chain_plugin` — required dependency.
- `http_plugin` — required dependency; owns the listener, the `test_control` API category, and the parameter and exception handling.

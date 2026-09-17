# test_control_plugin

`test_control_plugin` lets a test harness reach into a running `nodeop` and make it misbehave on purpose:
shut down at a chosen point in a named producer's round, throw from a chosen controller signal handler, or
republish the next block with one action swapped for another. It is a test-only plugin — nothing in normal
operation drives it — and it is opt-in, so a node that does not name it carries none of this behavior. On its
own it exposes no way to be driven; the request surface is
[`test_control_api_plugin`](../test_control_api_plugin/README.md), which loads this plugin as a dependency.

## How it works

The plugin declares `APPBASE_PLUGIN_REQUIRES((chain_plugin)(net_plugin))` and does all of its work on the
block-application path. `set_program_options` and `plugin_initialize` are empty; `plugin_startup` constructs
the implementation over `chain_plugin`'s `controller` and connects it to seven controller signals:
`block_start`, `accepted_block_header`, `accepted_block`, `irreversible_block`, `applied_transaction`,
`voted_block`, and `aggregated_vote`. Each connection is held as a `boost::signals2::scoped_connection`.

Nothing happens on those signals until a request arms one of three independent behaviors through the
read-write API (`test_control_apis::read_write`, obtained by the API plugin via `get_read_write_api()`).

**Kill on producer.** `kill_node_on_producer(producer, where_in_sequence, based_on_lib)` arms a shutdown
tracked either on `irreversible_block` (`based_on_lib` true) or on `accepted_block` (false). On each tracked
signal the plugin computes the producer scheduled for the next block slot and the current slot within the
producer's round. It first waits for a *clean* sequence — a block from some other producer — so that a
partially observed round cannot trigger early; once the named producer's round has started it calls
`app().quit()` as soon as the slot reaches `where_in_sequence`, or as soon as another producer takes over
(so a producer that does not complete a full round still shuts the node down). While armed it logs which
producer and slot it sees and which it is waiting for.

**Throw on signal.** `throw_on(signal, exception)` arms a single throw from the named signal handler. When
that handler next fires, the plugin throws `chain::controller_emit_signal_exception` if `exception` is the
string `controller_emit_signal_exception`, and `chain::misc_exception` otherwise. It resets itself first, so
exactly one throw happens per request.

**Swap action.** `swap_action(from, to, trx_priv_key, blk_priv_key, shutdown)` arms a one-shot block rewrite
that requires Savanna to be active. On the next `accepted_block` containing a transaction with an action
named `from`, the plugin clones the block, points it at the original as its previous block, drops the QC,
advances the timestamp by one slot, renames the matching action to `to`, re-signs the transaction with
`trx_priv_key`, recomputes the transaction merkle root, re-signs the block with `blk_priv_key`, and feeds the
result back through `controller::accept_block`. The rewritten block is then broadcast through `net_plugin`.
When `shutdown` is set the node quits immediately afterwards. The arming is cleared either way, so one
request rewrites one block.

## Enabling / configuration

The plugin is registered by `nodeop` but not loaded unless named. In practice an operator loads the API
plugin instead, which pulls this one in; loading it directly is only useful for a harness that drives the
read-write API in-process.

```ini
plugin = sysio::test_control_plugin
```

```bash
nodeop --plugin sysio::test_control_plugin
```

To drive it over HTTP, load the API plugin — it requires this one:

```bash
nodeop --plugin sysio::test_control_api_plugin
```

## Options

`set_program_options` registers no options. Every behavior is armed at runtime through the read-write API,
not through configuration.

## HTTP API

None in this plugin. The endpoints that reach these behaviors are registered by
[`test_control_api_plugin`](../test_control_api_plugin/README.md).

## Diagnostics

All lines go through fc's `default` logger.

- `test_control_plugin starting up` and `test_control_plugin shutting down` at debug level.
- `kill on lib for producer: <name> at their <n> slot in sequence` or `kill on head for producer: ...` when a
  kill is armed.
- While a kill is armed, one line per tracked block: `producer <name> slot <n>, looking for <name> slot <n>`,
  or `producer <name> slot <n>, looking for start of <name> production round` before the sequence is clean;
  then `producer <name> slot: <n>` and finally `shutting down`.
- `received throw on: <params>` when a throw is armed, and
  `throwing controller_emit_signal_exception for signal <name>` or `throwing misc_exception for signal <name>`
  when it fires.
- `received swap_action: <params>` when a swap is armed, and
  `Swapped action <from> to <to>, add_result <result>, block <num>` when the rewritten block is accepted.

## Tests

The directory has no `test/` subdirectory, so the build produces no per-plugin test target. The behaviors are
exercised by the Python integration tests under `tests/`, which drive them through the HTTP endpoints:
`nodeop_signal_throw_test.py` walks every signal name for `throw_on`, `interrupt_trx_test.py` drives
`swap_action` with and without `shutdown`, and the `TestHarness` `Node` helper calls `kill_node_on_producer`
on behalf of the fork, snapshot, and LIB-advance tests. `nodeop_signal_throw_test.py` notes that
`voted_block` is only applicable to producers and `aggregated_vote` only to finalizers.

## Related plugins

- [`test_control_api_plugin`](../test_control_api_plugin/README.md) — the HTTP surface for these behaviors; requires this plugin.
- `chain_plugin` — required dependency; supplies the `controller` whose signals are connected.
- `net_plugin` — required dependency; broadcasts the block produced by `swap_action`.

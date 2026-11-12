# account_watch_plugin

`account_watch_plugin` writes one JSON record for every action in an accepted block that a watched authorization
signed, or that runs at a watched account. The records go to the `account_watch` logger, so `logging.json` decides
where they land: a file a watchdog reads, the es sink, or both. The plugin has no threads and no HTTP client; its work
runs in the controller's signal handlers on the application thread. The rules are indexed by the account they name,
so an action no rule can match costs one hash lookup per authorization and per receiver, whatever the number of rules,
plus a check of the rules whose account is `*`.

It is opt-in (`plugin = sysio::account_watch_plugin`), and once enabled it needs at least one `--account-watch-include`
or `--account-watch-receiver` rule: startup fails without one. Every option is an `account-watch-*` option, accepted both in `config.ini` and on the
command line, and is fixed at startup.

## Enabling the plugin

A producer, watching its producer account and its snapshot provider account, with the snapshot provider's routine
attestation excluded:

```ini
plugin = sysio::account_watch_plugin
account-watch-include = prod.a,*::*
account-watch-include = snap.a,*::*
account-watch-exclude = snap.a@active,sysio::votesnaphash
```

An account holder watching everything their account signs:

```ini
plugin = sysio::account_watch_plugin
account-watch-include = alice,*::*
```

An account holder watching only the token transfers their account sends:

```ini
plugin = sysio::account_watch_plugin
account-watch-include = alice,sysio.token::transfer
```

An account holder watching every token transfer their account sends or receives. `sysio.token` notifies both the
sender and the recipient, so a receiver rule reports both; `data.from` and `data.to` tell them apart. Anyone can send
the account a transfer, so anyone can make this rule fire.

```ini
plugin = sysio::account_watch_plugin
account-watch-receiver = alice,sysio.token::transfer
```

| Option | Default | Meaning |
|---|---|---|
| `account-watch-include` | none | Repeatable authorization rule. An action is reported when one of its authorizations matches an include rule and no exclude rule. |
| `account-watch-exclude` | none | Repeatable authorization rule. Applies only to include rules, so it needs one. |
| `account-watch-receiver` | none | Repeatable receiver rule. An action is reported when it runs at an account a receiver rule names. |

At least one include or receiver rule is required.
| `account-watch-max-data-bytes` | 1024 | Largest action data, in bytes, a match record decodes. Larger data is reported by size and sha256 only. |
| `account-watch-heartbeat-seconds` | 600 | Seconds between heartbeat records. 0 disables them. |

## Rules

An include or exclude rule is `<actor>[@<permission>],<contract>::<action>`. Each field is an account name or `*`; a
rule without `@<permission>` matches every permission. There are no partial wildcards (`prod.*` is rejected).

| Rule | Matches |
|---|---|
| `prod.a,*::*` | every action prod.a authorizes, with any permission |
| `prod.a@owner,*::*` | every use of prod.a's owner permission |
| `*,sysio::setcode` | every setcode, whoever authorizes it |
| `snap.a@active,sysio::votesnaphash` | snap.a's attestations, as an exclude |

- **Authorizations, not receivers.** Include and exclude rules match only an action's declared authorizations. A
  transfer to a watched account notifies it but does not match its include rule: anyone could send one. Receiver rules
  cover that case.
- **Excludes are per authorization.** An exclude removes only the authorization it matches. With the exclude above,
  `snap.a@active` sending `votesnaphash` is silent, but the same action co-signed by `prod.a@active` is reported, and
  so is `snap.a@owner` sending it.
- **An explicit payer counts only alone.** An `<actor>@sysio.payer` entry beside a real authorization of the same
  actor only names who pays, so it is skipped and the real one is matched: a payer entry never reports an action an
  exclude has silenced. A payer entry with no such partner, which only a privileged contract's inline action can
  carry, is matched like any other authorization.
- **Inline actions count**, with the authorization they were sent with. A privileged contract such as `sysio.wrap` or
  `sysio.msig` executing an action as a watched account is reported, although that account signed nothing.
- **Notifications are not checked against authorizations.** Each action is checked once against the include rules, at
  its own execution, never again for each account it notifies.
- **Implicit transactions are never reported.** `onblock` is not signed by anyone; a `sysio,*::*` rule does not fire
  on every block.

### Receiver rules

A receiver rule is `<receiver>,<contract>::<action>`, each field an account name or `*`. It matches every execution of
an action at the receiver: the contract's own execution when the receiver is the contract, and every notification
the action sends the receiver, such as a token transfer the receiver sends or receives.

| Rule | Matches |
|---|---|
| `alice,sysio.token::transfer` | every sysio.token transfer that notifies alice, sent or received |
| `alice,*::*` | everything alice's contract executes, and everything alice is notified of |
| `mycontract,mycontract::*` | every call to mycontract's own actions |

- **Anyone can trigger one.** Notifications need no authorization from the receiver, so receiver rules suit an account
  holder watching activity, not key-use monitoring.
- **Excludes do not apply.** Narrow a receiver rule by its contract and action instead.
- **One record per execution.** A notification is a separate execution with its own `action_ordinal`, so an action
  matching an include rule at its execution and a receiver rule at a notification is reported twice, once at each.

An invalid rule fails startup with `plugin_config_exception` naming the option, the value's 0-based position among
the option's values (command-line values first), the value, and what is wrong with it.

## When records are written

Matches are found as transactions execute and held until the block containing them is accepted, then reported in
block order. Transactions that fail, are only executed speculatively, or never make it into a block are not reported.

- **The read mode sets the latency.** In head mode a block is accepted at head, so a record follows its block within
  the block interval; in irreversible mode blocks are accepted once irreversible. Each match record carries `lib_num`,
  the last irreversible block when it was written.
- **Forks.** A fork switch applies the winning branch's blocks, so an action in both branches is reported once per
  block it appears in, with a different `block_id`. Deduplicate on `trx_id`, `action_ordinal`, `contract` and
  `action`; `global_sequence` can differ between branches, and so can an inline action whose contract decides it from
  state. An action only in the losing branch was still signed by its key and is not retracted.
- **Restarts.** The plugin subscribes after `chain_plugin` has replayed its block log, so a restart does not report
  block-log blocks again. Reversible blocks the fork database still holds, such as after `--replay-blockchain`, are
  applied after startup and reported again. Blocks synced from peers after a restart are reported, however old;
  compare `block_time` with the wall clock to tell late records from live ones.
- **Several relays** watching the same accounts each report every match. Deduplicate across them on the same key.
- **Run it on relays.** Records are written as the block is committed, before a finalizer votes on it, so on a
  finalizer decoding would delay the vote.

## Records

Every record is one line of JSON with an `event` member, and the `account_watch` logger carries nothing else; decode
failures are logged at `trace` on the default logger. Matches are logged at `warn`, everything else at `info`.

| `event` | Members |
|---|---|
| `start` | `include`, `exclude`, `receiver` (each rule spelled out, e.g. `prod.a@*,*::*`), `max_data_bytes`, `heartbeat_seconds` |
| `match` | see below |
| `heartbeat` | `block_num`, `block_id`, `block_time` of the accepted block that carried it, `lib_num`, `matches` (match records since startup) |
| `stop` | `matches` |

A match record:

```json
{"event":"match","block_num":198,"block_id":"000000c6...","block_time":"2026-11-12T02:03:11.000",
 "producer":"defproducerh","lib_num":195,"trx_id":"0e118dc6...","action_ordinal":1,"global_sequence":"403",
 "contract":"sysio.token","action":"transfer","receiver":"sysio.token",
 "authorization":[{"actor":"alice","permission":"active"}],
 "matched":[{"actor":"alice","permission":"active"}],"data_size":45,
 "data":{"from":"alice","to":"bob","quantity":"0.1000 SYS","memo":"alice to bob"}}
```

| Member | Value |
|---|---|
| `block_num`, `block_id`, `block_time`, `producer` | The accepted block the action is in |
| `lib_num` | The last irreversible block when the record was written |
| `trx_id`, `action_ordinal` | The transaction and the action's position in its trace: the deduplication key |
| `global_sequence` | The action's global sequence, as a decimal string |
| `contract`, `action` | The action |
| `receiver` | The account it ran at: the contract for its own execution, the notified account for a notification |
| `authorization` | Every declared authorization, payer included |
| `matched` | The authorizations that matched an include rule; empty when only a receiver rule matched |
| `data_size` | Size of the action data in bytes |
| `data` | The data decoded with the contract's current ABI, when it is at most `max_data_bytes` |
| `data_hex` | The data as hex, when it is at most `max_data_bytes` but is not decoded: the contract has no ABI or none for the action, the decode fails, or the block's decode time is spent |
| `data_sha256` | The data's sha256, when it is larger than `max_data_bytes` |

Decoding runs on the application thread. All the matches of one block share one decode budget, chain_plugin's
`abi-serializer-max-time-ms`, ABI loads included; once it is spent, the block's remaining matches carry hex. A rule with
a wildcard contract decodes ABIs anyone can deploy, and an ABI can make a few bytes of data decode until the deadline,
so the budget, not the data size, bounds the cost per block. A failed decode is logged at `trace` on the default
logger.

A heartbeat is written on an accepted block once `heartbeat_seconds` have passed since the last one, the first on the
first accepted block. It stops when blocks stop being accepted, so a watchdog alerting on missing heartbeats also
covers a dead node, a stalled relay, or a logger routed nowhere.

## Logging configuration

The sample `logging.json`, `logging_default.json` and `logging_elasticsearch.json` under `programs/nodeop` route the
`account_watch` logger to `./logs/account_watch.jsonl` (and, in the Elasticsearch sample, to the es sink too). Without
an `account_watch` entry, the records go wherever the `default` logger goes. To give a watchdog a file of bare JSON
lines, add a sink whose pattern is the message alone:

```json
{
  "sinks": [{
    "name": "account_watch_file",
    "type": "rotating_file_sink",
    "args": { "base_filename": "./logs/account_watch.jsonl", "max_size": 10, "max_files": 10 },
    "format": { "type": "pattern", "args": { "pattern": "%v" } }
  }],
  "loggers": [{
    "name": "account_watch",
    "level": "info",
    "enabled": true,
    "sinks": ["account_watch_file"]
  }]
}
```

Merge both entries into the node's existing `logging.json`. Through the es sink, or any sink with the json format, a
record arrives as a string in the log document's message. `SIGHUP` re-binds the logger; the rules are never re-read.

## Tests

```bash
ninja -C build/debug test_account_watch_plugin
./build/debug/plugins/account_watch_plugin/test/test_account_watch_plugin
cd build/debug && python3 tests/account_watch_plugin_test.py -v
```

The Boost suite covers rule parsing and every rejected form for both rule kinds, the include and exclude semantics
including the explicit payer, option parsing from the command line and `config.ini`, and, on a chain: the record's
members, receiver rules on notifications and on a contract's own execution, notifications under include rules,
executions dropped before a block, failed transactions, `onblock`, inline actions executed through `sysio.wrap`, data
that is decoded, hashed, or hex (no ABI, no action type, a failed decode, a spent budget), the heartbeat including a
wall clock stepped back, a Savanna fork switch that reports an action once per branch, and an irreversible-mode node.
The Python test runs the plugin on a relay with the `%v` file sink above and checks the file's start, match,
heartbeat and stop records.

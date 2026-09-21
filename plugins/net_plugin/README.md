# net_plugin

`net_plugin` is the node's peer-to-peer layer. It listens for inbound peers, dials the configured outbound
peers, runs the block synchronization state machine, and relays blocks, transactions, and finalizer votes
across the network. `nodeop` initializes it on every run alongside `resource_monitor_plugin`, `chain_plugin`,
and `producer_plugin`, so there is no `plugin =` line to add — a node with no listen endpoint and no peer
addresses still loads it. It requires `chain_plugin`, `producer_plugin`, and
`signature_provider_manager_plugin` through `APPBASE_PLUGIN_REQUIRES`, and registers no HTTP endpoints of its
own; [`net_api_plugin`](../net_api_plugin/README.md) publishes its peer management as `/v1/net/*`.

## How it works

```
plugin_initialize
  |  parse and de-duplicate p2p-listen-endpoint, pair p2p-server-address positionally
  |  build the sync manager   (sync-fetch-span, sync-peer-limit, min block distance)
  |  build the connections manager (max-clients, cleanup period, cleanup time budget,
  |                                 keepalive interval x2 as the heartbeat timeout)
  |  resolve the block-nack default against producer-name
  |  load supplied peers, auto-bp peers, bp gossip endpoints, peer auth keys
  |  p2p-accept-transactions -> chain_plugin::enable_accept_transactions()
  |
plugin_startup
  |  start the net thread pool (net-threads)
  |  substitute p2p-server-address, or this host's name, for a wildcard listen host
  |  connect controller signals: accepted_block_header, accepted_block,
  |                              irreversible_block, aggregated_vote, voted_block
  |  subscribe to the transaction_ack channel published by producer_plugin
  |  open the listeners, start the monitors and the keepalive ticker,
  |  connect the supplied peers
```

### Threads

One pool, sized by `net-threads` (default 4). Every connection runs on its own strand inside that pool;
connection callbacks assert they are on the right strand and quit the node if they are not, so a strand
violation is a hard failure rather than silent corruption.

### What it sends and receives

- **Blocks.** During sync the node fetches blocks in chunks of `sync-fetch-span` from up to `sync-peer-limit`
  peers. Outside sync, blocks are propagated as they are accepted. Block notices and block nacks let a peer
  say it already has a block instead of receiving it again; `p2p-disable-block-nack` turns that off, and it
  defaults to on for a producing node — when `producer-name` is configured and the option was not given
  explicitly, block notice and nack are disabled and the choice is logged, so a producer always exchanges full
  blocks.
- **Transactions.** Accepted from peers only when `p2p-accept-transactions` is true, which also enables
  transaction acceptance in `chain_plugin`. Relay decisions come from the `transaction_ack` channel that
  `producer_plugin` publishes. `p2p-dedup-cache-expire-time-sec` bounds how long a transaction is remembered
  for duplicate suppression — `chain_plugin`'s transaction retry interval must be at least twice this value.
- **Votes.** Finalizer votes are broadcast from the controller's `aggregated_vote` and `voted_block` signals.
  Vote handling is gated by `chain_plugin`'s `vote-threads`, read once at startup: at 0 an incoming vote is
  dropped and nothing is relayed. `vote-threads` has no option default — the pool size is resolved only when
  `producer-name` or `vote-threads` is present. On a node with `producer-name`, an unset value and an explicit
  0 are both replaced by 4; without `producer-name`, unset leaves the pool at 0, so a plain relay drops
  incoming votes unless `vote-threads` is set explicitly.

### Listen endpoints

`p2p-listen-endpoint` takes `host:port[:trx|:blk][:<rate-cap>]` and may be repeated; an empty value means the
node does not listen at all. The optional `trx` / `blk` marker tells peers to send only transactions or only
blocks. The optional rate cap limits per-connection block-sync bandwidth — a bare number is bytes per second,
and `B/s`, `KB/s`, `MB/s`, `GB/s`, `TB/s`, `KiB/s`, `MiB/s`, `GiB/s`, `TiB/s` suffixes are accepted. Total
allowed bandwidth is the cap times the connection limit; transactions and blocks outside sync mode are not
throttled.

Duplicate listen endpoints are removed and the removal is logged. `p2p-server-address` supplies the
externally reachable address advertised in handshakes and is paired **positionally** with
`p2p-listen-endpoint`, so order matters and it may not be given more times than there are listen endpoints.
When a listen host is a wildcard (`0.0.0.0` or `[::]`) and no server address is given, the host's own name is
advertised instead. Which of the paired addresses a handshake carries depends on the direction: an outbound
connection advertises the first address, while an inbound connection advertises the address paired with the
listen endpoint it arrived on.

### Peer discovery and authentication

Two mechanisms add peers beyond the static `p2p-peer-address` list:

- **`p2p-auto-bp-peer`** — `bp_account,host:port[:trx|:blk]`. The node connects to that producer's endpoint
  automatically whenever the account is in the producer schedule. These entries are not gossiped.
- **`p2p-bp-gossip-endpoint`** — `bp_account,inbound_endpoint,outbound_ip_address`. The producer's peer key is
  read from the on-chain `peerkeys` table, registered there with the `regpeerkey` action, and the private half
  must be available as a `signature-provider` so this node can sign the gossip message it publishes. The
  startup check only asserts that some `wire` signature provider is registered, and `chain_plugin` registers a
  default `wire` provider when none is configured, so it does not catch a missing gossip key. That failure
  appears at runtime instead. The gossip key has to be reachable through `producer_plugin`'s signature-provider
  set, which is populated only when `producer-name` is configured; when it is missing there, signing the gossip
  message throws `producer_priv_key_not_found` — `Local producer has no private key in config.ini corresponding
  to public key "<key>"` — which surfaces as `Unable to update bp producer peers, error: ...` on `p2p_log`, and
  the endpoints are never advertised. The inbound endpoint is normally this node's listen endpoint, and the
  outbound IP address is what peers use to allow this node through a firewall.

Which peers may connect is a separate question, answered by the connection allow-list: `allowed-connection`
plus `peer-key` / `peer-private-key`. Repeated values are combined; `none` resets the policy to accept nobody,
but only what precedes it, so a value listed after `none` is still combined in. `any` on its own accepts every
peer without authentication. `producers` and `specified` both turn authentication on — including when either
is combined with `any` — and they share one authorization check, which accepts a key that is listed in
`peer-key`, is the public half of a `peer-private-key` pair, or
is one of this node's own producer signing keys. Those producer keys are loaded only when `producer-name` is
configured, so on a non-producing node `producers` admits nothing by itself. `specified` additionally requires
at least one `peer-key`, or startup fails.

`p2p-max-nodes-per-host` caps how many client nodes may connect from a single /24 (IPv4) or /48 (IPv6) subnet,
and a connection over that cap is closed outright. `max-clients` caps accepted inbound connections overall,
with 0 meaning no limit. Reaching that limit does not simply refuse the new peer: the node looks for the
lowest-scoring inbound non-BP connection that is below the eviction threshold, and if it finds one it closes
that peer and accepts the new connection, logging `Evicting low-score peer <id> (score <n>)`. Only when no
peer is evictable is the new socket closed, with `max_client_count <n> exceeded, no evictable peer, closing`.
With BP peering enabled the accept-time check is skipped and the limit is enforced after the first handshake
instead — a peer's BP status is not known before then — and it counts only inbound non-BP connections.

## Enabling / configuration

`net_plugin` is always initialized, so `config.ini` carries only its settings. A typical node:

```ini
p2p-listen-endpoint = 0.0.0.0:9876
p2p-server-address = <your-host>:9876
p2p-peer-address = peer1.<your-host>:9876
p2p-peer-address = peer2.<your-host>:9876

agent-name = <your-host>
max-clients = 25
net-threads = 4
sync-fetch-span = 1000
sync-peer-limit = 3
p2p-accept-transactions = true
```

A blocks-only relay with a per-connection sync rate cap, and no inbound transactions:

```ini
p2p-listen-endpoint = 0.0.0.0:9876:blk:10MiB/s
p2p-accept-transactions = false
max-clients = 200
```

A producer connecting automatically to its peers, and gossiping its own endpoint. `p2p-auto-bp-peer` names
another producer's account and endpoint — naming this node's own account and host makes it dial itself, and
the handshake closes the connection as a self-connection:

```ini
p2p-listen-endpoint = 0.0.0.0:9876
p2p-auto-bp-peer = <peer-account>,p2p.<peer-host>:9876
p2p-bp-gossip-endpoint = <your-account>,<your-host>:9876,198.51.100.1
```

The equivalent command line:

```bash
nodeop --p2p-listen-endpoint 0.0.0.0:9876 \
       --p2p-server-address <your-host>:9876 \
       --p2p-peer-address peer1.<your-host>:9876 \
       --p2p-peer-address peer2.<your-host>:9876 \
       --agent-name <your-host> \
       --max-clients 25 \
       --net-threads 4
```

Runtime peer changes go through `net_api_plugin`, not a restart:

```bash
nodeop --plugin sysio::net_api_plugin
curl -X POST http://127.0.0.1:8888/v1/net/connections -d '{}'
curl -X POST http://127.0.0.1:8888/v1/net/connect     -d '"peer3.<your-host>:9876"'
```

## Options

Every `net_plugin` option is registered as a config-file option, so each is equally valid in `config.ini` and
as a `nodeop --<name>` argument.

### Listening and identity

| Option | Default | Meaning |
|---|---|---|
| `p2p-listen-endpoint` | `0.0.0.0:9876:0` | `host:port[:trx\|:blk][:<rate-cap>]` to listen on for incoming p2p connections; may be repeated. An empty value disables listening. `trx` / `blk` tells peers to send only transactions or only blocks. The rate cap limits per-connection block-sync bandwidth; a bare number is bytes per second, and `B/s`, `KB/s`, `MB/s`, `GB/s`, `TB/s`, `KiB/s`, `MiB/s`, `GiB/s`, `TiB/s` suffixes are accepted. |
| `p2p-server-address` | derived from `p2p-listen-endpoint` | Externally accessible `host:port` identifying this node. May be given as many times as `p2p-listen-endpoint` and is paired with it positionally. An outbound connection advertises the first address; an inbound connection advertises the address paired with the listen endpoint it arrived on. |
| `agent-name` | `Wire Agent` | Name supplied to identify this node to peers. |
| `max-clients` | `25` | Maximum number of clients whose connections are accepted; 0 for no limit. |
| `p2p-max-nodes-per-host` | `1` | Maximum number of client nodes from any single /24 (IPv4) or /48 (IPv6) subnet. |

### Peers

| Option | Default | Meaning |
|---|---|---|
| `p2p-peer-address` | unset | `host:port[:trx\|:blk]` of a peer to connect to; may be repeated to compose a network. |
| `p2p-auto-bp-peer` | unset | `bp_account,host:port[:trx\|:blk]` — connect to this block producer's node automatically while the account is in the producer schedule. Not gossiped. May be repeated. |
| `p2p-bp-gossip-endpoint` | unset | `bp_account,inbound_endpoint,outbound_ip_address`. The peer key is read from the on-chain `peerkeys` table registered via the `regpeerkey` action; its private key must be configured as a `signature-provider`. The inbound endpoint is typically this node's listen endpoint; the outbound IP address is what peers use to allow this node through a firewall. May be repeated. |
| `connection-cleanup-period` | `30` | Seconds to wait before cleaning up dead connections. |
| `max-cleanup-time-msec` | `10` | Maximum connection cleanup time per cleanup call, in milliseconds. |
| `p2p-keepalive-interval-ms` | `10000` | Peer heartbeat keepalive message interval, in milliseconds. Must be greater than 0. Twice this value is the connection heartbeat timeout. |

### Authentication

| Option | Default | Meaning |
|---|---|---|
| `allowed-connection` | `any` | `any`, `producers`, `specified`, or `none`; may be repeated, and the values are combined. `any` alone accepts every peer without authentication; combining it with `producers` or `specified` still requires authentication. `producers` and `specified` share one check that accepts `peer-key` values, `peer-private-key` public halves, and this node's own producer signing keys (present only with `producer-name`). `specified` requires at least one `peer-key`; `producers` does not. `none` resets the accumulated policy to accept nobody, but only what precedes it — a value listed after `none` is still combined in. |
| `peer-key` | unset | Public key of a peer allowed to connect; may be repeated. |
| `peer-private-key` | unset | `[PublicKey, WIF private key]` tuple; may be repeated. |

### Synchronization and relay

| Option | Default | Meaning |
|---|---|---|
| `sync-fetch-span` | `1000` | Number of blocks to retrieve in one chunk from any individual peer during synchronization. |
| `sync-peer-limit` | `3` | Number of peers to sync from. |
| `p2p-accept-transactions` | `true` | Allow transactions received over the p2p network to be evaluated and relayed if valid. |
| `p2p-disable-block-nack` | `false` | Disable block notice and block nack, so every received block is broadcast to all peers unless already received. Defaults to true when `producer-name` is configured, so a producing node always exchanges full blocks. |
| `p2p-dedup-cache-expire-time-sec` | `10` | Maximum time a transaction is tracked for duplicate-suppression. |
| `net-threads` | `4` | Number of worker threads in the `net_plugin` thread pool. Must be greater than 0. |

### Logging

| Option | Default | Meaning |
|---|---|---|
| `peer-log-format` | `["${_peer} - ${_sid}" - ${_cid} ${_ip}:${_port}] ` | Format string prefixed to messages about a peer. Variables: `_peer` endpoint name, `_name` self-reported name, `_cid` assigned connection id, `_id` self-reported 64-hex-character id, `_sid` its short id — 16 hex characters read from byte 4 of `_id`, i.e. characters 9-24, `_ip` and `_port` of the peer, `_lip` and `_lport` of the local side, `_agent` first 15 characters of the peer's agent name, `_nver` p2p protocol version. |

## HTTP API

`net_plugin` registers no HTTP handlers. [`net_api_plugin`](../net_api_plugin/README.md) publishes
`connections`, `status`, and `bp_gossip_peers` under `net_ro`, and `connect` and `disconnect` under `net_rw`.

## Diagnostics

Six `net_plugin` loggers plus the chain library's `vote` logger, all re-read on `SIGHUP`. `net_plugin_impl`
is the parent of the `net_plugin` set: the five `p2p_*` loggers inherit its configuration unless configured
individually, which is what makes it practical to raise one traffic class to debug without drowning in the
rest. `vote` is declared in the chain library and inherits nothing from `net_plugin_impl`, so raising the
`p2p_*` levels does not surface vote traffic.

| Logger | Carries |
|---|---|
| `net_plugin_impl` | Parent logger; the default for the five below. |
| `p2p_log` | General p2p activity. |
| `p2p_connection` | Connection lifecycle — accept, dial, handshake, close, cleanup. |
| `p2p_block` | Block propagation, notices, and nacks. |
| `p2p_trx` | Transaction propagation. |
| `p2p_message` | Individual protocol messages. |
| `vote` | Finalizer vote traffic: votes received and sent, duplicate and ID-mismatch drops, rebroadcast decisions, and the disconnect taken on an invalid vote. Declared by the chain library, shared with the vote processor. |

Every per-peer line is prefixed with `peer-log-format` rendered for that connection, so a connection id and
remote address are attached to the message.

Lines an operator should recognize:

- `my node_id is <id>` at startup, on `p2p_connection`.
- A boxed notice when `p2p-accept-transactions = false` and the node is listening:

  ```
  ***********************************
  * p2p-accept-transactions = false *
  *    Transactions not forwarded   *
  ***********************************
  ```

- `block notice and block nack disabled by default, this node is configured to produce blocks` — the
  producer-node default described above took effect.
- `Removed <n> duplicate p2p-listen-endpoint entries`.
- `Exception in net thread, exiting` — quits the node. So does `wrong strand: <func> : line <n>, exiting`,
  which means a connection callback ran off its strand.
- Startup configuration failures name the option: an over-long or malformed `p2p-listen-endpoint`,
  `p2p-server-address`, or `p2p-peer-address` (`syntax host:port:[trx|blk]`), more `p2p-server-address` values
  than listen endpoints, `net-threads` or the keepalive interval not greater than 0, an over-long
  `agent-name`, and `At least one peer-key must accompany 'allowed-connection=specified'`.
- `Unable to update bp producer peers, error: ...` on `p2p_log` — a configured `p2p-bp-gossip-endpoint` could
  not be published because signing its gossip message failed. The gossip key must be reachable through
  `producer_plugin`'s signature-provider set, which is populated only when `producer-name` is configured;
  otherwise signing throws `producer_priv_key_not_found` — `Local producer has no private key in config.ini
  corresponding to public key "<key>"`. A configured BP with no `peerkeys` row is a separate, non-throwing
  case: it logs `On-chain peer-key not found for configured BP <account>` and stops connecting to gossip
  peers, so it never reaches this line.
- Shutdown: `shutdown..` and `exit shutdown` at debug level on `p2p_log`.

## Tests

```bash
ninja -C build/debug test_net_plugin
./build/debug/plugins/net_plugin/test/test_net_plugin
```

The suite covers auto-BP peering, the block-nack producer default, block notice handling, connection type
parsing (`trx` / `blk`), the local transaction cache, listen-endpoint rate-limit parsing, protocol message
wire encoding, peer authentication, the queued send buffer, and /24 and /48 subnet masking.

## Related plugins

- [`net_api_plugin`](../net_api_plugin/README.md) — publishes this plugin's peer management as `/v1/net/*`.
- [`chain_plugin`](../chain_plugin/README.md) — required dependency; supplies the controller signals this
  plugin relays, and the `vote-threads` setting that gates vote propagation.
- [`producer_plugin`](../producer_plugin/README.md) — required dependency; publishes the `transaction_ack`
  channel this plugin subscribes to, and answers the producer-key check used by peer authentication.
- `signature_provider_manager_plugin` — required dependency; holds the `wire` key used to sign BP gossip
  messages. `chain_plugin` registers a default `wire` provider when none is configured, so the presence check
  at startup does not prove the configured gossip key is available.

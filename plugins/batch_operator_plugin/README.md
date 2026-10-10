# batch_operator_plugin

Cranks Depot and Outpost contracts, ferrying OPP message chains between the WIRE chain and external blockchains (Ethereum, Solana).

## Overview

Every batch operator runs this plugin. `sysio.epoch` keeps a sliding window of
`batch_op_groups` groups of `operators_per_epoch` operators (default 3 groups of 7, epochs
of 6 minutes); the group at the front of the window is elected for the current epoch and
its members execute the epoch cycle. Each poll the plugin also reads its own
`sysio.opreg` status and stops relaying once the operator is `SLASHED` or `TERMINATED`.

## Epoch Cycle

Run by every member of the elected group, for every active outpost chain:

**Outbound (WIRE → Outposts):**
1. Read the outpost's outbound envelope that `sysio.epoch::advance` built
   (`sysio.msgch::buildenv`) from the depot's `outenvelopes` table
2. Deliver it to the Outpost contract, chunked (ETH: `OPPInbound.epochIn()`, SOL: `epoch_in`)
3. The outpost reaches consensus once enough group members have delivered identical bytes

**Inbound (Outposts → WIRE):**
1. The consensus-reaching delivery emits the outpost's outbound envelope
2. Read the latest outbound envelope from Outpost storage
3. Deliver its raw protobuf bytes to Depot (`sysio.msgch::deliver`)
4. Depot evaluates consensus across the group's deliveries

## Depot cranks

Every epoch poll (`--batch-epoch-poll-ms`) also pushes the depot actions nothing on
chain schedules. `sysio.msgch::chkcons` — which advances the epoch once every active
outpost has consensus and the boundary has passed — comes only from elected operators;
the rest come from every ACTIVE operator, because the elected one may be the operator
that is offline, and each is a cheap no-op once its work is done:

| Action | When | Why nothing else drives it |
|--------|------|----------------------------|
| `sysio.chalg::chkdispute(dispute_id)` | every OPEN envelope dispute | a dispute pauses `sysio.epoch::advance`, so no inline poke can reach it |
| `sysio.swap::tickyield(pair_token)` | every yield pool whose reservoir has a queued balance, at most once per `--batch-yield-tick-interval-ms` per pool from this operator | the reservoir is sold into the pool as time passes; only a tick moves the clock |
| `sysio.liq::queueyield(sym)` | every shadow with yield pending from an outpost `LIQ_YIELD` report | the report lands in `sysio.liq`'s pending balance; queuing it into the swap is a separate, permissionless step |
| `sysio.kicker::kick(sym)` | every kicker pool whose `kickcfg.min_interval_sec` has elapsed since its `last_kick`, at most once per `--batch-kick-interval-ms` per pool from this operator | the gift accrues with time and only a kick pays it; its cadence is a security control (`contracts/sysio.system/EMISSIONS.md`) |

The two yield cranks stay idle, without logging a read failure per poll, until both
`sysio.swap` and `sysio.liq` are deployed on the depot; the kick crank likewise until
`sysio.kicker` is deployed, and it pushes nothing before `setconfig`. Between payments
the kick crank reads the pools and pushes nothing, because `kick` would return at its
own clock check. A kick that passes the clock but cannot pay — an Andon hold, a gift
below `min_gift`, an exhausted budget — leaves `last_kick` where it was, so
`--batch-kick-interval-ms` is what bounds the retries; it is spent on every attempt,
failed or not. `--batch-kick-crank=false` turns the kick crank off on this node. The
plugin does not crank `sysio.synd::crank`; syndication release is driven by keepers.

## Configuration

| Option | Default | Description |
|--------|---------|-------------|
| `--batch-operator-account` | — | WIRE account name for this operator. Configuring it enables the relay |
| `--batch-epoch-poll-ms` | 15000 | How often to check epoch state (ms) |
| `--batch-delivery-timeout-ms` | 15000 | Max time to wait for chain delivery confirmation (ms) |
| `--batch-yield-tick-interval-ms` | 60000 | Minimum spacing between this operator's `sysio.swap::tickyield` pushes per yield pool (ms) |
| `--batch-kick-crank` | true | Push `sysio.kicker::kick` for every kicker pool whose minimum interval has elapsed |
| `--batch-kick-interval-ms` | 60000 | Minimum spacing between this operator's `sysio.kicker::kick` pushes per LIQ token (ms) |

There is no separate enable flag: the relay runs when `--batch-operator-account`
is configured, the way `producer_plugin` keys off `--producer-name`. The plugin
must also be listed under `plugin =` (or pulled in as a dependency by
`external_debugging_plugin`), and requires `read-mode = irreversible`.

### Outpost wiring

Nothing about an outpost is declared per node.

* **Which chains** — every active non-depot `sysio.chains` row.
* **Where each one lives** — the row's own `outpost` struct (`opp_addr` /
  `opp_inbound_addr`), so every operator relays a chain through the same
  deployment.
* **How to reach it** — the RPC client registered under that chain's **own
  code**. `--outpost-ethereum-client` / `--outpost-solana-client` take the
  client id as their first field, and for an outpost that id must be the chain
  code (`ETHEREUM`, `SOLANA`, ...). The Ethereum client's verified `eth_chainId`
  is additionally asserted against the row's `external_chain_id`, so a client
  registered under the wrong code is rejected rather than relayed through.

An elected group must deliver on **every** active chain, so a missing RPC client
is fatal: the node logs the chains it cannot serve and shuts down. The check runs
after the sync gate, where `sysio.chains` is readable. Missing contract
*addresses* are not fatal — they are governance state, fixable with
`sysio.chains::setoutpost` without touching a node — so such a chain is skipped
fail-closed and picked up on a later tick. A `setoutpost` redeploy is likewise
picked up on the next epoch tick: the relay job is rebuilt against the new
address rather than left pointing at the old one.

## Dependencies

- `chain_plugin` — blockchain state access
- `cron_plugin` — irreversible block event subscription
- `signature_provider_manager_plugin` — signing key management
- `outpost_ethereum_client_plugin` — Ethereum outpost relay
- `outpost_solana_client_plugin` — Solana outpost relay

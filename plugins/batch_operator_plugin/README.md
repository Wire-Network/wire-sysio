# batch_operator_plugin

Cranks Depot and Outpost contracts, ferrying OPP message chains between the WIRE chain and external blockchains (Ethereum, Solana).

## Overview

All 21 batch operators run this plugin in perpetuity. The epoch scheduler (`sysio.epoch`) assigns operators into 3 fixed groups of 7. Each epoch (every 6 minutes), one group is elected; those 7 execute the full epoch cycle.

## Epoch Cycle

**Phase 1 — Outbound (WIRE → Outposts):**
1. Crank Depot (`sysio.msgch::crank`) to produce outbound OPP Message Chain
2. Read the produced chain from Depot tables
3. Deliver chain to Outpost contract (ETH: `OPPInbound.epochIn()`, SOL: `epoch_in()`)
4. All 7 independently verify the delivered chain

**Phase 2 — Inbound (Outposts → WIRE):**
1. The consensus-reaching delivery emits the outpost's outbound envelope
2. Read the latest outbound envelope from Outpost storage
3. Deliver its raw protobuf bytes to Depot (`sysio.msgch::deliver`)
4. Depot evaluates consensus across all 7 deliveries

## Configuration

| Option | Default | Description |
|--------|---------|-------------|
| `--batch-operator-account` | — | WIRE account name for this operator. Configuring it enables the relay |
| `--batch-epoch-poll-ms` | 15000 | How often to check epoch state (ms) |
| `--batch-delivery-timeout-ms` | 15000 | Max time to wait for chain delivery confirmation (ms) |

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
- `outpost_ethereum_client_plugin` — ETH RPC calls (future)
- `outpost_solana_client_plugin` — SOL RPC calls (future)

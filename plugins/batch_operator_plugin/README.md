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
| `--batch-sol-client-id` | `sol-default` | Solana outpost client ID (RPC connection) for SVM outpost rows |
| `--batch-outpost` | — | Remote OPP contract binding for one active `sysio.chains` row, repeatable once per chain code. Spec: `CHAIN_CODE,opp_addr[,opp_inbound_addr]` |

There is no separate enable flag: the relay runs when `--batch-operator-account`
is configured, the way `producer_plugin` keys off `--producer-name`. The plugin
must also be listed under `plugin =` (or pulled in as a dependency by
`external_debugging_plugin`), and requires `read-mode = irreversible`.

## Dependencies

- `chain_plugin` — blockchain state access
- `cron_plugin` — irreversible block event subscription
- `signature_provider_manager_plugin` — signing key management
- `outpost_ethereum_client_plugin` — ETH RPC calls (future)
- `outpost_solana_client_plugin` — SOL RPC calls (future)

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
1. Crank Outpost to finalize epoch (`OPP.finalizeEpoch()` / `finalize_epoch`)
2. Read inbound chain from Outpost (ETH: event logs, SOL: transaction logs)
3. Deliver to Depot (`sysio.msgch::deliver`) with chain hash
4. Depot evaluates consensus across all 7 deliveries

## Configuration

| Option | Default | Description |
|--------|---------|-------------|
| `--batch-operator-account` | — | WIRE account name for this operator |
| `--batch-epoch-poll-ms` | 5000 | How often to check epoch state (ms) |
| `--batch-outpost-poll-ms` | 3000 | How often to poll outpost for new messages (ms) |
| `--batch-delivery-timeout-ms` | 30000 | Max time to wait for chain delivery confirmation (ms) |
| `--batch-enabled` | false | Enable batch operator functionality |

## Dependencies

- `chain_plugin` — blockchain state access
- `cron_plugin` — irreversible block event subscription
- `signature_provider_manager_plugin` — signing key management
- `outpost_ethereum_client_plugin` — ETH RPC calls (future)
- `outpost_solana_client_plugin` — SOL RPC calls (future)

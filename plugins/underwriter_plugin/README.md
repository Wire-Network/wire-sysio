# underwriter_plugin

Autonomous underwriting daemon that reads pending swap messages and commits collateral to underwrite cross-chain transfers.

## Overview

The underwriter is a **separate daemon** from the batch operator. It does NOT crank any contracts. Instead it:

1. **Scans** PENDING messages from `sysio.msgch` for `ATTESTATION_TYPE_SWAP` entries
2. **Verifies** deposits independently on external chains (ETH/SOL) as a double-check
3. **Selects** swaps that maximize utilization of deposited collateral (greedy knapsack)
4. **Submits** underwriting intent to `sysio.uwrit`
5. **Monitors** for dual-outpost confirmation (both source and target must confirm)

## Configuration

| Option | Default | Description |
|--------|---------|-------------|
| `--underwriter-account` | — | WIRE account name for this underwriter |
| `--underwriter-scan-interval-ms` | 5000 | How often to scan for pending messages (ms) |
| `--underwriter-verify-timeout-ms` | 10000 | Timeout for external chain verification (ms) |
| `--underwriter-enabled` | false | Enable underwriter functionality |

## Underwriting Flow

1. Read PENDING swap messages from Depot
2. For each candidate, verify the deposit on the source chain (e.g., confirm 50 ETH was received)
3. Select swaps to maximize fee income within available collateral
4. Submit intent to `sysio.uwrit::submituw` — commits funds on BOTH source and target chains
5. Wait for BOTH outposts to confirm (via inbound message chains)
6. On dual confirmation: swap is scheduled for remit
7. Committed funds subject to 24hr challenge window hold

## Dependencies

- `chain_plugin` — blockchain state access
- `cron_plugin` — irreversible block event subscription
- `signature_provider_manager_plugin` — signing key management
- `outpost_ethereum_client_plugin` — ETH RPC for deposit verification (future)
- `outpost_solana_client_plugin` — SOL RPC for deposit verification (future)

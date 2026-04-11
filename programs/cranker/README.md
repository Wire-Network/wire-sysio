# Cranker

`cranker` is a lightweight standalone executable that periodically fetches Ethereum beacon chain state from [beaconcha.in](https://beaconcha.in) and pushes updates into on-chain smart contracts. It runs the minimum set of plugins needed for this purpose — no full Wire node is required.

## What it does

On each scheduled interval, `cranker`:

1. **Updates withdrawal delay** — Fetches the `exit_queue.estimated_processed_at` epoch from the beaconcha.in queues API and calls `WithdrawalQueue.setWithdrawDelay` with the queue length in seconds (plus a 9-day base floor).
2. **Updates entry queue** — Fetches the `deposit_queue.estimated_processed_at` epoch and calls `DepositManager.setEntryQueue` with the queue length in days (default: 1 day if the queue timestamp is in the past).
3. **Updates APY** — Fetches the `avgapr7d` field from the beaconcha.in ethstore API and calls `DepositManager.updateApyBPS` with the value expressed in basis points.
4. **Finalizes epochs** — Calls `OPP.finalizeEpoch` on a separate configurable interval.

Each on-chain call is submitted via the `outpost_ethereum_client_plugin` and awaits block confirmation (up to 10 minutes) before proceeding to the next step.

## Minimum required configuration

1. At least one Ethereum **signature provider** (`--signature-provider`)
2. At least one Ethereum **outpost client** (`--outpost-ethereum-client`)
3. At least one **ABI file** (`--ethereum-abi-file`) containing the relevant contract definitions
4. A **contract addresses file** (`--beacon-chain-contracts-addrs`) mapping contract names to addresses
5. A **beacon chain API key** (`--beacon-chain-api-key`) if queue/APY updates are enabled

## Configuration options

### Signature provider (`--signature-provider`)

Registers an Ethereum signing key. Format: `<name>,<chain-kind>,<key-type>,<public-key>,<private-key-spec>`

| Field | Description |
|---|---|
| `name` | Reference name for this provider (e.g. `eth-01`) |
| `chain-kind` | Chain kind: `ethereum` |
| `key-type` | Key format: `ethereum` |
| `public-key` | Hex-encoded public key |
| `private-key-spec` | Private key specifier, e.g. `KEY:0x<hex>` |

### Outpost Ethereum client (`--outpost-ethereum-client`)

Configures the connection to an Ethereum JSON-RPC node. Format: `<client-id>,<sig-provider-id>,<rpc-url>[,<chain-id>]`

| Field | Description |
|---|---|
| `client-id` | Unique name for this client |
| `sig-provider-id` | Name of the signature provider to use (must match a `--signature-provider` name) |
| `rpc-url` | Ethereum JSON-RPC endpoint URL |
| `chain-id` | (Optional) Ethereum chain ID; omit to let the client query it |

### Ethereum ABI file (`--ethereum-abi-file`)

Path to a JSON file containing an array of ABI-compliant contract definitions. The file must include a `contractName` field for each contract so that `cranker` can match entries against the expected contract names (`OPP`, `DepositManager`, `WithdrawalQueue`). Can be specified multiple times.

### Contract addresses (`--beacon-chain-contracts-addrs`)

Path to a JSON file mapping contract names to their deployed addresses. Example:

```json
{
  "OPP": "0x1234...",
  "DepositManager": "0xabcd...",
  "WithdrawalQueue": "0xef01..."
}
```

Can be specified multiple times. Contracts whose names are absent are silently skipped — only the contracts whose addresses are provided will be driven.

### Interval schedules (`--beacon-chain-interval`)

Defines named cron schedules. Format: `<name>,<cron-expression>`

The cron expression supports the standard 5-field format (`minute hour day-of-month month day-of-week`) and an extended 6-field format with a leading millisecond field. Common examples:

| Expression | Meaning |
|---|---|
| `* * * * *` | Every minute |
| `0 * * * *` | Every hour |
| `0 */6 * * *` | Every 6 hours |
| `0 0 * * *` | Daily at midnight |

If no `--beacon-chain-interval` is provided, a single default interval named `default` is created with a schedule of `* */1 * * *` (every hour).

A built-in `once` interval is always available — it runs immediately on startup and does not repeat. This is the default for both `--beacon-chain-update-interval` and `--beacon-chain-finalize-epoch-interval` if not overridden.

**Reserved name:** `once` cannot be used as a custom interval name.

### Beacon chain API key (`--beacon-chain-api-key`)

Bearer token for authenticating with the beaconcha.in API. **Required** to enable queue length and APY updates. When this option is absent, only epoch finalization runs (if configured).

### Queue/APY update interval (`--beacon-chain-update-interval`)

Name of the interval (defined via `--beacon-chain-interval`) on which to run the queue and APY update. Defaults to `once` (runs immediately on startup, does not repeat).

### Finalize epoch interval (`--beacon-chain-finalize-epoch-interval`)

Name of the interval on which to call `OPP.finalizeEpoch`. Defaults to `once`. Has no effect if no `OPP` contract address is configured.

### Beacon chain endpoint URLs (optional overrides)

| Option | Default |
|---|---|
| `--beacon-chain-queue-url` | `https://beaconcha.in/api/v2/ethereum/queues` |
| `--beacon-chain-apy-url` | `https://beaconcha.in/api/v1/ethstore/latest` |

## Example

```shell
cranker \
  --signature-provider eth-signer,ethereum,ethereum,0x<pubkey>,KEY:0x<privkey> \
  --outpost-ethereum-client mainnet,eth-signer,https://eth-rpc.example.com,1 \
  --ethereum-abi-file /etc/cranker/abis.json \
  --beacon-chain-contracts-addrs /etc/cranker/addresses.json \
  --beacon-chain-api-key <beaconchain-api-key> \
  --beacon-chain-interval "hourly,0 * * * *" \
  --beacon-chain-update-interval hourly \
  --beacon-chain-finalize-epoch-interval hourly
```

This runs queue/APY updates and epoch finalization once per hour.

To run everything once immediately and exit:

```shell
cranker \
  --signature-provider eth-signer,ethereum,ethereum,0x<pubkey>,KEY:0x<privkey> \
  --outpost-ethereum-client mainnet,eth-signer,https://eth-rpc.example.com,1 \
  --ethereum-abi-file /etc/cranker/abis.json \
  --beacon-chain-contracts-addrs /etc/cranker/addresses.json \
  --beacon-chain-api-key <beaconchain-api-key>
```

Omitting `--beacon-chain-interval` uses the default `once` interval for both `--beacon-chain-update-interval` and `--beacon-chain-finalize-epoch-interval`.

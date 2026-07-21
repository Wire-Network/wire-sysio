# Outpost Clients & specifically Ethereum interoperability

> SOLANA client is similar to Ethereum in that both contract/program scaffolding will use the same approach and they both use the new `fc::network::json_rpc::json_rpc_client`

## Configuration

The preferred configuration keeps each RPC connection, replay-protection domain, and transaction policy in one
versioned JSON file. Signature providers remain separate because they may use `KEY`, wallet, or KMS backends:

```sh
--signature-provider "eth-01,ethereum,ethereum,0x8318535b54105d4a7aae60c08fc45f9687181b4fdfc625bd1a753fa7397fed753547f11ca8696646f2f3acb08e31016afac23e630c5d11f59f61fef57b0d2aa5,KEY:0xac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80"
--outpost-ethereum-client-config-file /etc/wire/ethereum-clients.json
```

The unified file can configure one or more EVM outposts:

```json
{
  "version": 1,
  "clients": [
    {
      "client_id": "eth-anvil-local",
      "signature_provider_id": "eth-01",
      "rpc_url": "http://localhost:8545",
      "chain_id": "31337",
      "transaction_policy": {
        "max_priority_fee_per_gas_wei": "2000000000",
        "max_fee_per_gas_wei": "100000000000",
        "max_gas_limit": "2000000",
        "max_total_native_cost_wei": "250000000000000000"
      }
    }
  ]
}
```

`transaction_policy` is optional. If omitted, all four caps are set to the maximum `uint256` value. This permissive
default preserves the behavior from before transaction policies were introduced; production operators should set
finite limits explicitly.

The chain id is a positive canonical decimal string in the external outpost domain `1..UINT32_MAX`. The four policy
limits are positive canonical decimal strings up to `uint256`; strings avoid JSON number precision loss. The loader
rejects unknown or duplicate fields, unsupported versions, invalid or zero limits, and duplicate client ids.

For backward compatibility, `--outpost-ethereum-client` remains available and cannot be combined with the unified
file:

```sh
--outpost-ethereum-client eth-anvil-local,eth-01,http://localhost:8545,31337
```

Legacy clients always receive the permissive maximum-value policy. A fourth chain-id field is preferred and remains
locally authoritative. If the original three-field form is used, startup obtains `eth_chainId` from the RPC endpoint
under one aggregate five-second deadline, so an unavailable endpoint fails startup instead of blocking it indefinitely.
The removed `--outpost-ethereum-transaction-policy-file` option is not supported.

Immediately before signing, the client requires:

- `maxPriorityFeePerGas <= max_priority_fee_per_gas_wei`
- `maxFeePerGas <= max_fee_per_gas_wei`
- `gasLimit <= max_gas_limit`
- `maxFeePerGas >= maxPriorityFeePerGas`
- `gasLimit * maxFeePerGas + value <= max_total_native_cost_wei`

All arithmetic is checked for `uint256` overflow. Limits are inclusive: a value equal to a cap is allowed and cap plus
one is rejected. Rejected transactions are not clamped, signed, or broadcast. In the unified and four-field legacy
modes, the configured `chain_id` is authoritative for signing and the RPC endpoint cannot select the
replay-protection domain.

Fee-cap sizing must account for the EIP-1559 derivation `maxFeePerGas = 2 * baseFeePerGas + maxPriorityFeePerGas`. Startup can validate the two configured caps, but it cannot choose a minimum base fee because base fees are dynamic and chain-specific. For an actual priority fee `p`, the largest base fee admitted by the policy is `floor((max_fee_per_gas_wei - p) / 2)`. Operators should size the two caps for the intended chain; insufficient headroom fails closed with reason code `max_fee_cap_exceeded` and field `max_fee_per_gas`. The diagnostic's observed value names the `2 * base_fee_per_gas + max_priority_fee_per_gas` operands; its allowed value is the configured cap.

The `eth-01` reference in the client configuration matches the Ethereum signature provider. A process needs more than
one `client_id` only when it serves multiple distinct EVM outposts or chains, or when callers explicitly select
different same-chain endpoints or signers. Multiple same-chain RPC endpoints are not automatic failover: chain-id
lookup becomes ambiguous and fails closed unless a caller selects a client id explicitly.

With the above configuration and the appropriate `app` and plugin config, you can access the Ethereum client configured
with name/id `eth-anvil-local` as follows:

```cpp
// GET `outpost_ethereum_client_plugin`
auto& eth_plug = app->get_plugin<sysio::outpost_ethereum_client_plugin>();

// GET THE CLIENT (REMEMBER `1..n` SUPPORT)
auto  client_entry = eth_plug.get_clients()[0];

// CLIENT IS A `std::shared_ptr<ethereum_client>`
auto& client       = client_entry->client;

// GET THE AUTHORITATIVE POLICY CHAIN ID, JUST AN EXAMPLE
// `chain_id` will have the type `fc::uint256`
auto chain_id = client->get_chain_id();
```

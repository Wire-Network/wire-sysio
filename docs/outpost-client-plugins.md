# Outpost Clients & specifically Ethereum interoperability

> SOLANA client is similar to Ethereum in that both contract/program scaffolding will use the same approach and they both use the new `fc::network::json_rpc::json_rpc_client`

## Configuration

The Ethereum client plugin is configured via program options as follows:

```sh
--signature-provider "eth-01,ethereum,ethereum,0x8318535b54105d4a7aae60c08fc45f9687181b4fdfc625bd1a753fa7397fed753547f11ca8696646f2f3acb08e31016afac23e630c5d11f59f61fef57b0d2aa5,KEY:0xac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80"
--outpost-ethereum-client eth-anvil-local,eth-01,http://localhost:8545,31337
```

HTTPS endpoints use system CA roots and mandatory DNS/IP identity verification. Private PKI can be added with
`--outpost-ethereum-additional-ca-file` or `--outpost-ethereum-additional-ca-path`; an explicit proxy can be set
with `--outpost-ethereum-proxy`. The equivalent Solana options are
`--outpost-solana-additional-ca-file`, `--outpost-solana-additional-ca-path`, and
`--outpost-solana-proxy`. See [Outbound HTTP transport](outbound-http-transport.md) for the complete security and
resource policy.

> NOTE:  If you look closely, the reference to `eth-01` in the Ethereum client config, matches the signature provider configured for `Ethereum`.  This mapping is what enables `1..n` clients in a single process

The signer reference is validated during startup. Each
`--outpost-ethereum-client` must reference the explicit, non-empty name of a
configured `--signature-provider`; anonymous signature-provider specs cannot be
referenced by an Ethereum client. When the optional Ethereum chain ID is
present, startup also calls `eth_chainId` on the configured RPC endpoint and
fails if the endpoint is unavailable, returns an invalid value, or reports a
different chain ID.

With the above configuration and the appropriate `app` & `plugin` config, you can access the `outpost-ethereum-client` configured with name/id == `eth-anvil-local` as follows

```cpp
// GET `outpost_ethereum_client_plugin`
auto& eth_plug = app->get_plugin<sysio::outpost_ethereum_client_plugin>();

// GET THE CLIENT (REMEMBER `1..n` SUPPORT)
auto  client_entry = eth_plug.get_clients()[0];

// CLIENT IS A `std::shared_ptr<ethereum_client>`
auto& client       = client_entry->client;

// GET CHAIN ID, JUST AN EXAMPLE
// `chain_id` will have the type `fc::uint256`
auto chain_id = client->get_chain_id();
```

# Outpost Clients & specifically Ethereum interoperability

> SOLANA client is similar to Ethereum in that both contract/program scaffolding will use the same approach and they both use the new `fc::network::json_rpc::json_rpc_client`

## Configuration

The preferred Ethereum client configuration is a versioned protobuf-JSON file:

```sh
--signature-provider "eth-01,ethereum,ethereum,0x8318535b54105d4a7aae60c08fc45f9687181b4fdfc625bd1a753fa7397fed753547f11ca8696646f2f3acb08e31016afac23e630c5d11f59f61fef57b0d2aa5,KEY:0xac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80"
--outpost-ethereum-client-config-file /etc/wire/ethereum-client-config.json
```

See [ethereum-client-config.example.json](ethereum-client-config.example.json). The root
`schema_version` must be `1`. Each `clients` entry has a signing `connection`, a positive 32-bit
`chain_id` using standard ProtoJSON numeric forms, and an optional complete `transaction_policy`
containing canonical decimal-string caps for priority fee, maximum fee, gas limit, and total native
cost. When `transaction_policy` is omitted or `null`, ProtoJSON leaves it unset and all four caps
default to `UINT256_MAX` for backward compatibility. This maximum default is
compatibility-only: it provides no finite economic boundary, is not a production recommendation,
and production operators must configure reviewed finite `transaction_policy` values.

HTTPS endpoints use system CA roots and mandatory DNS/IP identity verification. Private PKI can be added with
`--outpost-ethereum-additional-ca-file` or `--outpost-ethereum-additional-ca-path`; an explicit proxy can be set
with `--outpost-ethereum-proxy`. The equivalent Solana options are
`--outpost-solana-additional-ca-file`, `--outpost-solana-additional-ca-path`, and
`--outpost-solana-proxy`. The `--outbound-http-*` options provide process-wide fallbacks; chain-specific values
take precedence. See [Outbound HTTP transport](outbound-http-transport.md) for the complete security and
resource policy.

> NOTE:  If you look closely, the reference to `eth-01` in the Ethereum client config, matches the signature provider configured for `Ethereum`.  This mapping is what enables `1..n` clients in a single process

The signer reference is validated during startup and must identify an explicit Ethereum
`--signature-provider`; anonymous signature-provider specs cannot be referenced. File-configured
chain IDs are locally authoritative for signing, and startup verifies them against `eth_chainId`
reported by the configured RPC endpoint.

The CLI option remains available and cannot be combined with the file option:

```sh
--outpost-ethereum-client eth-anvil-local,eth-01,http://localhost:8545,31337
```

The four-field CLI form also treats its chain ID as locally authoritative for signing and
verifies it against the configured RPC endpoint. The historical three-field CLI form remains
compatible by resolving `eth_chainId` during startup. File-configured, four-field CLI, and
three-field CLI clients all require a reachable RPC endpoint at startup; each client receives
an independent five-second budget for chain ID verification or resolution. CLI clients receive
`UINT256_MAX` expenditure caps, with the same compatibility-only warning above.

Every signing-capable client enforces its local policy after the transaction is fully assembled
and immediately before signing. A rejected transaction is neither signed nor broadcast.

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

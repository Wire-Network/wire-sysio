# Cranker Example

`cranker-example` is a demonstration application that uses the `outpost_ethereum_client_plugin` and `cron_plugin` to monitor Ethereum gas prices. It periodically fetches the current gas price from a configured Ethereum node.

## Usage

To run `cranker-example`, you need to provide at least one Ethereum signature provider, one Ethereum outpost client, and one Ethereum ABI file.

### Example Command Line

```shell
cranker-example \
  --signature-provider eth-01,ethereum,ethereum,0x8318535b54105d4a7aae60c08fc45f9687181b4fdfc625bd1a753fa7397fed753547f11ca8696646f2f3acb08e31016afac23e630c5d11f59f61fef57b0d2aa5,KEY:0xac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80 \
  --outpost-ethereum-client eth-anvil-local,eth-01,http://localhost:8545,31337 \
  --ethereum-abi-file tests/fixtures/ethereum-abi-counter-01.json
```

### Configuration Options

#### Signature Provider (`--signature-provider`)
Defines a signature provider. The format is:
`<name>,<chain-kind>,<key-type>,<public-key>,<private-key-provider-spec>`

- **name**: Reference name for this provider (e.g., `eth-01`).
- **chain-kind**: The chain kind (e.g., `wire` or `ethereum`).
- **key-type**: The key format (e.g., `wire` or `ethereum`).
- **public-key**: The public key string.
- **private-key-provider-spec**: Specifier for the private key, typically `KEY:<private-key>`.

#### Outpost Ethereum Client (`--outpost-ethereum-client`)
Defines an Ethereum client connection. The format is:
`<eth-client-id>,<sig-provider-id>,<eth-node-url>[,<eth-chain-id>]`

- **eth-client-id**: Unique identifier for this client.
- **sig-provider-id**: The name of the signature provider to use (must match a name defined in `--signature-provider`).
- **eth-node-url**: The URL of the Ethereum JSON-RPC endpoint.
- **eth-chain-id**: (Optional) The Ethereum chain ID.

#### Ethereum ABI File (`--ethereum-abi-file`)
Path to an Ethereum contract ABI file (relative from current working directory or absolute path). The file should contain a JSON array of ABI-compliant contract definitions.

## Minimum Configuration
To successfully start the application, the following are required:
1.  At least **one** Ethereum signature provider.
2.  At least **one** Ethereum outpost client.
3.  At least **one** Ethereum ABI file reference.

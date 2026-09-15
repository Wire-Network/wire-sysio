# outpost_ethereum_client_plugin

`outpost_ethereum_client_plugin` owns this node's connections to EVM chains. It builds one policy-enforcing
`ethereum_client` per configured endpoint — each bound to a named Ethereum signature provider and to a
verified numeric chain id — loads the contract ABIs those connections are driven through, and hands out
`outpost_client` concretes that speak the chain-agnostic OPP SPI on top of `OPP.sol`, `OPPInbound.sol`, and
`OperatorRegistry.sol`. An operator enables it whenever the node runs an OPP daemon against an EVM outpost:
`batch_operator_plugin` and `underwriter_plugin` both name it in `APPBASE_PLUGIN_REQUIRES`, so either of
those pulls it in, and it can also be loaded alone (as the bundled RPC tool does) to talk to an EVM endpoint
directly.

## How it works

The plugin declares `APPBASE_PLUGIN_REQUIRES((outpost_client_plugin)(signature_provider_manager_plugin))`,
so the SPI base and every signing provider are initialized before it runs. All of its work happens in
`plugin_initialize`; startup and shutdown only log.

```
plugin_initialize
   |
   |  1. --ethereum-abi-file ...   -> parse each JSON array of contract definitions,
   |                                  de-duplicated by absolute path
   |  2. exactly one of --outpost-ethereum-client-config-file / --outpost-ethereum-client
   |  3. per client: resolve the named signature provider (must be chain=ethereum, key-type=ethereum)
   |                 resolve or verify the numeric chain id against eth_chainId
   |                 construct the ethereum_client under the shared RPC policy
   v
   publish the client map (only after every client succeeded)

batch_operator_plugin / underwriter_plugin
   |  create_outpost_client(client-id, chain_code, chain_id, opp, oppInbound, operatorRegistry)
   v
outpost_ethereum_client  -- deliver_outbound_envelope / read_inbound_envelope / uw_commit
```

### Endpoints, signers, and chain ids

Clients come from exactly one of two sources, and configuring both is rejected with
`Configure exactly one of --outpost-ethereum-client-config-file or --outpost-ethereum-client`.

A **command-line spec** is `<client-id>,<signature-provider-id>,<rpc-url>[,<chain-id>]`. With three fields
the chain id is resolved from the endpoint's `eth_chainId` at startup. With four fields the supplied value —
a positive 32-bit decimal or `0x` hex quantity — is what the client signs with, and startup additionally
verifies the endpoint reports the same id, failing with `Chain id mismatch for outpost Ethereum client` when
it does not. A CLI-configured client carries no local expenditure limits: it is given the maximum transaction
policy.

A **configuration file** is versioned protobuf-JSON validated strictly — unknown fields are rejected, the
document is capped at 1 MiB, and `schema_version` must be `1`. Every client needs a `connection` with a
non-empty `client_id`, `signature_provider_id`, and `rpc_url`, plus a non-zero `chain_id`; client ids and
chain ids must each be unique within the file. A `transaction_policy` is optional, but when present all four
fields must be set and `max_priority_fee_per_gas_wei` must not exceed `max_fee_per_gas_wei`. File-configured
chain ids are always verified against `eth_chainId`.

Chain-id resolution is retried before it gives up: initial backoff 200 ms, doubling to a 1 s ceiling, under a
5 s total budget. Failure raises `plugin_config_exception` naming the client, the sanitized endpoint, and a
stable `last_failure` category rather than the response body.

The signature provider is resolved by name and must be explicitly configured — an anonymous alias is
rejected — and must use `chain=ethereum` with `key-type=ethereum`.

**For a client that serves an OPP outpost the `client-id` must be that chain's `sysio.chains` code**, because
the operator daemons look their RPC client up under the chain code. `create_outpost_client` re-checks that
the client's chain id equals the chain id on the outpost registry row and refuses the pairing otherwise, so a
misconfigured node fails closed instead of signing against the wrong network.

### Transport and deadlines

The JSON-RPC policy comes from `outpost_client_plugin`'s shared `outpost_rpc::rpc_options`: a 1 MiB request
cap, a 4 MiB response cap, a 10 s connect timeout, and 30 s header, read, idle, and total timeouts. These are
compiled in, not configurable. HTTPS endpoints use system CA roots with mandatory identity verification;
`--outpost-ethereum-additional-ca-file` and `--outpost-ethereum-additional-ca-path` add private trust roots
and `--outpost-ethereum-proxy` sets an explicit proxy. The process-wide `--outbound-http-*` options act as
fallbacks and the Ethereum-specific values take precedence. Verification cannot be disabled, and proxy
environment variables are not applied implicitly.

Separately, every SPI call takes a caller-supplied `deadline`: the concrete opens an
`fc::task::deadline_scope` at `now + deadline` and re-checks the clock before each blocking RPC, so a hung
endpoint cannot occupy a cron worker past its budget.

### The contract clients

Three typed wrappers are built over the shared connection, each only when its address was supplied to
`create_outpost_client`; passing an empty string for the others is normal, and calling an SPI method whose
wrapper was not provisioned asserts with a message naming the missing address. State-changing calls go
through `create_tx_and_confirm`, which returns only after on-chain inclusion plus confirmations — OPP writes
are consensus-critical and must not silently drop.

| Wrapper | Contract | Members |
|---|---|---|
| `opp_contract_client` | `OPP.sol` | `emitOutboundEnvelope(uint32)` (recovery-only write; no in-tree steady-state caller), `getLatestOutboundEnvelope()` view |
| `opp_inbound_contract_client` | `OPPInbound.sol` | `epochIn(uint32,uint16,uint16,uint32,bytes)`, `discardEnvelopeChunks()`, `nextEpochIndex()` view, `envelopeChunkState(address)` view |
| `operator_registry_contract_client` | `OperatorRegistry.sol` | `commit(bytes)` |

### Outbound delivery and chunking

`deliver_outbound_envelope` refuses an empty envelope and one larger than `OPP_MAX_ENVELOPE_BYTES` (32 768),
then splits the rest into `ceil(size / ETHEREUM_MAX_CHUNK_BYTES)` `epochIn` transactions.
`ETHEREUM_MAX_CHUNK_BYTES` is 8 192 — a compiled-in mirror of `MAX_CHUNK_BYTES` in wire-ethereum's
`OPPCommon.sol`, never configured — so an envelope costs at most four transactions. Every non-final chunk is
exactly that size and the final chunk carries the remainder; there is no terminal call and no crank, because
the contract finalizes inline on the chunk that completes the envelope. Chunks are submitted sequentially
with one transaction in flight at a time, so the signer's nonce advances in lock-step.

A single-chunk envelope stages nothing on chain and pays no extra round trip. A multi-chunk delivery first
reads `envelopeChunkState(self)` at the `latest` block tag — this is the relay's own staging high-water mark,
not content WIRE commits consensus against — and decides what to do from the header alone:

| Staged header | Action |
|---|---|
| Empty (`totalChunks == 0`), or owned by another signer | Send every chunk from index 0 |
| Ours, same epoch, same chunk count and total size, plausible progress | Resume from `receivedChunks` |
| Ours, same epoch, different shape, or more stored chunks than a well-formed upload can hold | `discardEnvelopeChunks()`, then send from index 0 |
| Ours, a different epoch | Read `nextEpochIndex()`; if the outpost has already consumed this epoch, skip the delivery entirely rather than pay for late no-ops |

A `discardEnvelopeChunks()` that reverts at estimate time is treated as "already clear". An unreadable or
malformed state decodes as all-zero and degrades to starting fresh, which the contract absorbs as idempotent
no-ops. A mid-sequence failure simply abandons the tick; the next one restarts from the on-chain high-water
mark.

### Inbound reads and underwriter commits

`read_inbound_envelope` makes one `getLatestOutboundEnvelope` view call at the **`finalized`** block tag, not
`latest`. WIRE consensus on inbound is committed forward against this read, so an operator that read at
`latest` could reach WIRE-side consensus on a slot that a reorg then removes. The decoded result's epoch is
compared with the requested one and a mismatch returns an empty vector at debug level, since observing the
preceding epoch is normal until the consensus-reaching delivery overwrites the slot.

`uw_commit` hex-encodes the canonical `UnderwriteIntentCommit` bytes and calls `OperatorRegistry.commit`,
returning the transaction hash only after confirmation. The outpost binds the signed EVM caller and the
claimed ACTIVE roster identity before queuing the unchanged bytes; the WIRE depot remains authoritative for
the embedded permission signature and bond.

## Enabling / configuration

### `config.ini`

```ini
plugin = sysio::outpost_ethereum_client_plugin

# The signer this node's EVM writes are authenticated with. Must be chain=ethereum, key-type=ethereum.
signature-provider = eth-signer,ethereum,ethereum,<public-key>,KIOD:<kiod-url>

# One client per EVM outpost. For an OPP outpost the client-id MUST be that chain's sysio.chains code.
# Four fields pin the chain id locally and verify it against the endpoint; three fields resolve it.
outpost-ethereum-client = ETHEREUM,eth-signer,https://evm-rpc.example.com,31337

# Contract ABIs: JSON arrays of ABI-compliant contract definitions.
ethereum-abi-file = /etc/wire/abi/outpost-contracts.json

# Optional transport overrides for Ethereum RPC only (the --outbound-http-* options are the fallback).
outpost-ethereum-additional-ca-file = /etc/wire/private-roots.pem
outpost-ethereum-proxy = http://proxy.example.com:3128
```

### Command line

```bash
nodeop --plugin sysio::outpost_ethereum_client_plugin \
       --signature-provider eth-signer,ethereum,ethereum,<public-key>,KIOD:<kiod-url> \
       --outpost-ethereum-client ETHEREUM,eth-signer,https://evm-rpc.example.com,31337 \
       --ethereum-abi-file /etc/wire/abi/outpost-contracts.json
```

### Configuration-file form

For per-client expenditure limits, replace `--outpost-ethereum-client` with
`--outpost-ethereum-client-config-file /etc/wire/ethereum-clients.json`. The two cannot be combined.

```json
{
  "schema_version": 1,
  "clients": [{
    "connection": {
      "client_id": "ETHEREUM",
      "signature_provider_id": "eth-signer",
      "rpc_url": "https://evm-rpc.example.com"
    },
    "chain_id": 31337,
    "transaction_policy": {
      "max_priority_fee_per_gas_wei": "2000000000",
      "max_fee_per_gas_wei": "120000000000",
      "max_gas_limit": "16000000",
      "max_total_native_cost_wei": "2000000000000000000"
    }
  }]
}
```

A client with no `transaction_policy` runs under the maximum policy — the same posture every CLI-configured
client gets. Key material never appears in this document; it stays in the separately configured signature
provider.

## Options

Every option is registered in the config-file description, which appbase also accepts on the command line.

| Option | Default | Meaning |
|---|---|---|
| `outpost-ethereum-client` | unset | Multi-token CLI client spec, `<client-id>,<signature-provider-id>,<rpc-url>[,<chain-id>]`. A three-field spec resolves `eth_chainId` during startup; a four-field chain id controls signing and is verified against the endpoint. For a client serving an OPP outpost the client-id must be that chain's `sysio.chains` code. Mutually exclusive with the config-file option. |
| `outpost-ethereum-client-config-file` | unset | Path to the versioned protobuf-JSON client configuration file. Cannot be combined with `--outpost-ethereum-client`. |
| `ethereum-abi-file` | unset | Multi-token list of Ethereum contract ABI files; each is a JSON array of ABI-compliant contract definitions. Repeated paths are ignored with a warning. |
| `outpost-ethereum-additional-ca-file` | unset | PEM CA bundle added to system trust for Ethereum RPC HTTPS requests. |
| `outpost-ethereum-additional-ca-path` | unset | Hashed CA directory added to system trust for Ethereum RPC HTTPS requests. |
| `outpost-ethereum-proxy` | unset | Explicit proxy URL for Ethereum RPC HTTP requests. |

No option is marked deprecated in the source.

## HTTP API

None. The plugin registers no endpoints and does not depend on `http_plugin`.

## Diagnostics

Log lines go through fc's `default` logger. Endpoints are always printed through
`fc::http::sanitized_endpoint`, so credentials embedded in an RPC URL are not reflected into the log, and
configuration rejections carry stable reason codes rather than document or response content.

Startup and configuration:

- `Loading ABI file: <path>` per ABI file, and `Already registered ABI file: <path>` at warning level for a
  repeated path.
- `Added Ethereum client (id=...,endpoint=...,chain_id=...)` per successfully constructed client.
- `Starting outpost Ethereum client plugin` / `Shutdown outpost Ethereum client plugin`.
- `Rejected Ethereum client configuration (reason_code=...,field=...,observed=...,allowed=...)` at error
  level when the configuration file fails validation, and
  `Rejected Ethereum client policy (reason_code=...,field=...,observed=...,allowed=...)` when a transaction
  policy value is refused. Both rethrow, so the node does not start.

Per-client runtime lines are prefixed with the SPI label `outpost_ethereum_client[{chain_code}:{ChainKind}:{chain_id}]`:

- `epochIn chunk sent epoch=... chunk=.../... bytes=... tx=...` for each delivered chunk.
- `resuming epoch=... delivery at chunk ...`, `discarded a superseded epoch=... staging header`, and
  `skipping epoch=... delivery — the outpost has ...` on the chunk-resume paths.
- `read inbound envelope epoch=... bytes=...` after a successful inbound read.
- `uw_commit confirmed uwreq=... tx_hash=... bytes=...` after a confirmed underwriter commit.
- Warning-level lines for every malformed view result (`envelopeChunkState returned non-string variant`,
  `latestOutboundEnvelope data_ not a string`, and siblings); an epoch mismatch on the inbound slot stays at
  debug level so steady-state polling is not noisy.

## Tests

```bash
ninja -C build/debug test_outpost_ethereum_client_plugin
./build/debug/plugins/outpost_ethereum_client_plugin/test_outpost_ethereum_client_plugin
```

Two suites run in that binary. `outpost_ethereum_client_plugin` covers option registration, chain-id
resolution and verification (explicit, RPC-resolved, mismatched, malformed, out-of-range, and transient
transport failure), signature-provider rejection cases, contract-client construction and ABI encoding, the
chunk-count and chunk-resume decision tables, and every delivery path — single-chunk, multi-chunk in order,
resume from a staged high-water mark, peer-owned header, superseded header, reverting discard, epoch
advanced, deadline abandonment, and the empty and over-cap rejections.
`outpost_ethereum_transaction_policy_tests` covers the expenditure-policy boundary: that a rejection happens
before signing or broadcasting, that exact caps pass through once, that two clients enforce their own
policies, that file and CLI configuration produce the expected policies, and that a partial client map is
never published. No test dials a public endpoint.

A companion RPC tool is built alongside the plugin when `ENABLE_TESTS` is on:

```bash
ninja -C build/debug outpost_ethereum_client_tool
./build/debug/bin/outpost_ethereum_client_tool --signature-provider ... --outpost-ethereum-client ... --ethereum-abi-file ...
```

It initializes `signature_provider_manager_plugin`, `outpost_client_plugin`, and this plugin, then exercises
the first configured client's RPC surface directly. It takes the same options this README documents.

## Related plugins

- [`outpost_client_plugin`](../outpost_client_plugin/README.md) — the SPI and the shared RPC policy this plugin implements.
- [`outpost_solana_client_plugin`](../outpost_solana_client_plugin/README.md) — the sibling concrete for Solana outposts.
- `signature_provider_manager_plugin` — required dependency; supplies the Ethereum signer resolved by name.
- `http_client_plugin` — owns the process-wide `--outbound-http-*` transport fallbacks.
- `batch_operator_plugin` — calls `create_outpost_client` for the OPP envelope path (`OPP.sol` + `OPPInbound.sol`).
- `underwriter_plugin` — calls `create_outpost_client` for the `OperatorRegistry.sol` commit relay.

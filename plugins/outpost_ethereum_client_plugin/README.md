# outpost_ethereum_client_plugin

`outpost_ethereum_client_plugin` owns this node's connections to EVM chains. It builds one policy-enforcing
`ethereum_client` per configured endpoint — each bound to a named Ethereum signature provider and to a
verified numeric chain id — loads the contract ABIs those connections are driven through, and hands out
`outpost_client` concretes that speak the chain-agnostic OPP SPI on top of `OPP.sol` and `OPPInbound.sol`
(plus the outpost's `SyndicationPool.sol` for the per-epoch crank). An operator enables it whenever the node
runs an OPP daemon against an EVM outpost: `batch_operator_plugin` names it in `APPBASE_PLUGIN_REQUIRES`, so
the batch operator pulls it in, and it can also be loaded alone (as the bundled RPC tool does) to talk to an EVM endpoint
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

batch_operator_plugin
   |  create_outpost_client(client-id, chain_code, chain_id, opp, oppInbound)
   v
outpost_ethereum_client  -- deliver_outbound_envelope / read_inbound_envelope / crank_outpost
```

### Endpoints, signers, and chain ids

Clients come from exactly one of two sources. The check is an exclusive-or, so configuring both **and**
configuring neither are rejected with the same message, `Configure exactly one of
--outpost-ethereum-client-config-file or --outpost-ethereum-client`. Because `batch_operator_plugin` names
this plugin in `APPBASE_PLUGIN_REQUIRES`, a node that loads it must configure an Ethereum client source or it
will not start, even if it only intends to serve a Solana
outpost.

A **command-line spec** is `<client-id>,<signature-provider-id>,<rpc-url>[,<chain-id>]`. With three fields
the chain id is resolved from the endpoint's `eth_chainId` at startup. With four fields the supplied value —
a positive 32-bit decimal or `0x` hex quantity — is what the client signs with, and startup additionally
verifies the endpoint reports the same id, failing with `Chain id mismatch for outpost Ethereum client` when
it does not. A CLI-configured client carries no local expenditure limits: it is given the maximum transaction
policy.

A **configuration file** is versioned protobuf-JSON validated strictly — unknown fields are rejected, the
document is capped at 1 MiB, and `schema_version` must be `1`. The file must declare at least one client.
Every client needs a `connection` with a `client_id`, a non-empty `signature_provider_id`, and an `rpc_url`,
plus a non-zero `chain_id`; client ids and chain ids must each be unique within the file. A `client_id` is 1
to 64 characters drawn from `[A-Za-z0-9._-]`. An `rpc_url` must parse as `http` or `https`, must resolve to a
host the outbound HTTP layer considers safe, and must carry no `#` fragment. A `transaction_policy` is
optional, but when present all four fields must be set, each amount must be a canonical decimal string
(digits only, non-zero, no leading zero), and `max_priority_fee_per_gas_wei` must not exceed
`max_fee_per_gas_wei`. File-configured chain ids are always verified against `eth_chainId`.

A client that delivers OPP envelopes (one handed an `OPPInbound` address by `create_outpost_client`) must
also fund a delivery: its `max_gas_limit`, bounded by EIP-7825's 16 777 216 per-transaction cap, has to
reach `DELIVERY_MINIMUM_GAS_CEILING` (9 932 160 gas — what an envelope at the 32 768-byte platform cap
costs to deliver and tip, dispatch one attestation at its allowance, and emit). The relay refuses to be
built on a smaller ceiling, since the largest envelope the platform allows could never complete on it. The
total-cost term binds at the fee a call is sent at: when `max_total_native_cost_wei / max_fee_per_gas_wei`
is below that minimum the relay starts with a warning, and deliveries at fees near the policy maximum spill
into more continuations than their budgets size for.

What the signer needs follows from the same arithmetic. Every `epochIn` is sent with its budget as the gas
limit — a full-cap first delivery at the EIP-7825 cap, less for a continuation — and a node reserves
`gas_limit x max_fee_per_gas` of the signer's balance for it whatever the call uses (about 0.7 ETH at
16 777 216 gas and a 20 gwei base fee, returned on inclusion minus the gas burnt). A default geth also
refuses a submission whose `gas_limit x maxFeePerGas` exceeds its 1 ETH `--rpc.txfeecap` (about 59.6 gwei at
the cap); raise that cap on the operator's own node or accept that deliveries at higher fees are refused by
the node rather than by the policy.

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

Two typed wrappers are built over the shared connection, each only when its address was supplied to
`create_outpost_client`; passing an empty string for the other is normal, and calling an SPI method whose
wrapper was not provisioned asserts with a message naming the missing address. A third, the syndication
pool's, is bound by the crank once the outpost names the pool (see [Outpost cranks](#outpost-cranks)).
State-changing calls go through `create_tx_and_confirm`, which returns only after on-chain inclusion plus
confirmations — OPP writes are consensus-critical and must not silently drop.

| Wrapper | Contract | Members |
|---|---|---|
| `opp_contract_client` | `OPP.sol` | `emitOutboundEnvelope(uint32)` (recovery-only write; no in-tree steady-state caller), `getLatestOutboundEnvelope()` view |
| `opp_inbound_contract_client` | `OPPInbound.sol` | `epochIn(uint32,bytes)` (funded per call, returns the confirmed receipt), `nextEpochIndex()`, `dispatchSpill(uint32)`, `epochDeliveries(uint32,address)`, `pendingEpochHash()`, `pendingConsensusForDigest(bytes32)` and `attestationHandlers(uint16)` views |
| `syndication_pool_contract_client` | `SyndicationPool.sol` (Wire-Network/wire-ethereum#207) | `realizeYield()` |

### Outbound delivery

`deliver_outbound_envelope` refuses an empty envelope and one larger than `OPP_MAX_ENVELOPE_BYTES` (32 768),
then sends the WHOLE envelope in ONE `epochIn(uint32,bytes)`. Ethereum bounds a transaction by gas, not
size — a full-cap envelope is ~1.3 M gas of calldata against EIP-7825's 16.7 M cap — and what may not fit
in one call is dispatch, which the contract spills: the call that reaches consensus routes as many
attestations as its gas allows, records where it stopped in `dispatchSpill(epoch)`, and a continuation is
the SAME call with the same arguments, resumed from that cursor. The outpost never stores the envelope's
bytes; every continuation re-supplies them.

**Reads are pinned to one block.** Every decision is taken from `nextEpochIndex()`, `dispatchSpill(epoch)`,
`epochDeliveries(epoch, self)` and `pendingEpochHash()` read at ONE block number — the head when the tick
starts, then the block each confirmed call landed in (from its receipt) — never at `latest`, which a
backend behind the block that just confirmed could serve from before the call. A backend that does not
know the block yet is retried for a bounded time. The reads are not at `finalized` either: the cursor is
the outpost's own bookkeeping about work this relay is doing now, a reorg costs at most a re-sent call the
contract treats as an idempotent no-op, and a continuation gated on finality would wait ~64 blocks between
every stretch of dispatch.

From those reads the relay decides, per `outpost_ethereum_client_detail::decide_delivery`:

| Outpost state | Action |
|---|---|
| `nextEpochIndex > epoch`, or the spill cursor reports `finalized` | Nothing to send; an EMPTY tx id marks the epoch handled |
| `nextEpochIndex < epoch` | The outpost is behind: `outpost_delivery_incomplete_exception`, retried next tick |
| Tipped and unfinished, and this relay's recorded digest IS `pendingEpochHash` | Continue: re-send the envelope (after checking its `keccak256` is the settled digest) |
| Tipped and unfinished on a digest this relay did not deliver, or delivered a minority of | Nothing to send; its deliverers carry the continuation |
| Untipped, nothing recorded for this relay | Deliver |
| Untipped, this relay recorded, and `pendingConsensusForDigest(own digest)` says the boundary has elapsed and a strict majority agrees | Re-deliver, so the outpost re-runs its path-2 tip |
| Untipped, this relay recorded, otherwise | Nothing to send until more of the group delivers |

**Each call is funded to what it has left to carry** (`delivery_gas_budget`): a fixed cost, a per-byte cost,
an allowance per attestation the cursor says is still to dispatch, and the emit — doubled for each call this
tick that fell short, never above the ceiling. The ceiling (`delivery_gas_ceiling`) is the policy's
`max_gas_limit` bounded by EIP-7825's cap AND by what `max_total_native_cost` pays for at the current fee,
so a budget it admits is never refused by the policy at signing. The node's estimate is deliberately not
the budget: `epochIn` stops on a `gasleft()` watchdog and records where it stopped, so `eth_estimateGas`
converges on the least gas at which the call succeeds — the tip plus one attestation. Instead the budget is
both the pre-flight estimate's gas and the limit the transaction is sent with, so a call that cannot run
inside it is refused before anything is signed, and one that can is sent with exactly it.

**Every call paid for must move the cursor.** After each confirmed call the relay reads the outpost again at
the receipt's block; a call that advanced nothing (no delivery recorded, no tip, no dispatch progress, no
finalization) was under-funded for the attestation at the cursor, and the next one is funded higher. A
node refusal at estimate time is classified by its revert: `OPP_DispatchUnderfunded` and
`OPP_HandlerGasExhausted` below the ceiling retry once at the ceiling; `OPP_NonSequentialEpoch`,
`OPP_OperatorAlreadyDelivered`, `OPP_NotActiveOperator` and `OPP_DigestMismatch` mean the outpost moved
under the read (another relay's call landed), so the relay re-reads at the head and decides again, a
bounded number of times. A call at the ceiling that advances nothing, or is refused for gas at the ceiling,
is an attestation no funding carries: logged once at error level per cursor position, then the tick ends in
`outpost_delivery_incomplete_exception` so the job retries rather than marks the epoch handled. The same
exception ends a tick that reaches `MAX_CONTINUATIONS_PER_TICK` (32) calls with the epoch still open.

Calls are submitted sequentially, one in flight at a time, so the signer's nonce advances in lock-step; a
mid-sequence failure or deadline abandons the tick, and the next one resumes from the outpost's cursor.

### Inbound reads

`read_inbound_envelope` makes one `getLatestOutboundEnvelope` view call at the **`finalized`** block tag, not
`latest`. WIRE consensus on inbound is committed forward against this read, so an operator that read at
`latest` could reach WIRE-side consensus on a slot that a reorg then removes. The decoded result's epoch is
compared with the requested one and a mismatch returns an empty vector at debug level, since observing the
preceding epoch is normal until the consensus-reaching delivery overwrites the slot.

### Outpost cranks

Once per epoch, right after this operator's envelope delivery lands, the outbound relay job calls
`crank_outpost`. On Ethereum that is the liq syndication pool's `realizeYield()`
(Wire-Network/wire-ethereum#207): the pool folds the liqETH yield it accrued since its last report into its
principal and reports the delta as one `LIQ_YIELD` attestation — the counterpart of the Solana relay's
`report_liq_yield`. Nothing has to tell the relay the pool's address: it reads
`OPPInbound.attestationHandlers(DESYNDICATE_LIQ)` at `latest`, because the pool registers itself as that
handler when its OPP endpoint is configured, binds a `syndication_pool_contract_client` to the address that
comes back, and re-binds if it changes. `address(0)` and `ATTESTATION_BLACKHOLE` mean no pool, and an ABI set
without `realizeYield` means an outpost deployment that predates the pool; both leave the crank idle at debug
level.

`realizeYield()` refuses at estimate time, before any gas is spent, and the relay reads the pool's own
refusals as outcomes rather than failures: `WIRE_NoYield()` and `WIRE_YieldBelowDeadband(uint64,uint64)` are
the quiet steady state (debug), `WIRE_PoolUnderbacked(uint64,uint64)` is a warning (the loss path is not in
that contract), and `EnforcedPause()` — raised only by `SyndicationPool`'s own pause, when its panic role has
frozen the pool (the OPP endpoint has no pause) — is logged at info every epoch until it is unpaused. Any other
revert — a signer without the pool's `yield_operator` role, a foreign implementation — and any transport failure
propagate to the job, which logs the failed crank and retries with the next epoch's delivery. The relay never cranks `payPendingDesyndication`; paying a
desyndication stored while the pool was paused is the operator playbook's step.

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
  level when client configuration fails validation, then rethrown so the node does not start. Policy-value
  failures reach this line too: the configuration loader converts them into the configuration vocabulary
  before they escape, with `reason_code=policy_value_invalid`.

Per-client runtime lines are prefixed with the SPI label `outpost_ethereum_client[{chain_code}:{ChainKind}:{chain_id}]`:

- `epochIn delivered|continued epoch=... bytes=... gas=... block=... tx=... — recorded=... tipped=...
  dispatched=... finalized=...` for each confirmed call, summarising the `OPPInbound` events in its receipt.
- `skipping epoch=... delivery — the outpost has already finalized it`, `tipped on a digest this relay did
  not deliver`, `delivery already recorded; consensus not tipped`, `re-delivering to run the boundary tip`
  and `not delivered — the outpost is still on epoch ...` for each decision that sends nothing (or
  re-sends).
- `refused at ... gas (...); retrying at the ceiling` at warning level for a gas refusal below the ceiling,
  and `cannot advance past attestation N at the ceiling` at error level — once per cursor position — when
  no funding carries the attestation there; `still open after N calls in one tick` at warning level when
  the per-tick bound is reached.
- `read inbound envelope epoch=... bytes=...` after a successful inbound read.
- Warning-level lines for every malformed view result (`latestOutboundEnvelope data_ not a string` and
  siblings); an epoch mismatch on the inbound slot stays at debug level so steady-state polling is not
  noisy.

## Tests

```bash
ninja -C build/debug test_outpost_ethereum_client_plugin
./build/debug/plugins/outpost_ethereum_client_plugin/test_outpost_ethereum_client_plugin
```

Two suites run in that binary. `outpost_ethereum_client_plugin` covers option registration, chain-id
resolution and verification (explicit, RPC-resolved, mismatched, malformed, out-of-range, and transient
transport failure), signature-provider rejection cases, contract-client construction and ABI encoding, the
delivery decision table, settlement and receipt classification, the gas budget and ceilings, and every
delivery path — one call, tip-and-spill continued in the same tick, resume from a tipped cursor, minority
and never-delivered digests, consensus retry gated on the outpost's own majority view, pinned-block reads,
escalation after a call that advanced nothing and the stall report at the ceiling, revert classification,
outpost behind, the per-tick bound, epoch advanced, deadline abandonment, and the empty and over-cap
rejections — and the outpost crank: idle without
the pool ABI or a registered handler, binding the registered pool and re-binding on a change, the pool's own
three refusals, unrecognised reverts and protocol errors, and deadline abandonment.
`outpost_ethereum_transaction_policy_tests` covers the expenditure-policy boundary: that a rejection happens
before signing or broadcasting, that exact caps pass through once, that two clients enforce their own
policies, that file and CLI configuration produce the expected policies, and that a partial client map is
never published. No test dials a public endpoint.

A companion RPC tool is built alongside the plugin when `ENABLE_TESTS` is on:

```bash
ninja -C build/debug outpost_ethereum_client_tool
./build/debug/bin/outpost_ethereum_client_tool --signature-provider ... --outpost-ethereum-client ... --ethereum-abi-file ...
```

It is a fixed demonstration against a local anvil chain, not a general RPC probe. It initializes
`signature_provider_manager_plugin`, `outpost_client_plugin`, and this plugin, then drives the **first**
configured client through a hard-coded script: it requires at least two `--ethereum-abi-file` entries and
uses the second for an `OPPEnvelope` event read against a fixed contract address, then uses the first to read
a counter contract at another fixed address and **sign and broadcast a `setNumber` transaction** that
increments it. Both addresses are compiled in and match a local anvil deployment; it takes the same options
this README documents, but there is no option that redirects those addresses. Do not point it at a real
network: it will spend gas from the configured signer and write to whatever contract happens to sit at that
counter address.

## Related plugins

- [`outpost_client_plugin`](../outpost_client_plugin/README.md) — the SPI and the shared RPC policy this plugin implements.
- [`outpost_solana_client_plugin`](../outpost_solana_client_plugin/README.md) — the sibling concrete for Solana outposts.
- `signature_provider_manager_plugin` — required dependency; supplies the Ethereum signer resolved by name.
- `http_client_plugin` — owns the process-wide `--outbound-http-*` transport fallbacks.
- `batch_operator_plugin` — calls `create_outpost_client` for the OPP envelope path (`OPP.sol` + `OPPInbound.sol`).

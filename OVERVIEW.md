# Wire Network & the Omnichain Proof Protocol (OPP)

## Wire Network

Wire Network is a multi-chain consensus and interoperability platform built on the AntelopeIO protocol. It enables secure, trustless cross-chain communication through a specialized consensus mechanism and unified proof protocol. The network connects the Wire blockchain (the coordination layer, or **depot**) with external chains — Ethereum (EVM) and Solana (SVM) — via on-chain **Outpost** contracts deployed on each supported chain.

### Architecture at a Glance

| Layer | Components |
|-------|-----------|
| **Wire Blockchain (Depot)** | Block producers, system contracts (`sysio.msgch`, `sysio.epoch`, `sysio.opreg`, `sysio.chalg`, `sysio.synd`, `sysio.bond`, `sysio.liq`, `sysio.swap`, `sysio.dclaim`, `sysio.andon`, …) |
| **Operator Network** | Batch operators relaying OPP envelopes in rotating groups (default 21 operators, 3 groups of 7); bond providers underwriting syndication statements on the depot |
| **External Chains** | Outpost contracts on Ethereum and Solana |
| **Protocol Layer** | OPP — protobuf-encoded envelopes, per-outpost envelope hash chains, group consensus |

---

## The Omnichain Proof Protocol (OPP)

OPP is the cryptographic wire protocol that enables deterministic, consensus-backed proof transmission across blockchains. It uses Protocol Buffers for serialization and defines a hierarchical message structure:

```
Envelope (one per outpost per epoch, per direction)
  └─ Message (individual message within an envelope)
       └─ Attestation (typed data entry within a message)
```

### Key Concepts

- **Epochs**: Time-bounded intervals managed by `sysio.epoch` — one global epoch clock for every chain. Each epoch has one group of batch operators on duty as relayers.
- **Envelopes**: Epoch-level containers. Each envelope carries the epoch index and timestamp, its route endpoints, and `previous_envelope_hash` — the digest of the previous envelope on the same (depot, outpost) stream — so each direction forms a hash chain. An envelope is capped at 32,768 bytes on every chain.
- **Messages**: Individual cross-chain payloads containing a header (message ID, sequence, checksum, encoding flags) and a payload (an array of attestation entries).
- **Attestations**: Typed data units that represent cross-chain actions — operator rosters, liquidity syndication and desyndication, yield reports and node-owner registrations.

### Chain Addressing

OPP uses a universal chain-agnostic address system:

- **ChainKind** enum: `WIRE`, `EVM`, `SVM`
- **ChainAddress**: `(ChainKind, raw_bytes_address)` — no translation or mapping needed

This allows a single unified message format across all supported chains.

---

## How Cross-Chain Messages Flow

### Inbound (External Chain → Wire)

1. A user action on an external chain (e.g. syndicating liqETH into the Ethereum `SyndicationPool`) queues a protobuf-encoded attestation in the outpost's outbound buffer, which the outpost seals into its envelope for the epoch.
2. Every batch operator in the active group independently reads that envelope through its chain-specific client plugin (`outpost_ethereum_client_plugin`, `outpost_solana_client_plugin`).
3. Each operator delivers the envelope to Wire's `sysio.msgch` contract via the `deliver()` action.
4. `sysio.msgch` evaluates consensus: every eligible operator delivering an identical envelope is accepted immediately; otherwise a strict majority is accepted once the epoch boundary has passed. A split with no strict majority opens a `sysio.chalg` dispute.
5. Attestations are extracted, validated, and routed to the appropriate system contract handlers.

### Outbound (Wire → External Chain)

1. System contracts queue attestations for a target outpost via `sysio.msgch::queueout()`.
2. When the epoch advances, `buildenv()` packs the `READY` attestations into a protobuf-encoded outbound envelope; anything that does not fit stays queued for the next epoch.
3. Batch operators read the outbound envelope and submit it to the target chain's Outpost — chunked on both chains (`OPPInbound.epochIn` on Ethereum, `epoch_in` on Solana).
4. The Outpost verifies the envelope chain and group consensus, then routes attestations to their registered handlers. An unhandled type is skipped, never reverted.

---

## Key Attestation Types

| Category | Types | Direction | Purpose |
|----------|-------|-----------|---------|
| **Operator Rosters** | `OPERATORS`, `BATCH_OPERATOR_GROUPS` | depot → outpost | Full operator roster and the next epoch's batch-operator group, sent every epoch |
| **Liquidity Syndication** | `SYNDICATE_LIQ`, `LIQ_YIELD` | outpost → depot | A user syndicating liq tokens into outpost custody; realized custody yield |
| **Liquidity Desyndication** | `DESYNDICATE_LIQ` | depot → outpost | Return syndicated liq tokens to a holder on the outpost |
| **Node Owners** | `NODE_OWNER_REG` | outpost → depot | Node-owner NFT committed on Ethereum, registering the owner on the depot |

Cross-chain swaps routed through outposts, outpost reserves and outpost-side operator collateral have been removed. Swaps are a depot-local AMM (`sysio.swap`) with no outpost participation; operator collateral is held on the depot by `sysio.opreg`.

---

## Participant Roles

### Block Producers
Elected via Appointed Proof of Stake (APoS). They validate transactions and produce blocks on the Wire blockchain — the standard Antelope producer role. Producers register with `sysio.opreg` and post depot-native collateral there.

### Batch Operators
Operators scheduled in groups of `operators_per_epoch` (default 21 operators in 3 groups of 7); one group is on duty per epoch, and the schedule slides forward every epoch. They run the `batch_operator_plugin` and are responsible for:
- Reading each outpost's outbound envelope and delivering it to `sysio.msgch`
- Relaying the depot's outbound envelopes to each outpost
- Cranking the depot: `sysio.msgch::chkcons` (epoch advance), `sysio.chalg::chkdispute`, `sysio.swap::tickyield`, `sysio.liq::queueyield`

Batch operators post depot-native collateral on `sysio.opreg`; a non-canonical delivery is slashed and missed deliveries lead to termination.

Signing keys for outbound submissions can be held locally (`KEY:`) or in a `kiod` daemon (`KIOD:`) — both built into `signature_provider_manager_plugin`. AWS KMS support (`KMS:` — secp256k1/ethereum keys only, remote signing so the key never leaves KMS) is provided by `signature_provider_kms_plugin`, enabled per config with `plugin = sysio::signature_provider_kms_plugin`. See `plugins/signature_provider_kms_plugin/test/README.md` for KMS key setup, IAM requirements, the `KMS:` spec format, and operational notes. AWS SSM Parameter Store support (`SSM:` — all key types) is `signature_provider_ssm_plugin`, enabled with `plugin = sysio::signature_provider_ssm_plugin`: the private key is fetched from a KMS-encrypted `SecureString` parameter once at startup and signing is local thereafter, fast enough for every signing path including block production. See `plugins/signature_provider_ssm_plugin/test/README.md` for parameter setup, IAM requirements, the `SSM:` spec format, and rotation notes. A `KMS:`/`SSM:` spec whose plugin is not enabled fails the boot with an error naming the `plugin =` line to add; host applications that embed the manager without the plugins can still register handlers from `main()` via `register_spec_handler()`.

### Bond Providers (Underwriters)
Depot accounts that underwrite provable statements on `sysio.bond` with depot-native tokens (WIRE or shadow LIQ). Today the issuer is `sysio.synd`: each syndication envelope is released only after its statement is bonded. A bond provider accepts a share of a request, earns a pro-rata share of the bounty when the statement is approved, and forfeits the bond to the issuer if the statement is resolved invalid. No plugin or outpost interaction is involved.

### Node Owners
Holders of a node-owner NFT who commit it on the Ethereum outpost (`NODE_OWNER_REG`). They are registered on `sysio.roa`, receive node-owner WIRE distributions from `sysio.system`, and form the Tier-1 electorate that votes on `sysio.chalg` envelope disputes.

---

## System Contracts

| Contract | Role |
|----------|------|
| **sysio.msgch** | OPP channel — receives inbound envelopes, evaluates consensus, routes attestations, queues and builds outbound envelopes, cranks the epoch |
| **sysio.epoch** | Global epoch clock, sliding-window batch-operator schedule, emissions gate |
| **sysio.opreg** | Operator registry and depot-native collateral ledger (deposit, withdraw queue, pull claims, slashing, termination) |
| **sysio.chalg** | Envelope-dispute vote by Tier-1 node owners; slash execution |
| **sysio.synd** | Liquidity syndication ledger: syndication envelopes, bonded release, desyndication, rate limits |
| **sysio.bond** | Generic bonded underwriting of provable statements |
| **sysio.liq** | Shadow liq tokens (`LIQETH`, `LIQSOL`) minted against outpost custody, with a WIRE yield index |
| **sysio.swap** | Depot-local constant-product AMM; every pair's second leg is WIRE |
| **sysio.dclaim** | Funded WIRE launch import, account linking and claims |
| **sysio.andon** | Emergency stop: one cord freezing depot fund egress |
| **sysio.roa** | Resources and node-owner registry |
| **sysio.authex** | Links a depot account to its external-chain keys |
| **sysio.chains** / **sysio.tokens** | Chain (outpost) and token registries |
| **sysio.system** | Emissions and producer / batch-operator / node-owner pay |

---

## Ethereum Outpost Contracts

| Contract | Role |
|----------|------|
| **OPP.sol** | Outbound — queues attestations, seals and emits the outpost's per-epoch envelope |
| **OPPInbound.sol** | Inbound — accepts chunked `epochIn` deliveries, validates group consensus and the envelope chain, routes attestations to handlers |
| **SyndicationPool.sol** | liqETH custody: `syndicate` (`SYNDICATE_LIQ`), `realizeYield` (`LIQ_YIELD`), `DESYNDICATE_LIQ` payout |
| **BAR.sol** | Node-owner NFT escrow emitting `NODE_OWNER_REG` |
| **OutpostManager.sol** | OPP endpoint and role wiring |
| **StakingManager.sol** | Placeholder for the deferred staking track; its entry points revert |

On Solana the outpost lives in the `liqsol-core` program (`instructions/opp/`), alongside liqSOL staking and syndication.

---

## Consensus & Security

- **Group consensus**: An envelope is accepted when every eligible operator in the active group delivers it identically, or by strict majority once the epoch boundary has passed.
- **Envelope hash chains**: Each envelope carries the digest of its predecessor on the same stream, and messages carry sequence numbers, so envelopes and messages cannot be reordered or replayed.
- **Disputes**: A post-boundary split with no strict majority opens a `sysio.chalg` dispute that pauses the epoch until Tier-1 node owners vote a canonical envelope. Operators that delivered a non-canonical envelope are slashed.
- **Bonded syndication**: Syndicated value is released on the depot only after `sysio.bond` underwriters bond the syndication statement; anyone may challenge it during the challenge window.
- **Emergency stop**: `sysio.andon` freezes depot fund egress; outposts pause independently (`SyndicationPool` pause on Ethereum, `frozen` on Solana).

---

## The Vision

Wire Network implements a trustless, consensus-backed omnichain infrastructure where:

1. **OPP** provides a single deterministic message encoding across all blockchains
2. **Batch operators** collectively relay and validate messages with no single point of failure
3. **Bond providers** stand behind every syndication statement with slashable bonds
4. **On-chain governance** handles scheduling (epochs), routing (message channel), and economic enforcement (collateral, bonds, disputes)
5. **Atomic cross-chain settlement** — no bridging overhead, no trusted intermediaries

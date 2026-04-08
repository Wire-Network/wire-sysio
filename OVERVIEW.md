# Wire Network & the Omnichain Proof Protocol (OPP)

## Wire Network

Wire Network is a multi-chain consensus and interoperability platform built on the AntelopeIO protocol. It enables secure, trustless cross-chain communication through a specialized consensus mechanism and unified proof protocol. The network connects the Wire blockchain (the coordination layer) with external chains — Ethereum, Solana, and Sui — via on-chain **Outpost** contracts deployed on each supported chain.

### Architecture at a Glance

| Layer | Components |
|-------|-----------|
| **Wire Blockchain (Depot)** | Block producers, system contracts (`sysio.msgch`, `sysio.epoch`, `sysio.uwrit`, `sysio.chalg`) |
| **Operator Network** | 21 batch operators (3 groups of 7), underwriters |
| **External Chains** | Outpost smart contracts on Ethereum, Solana, Sui |
| **Protocol Layer** | OPP — protobuf-encoded messages, merkle proofs, multi-sig consensus |

---

## The Omnichain Proof Protocol (OPP)

OPP is the cryptographic wire protocol that enables deterministic, consensus-backed proof transmission across blockchains. It uses Protocol Buffers for serialization and defines a hierarchical message structure:

```
Envelope (epoch-spanning container)
  └─ Message (individual message within an epoch)
       └─ Attestation (typed data entry within a message)
```

### Key Concepts

- **Epochs**: Time-bounded intervals managed by `sysio.epoch`. Each epoch assigns one group of 7 batch operators as active relayers.
- **Envelopes**: Epoch-level containers carrying a merkle root, epoch index, timestamp, and the range of messages they cover.
- **Messages**: Individual cross-chain payloads containing a header (message ID, checksum, encoding flags) and a payload (an array of attestation entries).
- **Attestations**: Typed data units that represent cross-chain actions — swaps, stake updates, operator actions, underwriting intents, and more.

### Chain Addressing

OPP uses a universal chain-agnostic address system:

- **ChainKind** enum: `WIRE`, `ETHEREUM`, `SOLANA`, `SUI`
- **ChainAddress**: `(ChainKind, raw_bytes_address)` — no translation or mapping needed
- **ChainSignature**: `(actor, key_type, signature_bytes)` — supports native chain signatures

This allows a single unified message format across all supported chains.

---

## How Cross-Chain Messages Flow

### Inbound (External Chain → Wire)

1. A user action on an external chain (e.g., depositing ETH on the Ethereum Outpost) triggers an `OPPMessage` event containing protobuf-encoded attestations.
2. All 7 active batch operators independently observe the event via chain-specific client plugins (`outpost_ethereum_client_plugin`, `outpost_solana_client_plugin`).
3. Each operator delivers the envelope to Wire's `sysio.msgch` contract via the `deliver()` action.
4. `sysio.msgch` evaluates consensus — when all operators in the active group submit identical envelope checksums, the messages are accepted.
5. Attestations are extracted, parsed, and routed to the appropriate system contract handlers.

### Outbound (Wire → External Chain)

1. System contracts queue attestations for a target outpost via `sysio.msgch::queueout()`.
2. When the epoch advances, `buildenv()` packs all `READY` attestations into a protobuf-encoded outbound envelope with a merkle root.
3. Batch operators read the outbound envelope and submit it to the target chain's Outpost contract.
4. The Outpost contract (`OPPInbound.sol` on Ethereum) verifies consensus, checks merkle proofs, and routes attestations to registered handler contracts.

---

## Key Attestation Types

| Category | Types | Purpose |
|----------|-------|---------|
| **Operator Management** | `OPERATOR_ACTION`, `BATCH_OPERATOR_NEXT_GROUP`, `SLASH_OPERATOR` | Operator registration, rotation, penalties |
| **Staking** | `STAKE_UPDATE`, `PRETOKEN_STAKE_CHANGE`, `NATIVE_YIELD_REWARD` | Cross-chain staking and yield distribution |
| **Swaps & Liquidity** | `SWAP`, `UNDERWRITE_INTENT`, `UNDERWRITE_CONFIRM`, `REMIT` | Cross-chain swaps backed by underwriter collateral |
| **Governance** | `CHALLENGE_REQUEST`, `CHALLENGE_RESPONSE`, `EPOCH_SYNC`, `ROSTER_UPDATE` | Dispute resolution and epoch synchronization |
| **Reserves** | `RESERVE_BALANCE_SHEET` | Collateral snapshots across chains |

---

## Participant Roles

### Block Producers
Elected via Appointed Proof of Stake (APoS). They validate transactions and produce blocks on the Wire blockchain — the standard Antelope producer role.

### Batch Operators
21 operators split into 3 groups of 7. One group is active per epoch. They run the `batch_operator_plugin` and are responsible for:
- Observing events on external chain Outposts
- Delivering inbound envelopes to `sysio.msgch`
- Building and submitting outbound envelopes to external chains
- Responding to challenges if disputed

### Underwriters
Independent operators running the `underwriter_plugin`. They provide collateral-backed liquidity for cross-chain swaps:
1. Monitor `sysio.msgch` for `SWAP` attestations
2. Verify deposits on external chains
3. Submit underwriting intent to `sysio.uwrit` with locked collateral
4. Earn fees on successful settlement; face slashing on failure

---

## System Contracts

| Contract | Role |
|----------|------|
| **sysio.msgch** | Central message router — receives inbound envelopes, evaluates consensus, extracts attestations, queues outbound messages |
| **sysio.epoch** | Manages epoch cycles, assigns operator groups, registers outposts |
| **sysio.uwrit** | Underwriting ledger — tracks collateral, locks/releases funds, distributes fees, handles slashing |
| **sysio.chalg** | Challenge handler — manages dispute resolution with economic penalties |

---

## Ethereum Outpost Contracts

| Contract | Role |
|----------|------|
| **OPP.sol** | Message sender — collects attestations in send mode, builds merkle trees, emits `OPPMessage` / `OPPEpoch` events |
| **OPPInbound.sol** | Message receiver — validates consensus, verifies merkle proofs, routes attestations to handlers |
| **OperatorRegistry.sol** | Maps operators to roles, keys, and status for identity validation |
| **BAR.sol** (Bond Agreement Registry) | Receives collateral bonds, emits `OPERATOR_ACTION` attestations |
| **IOPPReceiver implementations** | Domain-specific handlers (Depositor, Pool, Pretoken) that process individual attestation types |

---

## Consensus & Security

- **Multi-signature consensus**: All 7 active operators must deliver identical envelope checksums for acceptance.
- **Merkle proofs**: Each message is a leaf in the epoch merkle tree; inbound processors verify proofs before accepting.
- **Challenge mechanism**: Any party can challenge a disputed epoch via `sysio.chalg`. Operators must respond with correct hashes or face slashing.
- **Collateralized underwriting**: Swaps are backed by locked collateral with a full lifecycle (intent → submit → confirm → release/slash).

---

## The Vision

Wire Network implements a trustless, consensus-backed omnichain infrastructure where:

1. **OPP** provides a single deterministic message encoding across all blockchains
2. **Batch operators** collectively relay and validate messages with no single point of failure
3. **Underwriters** provide collateral-backed liquidity for cross-chain swaps
4. **On-chain governance** handles scheduling (epochs), routing (message channel), and economic enforcement (underwriting + challenges)
5. **Atomic cross-chain settlement** — no bridging overhead, no wrapped tokens, no trusted intermediaries

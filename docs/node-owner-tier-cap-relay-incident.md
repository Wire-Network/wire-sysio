# Node-owner tier-cap OPP relay incident

## Summary

On September 28, 2026, Bearbox accepted two Tier-1 node-owner NFT commits on Ethereum but did not create either node owner on Wire. Ethereum and the OPP transport were healthy. The Wire destination rejected the second registration because the Tier-1 roster reached its consensus-defined limit while processing a two-registration envelope.

The two registrations were dispatched atomically. The first temporarily filled the final Tier-1 slot; the second hit a hard `check()` in `sysio.roa::regnodeowner`. That assertion reverted the complete `sysio.msgch::deliver` transaction, including the first registration, its audit state, and the envelope-consensus record. Batch operators consequently retried the same deterministically failing envelope every 15 seconds.

This is a destination-side claim-handling bug. It is not an Ethereum RPC, BAR, envelope-generation, decoding, quorum, or network-liveness failure.

## Affected environment

- Wire chain ID: `6e20188abb586ed7e8097e8fb769a1da2bba294f8e97910d3a88cf838f9a259f`
- Deployed `wire-sysio`: `33e86e8f9c134391f1910c815fb4ffc85b84edd3`
- Deployed `wire-ethereum`: `b90035b48414267d1b3ca183b88e2118a8c5b16e`
- Ethereum chain ID: `31337`
- Fix branch base: `wire-sysio` `master` at `9c031c9a5b`

The affected Bearbox chain was disposable and has been scheduled for a same-build destructive reset. The reset removes the blocked state but continues using the deployed `33e86e8f` behavior; the source fix must be built and deployed before it prevents recurrence.

## Evidence

Immediately before the failure, `sysio.roa` contained:

- 20 Tier-1 node owners;
- 3 Tier-3 node owners;
- 23 confirmed `nodeownerreg` audit rows;
- no audit row for either blocked claim.

Tier 1 has a hard maximum of 21 owners in `sysio.system/emissions.hpp`.

Ethereum accepted both claims:

| Claim | Ethereum block | Transaction |
| --- | --- | --- |
| `yarkin`, Tier 1 | `0x51f56` | `0x4ad3cefe720c7e25dc5aa89acc0789bb4c0c0963241e28d511d79a7d840117d6` |
| `bear2`, Tier 1 | `0x51f58` | `0xe75bdd4888e7f4a0f882e4d9aab9e2e42f880a1d784cf7b42b6f949f162756c4` |

Both transactions emitted `BAR.NodeCommitted`. Ethereum then emitted OPP envelope `5749` at block `0x51f79` in transaction `0x2f093b5aa184f412bd9a721858d2ec0c8fc705b29c2ce4639bc5ad394b1be54d`. Decoding its 792-byte payload produced exactly two messages:

1. `yarkin`, Tier 1;
2. `bear2`, Tier 1.

The preceding envelopes prove that transport was advancing normally:

| Epoch | Size | Node-owner claims |
| --- | ---: | --- |
| 5746 | 793 bytes | two Tier-1 claims |
| 5747 | 427 bytes | one Tier-1 claim (`bear1`) |
| 5748 | 63 bytes | none |
| 5749 | 792 bytes | `yarkin`, `bear2` |

Batch-operator logs then repeated the same failure every 15 seconds:

```text
batch_operator: push sysio.msgch::deliver failed: ...
assertion failure with message: node owner tier cap reached
```

The relay continued reading Ethereum envelope `5749`; it did not lose or fail to decode the envelope.

## Execution path

1. `wire-ethereum/contracts/outpost/BAR.sol::commitNode` escrows one canonical WireNodes NFT and queues a `NODE_OWNER_REG` attestation.
2. Ethereum OPP emits the queued attestations in `OPPEnvelope(bytes)`.
3. `outpost_ethereum_client` reads the envelope and the batch operator submits `sysio.msgch::deliver`.
4. `sysio.msgch::apply_consensus` iterates every message and attestation in one transaction.
5. `dispatch_node_owner_reg` sends `sysio.roa::newnameduser` followed by `sysio.roa::nodeownreg` as inline actions.
6. `nodeownreg` soft-rejects known claim errors, but calls `regnodeowner` for an otherwise valid claim.
7. `regnodeowner` enforces the economic tier cap with a hard assertion.

At a starting count of 20:

- `yarkin` passes `20 < 21` and temporarily becomes owner 21;
- `bear2` fails `21 < 21`;
- Antelope transaction atomicity rolls both registrations back;
- the durable count returns to 20;
- neither claim receives a `nodeownerreg` audit row;
- no accepted envelope-consensus row is committed, so the relay retries epoch 5749.

This explains why the observed durable count remained one below the cap while logs reported that the cap had been reached.

## Root cause

`sysio.roa::nodeownreg` already classifies bad user claims as terminal soft failures. It records `REJECTED` and returns specifically to prevent one claim from aborting OPP delivery. Capacity exhaustion was omitted from that classification and remained only in the lower-level hard invariant:

```cpp
check(nodeowner_count(get_self(), state.network_gen, tier) < tier_cap,
      "node owner tier cap reached");
```

That invariant is correct for direct/internal registration safety, but it is the wrong boundary behavior for an externally sourced claim inside a multi-attestation consensus envelope. A valid but over-cap claim is terminal business state, not a malformed envelope or system-integrity failure.

The relay correctly retries transactions that do not commit. Because this failure is deterministic and there was no terminal rejection path, retrying could never recover.

## Minimum viable fix

The fix keeps the consensus-defined cap unchanged and preserves the hard assertion as defense in depth:

1. Add `TIER_CAP_REACHED = 6` to the existing `nodeownerreg` rejection reasons.
2. Centralize per-tier capacity lookup in `nodeowner_cap()` so the preflight and invariant cannot drift.
3. In `nodeownreg`, after existing claim validation and before AuthX or registration side effects, detect a full tier.
4. Write `REJECTED / TIER_CAP_REACHED` and return normally.
5. Retain the hard capacity assertion inside `regnodeowner` for privileged callers and programming errors.

When an envelope contains two claims with one slot remaining, the first registration now commits and the second receives a durable rejection. The envelope-consensus transaction can commit and the OPP stream can advance.

The table layout and ABI field types do not change: `nodeownerreg.reason` remains `uint8_t`. Consumers that render reason values should add label `6` in a follow-up; older consumers may display an unknown reason but can still decode the row.

`sysio.msgch` invokes `newnameduser` before `nodeownreg`. Consequently, an otherwise valid over-cap claimant can retain the newly created Wire account while its node-owner registration is rejected. This follows the existing two-action soft-failure model: the account is not a node owner and receives no tier allocation.

The rejected claim's NFT remains escrowed by BAR, matching the existing rejected-claim model. Governance can return it through BAR's existing `releaseNode` path.

## Deferred regression coverage

Regression tests are intentionally deferred from the minimum draft fix. Before merge, coverage should include:

1. `nodeownreg` at a full tier returns successfully and writes `REJECTED / TIER_CAP_REACHED`.
2. An envelope delivered from a starting count of 20 containing two Tier-1 registrations confirms the first, rejects the second, records consensus, and permits the following epoch to advance.
3. Direct `regnodeowner`/`forcereg` still aborts beyond the tier cap.
4. Existing rejection precedence remains stable: malformed name, missing account, key mismatch, duplicate, and AuthX link mismatch continue to win over capacity when applicable.
5. SDK/tooling enum displays are updated for rejection reason `6`.

## Operational recovery

For an affected non-disposable chain, deploy the corrected `sysio.roa` contract and allow the existing batch operators to retry. No Ethereum transaction replay is required: the committed BAR events and envelope already exist. On retry, the final available registration can commit, the excess registration is audited as rejected, and the OPP stream advances.

For a disposable sandbox, a destructive reset clears both the roster and blocked envelope but does not remove the underlying bug unless the fixed contract is included in the build.

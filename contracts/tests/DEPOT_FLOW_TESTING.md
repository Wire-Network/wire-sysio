# Depot flow tests

## Test layers

1. **Contract boundary tests** exercise production WASM, authorization, arithmetic,
   rejection, replay, pagination and individual state transitions. The existing
   bond, syndication, collateral, emissions and producer suites remain essential.
2. **Depot integration tests** exercise inline calls between the real contracts.
   Generic scenarios cover backed import, held intake, bonding, partial
   release, challenge adjudication, freeze/repair, fee rounding, link delivery,
   yield conversion, funded import claims and return instructions.
3. **Cluster flows** retain responsibility for external contract execution,
   operator daemons, networking, finality, actual producer participation and soak
   stability. Native virtual-time tests cannot establish those properties.

`sysio_dispatch_tests/generic_external_simulator_drives_production_dispatch`
feeds canonical protobuf envelopes into real `sysio.msgch`, its epoch consensus
path, `sysio.synd` and `sysio.liq` for both transport families. The funded import
case verifies that pre-launch credits link and pay.
LIQ yield distributes only supplied WIRE. Its regression checks exact holder settlement and no treasury draw.
Ethereum NFT registration remains covered by the Ethereum-specific dispatch and
ROA tests: changing that source chain would hide an actual protocol constraint.

The focused syndication sequences enter at the **authenticated downstream
msgch action boundary** and use the existing `epoch_stub` to control bucket epochs.
They deploy real syndication, bond, LIQ, AuthEx, swap and Andon contracts. They do
not claim to test consensus or epoch maintenance; production dispatch and epoch
suites cover those separately. The generic collateral test isolates opreg's role
minimum and withdrawal policy; the producer eligibility/score suites additionally
cover producer registration, finalizer keys and schedule selection.

## Generic external-chain model

[external_chain_simulator.hpp](external_chain_simulator.hpp) is test-only host
code. EC1/NTA uses the EVM key encoding; EC2/NTB uses the SVM encoding. The
9-decimal symbols identify WIRE shadow units. They are not assertions that every
external asset has nine decimals.

The simulator tracks external custody, the shared deposit/yield sequence,
canonical message/envelope hash chaining, pending returns and idempotent payouts.
It builds protobuf reports and consumes the depot's actual `DesyndicateLIQ`
message. `lose()` is explicit fault injection; `donate()` backs bootstrap liquidity
or repairs a deficit. Freeze is explicitly controlled by the test, not delivered
by a simulated daemon. This deliberately small model implements no depot fee,
bond, governance, eligibility, token-minting or AMM policy. There is no modified
production OPP contract, socket, daemon, external validator or external SDK.

The generic syndication fixture imports real backed balances for both the
underwriter and challenger. Accepting a request spends those balances; it does
not mint collateral. This differs intentionally from narrow unit fixtures that
mint balances to isolate a single contract.

## Generated sequences and invariants

`generated_generic_custody_bond_challenge_and_return_sequences` runs eight linked
rounds for each of three fixed seeds: `0x51a7`, `0xc011a7`, `0xb0ad`. Each round
uses prior balances and cursors. Choices vary the chain, amount, challenge
outcome, freeze timing and return amount; queue epochs advance explicitly. A
failure includes its seed and step in Boost's context. Re-run the named test to
reproduce the schedule. Keys are fresh fixture identities; economic choices are
seeded. This is a deterministic regression campaign, not an exhaustive fuzzer.

Assertions include:

- Supply equals the sum of all fixture holder balances, including contract custody.
- Supply plus pending yield never exceeds simulated custody in healthy scenarios.
- Replayed intake cannot mint twice; replayed returns cannot pay twice.
- Invalid adjudication restores pre-intake supply while retaining released user funds.
- Partial release stops during challenge and resumes only after a valid ruling.
- Frozen user redemptions have no supply side effect.
- Each required collateral asset must independently meet its minimum. Surplus in
  one cannot substitute for the other, and withdrawal reservation removes eligibility
  before maturity moves funds.
- Claims return the right asset and exact amount, preserving the other asset's balance.
- Generic yield is held before bonding, converted by the real AMM, and pays the exact
  indexed WIRE claim; syndication/redemption fees are independently floor-rounded.
- A reported custody deficit pulls the cord and retains evidence; repairing custody
  and clearing the cord resumes release without erasing the incident.

Add small named regressions for failures found by larger campaigns rather than
relying only on a random seed.

## Governance shortcut parity

`generic_governance_and_provider_release_have_identical_settlement` compares
unbonded `sysio.bond::rslvvalid` with real funded `accept`, followed by ordinary
window-gated `approve` and collateral claim. Both EC1/NTA and EC2/NTB run with
linked and initially unlinked recipients. The test compares the economic trace
through frozen release, successive partial releases, same-epoch no-refill, fee
rounding, principal/link delivery, pending yield, and actual return instructions
paid exactly once by the external model. It also rejects unauthorized governance
and early provider approval.

Only the downstream settlement trace is equivalent. Bond state, escrow ownership,
challenge timing, provider returns and bounty allocation deliberately differ.
Provider, challenge and INVALID native tests remain essential, as do the live
flows specifically exercising providers. The governance shortcut in transport
flows cannot stand in for their coverage. The parity regression is included in the regular `sysio_synd_tests` suite.

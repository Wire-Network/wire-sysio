# Depot flow coverage

The reviewed coverage inventory is [depot_flow_coverage.json](depot_flow_coverage.json).
It maps every enabled `wire-tools-ts` flow at the recorded feature-branch head to
specific native contract cases, describes its WIRE obligations, and states the
behaviors that still require a cluster. The map is a reviewable traceability
record, not a line-coverage percentage or a proof that every possible sequence is covered.

## Test layers

1. **Contract boundary tests** exercise production WASM, authorization, arithmetic,
   rejection, replay, pagination and individual state transitions. The existing
   bond, syndication, collateral, emissions and producer suites remain essential.
2. **Depot integration tests** exercise inline calls between the real contracts.
   New generic scenarios cover backed import, held intake, bonding, partial
   release, challenge adjudication, freeze/repair, fee rounding, link delivery,
   yield conversion, reward funding and return instructions.
3. **Cluster flows** retain responsibility for external contract execution,
   operator daemons, networking, finality, actual producer participation and soak
   stability. Native virtual-time tests cannot establish those properties.

`sysio_dispatch_tests/generic_external_simulator_drives_production_dispatch`
feeds canonical protobuf envelopes into real `sysio.msgch`, its epoch consensus
path, `sysio.synd` and `sysio.liq` for both transport families. The reward dispatch
case additionally exercises `sysio.dclaim -> sysio.system -> sysio.token`.
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

The map also selects focused edge cases for FIFO/budget saturation, nonzero
challenge fees/bounties, retained outcomes after pruning, frozen budget accounting,
producer scoring and authorization. Add small named regressions for failures found
by larger campaigns rather than relying only on a random seed.

## Running

From `wire-sysio`, build the default target using the repository's configured
build procedure, for example:

```sh
cmake --build build/release -- -j12
python3 contracts/tools/verify_depot_flow_coverage.py
python3 contracts/tools/verify_depot_flow_coverage.py --tools-root ../wire-tools-ts
```

Use the correct feature checkout for `--tools-root` (a separate worktree may have a
different path). Flow enablement uses the same rule as `scripts/run-flow.mjs`: a
`packages/flow-*/package.json` with a nonempty `scripts.test`. Newly enabled,
disabled or renamed flows require a reviewed map update. Standalone sysio CI
validates the committed map and native references without requiring a sibling repo.
This check detects inventory drift; reviewers must still assess changed assertions.

Run the focused native mirror set:

```sh
flow_filter=$(python3 contracts/tools/verify_depot_flow_coverage.py --filter)
build/release/contracts/tests/contracts_unit_test \
  --run_test="$flow_filter" --report_level=detailed -- --sys-vm
```

Run the broader contract gate before publishing:

```sh
build/release/contracts/tests/contracts_unit_test --report_level=detailed -- --sys-vm
ctest --test-dir build/release -R '^depot_flow_coverage_map' --output-on-failure
```

Run tests only after the build completes, with no concurrent build in that directory: the
default build can clean and replace contract WASM while regenerating dependencies.
These commands use the WASM/ABI under the selected build directory. A passing old
binary or stale contract artifact is not evidence for the edited source. Record
the branch head, dirty test changes, build completion, runtime, case/assertion
counts and any artifact differences with the validation result. No fresh external
cluster is needed for these native tests.

## Validation record — 2026-10-02

Validated on `feature/syndication-underwriting`, based on
`5bbb0c5e4d57e6133181a3d9dc4b58a0ddef14ed` plus the test changes in this patch.
The default all-target build completed, and tests used the freshly built WASM/ABI.
The final full run used a stable build directory with no concurrent build.

| Gate | Result | Elapsed |
| --- | --- | --- |
| All native contract cases (`--sys-vm`) | 1,087 cases; 248,033 assertions passed | 10m 37s |
| Eleven new simulator/integration cases | 2,911 assertions passed | 3.64s |
| Coverage-map CTest checks | Both passed, including five validator regression tests | 0.11s |
| Enabled-flow comparison against recorded tools head | All 13 flows mapped to 67 existing native cases | — |

Broad validation also exposed two old fixture assumptions. Dispute deployment now
produces a block between contracts to avoid exhausting cumulative block NET.
Multisig chunk tests now use bounded valid WASM padding instead of depending on
production `sysio.system` size. Their complete suites passed in the final gate.
No production contract changes were needed for these fixture repairs.

The external cluster flows were not rerun for this test-only patch. The generated
campaign covers 24 linked rounds across three seeds; this result does not claim
exhaustive exploration or a measured production line-coverage percentage.

## Governance shortcut parity

`generic_governance_and_provider_release_have_identical_settlement` compares
unbonded `sysio.bond::rslvvalid` with real funded `accept`, followed by ordinary
window-gated `approve` and collateral claim. Both EC1/NTA and EC2/NTB run with
linked and initially unlinked recipients. The test compares the economic trace
through frozen release, successive partial releases, same-epoch no-refill, fee
rounding, principal/link delivery, pending yield, and actual return instructions
paid exactly once by the external model. It also rejects unauthorized governance
and early provider approval. All 1,824 assertions passed; the complete syndication
and bond suites passed 152 cases and 12,311 assertions.

Only the downstream settlement trace is equivalent. Bond state, escrow ownership,
challenge timing, provider returns and bounty allocation deliberately differ.
Provider, challenge and INVALID native tests remain essential, as do the live
flows specifically exercising providers. The governance shortcut in transport
flows cannot stand in for their coverage. The parity regression is included in the regular `sysio_synd_tests` suite.

# Retired OPP carrier audit

This is the follow-up to the external settlement retirement and merged PR #670.
The earlier pass removed the implementations but left some shared schemas,
serializers, enum reflections, and descriptions advertising the retired flows.
The audit below distinguishes a payload's name from an executable producer or
settlement handler. Searches covered tracked source in Sysio, Ethereum, Solana,
Tools, Libraries, and Hub; generated build output was checked separately.

## Removed by this follow-up

| Surface | Evidence for retirement | Removal |
| --- | --- | --- |
| Reserve creation cancellation and matching | `sysio.reserv` was deleted in `e9ac87c194`; the remaining msgch cases were no-ops. Solana dispatch no longer handles these messages; Ethereum registers no handlers. | `ReserveCreateCancel`, `ReserveCreateCancelled`, `ReserveReady`, and their discriminants |
| External reserve reporting and custody registry | No producer remains; msgch's balance-sheet case did not decode or mutate state. `Reserve` and `ReserveAmount` only fed the retired schemas and unused serializers. | `ReserveBalanceSheet`, `ReserveDisbursement`, `Reserve`, `ReserveAmount`, `ReserveStatus` |
| Cross-chain emissions-block notification | The schema explicitly marked it as no longer emitted. Readiness and refusal diagnostics are depot-local in `sysio.epoch::blocklog`. | `EmissionsBlocked` and its discriminant; retain `EmissionsBlockReason` |
| Legacy pretoken purchase/yield | Ethereum's purchase/yield senders are deleted (documented in `StakingManager.sol`); remaining references are helper imports, display decoders, and classifications. | `PretokenPurchase`, `PretokenYield`, `WireTokenPurchase`, and their discriminants |
| Cross-chain swaps and dual-commit underwriting refunds | `sysio.reserv` and `sysio.uwrit` were deleted in `e9ac87c194`; remaining Sysio cases were no-ops. Solana dispatch handles only rosters, operator actions, and desyndication; Ethereum registers no corresponding settlement handlers. Remaining consumer references are retired classifications, display utilities, and fixtures. | `SwapRequest`, `UnderwriteIntentCommit`, `SwapRemit`, `SwapRevert`, `DepositRevert`, their discriminants, serializers, reflections, and inert depot cases |
| Old underwriting states | The two enums have no executable producer or consumer; the deleted dual-commit implementation owned them. Native `sysio.bond` defines its own request and bond states. | `UnderwriteRequestStatus`, `UnderwriteStatus` |
| Three-field node-owner payload | BAR emits `NodeOwnerRegistration`; msgch and the Tools decoder consume that full payload. The three-field message had only a declaration and unused serializers. | `NodeOwnerReg`; retain `NODE_OWNER_REG` and `NodeOwnerRegistration` |
| OPP state wrapper | `ProtocolState` had only a declaration and unused serializers across all audited repositories. Envelope transport uses `Envelope` and its message/header types directly. | OPP `ProtocolState`; native chain `protocol_state_object` is unrelated |
| Dead dispatch work | `attestation_id` and `original_message_id` were never read by the complete dispatcher after swap dispatch retirement. Stored attestation identities and semantic header validation remain intact. | Remove the unused arguments and message-id reconstruction performed solely for them |
| Documentation residue | Relay comments and registry documentation still described removed reserves or the deleted underwriter daemon. Three unreferenced OPP diagrams describe an obsolete pre-refactor schema. | Refresh current comments and remove the obsolete diagrams |
| Underwriter outpost roles in the chain registry | `operator_registry_addr` (the underwriter's `uw_commit` target) and `source_deposit_addr` (its swap-deposit verify path) had no reader once the underwriter daemon was deleted; the batch-operator relay reads only `opp_addr` and `opp_inbound_addr`, and the shared `resolve_role_addr` helper had no caller. | Both `sysio.chains::outpost_addrs` fields, their validation, their shared field names, and `resolve_role_addr` |
| Staking lifecycle carriers | Ethereum's only STAKE/UNSTAKE producers, `StakingManager.sendStakeAttestation` and `sendUnstakeAttestation`, are internal and have no caller (`stakeLiqETH` is a `_notWired` stub). Nothing produces `StakeUpdate` or `StakeResult`, msgch dropped all four types in one deferred case, and Solana listed them only to keep its classification exhaustive. | `PretokenStakeChange`, `StakeUpdate`, `StakeResult`, `StakeStatus`, the STAKE, UNSTAKE, STAKE_UPDATE and STAKE_RESULT discriminants, their serializers, reflections and depot case |
| Attestation processing error report | No chain produces `AttestationProcessingError`; msgch ignored it and the Tools codec only displayed it. | `AttestationProcessingError` and its discriminant |
| Challenge attestations | No chain produces CHALLENGE_REQUEST or CHALLENGE_RESPONSE; msgch dropped both, Solana listed them only to keep its classification exhaustive, and Tools only displayed them. `sysio.chalg` disputes run on `DisputeStatus` through evalcons, not on inbound attestations. | `ChallengeRequest`, `ChallengeOperatorHash`, their discriminants, serializers and depot case |
| Unused schema declarations | `ChainKeyType`, `ChainSignature`, `WirePermission`, `ChainRequestStatus`, `ChallengeStatus` and `DebugEnvelopeDataRecord` had only declarations, enum reflections or uncalled serializers. `Chain`, `Token` and `ChainToken` were described as the registry's carriers, but `sysio.chains` and `sysio.tokens` store their own row structs and nothing encodes these messages. | The declarations, their reflections and serializers; `ChainKind` and `TokenKind` stay |
| Swap-fee emission audit fields | The `sysio.reserv` rewards-bucket sweep that fed them was deleted; `payepoch` wrote both as constant zero. | `epochlog.fee_distributed`, `epochlog.batch_fee_retained`, and the fee-sweep descriptions |
| Dead depot helpers | msgch's operator-address resolver and its payload-code canonicality check served only the retired operator/swap/reserve ingress (the live LIQ paths reject unregistered codes through the token registry); the `uwreqs` locked-amount row type had no table; the underwriter-challenge declarations in `sysio.chalg` were gone but their documentation remained. | `resolve_account_from_op_address`, `payload_codes_canonical`, `opp_table::locked_amount_t`, the orphaned challenge documentation, and the dispatch suite's swap-race fixture configuration |

No compatibility exports, replacement enum members, or reserved slots are added.
Existing active discriminants and payload field numbers keep their values.

## Retained live or planned surfaces

- `OperatorAction` and `OperatorActionLog` record native collateral changes in
  `sysio.opreg`. They are audit records: the depot queues no OPERATOR_ACTION
  attestation, and outposts learn a slashed status from the OPERATORS roster.
  Removing external custody does not remove this audit/eligibility protocol.
  The existing `reserve_code` field stays zero in depot-native audit records;
  changing the live table payload is outside this carrier cleanup.
- `StakingManager.sol` remains the staking-track placeholder. The post-launch
  staking design defines its own attestations; none of the removed carriers is
  reserved for it.
- `sysio.chalg` owns live depot disputes (`DisputeStatus`, opened by evalcons);
  only the producer-less challenge attestations were removed.
- Operators, batch-operator groups, node-owner registration, LIQ syndication,
  yield, and desyndication remain active. Their schemas and routing are retained.
- Chain/token registries, native swaps and their AMM helper, native collateral,
  envelope consensus, and opaque unknown-attestation handling remain active.
  `sysio.swap` and everything it consumes (`amm_math`, `twap`, `shadow_yield`,
  `sysio.andon`) keep their code unchanged: that implementation is not final.
  Only `amm_math`'s comments, and its test's, stop naming the deleted
  `sysio.reserv` / `sysio.uwrit` as its callers and fee authorities.

## Consumer release coordination

The four open companion PRs (#102 Libraries, #116 Tools, #226 Ethereum, #568
Solana) belong to the #670 wave and are unchanged by this follow-up. Land that
wave first. The OPP Bundles workflow publishes on a `master` push that touches
`libraries/opp/**`, so merging this follow-up publishes a new model API. Prepare
the subsequent consumer changes before merging it and coordinate their landing
and the SDK regeneration/release as a separate wave:

| Consumer | Remaining work for the new model API |
| --- | --- |
| Libraries | Regenerate contract declarations from the new producer ABI, including the two-field `sysio.chains` outpost address struct (`chains/Structs.ts`, `Types.ts`, `Actions.ts`, `Client.ts`) and the `sysio.system` `epochlog` row; remove the unused root OPP package dependency, which contradicts the generator/output boundary. |
| Tools | Remove retired display imports/routes (including `StakeUpdate`, `StakeResult`, `AttestationProcessingError`) and the retired swap-revert scanner; stop writing `operator_registry_addr` / `source_deposit_addr` in `RegistrySteps.ts` and its tests. Verify unknown entries still render raw beside active entries. |
| Ethereum | Remove dead pretoken helper imports/unions and the emissions-notification negative fixture; replace removed enum references in retired-handler assertions with opaque wire values. Remove `StakingManager`'s uncalled STAKE/UNSTAKE senders, `nativeTokenCode`, and the deploy script's STAKE/UNSTAKE endpoint roles; OPP routing tests use live types. |
| Solana | Remove retired enum classification/correlation code (including the staking and processing-error variants) and stale settlement test branches. Retain active desyndication replay checks, operator status mirroring, roster/consensus tests, and liquid-staking reserve-pool accounts. |

Ethereum's unused reserve/swap custom errors and Solana's corresponding error
variants, comments, and old fixtures also deserve cleanup in that later consumer
wave. Liquid-staking reserve pools are live custody and must not be confused with
the retired cross-chain `Reserve` protocol.

Opening this PR does not publish packages or edit the existing companion branches.
Its generated-model build can be validated locally independently of adopting the
new packages in those consumers.

## Regression checks

Generated C++ descriptors must omit retired payloads while retaining current
node-owner, staking, collateral audit, and LIQ messages. An opaque malformed
attestation round-trip must preserve a following decodable LIQ yield entry.
Contract dispatch receives all removed wire values alongside a real syndication:
no collateral/reward state changes, the live intake succeeds, and the envelope
continues. Rebuild host/CDT and Solidity/TypeScript/Solana model bundles through
the normal Sysio build, copy rebuilt contract ABI/WASM artifacts back to source,
and verify the removed declarations are absent from all generated model APIs.

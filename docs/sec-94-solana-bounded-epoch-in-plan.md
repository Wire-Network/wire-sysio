# SEC-94 Solana Outpost Bounded Epoch-In Plan

Status: revised design plan, not implementation

Related issue: SEC-94 / WSA-212

This plan supersedes the earlier finalization paging proposal. The paging
proposal introduced a post-consensus crank/page action. That shape is rejected:
Solana outpost consensus must continue to hold inside the existing
consensus-reaching `epoch_in` transaction. The fix should make each outbound
envelope safe before any batch operator delivers it to Solana, and it should
prefer a small pre-consensus Solana program change if that materially simplifies
the safety proof.

Primary repositories:

- `Wire-Network/wire-sysio`: WIRE OPP envelope builder, Solana relay/client,
  and Solana transaction builder.
- `Wire-Network/wire-solana`: Solana outpost program, Anchor IDL, and
  `epoch_in` remaining-account contract.

Reference inspected for this plan:

- `Wire-Network/wire-solana` `origin/next` at
  `48c2d968d7ce09b29db144f3f9de61a27b677cd8`.
- `wire-sysio` SEC-94 worktree on branch `fix/sec-94`.

## Design Constraint

There must be no separate crank action for Solana finalization.

Current `wire-solana origin/next` intentionally performs all of the following
inside the final `epoch_in` chunk:

1. assemble the operator's envelope chunks;
2. record the operator delivery in `EpochDeliveries`;
3. determine whether primary or fallback consensus has been reached;
4. when consensus is reached, advance the outpost epoch cursor;
5. process every inbound attestation inline;
6. emit the next outbound envelope by calling `emit_outbound_inner`;
7. close the operator's chunk buffer.

WIRE also gates the inbound delivery path before consensus:

- `batch_operator_plugin` pushes `sysio.msgch::deliver`;
- `sysio.msgch::deliver` calls `is_batch_operator_active`;
- `is_batch_operator_active` requires the caller to be in the active
  `sysio.epoch` batch-op group.

That active-group gate bounds the consensus participants. It does not by itself
bound every Solana account referenced by the envelope's effects.

## Problem Description

`wire-sysio` delivers WIRE-to-Solana OPP envelopes through
`outpost_solana_client::deliver_outbound_envelope`. The relay splits the
encoded envelope into `epoch_in` chunks. Non-final chunks only write bytes to
the per-operator `EnvelopeChunks` PDA. The final chunk sends all dynamic
Solana accounts needed by inline finalization as Anchor `remaining_accounts`.

Current dynamic accounts include:

- operator wallets for `OPERATOR_ACTION(WITHDRAW_REMIT)`;
- depositor wallets for `DEPOSIT_REVERT`;
- recipient wallets for `SWAP_REMIT`;
- Reserve PDAs derived from `(token_code, reserve_code)`;
- SPL reserve vault PDAs;
- SPL recipient associated token accounts;
- SPL mint accounts;
- the SPL Token Program id when SPL transfers are present;
- reserve lifecycle accounts used by `RESERVE_READY` and
  `RESERVE_CREATE_CANCELLED`.

The existing WIRE envelope builder, `sysio.msgch::buildenv`, only budgets the
encoded OPP envelope size. It packs a stable prefix of READY attestations until
the encoded envelope is at most `MAX_ENVELOPE_BYTES` (`65,536` bytes). This is
necessary but not sufficient for Solana because account metas also consume
transaction packet bytes and instruction account indices.

The lower-level Solana transaction builder currently compiles account-key
indices into `uint8_t` without a guard. Any transaction with more than 256
unique account keys cannot be represented correctly. The raw transaction packet
limit is stricter in practice and can fail with far fewer accounts.

## Proof That Active-Group Gating Is Insufficient

Active-group gating bounds who can deliver the envelope. The failure mode is
the number of distinct effect accounts inside a valid envelope.

Using `protoc` against the current OPP protobuf schema:

- a minimal `OPERATOR_ACTION(WITHDRAW_REMIT)` body with one SVM pubkey is
  50 bytes;
- a minimal `SWAP_REMIT` body with one SVM recipient, one SVM underwriter, and
  a 32-byte original message id is 122 bytes;
- a minimal `DEPOSIT_REVERT` body with one SVM depositor and a 32-byte original
  message id is 80 bytes;
- a minimal `RESERVE_READY` body is 4 bytes.

Measured native `SWAP_REMIT` envelope sizes:

| `SWAP_REMIT` entries | Encoded envelope bytes | Final chunk bytes | Native final extras |
| ---: | ---: | ---: | ---: |
| 8 | 1,077 | 405 | 16 |
| 16 | 2,141 | 125 | 32 |
| 128 | 17,040 | 240 | 256 |
| 400 | 53,488 | 400 | 800 |

For native SOL `SWAP_REMIT`, each unique swap needs at least two dynamic
accounts on the final chunk: the recipient wallet and the Reserve PDA. The
current final chunk already carries one ComputeBudget pre-instruction. Current
code comments budget the final no-extra transaction as:

```text
492 bytes static overhead + 40 bytes ComputeBudget pre-ix + final_chunk <= 1232
```

Each unique dynamic account adds at least:

```text
32 bytes account key + 1 byte instruction account index
```

Therefore 8 native swaps have a lower-bound final transaction size of:

```text
492 + 40 + 405 + (16 * 33) = 1465 bytes
```

That already exceeds Solana's 1232-byte raw packet budget while the envelope is
only 1,077 bytes. The 128-swap case stays well under the 65 KiB envelope cap but
requires 256 dynamic extras before counting the static `epoch_in` accounts, so
it also exceeds the 256-key account-index limit.

Conclusion: the current byte-only envelope cap does not prove the final
`epoch_in` transaction is representable. SEC-94 is a real account-budget issue,
but it should be fixed before consensus delivery, not by adding a post-consensus
crank.

## Proposed Protocol Change

Change the WIRE-to-Solana envelope packing invariant:

Current invariant:

```text
encoded_envelope_bytes <= MAX_ENVELOPE_BYTES
```

New invariant for SVM destination chains:

```text
encoded_envelope_bytes <= MAX_ENVELOPE_BYTES
and
estimated_final_epoch_in_account_keys <= SVM_FINAL_EPOCH_IN_KEY_LIMIT
and
estimated_final_epoch_in_packet_bytes <= SVM_FINAL_EPOCH_IN_PACKET_LIMIT
```

This is a protocol behavior change, but not necessarily an OPP protobuf schema
change. The existing OPP envelope already carries the attestations. The change
is how `sysio.msgch::buildenv` chooses the prefix of READY attestations for an
SVM outpost.

The estimator is consensus-critical. It must be a guaranteed upper bound on the
real final-transaction account count and packet bytes, including safety margin.
Under-packing is acceptable because it only reduces throughput. Over-packing is
unsafe because `buildenv` has already committed the PROCESSED/READY split and
the pending outbound envelope. If that committed envelope cannot be represented
as a Solana transaction, every batch operator will see the same unrepresentable
envelope and the system needs an explicit recovery path.

Overflow behavior should mirror the existing byte-cap behavior:

- build the largest stable prefix that fits all applicable limits;
- mark only included attestations `PROCESSED`;
- leave non-included READY attestations in the table;
- deliver those remaining attestations in later epochs;
- never drop, reorder, or post-consensus crank them.

This keeps all batch operators deterministic. Every operator reads the same
bounded `outenvelopes` row, delivers the same bytes, and consensus/finalization
still happens in the ordinary `epoch_in` path.

## Preferred Crank-Free Shape

The cleanest version of this plan makes one small Solana program/API change:
separate the last byte-upload from the consensus/finalization call.

Today the final `epoch_in` transaction carries both:

- the last data chunk; and
- every dynamic effect account needed by inline finalization.

That coupling makes the packet budget depend on `encoded_envelope_size %
SOLANA_MAX_CHUNK_BYTES`. With the current `672` byte chunk size, the dynamic
account budget is approximately:

```text
floor((1232 - 492 - 40 - final_chunk_bytes) / 33)
```

So a near-empty final chunk allows about 21 dynamic accounts, while a near-full
final chunk allows about 0. The result is a sawtooth budget: removing one
attestation can make `final_chunk_bytes` jump upward by hundreds of bytes, even
though the dynamic account count dropped. A naive "pop one and retry" loop must
not assume this estimate is monotonic.

Preferred fix:

- upload all non-empty envelope chunks first;
- submit a terminal `epoch_in` mode with zero chunk data, or an
  `epoch_in_finalize` instruction, that consumes the already-complete
  `EnvelopeChunks` buffer and carries the dynamic effect accounts;
- perform the existing delivery record, consensus check, attestation dispatch,
  outbound emit, and chunk-buffer close inside that terminal call.

This is not a post-consensus crank. It is the final pre-consensus delivery step
for that operator, and consensus still becomes true inside the same
`epoch_in`-family transaction. The benefit is that the dynamic-account budget
is stable and monotonic, roughly 21 accounts before safety margin, independent
of envelope byte alignment.

If the team rejects this small Solana change, `sysio.msgch::buildenv` must use
an exhaustive descending-prefix search or another algorithm that handles the
non-monotonic modulo budget correctly. It must not rely on the existing byte
trim loop's monotonic intuition.

## Throughput Decision

The bounded-envelope fix converts a hard final-chunk failure into intentional
READY backlog. That is safer, but it creates a real throughput ceiling for
Solana-bound value transfers.

Using the current final-chunk shape:

- near-empty final chunk: about 21 dynamic accounts;
- native `SWAP_REMIT`: about 2 dynamic accounts per swap, so roughly 9 native
  swaps per epoch after safety margin;
- SPL `SWAP_REMIT`: up to about 6 dynamic accounts per swap, so roughly 3 SPL
  swaps per epoch after safety margin;
- near-full final chunk: about 0 dynamic accounts.

With the preferred zero-data terminal finalize shape:

- the budget is stable at about 21 dynamic accounts before safety margin;
- native swap throughput is still single-digit per epoch unless the account
  set is further reduced;
- SPL swap throughput remains lower because each swap needs more accounts.

Before implementation, the team should size expected Solana-bound withdrawals,
deposit reverts, and swap remits against this ceiling. If bursts of dozens of
Solana-bound value transfers per epoch are realistic, bounded packing alone
will avoid wedges but can still produce unacceptable withdrawal latency. In
that case the team should either:

- implement the zero-data terminal finalize shape and accept the remaining
  ceiling as sufficient;
- adopt Address Lookup Tables as a later transaction-format upgrade; or
- redesign specific value-transfer effects so one transaction needs fewer
  dynamic accounts.

## Account Budget Model

Add an SVM budget estimator to WIRE-side envelope packing. The estimator should
be conservative: it may under-pack, but it must not over-pack. The source of
truth for the no-extra transaction size must be generated from the actual
`epoch_in` IDL/program client layout, not copied from a comment. The current
`492` byte number is evidence, not a consensus parameter definition.

Inputs available to the WIRE contract:

- destination chain row and chain kind;
- candidate READY attestations for the outpost;
- protobuf attestation type and data;
- the serialized candidate envelope bytes after each pack attempt;
- token/reserve metadata if the contract chooses to read it;
- stable constants matching the current Solana IDL and relay transaction
  construction.

Recommended hard constants:

- `SVM_MAX_TX_PACKET_BYTES`: raw Solana packet ceiling used for final
  `epoch_in`; use `1232` bytes minus a project safety margin. Do not use the
  observed `1644` base64-encoded transport length as a raw packet budget.
- `SVM_MAX_ACCOUNT_KEYS`: 256 account-index ceiling, with any project safety
  margin applied before comparison.
- `SVM_FINAL_EPOCH_IN_STATIC_ACCOUNT_KEYS`: fee payer, declared `epoch_in`
  accounts, outpost program id, system program, and ComputeBudget program.
- `SVM_FINAL_EPOCH_IN_STATIC_TX_BYTES`: conservative no-extra terminal
  transaction overhead, generated from the real client layout and pinned by
  tests.
- `SVM_FINAL_EPOCH_IN_PER_EXTRA_ACCOUNT_BYTES`: at least 33 bytes, plus any
  extra slop needed for compact-u16 length expansion.
- `SVM_FINAL_EPOCH_IN_SAFETY_BYTES`: fixed safety margin for IDL drift and
  compact-length boundaries.

Per-attestation effect-account estimates:

| Attestation | Conservative dynamic account estimate |
| --- | --- |
| `OPERATOR_ACTION(WITHDRAW_REMIT)` | unique SVM `op_address` wallet |
| `DEPOSIT_REVERT` | unique SVM depositor wallet |
| `SWAP_REMIT` native lower bound | unique recipient wallet plus Reserve PDA |
| `SWAP_REMIT` SPL worst case | recipient wallet, Reserve PDA, reserve vault PDA, recipient ATA, mint account, and shared token program |
| `SWAP_REVERT` | source reserve/depositor accounts, plus SPL vault/mint/ATA/program accounts when applicable |
| `RESERVE_READY` | Reserve PDA |
| `RESERVE_CREATE_CANCELLED` | Reserve PDA plus refund accounts that the Solana handler may need |

The first implementation can use an intentionally pessimistic SVM estimate for
token effects if exact native-vs-SPL classification is not cheaply available to
`sysio.msgch`. Pessimistic packing is acceptable because it only rolls more
READY attestations into the next epoch. Over-packing can wedge delivery.

The estimator should dedupe by semantic account identity where deterministic:

- SVM wallet bytes for recipient/depositor/operator accounts;
- `(token_code, reserve_code)` for Reserve PDAs;
- `(token_code, reserve_code)` for reserve vault PDAs;
- `(recipient, token_code)` for recipient ATAs when token classification is
  known;
- token code for mint accounts when token classification is known;
- singleton program ids.

Dedup is safe only when the contract can prove that two references collapse to
the same on-chain account that the relay will also dedupe. If exact derivation
is unavailable in the contract, dedupe by the stable fields that determine the
PDA or account. If the stable identity is uncertain, over-count. The estimator
must model the final union size, and the required parity fixture must assert:

```text
contract_estimated_dynamic_accounts >= relay_actual_dynamic_account_union
contract_estimated_packet_bytes     >= relay_actual_packet_bytes
```

The constants above are consensus parameters. Changing them changes which
attestations become PROCESSED versus remain READY. A change to the Solana IDL,
the terminal `epoch_in` account list, chunk sizing, or transaction format must
therefore be treated as a coordinated contract/relay/program rollout.

## Recovery And Escape Hatch

The relay-side guard is a diagnostic tripwire, not a liveness recovery path. If
the WIRE contract over-packs, the pending outbound envelope is already
committed. A relay guard firing in production means every honest operator will
reject the same envelope locally. That is a Sev1 condition, not graceful
degradation.

Add a governance-controlled recovery action for pending Solana-bound outbound
envelopes that are proven unrepresentable. The recovery action should:

- require privileged governance or emergency authority;
- operate only on a pending outbound envelope that has not finalized on the
  destination outpost;
- parse the pending raw OPP envelope and reconstruct its attestation entries;
- recreate those entries as READY in the original order, or directly re-pack
  them under corrected budget constants;
- mark the bad pending envelope as abandoned or errored so batch operators stop
  trying to deliver it;
- preserve auditability with the old envelope hash, replacement envelope hash,
  reason, timestamp, and actor;
- avoid silently dropping any value-transfer attestation.

This action is not part of the happy path and should not be used for routine
backlog management. It exists because the estimator is consensus-critical and a
bad constant otherwise creates a permanent wedge.

## Wire-Sysio Changes

### `contracts/sysio.msgch`

Extend `buildenv(chain_code)` so the existing pack-and-trim loop checks both
the byte cap and the SVM final-transaction budget.

Proposed flow:

1. Read the destination chain row before packing.
2. Collect READY attestations in existing stable order.
3. Build the candidate prefix using the current byte estimator.
4. Serialize the candidate envelope.
5. If destination is not SVM, keep current byte-only behavior.
6. If destination is SVM, compute:
   - terminal transaction payload length. With the preferred zero-data terminal
     finalize shape, this is zero. Without that Solana change, it is
     `encoded_size % SOLANA_MAX_CHUNK_BYTES`, treating exact multiples as a
     full final chunk;
   - estimated dynamic account count for included attestations;
   - estimated total static plus dynamic account-key count;
   - estimated final transaction packet bytes.
7. If either SVM limit is exceeded, shrink the candidate prefix and retry. If
   the final-chunk-carries-data shape remains, use an algorithm that handles
   the non-monotonic modulo budget correctly.
8. Mark only the final included candidates as `PROCESSED`.
9. Leave omitted candidates READY for the next epoch.
10. Provide the governance recovery action for unrepresentable pending
    envelopes.

Add tests that prove:

- non-SVM destinations retain current byte-cap behavior;
- SVM buildenv does not pack the 8-native-`SWAP_REMIT` packet counterexample
  into one envelope;
- SVM buildenv does not pack the 128-native-`SWAP_REMIT` account-key
  counterexample into one envelope;
- omitted READY attestations remain READY and preserve ordering;
- if one single attestation exceeds the conservative budget, it does not DoS
  the whole outpost queue. The preferred behavior is to mark that attestation
  as a dead-letter/error state with an explicit refund or remediation policy,
  not to hard-abort `buildenv` forever.
- the governance recovery action can reconstruct READY attestations from a
  pending bad envelope without dropping or reordering them.

### `plugins/outpost_solana_client_plugin`

Keep the relay's final chunk behavior: append dynamic accounts only on the
final `epoch_in` chunk.

Add defensive guards before submitting the final chunk:

- compute the final account list after `resolve_accounts` plus extras;
- fail locally with an explicit `fc::exception` if unique account keys exceed
  the supported account-index limit;
- serialize or estimate the transaction packet and fail locally if it exceeds
  the configured packet limit;
- include the envelope epoch, chunk index, static account count, dynamic extra
  count, estimated packet bytes, and first few attestation ids/types in logs.

These guards are not the primary liveness fix. They catch drift between the
WIRE contract budget model, relay extraction, and Solana IDL changes before the
builder wraps `uint8_t` account indices or submits an opaque RPC failure. If
they fire in production, the committed envelope is already bad and the
governance recovery action is required.

Also update the extractor tests:

- native `SWAP_REMIT` derives recipient plus Reserve PDA;
- SPL `SWAP_REMIT` derives the additional SPL accounts;
- duplicated recipients/reserves are deduped;
- generated account-budget fixtures match the contract-side estimator;
- CI fails if the relay's actual dynamic account union exceeds the contract
  estimate for representative native and SPL envelopes.

### `libraries/libfc/src/network/solana`

Add a hard guard in `solana_client::create_transaction` before building the
`std::map<solana_public_key, uint8_t>`:

```text
account_keys.size() <= 256
```

Fail with a clear exception if the limit is exceeded. This is a correctness
guard for all Solana callers, independent of SEC-94. It prevents silent
`uint8_t` truncation.

If there is already a packet-size guard elsewhere in the send path, reuse it.
Otherwise add a clear pre-submit guard after serialization and before RPC
submission.

## Wire-Solana Changes

Do not add `process_finalization_page`, `complete_finalization`, or any other
post-consensus finalization crank.

Expected Solana-side changes are intentionally small but should not be limited
to docs if the team accepts the preferred shape:

- add a terminal zero-data `epoch_in` mode or a pre-consensus
  `epoch_in_finalize` instruction that consumes a complete chunk buffer and
  performs the existing delivery/consensus/finalization path;
- keep this terminal call in the normal operator delivery flow, before
  consensus is reached, not as a post-consensus page/crank;
- update the Anchor IDL and relay account resolution for the terminal call.

Additional Solana-side work:

- document the remaining-account contract for every attestation branch in
  `epoch_in.rs`;
- add tests or fixtures that define the expected remaining-account set for
  `SWAP_REMIT`, `DEPOSIT_REVERT`, `SWAP_REVERT`, `RESERVE_READY`, and
  `RESERVE_CREATE_CANCELLED`;
- expose stable constants in Rust tests/docs for:
  - declared final `epoch_in` accounts;
  - final-chunk ComputeBudget pre-instruction requirement;
  - maximum accepted final dynamic accounts under the current IDL;
- keep missing effect-account behavior aligned with the existing
  "do not stall consensus" rule.
- fix stale comments that still say `MAX_CHUNK_BYTES = 768`; the current
  value is `672`.

If the Solana program adds any new required finalization accounts, the
wire-sysio budget constants and tests must be updated in the same release.

## OPP Protobuf Impact

No OPP protobuf schema change is required for the primary fix.

The existing OPP protobufs already express every attestation. SEC-94 is about
how many READY attestations may be packed into a single Solana-bound envelope,
not about missing wire fields.

A protobuf change should only be considered if the team wants to publish an
explicit cross-chain "execution budget" attestation or metadata field. That is
not needed for the bounded packing fix and would add avoidable protocol
surface.

## Recovery Semantics

The recovery point moves earlier.

Before this fix:

- `buildenv` can emit a byte-valid but Solana-unrepresentable envelope;
- earlier `epoch_in` chunk transactions may succeed;
- the final chunk fails;
- retries rebuild the same final transaction and can fail forever.

After this fix:

- `buildenv` emits only envelopes whose terminal `epoch_in` transaction is
  within the conservative Solana budget;
- excess attestations stay READY and are emitted in later epochs;
- operators do not need a post-consensus crank;
- Solana consensus/finalization remains atomic inside `epoch_in`;
- relay-side guards produce explicit local errors if code drift violates the
  budget model;
- if a bad envelope is nevertheless committed, governance has an explicit
  recovery path to reconstruct or re-pack the pending attestations.

## Validation Plan

### Focused Contract Tests

Add or extend `sysio.msgch` tests to construct Solana-bound READY attestations:

- `buildenv_svm_limits_native_swap_remit_by_packet_budget`;
- `buildenv_svm_limits_native_swap_remit_by_account_key_budget`;
- `buildenv_svm_keeps_omitted_attestations_ready`;
- `buildenv_non_svm_preserves_existing_byte_cap_behavior`;
- `buildenv_svm_single_attestation_over_budget_deadletters_or_errors`;
- `buildenv_svm_recovery_requeues_bad_pending_envelope`;
- `buildenv_svm_estimator_is_upper_bound_for_relay_fixtures`.

### Focused Relay Tests

Add `outpost_solana_client_plugin` tests for:

- exact extraction/dedupe of dynamic accounts from packed envelopes;
- explicit account-key overflow failure before `uint8_t` index compilation;
- explicit packet-size overflow failure before RPC submission;
- parity fixture between contract budget estimates and relay extraction for
  representative native and SPL envelopes.

The parity fixture is mandatory CI coverage, not optional test polish. It
duplicates a consensus-critical transaction layout across `wire-sysio`,
`wire-solana`, and the relay extractor. A drift in the Solana account set must
fail CI before it can produce a committed unrepresentable envelope.

### Libfc Solana Tests

Add `test_fc` coverage for:

- `create_transaction` rejects more than 256 account keys;
- a boundary case at exactly 256 keys behaves as expected;
- packet-size guard reports the serialized byte count.

### Wire-Solana Tests

Add Rust/Anchor tests or fixtures that:

- build the real terminal no-extra `epoch_in` transaction shape and report the
  raw serialized byte count used by the WIRE-side constant;
- prove the terminal zero-data finalize mode, if implemented, consumes a
  complete buffer and reaches consensus/finalization in the same handler path;
- fail when the declared terminal `epoch_in` account set changes without
  updating the shared budget fixture.

### Integration Validation

Run focused test binaries first, then normal Release validation for the
affected targets. For `wire-sysio`, this should include at minimum:

- `plugin_test` cases for `batch_operator_plugin` and
  `outpost_solana_client_plugin`;
- `contracts_unit_test` or the owning contract test target for
  `sysio.msgch`;
- `test_fc` Solana transaction builder tests;
- broader Release `ctest` sweep when the implementation is ready for PR.

## Rollout Plan

1. Decide whether expected Solana-bound value-transfer volume fits the
   conservative throughput ceiling. If it does not, include the terminal
   zero-data finalize change or an ALT plan in the release.
2. Land `wire-solana` terminal-call changes and generated budget fixtures if
   the team accepts the preferred shape.
3. Merge and deploy the WIRE-side account-budget-aware `buildenv` change with
   the same constants used by the relay/program fixtures.
4. Deploy relay binaries with the defensive terminal-call guards.
5. Treat budget constants as consensus parameters. Any later account-list,
   packet-format, or chunk-size change requires a coordinated
   contract/relay/program rollout.
6. Monitor logs for "SVM envelope budget trim" events to confirm that excess
   READY attestations are rolling to later epochs rather than causing final
   chunk failures.
7. Monitor READY backlog age and count for Solana-bound value transfers. A
   growing backlog means funds are delayed even though consensus is no longer
   wedged.

## Open Questions

1. What Solana-bound per-epoch volume is expected for withdrawals, deposit
   reverts, and swap remits? Does the conservative dynamic-account ceiling meet
   that requirement without unacceptable latency?
2. Should the release include the preferred terminal zero-data `epoch_in`
   shape, or should the first implementation absorb the non-monotonic
   final-chunk budget in `sysio.msgch`?
3. Should `sysio.msgch` use exact native-vs-SPL classification by reading token
   metadata, or should the first implementation pessimistically treat all SVM
   token effects as SPL worst case?
4. Should the contract expose a counter or log row when SVM budget trimming
   happens, so operators can observe that backlog is intentional?
5. What is the exact dead-letter or refund policy for a single
   attestation-over-budget case? Do not hard-abort the whole outpost queue.
6. Should address lookup tables be considered later for operator convenience?
   They are not part of this primary fix because the current client builds
   legacy transactions and consensus liveness should not depend on ALT
   availability.

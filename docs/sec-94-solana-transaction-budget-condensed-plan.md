# SEC-94 Solana Transaction Budget Fix - Condensed Plan

Status: condensed plan, not implementation

Related issue: SEC-94 / WSA-212

This document is a shorter companion to the detailed bounded-`epoch_in` plan.
It keeps only the problem, selected fix, validation, rollout, and remaining
decisions.

## Problem

WIRE sends WIRE-to-Solana outbound envelopes through
`outpost_solana_client::deliver_outbound_envelope`. The relay uploads the
encoded envelope to the Solana outpost in `epoch_in` chunks.

The current final `epoch_in` transaction is special:

- it carries the last envelope data chunk; and
- it carries every dynamic Solana account needed to process every attestation
  in the envelope as `remaining_accounts`.

Those dynamic accounts include recipient/depositor/operator wallets, Reserve
PDAs, SPL vaults, recipient ATAs, mints, and token program accounts. The exact
set depends on the attestations in the envelope.

Today `sysio.msgch::buildenv` only enforces the OPP envelope byte cap
(`MAX_ENVELOPE_BYTES = 65,536`). It does not check whether the final Solana
transaction can actually fit:

- Solana legacy transactions have a raw packet limit of about `1232` bytes.
- Solana compiled instructions index accounts with one-byte account indices.
- The current C++ transaction builder mirrors that with `uint8_t` account-key
  indices, but does not guard overflow.

### Why `uint16_t` Does Not Fix It

Changing the local C++ index type from `uint8_t` to `uint16_t` would not make
the transaction valid on Solana.

The one-byte index is part of Solana's transaction format and runtime
semantics. A client cannot unilaterally send 16-bit account indices in a legacy
transaction and expect the chain to accept them. If the local builder used
`uint16_t`, it would only hide the local truncation bug while still producing a
transaction Solana cannot represent.

Also, the packet-size limit fails much earlier than the 256-account index
limit. With the current final chunk shape, the lower bound is roughly:

```text
492 static bytes + 40 ComputeBudget bytes + final_chunk_bytes + 33 bytes per dynamic account
```

At a 405-byte final chunk, just 16 dynamic accounts already gives:

```text
492 + 40 + 405 + (16 * 33) = 1465 bytes
```

That exceeds the `1232` byte packet budget even though it is far below 256
dynamic accounts. So SEC-94 is not just a local integer-size bug. The real bug
is that WIRE can commit an outbound envelope whose Solana finalization
transaction is unrepresentable.

## Impact

The impact is liveness, not value theft.

If the final Solana transaction is too large:

1. earlier chunks may already have landed;
2. the final `epoch_in` transaction fails every retry;
3. the Solana outpost never emits its epoch reply;
4. WIRE never reaches inbound consensus for that outpost;
5. global epoch advancement stops because `sysio.msgch::chkcons` waits for all
   active outposts.

That means one oversized Solana envelope can halt OPP advancement chain-wide.

## Selected Fix

Fix the problem before the bad envelope is committed.

The selected design has three parts:

1. Add a zero-data terminal finalization call on the Solana outpost.
2. Teach `sysio.msgch::buildenv` to budget Solana-bound envelopes by final
   transaction account count and packet bytes.
3. Add relay/client guards so any estimator drift fails loudly before RPC
   submission.

There must be no post-consensus crank or paging action.

## Solana Program Change

Change the Solana outpost delivery flow so data upload and finalization are
separate steps:

1. upload all non-empty envelope chunks first;
2. submit a terminal zero-data `epoch_in` mode, or an `epoch_in_finalize`
   instruction, that consumes the completed chunk buffer;
3. perform delivery recording, consensus check, attestation processing,
   outbound emit, and buffer close inside that terminal call.

This keeps consensus atomic inside the `epoch_in` delivery path, but removes
the current "last data chunk plus all effect accounts" coupling.

The benefit is important: with zero terminal data, the dynamic-account budget is
stable. With the old final-chunk shape, the budget sawtooths based on
`encoded_envelope_size % 672`, and a near-full final chunk can leave almost no
room for dynamic accounts.

## Wire-Sysio Change

Extend `sysio.msgch::buildenv(chain_code)` for SVM destination chains.

For each candidate prefix of READY attestations, `buildenv` should check:

```text
encoded_envelope_bytes <= MAX_ENVELOPE_BYTES
estimated_terminal_account_keys <= SVM_FINAL_EPOCH_IN_KEY_LIMIT
estimated_terminal_packet_bytes <= SVM_FINAL_EPOCH_IN_PACKET_LIMIT
```

If the candidate does not fit:

- shrink the prefix;
- mark only the included attestations `PROCESSED`;
- leave omitted attestations `READY`;
- let the next epoch drain the remainder.

The estimator is consensus-critical. It must be an upper bound. Under-packing
only reduces throughput. Over-packing can commit an envelope that Solana cannot
deliver.

### Estimator Rules

Use conservative constants:

- raw packet budget: `1232` bytes minus safety margin;
- account-key backstop: 256 keys minus safety margin;
- static terminal `epoch_in` transaction size generated from the real Solana
  client layout and pinned by tests;
- per-extra-account cost at least `33` bytes;
- fixed safety margin for transaction-format drift.

Use pessimistic SPL worst-case estimation for SVM token effects unless
`sysio.msgch` can prove native-vs-SPL classification from the same source the
relay/outpost uses. Today that classification lives on the Solana side, so the
safe first implementation is pessimistic SPL.

Deduplicate only when the contract can prove two references resolve to the same
Solana account. If identity is uncertain, over-count.

## Required Capacity Caps

`Expected Solana-bound volume` is not a protocol variable today. For SEC-94 it
should be derived from explicit caps: first the consensus safety caps, then a
production capacity cap/SLO. The code already has some lower-level byte caps,
but it does not yet have the Solana terminal-transaction caps that matter for
this issue.

Already present:

| Cap | Current status |
| --- | --- |
| WIRE epoch duration | Present as `epoch_config::epoch_duration_sec`, default `360` seconds. This is the time window for per-epoch volume. |
| OPP envelope byte cap | Present as `MAX_ENVELOPE_BYTES = 65,536` in `sysio.msgch`, mirrored by the relay and Solana outpost. |
| Solana chunk payload cap | Present as `SOLANA_MAX_CHUNK_BYTES = 672` in the relay and `MAX_CHUNK_BYTES = 672` in `wire-solana`. |
| Byte-only READY spillover | Present in `sysio.msgch::buildenv`: attestations that do not fit the 65,536-byte envelope cap stay `READY`. |
| Relay dynamic-account extraction | Partial. The relay already extracts several account classes for `remaining_accounts`, but this is not a consensus packing cap. |

Missing and required for the SEC-94 fix:

| Missing cap | Why it is needed | Where it should live |
| --- | --- | --- |
| Raw terminal packet cap | Prevents committing an envelope whose terminal Solana transaction exceeds the raw `1232` byte packet limit. Use raw bytes minus margin, never base64 size. | `sysio.msgch` estimator, relay guard, tests |
| Terminal static transaction size | The estimator needs the measured no-extra-account terminal transaction size. This must be generated from the real Solana client/IDL layout and pinned by tests. | Cross-repo fixture, consumed by `sysio.msgch` constants |
| Terminal account-key cap | Prevents account-key index overflow. Solana legacy compiled instructions use one-byte account indices, and the C++ builder currently mirrors that with `uint8_t`. | `sysio.msgch` estimator and `libfc` transaction-builder guard |
| Dynamic-account budget | Converts the remaining packet/key space into a maximum number of Solana `remaining_accounts` for the terminal call. | `sysio.msgch::buildenv` |
| Per-attestation account-cost table | Lets `buildenv` decide whether adding the next READY attestation keeps the terminal transaction representable. The first version should pessimistically count SPL paths unless WIRE can prove native-vs-SPL from the same source as the relay/outpost. | `sysio.msgch`, with parity tests against relay extraction |
| Solo-attestation cap and dead-letter path | Ensures one impossible attestation cannot block all later Solana-bound work. Every current attestation type should fit solo with margin. | `sysio.msgch` plus governance/remediation policy |
| Production Solana-bound capacity cap | Defines the maximum Solana-bound value-transfer workload the system is willing to accept per WIRE epoch before backlog becomes an operational incident. This may be a source-admission/pacing cap, or an alerting/SLO cap tied to READY backlog growth. | Launch configuration/operations; optionally source contracts later |

Current source paths are not capped by Solana volume:

- `sysio.msgch::queueout` queues every authorized outbound attestation.
- `sysio.opreg::flushwtdw` drains all matured withdrawal rows.
- `sysio.uwrit::drainfwq` drains the full from-WIRE swap queue.
- `sysio.uwrit::chklocks` drains all expired locks and can emit deferred
  slash/remit attestations through `sysio.opreg::releaselock`.

The SEC-94 safety fix does not require all of those source drains to become
hard-throttled immediately if `buildenv` safely spills excess attestations back
to `READY`. It does require an enforced terminal transaction estimator so
excess work cannot be marked `PROCESSED` into an undeliverable Solana envelope.
Before production, define the capacity cap/SLO from the estimator budget and
monitor Solana-bound `READY` backlog age and count against it.

## Relay And Client Guards

Update `outpost_solana_client_plugin` to use the terminal finalization call.

Before submitting the terminal call, the relay should:

- resolve the final static plus dynamic account list;
- fail locally if unique account keys exceed the supported key limit;
- serialize or estimate the transaction and fail locally if packet bytes exceed
  the configured limit;
- log the epoch, static account count, dynamic account count, estimated bytes,
  and limiting reason.

Update `libfc` Solana transaction creation to guard account-key count before
building the `map<solana_public_key, uint8_t>`. This does not fix SEC-94 by
itself; it prevents silent local truncation and gives a clear error if any
Solana caller drifts past the protocol-supported index range.

## Single Attestation Over Budget

A single attestation that cannot fit by itself must not block the whole outpost
queue.

Handle it as a loud dead-letter/error state:

- preserve the full value-transfer record;
- exclude it from normal `buildenv` packing;
- require explicit remediation, such as governance refund, manual re-encoding,
  or protocol upgrade.

This should not happen for current attestation types under the zero-data
terminal finalization shape. Add a regression test that every current
attestation type fits solo with margin.

## Observability

When SVM budget trimming happens, emit an operator-visible signal with:

- chain code;
- epoch;
- included attestation count;
- omitted attestation count;
- estimated dynamic accounts;
- estimated packet bytes;
- limiting budget.

Also monitor READY backlog age and count for Solana-bound value transfers.
Growing backlog means user-visible withdrawal latency even though the
chain-wide halt is avoided.

## Validation

Required `wire-sysio` tests:

- SVM `buildenv` does not pack the measured oversized native-swap case;
- SVM `buildenv` leaves omitted attestations READY in original order;
- non-SVM destinations keep existing byte-only behavior;
- every current attestation type fits solo with margin;
- a single impossible attestation is dead-lettered instead of wedging
  `buildenv`;
- relay/client guards reject account-key and packet-size overflow clearly.

Required cross-repo parity tests:

- Rust/Anchor fixture measures the real terminal no-extra `epoch_in`
  transaction size;
- relay fixture extracts the actual dynamic account union;
- contract estimate is always greater than or equal to relay actual accounts
  and packet bytes;
- native swap fixture proves the contract safely over-counts when using
  pessimistic SPL estimation.

## Rollout Order

1. Land and deploy the Solana outpost terminal finalization mode/instruction.
2. Deploy relay support for the terminal call and its defensive guards.
3. Deploy WIRE-side `buildenv` budget enforcement with matching constants.
4. Monitor trim events and Solana READY backlog.

If the Solana outpost is already deployed anywhere, keep the old
final-chunk-with-data path accepted during the rollout window or use a
coordinated downtime window.

## Remaining Decisions

1. Expected Solana-bound volume.
   Size by the worst realistic operation mix and current `epoch_duration_sec`.
   This is not currently an adjustable protocol variable. Treat it as a
   production capacity cap/SLO derived from the terminal dynamic-account budget:
   if sustained demand is above roughly half the terminal budget per epoch after
   source-side pacing, schedule ALT or account-reduction work in the same
   release train.

2. Dead-letter remediation authority.
   Decide who can remediate a dead-lettered value-transfer attestation and what
   the allowed actions are: governance refund, manual re-encoding, protocol
   upgrade, or another explicit operational path.

## Non-Goals For The First Fix

- Do not change local account indices to `uint16_t` and call that a fix.
- Do not add post-consensus finalization paging or crank actions.
- Do not make ALT a dependency unless measured sustained volume requires it.
- Do not add a generic automatic refund in `sysio.msgch`.

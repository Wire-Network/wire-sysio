# SEC-94 Solana Transaction Budget Fix - Condensed Plan

Status: condensed plan, not implementation

Related issue: SEC-94 / WSA-212

This document is a shorter companion to the detailed bounded-`epoch_in` plan.
It keeps only the problem, selected fix, validation, rollout, and remaining
decisions.

## Problem

The main SEC-94 problem is that the final Solana transaction has a hard raw
packet limit of about `1232` bytes, and WIRE does not currently cap against
that limit when it builds a Solana-bound outbound envelope.

Today `sysio.msgch::buildenv` only enforces the WIRE/OPP envelope byte cap
(`MAX_ENVELOPE_BYTES = 65,536`). That cap answers "how much encoded OPP data
can this WIRE envelope contain?" It does not answer "will the final Solana
`epoch_in` transaction fit into one Solana packet?"

That distinction matters because the OPP envelope can be much larger than one
Solana packet. The relay sends it through
`outpost_solana_client::deliver_outbound_envelope` by uploading the encoded
envelope to the Solana outpost in multiple `epoch_in` chunks.

The current final `epoch_in` transaction is special:

- it carries the last envelope data chunk; and
- it carries every dynamic Solana account needed to process every attestation
  in the envelope as `remaining_accounts`.

Those dynamic accounts include recipient/depositor/operator wallets, Reserve
PDAs, SPL vaults, recipient ATAs, mints, and token program accounts. The exact
set depends on the attestations in the envelope.

WIRE currently does not cap that terminal transaction by raw Solana packet
bytes before committing the envelope. The Solana outpost program also cannot
recover from an oversized terminal packet, because the transaction is invalid
before the instruction can execute.

The relevant hard limits are:

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
| Terminal runtime account cap | Prevents committing an envelope whose terminal Solana transaction exceeds the currently enforced Solana runtime account limit. Treat `64` as the default unless the target cluster's `increase_tx_account_lock_limit` feature is explicitly confirmed and the relay/program support that higher limit. | `sysio.msgch` estimator and relay guard |
| Terminal account-key cap | Prevents account-key index overflow. Solana legacy compiled instructions use one-byte account indices, and the C++ builder currently mirrors that with `uint8_t`. | `sysio.msgch` estimator and `libfc` transaction-builder guard |
| Dynamic-account budget | Converts the remaining packet/key space into a maximum number of Solana `remaining_accounts` for the terminal call. | `sysio.msgch::buildenv` |
| Per-attestation account-cost table | Lets `buildenv` decide whether adding the next READY attestation keeps the terminal transaction representable. The first version should pessimistically count SPL paths unless WIRE can prove native-vs-SPL from the same source as the relay/outpost. | `sysio.msgch`, with parity tests against relay extraction |
| Solo-attestation cap and dead-letter path | Ensures one impossible attestation cannot block all later Solana-bound work. Every current attestation type should fit solo with margin. | `sysio.msgch` plus governance/remediation policy |
| Production Solana-bound capacity cap | Defines the maximum Solana-bound value-transfer workload the system is willing to accept per WIRE epoch before backlog becomes an operational incident. This may be a source-admission/pacing cap, or an alerting/SLO cap tied to READY backlog growth. | Launch configuration/operations; optionally source contracts later |

Solana system hard limits:

| Cap | System limit | Reference |
| --- | --- | --- |
| Raw Solana packet limit | `1232` bytes | Solana transaction limits list `1,232` bytes; Solana `PACKET_DATA_SIZE` source defines `1280 - 40 - 8`. |
| Runtime account limit | `64` loaded accounts | Solana transaction limits list `64` max accounts per transaction; Solana constants reference says the enforced runtime limit is `64`, or `128` only when `increase_tx_account_lock_limit` is activated. |
| Raw account-key limit | `256` unique keys | Solana constants reference describes `256` as the hard ceiling from `u8` index encoding; Solana transaction structure defines `CompiledInstruction.program_id_index: u8` and `accounts: Vec<u8>`. |

Reference URLs for Solana-enforced caps:

- Solana transactions limits: https://solana.com/docs/core/transactions
- Solana constants reference: https://solana.com/docs/core/constants-reference
- `PACKET_DATA_SIZE` source: https://github.com/solana-labs/solana/blob/master/sdk/src/packet.rs
- Solana transaction structure / `CompiledInstruction` fields:
  https://solana.com/docs/core/transactions/transaction-structure

Recommended WIRE estimator defaults:

| Cap | Default value | Reference |
| --- | --- | --- |
| Packet safety margin | `80` bytes | WIRE safety margin. |
| Effective packet budget | `1152` raw bytes | WIRE default derived from the raw Solana packet limit minus margin. |
| Effective runtime account budget | `64` loaded accounts | WIRE default follows the currently enforced Solana runtime limit unless the target cluster and client path explicitly support the higher feature-gated limit. |
| Account-key safety margin | `16` keys | WIRE safety margin. |
| Effective account-key budget | `240` unique keys | WIRE default derived from the raw `u8` representation ceiling minus margin. |
| Per dynamic account byte cost | `33` bytes | WIRE estimator default: 32-byte pubkey plus at least one account-index byte. |
| Terminal static transaction size | Measured no-extra terminal transaction size plus `64` bytes margin. The measured value must come from a fixture that builds the real terminal call from the current Anchor IDL/client layout. | Cross-repo measurement fixture, not a Solana protocol constant. |
| Production planning cap | `50%` of the measured hard dynamic-account budget per WIRE epoch. This is an SLO/source-pacing target, not the consensus hard cap. | WIRE launch/SLO default. |

Use these formulas:

```text
effective_packet_budget = 1232 - 80 = 1152
effective_runtime_account_budget = 64
effective_key_budget    = 256 - 16  = 240

packet_dynamic_account_budget =
  floor((effective_packet_budget - measured_terminal_static_bytes_with_margin) / 33)

hard_dynamic_account_budget =
  min(packet_dynamic_account_budget,
      effective_runtime_account_budget - measured_terminal_static_account_count,
      effective_key_budget - measured_terminal_static_key_count)

production_dynamic_account_slo =
  floor(hard_dynamic_account_budget / 2)
```

If the measured terminal static transaction size plus margin is around `532`
bytes, the hard packet-derived budget is:

```text
floor((1152 - 532) / 33) = 18 dynamic accounts
```

With the default `360` second WIRE epoch, the initial production planning SLO
would therefore be about `9` dynamic accounts per Solana-bound epoch. Do not
hard-code `9` as the consensus cap; derive it from the measured budget and use
it for launch sizing, alerts, and source-side pacing.

Recommended per-attestation account-cost defaults:

| Attestation shape | Default dynamic-account estimate |
| --- | --- |
| `OPERATOR_ACTION(SLASH)` | `0` |
| `OPERATOR_ACTION(WITHDRAW_REMIT)` | `1` |
| `DEPOSIT_REVERT` | `1` |
| `RESERVE_READY` | `1` |
| proven-native `SWAP_REMIT` or `SWAP_REVERT` | `2` |
| default `SWAP_REMIT` or `SWAP_REVERT` when WIRE cannot prove native-vs-SPL | `8` |
| proven-native `RESERVE_CREATE_CANCELLED` | `2` |
| default `RESERVE_CREATE_CANCELLED` when WIRE cannot prove native-vs-SPL | `6` |

The default estimator should use the pessimistic rows unless WIRE can prove the
native-vs-SPL classification from the same source the relay and Solana outpost
use. That preserves the upper-bound invariant.

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

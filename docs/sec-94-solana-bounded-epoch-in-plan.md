# SEC-94 Solana-Aware Envelope Creation Detailed Plan

Status: revised detailed design plan, not implementation

Related issue: SEC-94 / WSA-212

This plan replaces the earlier bounded-`epoch_in` writeup. The selected shape
is now: change outbound envelope creation so WIRE commits only a
Solana-deliverable prefix of READY attestations for a Solana outpost.

The fix is not "build a large envelope, then reject it." The fix is to make the
envelope builder destination-aware.

## Executive Summary

SEC-94 is caused by a mismatch between WIRE's outbound envelope cap and
Solana's terminal transaction cap.

WIRE currently caps the encoded OPP envelope at `65,536` bytes. That prevents a
huge OPP payload, but it does not prove the terminal Solana `epoch_in`
transaction can fit into one raw Solana packet. Solana transactions have a raw
packet limit of `1,232` bytes, and the terminal transaction also carries every
dynamic account needed to process the envelope's attestations.

The selected fix:

1. Keep one outbound envelope per outpost per WIRE epoch.
2. In `sysio.msgch::buildenv`, create a Solana-safe envelope by selecting only
   the READY attestation prefix that fits the Solana terminal transaction
   budget.
3. Leave overflow attestations as `READY` for a later epoch.
4. Add a dedicated zero-data `epoch_in_finalize` path in `wire-solana` so the
   final transaction no longer carries both the last data chunk and all
   dynamic effect accounts.
5. Add relay/client guards so estimator drift is detected before RPC
   submission.
6. Add a `libfc` transaction-builder guard for account-key/index overflow.

There must be no post-consensus crank or paging action.

SEC-94 now intentionally bundles two workstreams:

- the transaction-budget liveness fix that prevents Solana-bound envelopes from
  exceeding terminal packet/account limits;
- a reserve-custody/precision immutability fix required by the new manifest:
  reserve-backed effects must derive native-vs-SPL mode, mint, and decimals from
  pinned Reserve state instead of mutable admin config rows.

The second workstream moves value, so it needs focused implementation review in
the PR. It is still bundled here because the account-meta manifest and parity
fixtures are only trustworthy when the relay and Solana program use immutable
custody facts for existing reserves.

## Current Flow

### WIRE Side

Outbound attestations are queued into `sysio.msgch::attestations` with status
`READY`.

Current producers include:

- `sysio.epoch::advance`, which queues operator and active batch-operator group
  updates to every active outpost;
- `sysio.opreg::flushwtdw`, which queues matured withdrawal remits;
- `sysio.uwrit::drainfwq`, which queues from-WIRE swap work;
- `sysio.uwrit::chklocks`, which drains expired locks and can emit deferred
  slash/remit attestations through `sysio.opreg::releaselock`;
- reserve lifecycle paths that queue reserve-ready or reserve-cancel effects.

At the end of epoch advancement, `sysio.msgch::buildenv(chain_code)` reads
READY attestations for one outpost, serializes a prefix into an OPP envelope,
marks only the included rows `PROCESSED`, stores the outbound envelope in
`outenvelopes`, and later cleans up the consumed `PROCESSED` rows.

Today the prefix is chosen by encoded OPP envelope bytes only:

```text
packed_envelope_bytes <= MAX_ENVELOPE_BYTES
MAX_ENVELOPE_BYTES = 65,536
```

That byte-only packing is correct for the generic OPP envelope limit, but it is
not sufficient for Solana delivery.

### Relay Side

`outpost_solana_client::deliver_outbound_envelope` reads the committed
outbound envelope and uploads it to the Solana outpost in `epoch_in` chunks.

Current relay behavior:

1. split the encoded envelope into chunks of up to `SOLANA_MAX_CHUNK_BYTES`;
2. submit non-final chunks to append bytes into an on-chain chunk buffer;
3. on the final chunk, include the last chunk data and all dynamic
   `remaining_accounts` needed by the envelope's effects;
4. rely on the Solana outpost program to record delivery, check consensus,
   process attestations, emit outbound replies, and close the chunk buffer.

### Solana Outpost Side

The Solana outpost's `epoch_in` path performs consensus and finalization inside
the final delivery transaction. That shape must remain. Moving effect
processing to a separate post-consensus crank would break the consensus model
because consensus must cover the final effects that produce the outpost reply.

## Problem

The major SEC-94 limit is Solana's raw transaction packet limit:

```text
PACKET_DATA_SIZE = 1,232 raw bytes
```

A WIRE OPP envelope can be much larger because it is uploaded through multiple
Solana chunks. The terminal transaction is different: it must fit in one
Solana packet.

The current terminal transaction is expensive because it carries:

- the last envelope data chunk;
- static accounts required by `epoch_in`;
- ComputeBudget instruction overhead;
- every dynamic account required by every attestation in the envelope.

Dynamic accounts can include recipient wallets, depositor wallets, operator
wallets, Reserve PDAs, SPL reserve vaults, recipient ATAs, mints, and token
program accounts.

If the terminal transaction exceeds Solana packet or account limits, Solana
rejects it before the outpost program can run. Earlier upload chunks may have
landed, but finalization never completes. WIRE then never receives the Solana
outpost's epoch reply, and `sysio.msgch::chkcons` cannot advance the global OPP
epoch because it waits for every active outpost.

So the impact is liveness, not value theft: one oversized Solana-bound envelope
can halt OPP advancement chain-wide.

Because this plan is still in the development phase, recovery from a
drift-committed unrepresentable envelope is out of scope for this patch and can
be handled by redeploy/state reset. A production recovery path is a
pre-production gate, not part of the first SEC-94 implementation.

## Why Envelope Creation Is The Right Boundary

The committed outbound envelope is the consensus object that batch operators
deliver. Once `buildenv` has hashed and stored it, the relay should not invent
its own sharding strategy. Different operators must deliver the same committed
bytes.

Therefore the Solana-capacity decision must happen before the envelope is
committed:

```text
READY attestations
  -> destination-aware buildenv packing
  -> one Solana-safe committed envelope
  -> omitted attestations remain READY
```

This preserves the current READY spillover model. The change is that Solana
spillover is based on terminal transaction safety, not just OPP envelope bytes.

## Selected Design

For non-Solana outposts, keep current byte-only packing.

For Solana/SVM outposts, `sysio.msgch::buildenv` becomes a Solana-aware packer:

```text
encoded_envelope_bytes <= MAX_ENVELOPE_BYTES
estimated_terminal_packet_bytes <= SVM_TERMINAL_PACKET_BUDGET
estimated_terminal_loaded_accounts <= SVM_TERMINAL_ACCOUNT_BUDGET
estimated_terminal_account_keys <= SVM_TERMINAL_KEY_BUDGET
```

The first READY attestation that would exceed any Solana budget is not included
in the envelope. That row and every later row remain `READY`.

This means there is still one outbound envelope for the outpost and epoch, but
the envelope contains only the prefix that can be finalized on Solana.

## Component Responsibilities

| Component | Responsibility |
| --- | --- |
| `sysio.msgch::buildenv` | Select the Solana-safe READY prefix before hashing/storing the envelope. |
| `wire-solana` outpost program | Add dedicated zero-data `epoch_in_finalize` so the final transaction does not also carry the last chunk. |
| Solana relay/client | Upload data chunks, submit terminal finalization, and verify actual terminal tx size/account counts before RPC submission. |
| `libfc` Solana transaction builder | Guard account-key and instruction-index overflow explicitly. |
| Tests/fixtures | Prove the WIRE estimator upper-bounds the actual relay-built terminal transaction. |

## Detailed WIRE Algorithm

`sysio.msgch::buildenv(chain_code)` should keep its current high-level shape:
collect candidates, choose an included prefix, serialize, mark included rows
`PROCESSED`, store `outenvelopes`, log, and clean up consumed rows.

The prefix selection changes for SVM destinations.

### Step 1: Resolve Destination Kind

Look up `chain_code` in `sysio.chains::chains`.

If the chain kind is not `CHAIN_KIND_SVM`, use existing OPP byte-only packing.

If the chain kind is `CHAIN_KIND_SVM`, use Solana-aware packing.

### Step 2: Collect READY Candidates

Use the existing READY index order. Do not reorder by attestation type, account
cost, or token kind. Ordering must remain deterministic and preserve
cross-epoch attestation order.

The candidate list should contain:

- attestation table id;
- attestation type;
- attestation data;
- estimated OPP encoded bytes;
- estimated Solana dynamic-account contribution.

### Step 3: Accumulate A Candidate Prefix

Start with an empty prefix and a Solana terminal budget context.

For each READY candidate:

1. calculate the candidate envelope if this attestation is appended;
2. update encoded envelope byte estimate;
3. update dynamic account union estimate;
4. update terminal loaded-account estimate;
5. update terminal packet byte estimate;
6. include the candidate only if all budgets still fit;
7. otherwise stop before adding it.

The stop-before-add rule matters. Overflow work must never become part of the
committed `raw_envelope`.

### Step 4: Serialize And Trim

Serialize only the included prefix into the OPP envelope.

Keep the existing final serialized-size trim loop for `MAX_ENVELOPE_BYTES`.
After serialization, if the actual OPP bytes exceed the generic envelope cap,
pop from the included prefix and retry. Popped rows remain `READY`.

For SVM destinations, the trim loop should also re-check the Solana estimator
after any pop. With zero-data terminal finalization, popping is monotonic for
the terminal account budget, so this should converge.

### Step 5: Mark Only Included Rows `PROCESSED`

Only after the final prefix is chosen:

- mark included attestations `PROCESSED`;
- set `processed_timestamp`;
- leave omitted candidates `READY`;
- store one outbound envelope row with the serialized bytes;
- keep the existing cleanup that removes consumed `PROCESSED` rows for that
  outpost.

This avoids any need to revert an omitted attestation's status.

## Solana Estimator

The estimator is consensus-critical because it decides which READY rows become
part of the committed envelope. It must be a conservative upper bound.

Under-packing only reduces throughput. Over-packing can commit an envelope that
the Solana relay cannot finalize.

### System Hard Limits

| Limit | Value | Use In Plan |
| --- | ---: | --- |
| Raw transaction packet size | `1,232` bytes | Primary terminal packet budget. |
| Runtime loaded-account budget | `64` accounts by default | Loaded-account cap unless the target cluster is explicitly configured and tested for a higher limit. |
| Legacy raw account-key ceiling | `256` keys | Backstop for one-byte instruction account indices. Usually not binding before the 64-account runtime cap. |

References:

- Solana transactions: https://solana.com/docs/core/transactions
- Solana constants reference: https://solana.com/docs/core/constants-reference
- Solana transaction structure: https://solana.com/docs/core/transactions/transaction-structure
- Versioned transactions and ALTs: https://solana.com/docs/core/transactions/versioned-transactions

### Recommended WIRE Defaults

| Budget | Default |
| --- | ---: |
| Raw packet limit | `1,232` bytes |
| Packet safety margin | `80` bytes |
| Effective packet budget | `1,152` bytes |
| Effective runtime account budget | `64` loaded accounts |
| Raw account-key limit | `256` keys |
| Account-key safety margin | `16` keys |
| Effective account-key budget | `240` keys |
| Per dynamic account packet cost | `33` bytes |
| Terminal static transaction size | measured no-extra terminal tx size plus `64` bytes margin |

Use raw packet bytes, not base64-encoded log size.

### Budget Formula

For a zero-data terminal finalization transaction:

```text
effective_packet_budget = 1232 - 80
effective_key_budget = 256 - 16

packet_dynamic_account_budget =
  floor((effective_packet_budget - measured_terminal_static_bytes_with_margin) / 33)

hard_dynamic_account_budget =
  min(packet_dynamic_account_budget,
      effective_runtime_account_budget - measured_terminal_static_account_count,
      effective_key_budget - measured_terminal_static_key_count)
```

The measured terminal static values must come from a fixture that builds the
real terminal transaction using the current Solana IDL/client layout.

### Dynamic Account Estimation

The WIRE contract must estimate the union of Solana accounts, not a naive
per-attestation sum, but it may only deduplicate accounts whose identity it can
prove.

Safe dedup examples:

- known singleton program ids;
- Reserve PDAs when WIRE can derive the same `(token_code, reserve_code)` PDA
  identity as the relay;
- repeated recipient or operator pubkeys when the same exact public key bytes
  are present in attestation data.

Unsafe dedup examples:

- token accounts whose mint or owner cannot be proven from WIRE-visible data;
- native-vs-SPL classification that only the Solana side knows;
- any inferred account identity not derived from the same source as the relay.

If identity is uncertain, over-count.

### Exhaustive Estimator Manifest

The Solana estimator must cover every attestation type that can currently be
queued from WIRE to a Solana outpost. Implement it as an exhaustive switch over
the outbound-capable types, with no silent default that assumes zero dynamic
accounts. If a new outbound-capable type is added without an estimator row, the
build/test suite must fail before deployment.

At runtime, an unknown Solana-bound READY attestation should stop `buildenv`
before any envelope is committed. That is a safe deployment/configuration
failure: the row remains `READY` instead of being packed with an unsafe
under-estimate.

Use pessimistic defaults for the WIRE consensus estimator. Relay/program
fixtures may prove tighter actual manifests, but `buildenv` can use a tighter
row only when the needed identity and branch facts are visible to WIRE before
the envelope is committed.

| Attestation shape | Default dynamic-account estimate |
| --- | ---: |
| `OPERATORS` | `0` |
| `BATCH_OPERATOR_GROUPS` | `0` |
| `EMISSIONS_BLOCKED` | `0` |
| `OPERATOR_ACTION(SLASH)` | `0` |
| `OPERATOR_ACTION(WITHDRAW_REMIT)` | `1` |
| `DEPOSIT_REVERT` | `1` |
| `RESERVE_READY` | `1` |
| `SWAP_REMIT` or `SWAP_REVERT` in v1 | `8` |
| future WIRE-proven native `SWAP_REMIT` or `SWAP_REVERT` | `2` |
| proven-native `RESERVE_CREATE_CANCELLED` | `2` |
| SPL `RESERVE_CREATE_CANCELLED` canonical-ATA refund path | `8` |
| default `RESERVE_CREATE_CANCELLED` when native-vs-SPL is not proven | `8` |

The first implementation must use the pessimistic `8`-account row for all
reserve-backed `SWAP_REMIT` and `SWAP_REVERT` candidates. Pinning Reserve
custody lets the relay and Solana program agree on the real account set, but it
does not give the WIRE contract access to Solana Reserve state during
`buildenv`.

The relay/parity fixture must keep this table honest for every current effect
branch. The required assertion is not only "estimated account count is high
enough"; the representative Solana program test must also prove the supplied
accounts let the effect succeed or intentionally reach the effect's documented
non-throwing rejection path.

## Single-Attestation Fit Rule

Every valid current Solana-bound attestation type should fit by itself with
margin. Add tests for this.

If a future attestation shape cannot fit by itself, it must be handled before
production by source-level admission rules or an explicit remediation policy.
Do not silently drop value and do not add a generic automatic refund in
`sysio.msgch`.

The first SEC-94 implementation should not need a normal dead-letter path for
valid current attestation shapes. It does need tests proving that statement.

## Wire-Solana Program Change

Add a terminal finalization path that carries zero envelope data.

The current final chunk combines:

```text
last data chunk + all dynamic effect accounts
```

That makes the terminal packet budget unstable because the final chunk size is
`encoded_envelope_size % MAX_CHUNK_BYTES`.

The replacement flow:

1. relay uploads every non-empty chunk into the operator chunk buffer;
2. relay submits a dedicated zero-data `epoch_in_finalize` instruction;
3. the outpost verifies the buffer contains the complete envelope;
4. finalization records delivery, checks consensus, processes attestations,
   emits the next outbound envelope, and closes the buffer inside that same
   terminal instruction.

This keeps finalization inside the consensus-reaching `epoch_in` path and
removes the final-data-chunk packet pressure.

### Solana Program Requirements

The terminal finalization instruction must:

- reject finalization if the expected uploaded envelope is incomplete;
- preserve existing operator delivery and consensus semantics;
- process the winning envelope's attestations inline when consensus is reached;
- consume the same dynamic `remaining_accounts` contract as today, except
  without terminal chunk data;
- close or reset the operator chunk buffer exactly as the current final path
  does;
- update the Anchor IDL and generated client code;
- fix any stale comments that still describe the old chunk size or final-chunk
  behavior.

There is no requirement to support both the old final-data-chunk mode and the
new `epoch_in_finalize` terminal mode temporarily. Treat the terminal shape as a
coordinated cutover across the Solana program, relay/client, and WIRE
contracts.

### Reserve Custody Pinning

Do not derive existing reserve custody from mutable
`OutpostConfig.token_addresses_by_code` rows during terminal finalization.
Those rows can remain admin-upsertable for future inbound requests, but
reserve-backed effects need the mint/native mode that was true when the reserve
escrow was created.

Add pinned custody fields to `Reserve`, for example:

```text
custody_mint: Pubkey  // NATIVE_TOKEN_MARKER for native reserves
custody_decimals: u8  // DEPOT_PRECISION_DECIMALS for native reserves
```

Then set that field in every reserve creation path:

- `create_reserve`;
- `create_reserve_native`;
- `create_reserve_spl_authority`.

For native reserve creation, store `NATIVE_TOKEN_MARKER` and
`DEPOT_PRECISION_DECIMALS`. For SPL reserve creation, store the supplied mint
pubkey and the actual `Mint.decimals` value from the mint account used to create
the reserve vault.

Reserve-backed handlers and the relay manifest must use these pinned Reserve
custody fields, not the current mutable token-binding or precision tables, when
deciding native-vs-SPL, deriving SPL account addresses, and converting
depot-frame amounts to chain units. This applies to `SWAP_REMIT`,
`SWAP_REVERT`, `RESERVE_READY`, and `RESERVE_CREATE_CANCELLED`.

With no temporary compatibility requirement, there is no need to support old
Reserve accounts that lack the pinned field. The SEC-94 cutover can update the
account layout and tests together.

## Relay Effect Account Manifest

The relay must build an explicit effect-account manifest from the committed
envelope before it builds the terminal transaction. This manifest is stronger
than a pubkey set:

```text
pubkey + is_writable + is_signer + source/effect reason
```

Dynamic extras should be carried as `account_meta`, not only as
`solana_public_key`. The current pubkey-only path marks every extra account
writable, which is too blunt for mints and program ids and too weak as a parity
contract. The relay must deduplicate by pubkey while preserving sticky
permissions: writable wins over readonly, signer wins over non-signer.

Initial manifest requirements:

| Effect branch | Required dynamic accounts |
| --- | --- |
| `OPERATOR_ACTION(WITHDRAW_REMIT)` | operator wallet: writable, non-signer |
| `DEPOSIT_REVERT` | depositor wallet: writable, non-signer |
| native `SWAP_REMIT` | Reserve PDA: writable; recipient wallet: writable |
| SPL `SWAP_REMIT` | Reserve PDA: readonly; reserve vault: writable; recipient canonical ATA: writable; SPL token program: readonly |
| native `SWAP_REVERT` | Reserve PDA: writable; depositor wallet: writable |
| SPL `SWAP_REVERT` | Reserve PDA: writable; reserve vault: writable; mint: readonly; depositor wallet: writable; depositor token account: writable; SPL token program: readonly; Associated Token Program and System Program when the handler can create the token account |
| `RESERVE_READY` | Reserve PDA: writable |
| native `RESERVE_CREATE_CANCELLED` | Reserve PDA: writable; creator wallet: writable |
| SPL `RESERVE_CREATE_CANCELLED` | Reserve PDA: writable; reserve vault: writable; creator wallet: readonly; creator canonical ATA: writable; mint: readonly; SPL token program: readonly; Associated Token Program: readonly; System Program: readonly |

This manifest must be generated from the same account identities that the
Solana program handlers use. Do not depend on comments or a hand-maintained
count table alone.

For SPL `SWAP_REMIT`, the v1 protocol requires the recipient canonical ATA to
pre-exist. The terminal handler must not create that ATA during finalization.
If the ATA is missing or uninitialized, the handler should take its documented
non-throwing rejection path and queue the existing swap-rejected response rather
than silently dropping value. This is why the relay manifest does not include
the Associated Token Program or System Program for SPL `SWAP_REMIT`, even though
the WIRE estimator still budgets the pessimistic `8` accounts for every
reserve-backed swap candidate.

For `RESERVE_CREATE_CANCELLED`, use deterministic account discovery, not blind
over-supply. The relay must derive the Reserve PDA from the attestation and
fetch/decode the Reserve account to learn the creator, lifecycle state, and
pinned custody mint/native mode.

For SPL refunds, make the creator's canonical ATA the protocol refund
destination. Update `create_reserve` so the SPL source account must equal
`get_associated_token_address(creator, mint)`. The account therefore exists at
reserve creation time, and if the creator closes it before cancellation the
cancel handler can recreate the same canonical ATA from deterministic inputs.
Do not store or accept an arbitrary refund token account in Reserve state for
SEC-94.

The SPL cancel handler should derive the canonical creator ATA from
`Reserve.creator` and `Reserve.custody_mint`, require that exact account in
`remaining_accounts`, and create it on demand with the Reserve PDA as rent payer
before transferring from the reserve vault. That makes the relay manifest and
the WIRE estimator stable: always budget the creator wallet, canonical ATA,
mint, SPL Token Program, Associated Token Program, and System Program for the
SPL branch.

The relay manifest and the Solana handler must be tested together for every
current branch. A manifest that only makes the transaction fit but causes the
handler to silently skip a refund/revert is not acceptable.

## Relay And Client Change

The relay should not shard a committed envelope. It should verify and deliver
the envelope that WIRE committed.

Expected relay changes:

1. split the raw envelope into non-empty chunks;
2. submit upload transactions for every non-empty chunk;
3. build a zero-data `epoch_in_finalize` terminal transaction;
4. compute the terminal transaction's raw serialized size;
5. compute terminal account-key count and loaded-account count;
6. compute and validate account metas, including writable/readonly flags;
7. abort before RPC submission if the actual transaction exceeds the expected
   budget;
8. report the drift clearly as a local operator error.

The relay guard is a diagnostic tripwire. The normal recovery path is that
`buildenv` never committed an oversized envelope in the first place.

## `libfc` Transaction Builder Guard

The Solana transaction builder should reject unrepresentable transactions
before serialization.

Add explicit checks for:

- more than 256 legacy account keys;
- any compiled instruction `program_id_index` that cannot fit in `uint8_t`;
- any compiled instruction account index that cannot fit in `uint8_t`;
- raw serialized packet size over the configured packet budget.

These guards do not replace `buildenv`. They catch client-side bugs and future
drift.

## Why Not Multiple Envelopes Per Epoch

Multiple outbound envelope rows for the same outpost and WIRE epoch are not the
first SEC-94 implementation.

The current `outenvelopes` model and cleanup path keep only the latest outbound
row per outpost. Batch operators also consume a single pending outbound
envelope per outpost. Supporting multiple envelopes per outpost/epoch would
require:

- a sequence number in the outbound envelope identity;
- table/index changes;
- relay delivery loop changes;
- consensus/hash-chain semantics for multiple outbound payloads in one epoch;
- monitoring and retry changes.

The safer first version is to keep the current one-envelope model and make the
one envelope contain a Solana-safe prefix.

## Why Not Padding Or Byte Reshaping

Padding the encoded OPP envelope to control final chunk size is not the primary
fix.

Padding can sometimes change `encoded_envelope_size % 672`, but it does not
reduce the number of dynamic accounts needed by the envelope's effects. It also
adds protocol complexity and still requires a Solana estimator.

Zero-data terminal finalization removes the final-chunk coupling directly.
Solana-aware envelope creation bounds the dynamic-account pressure directly.

## Address Lookup Tables

Versioned transactions with Address Lookup Tables can be a future throughput
track, but they should not be the first SEC-94 safety fix.

ALTs can reduce packet bytes by replacing some inline 32-byte account keys with
1-byte lookup indices. They do not remove the raw `1,232` byte packet limit,
and they do not remove the loaded-account runtime limit. They also introduce
operational state: table creation, extension, activation timing, lookup-table
selection, cache freshness, and fallback behavior.

The first fix should work with legacy transactions and a conservative
estimator. ALT support can be added later if measured Solana-bound volume needs
more throughput.

## Observability

Add or expose operator-visible metrics/logs for:

- number of READY attestations considered per Solana `buildenv`;
- number included in the committed Solana envelope;
- number left READY due to packet budget;
- number left READY due to loaded-account budget;
- estimated terminal packet bytes;
- estimated terminal loaded accounts;
- estimated dynamic accounts;
- oldest Solana-bound READY attestation age;
- relay-measured terminal packet bytes and account counts.

Backlog is expected when demand exceeds capacity. Backlog age is the signal
that source pacing or more throughput work is needed.

## Implementation Phases

### Phase 1: Measurement And Fixtures

- Build a fixture that constructs the real no-extra terminal finalization
  transaction from the current Solana IDL/client layout.
- Record measured static bytes, static account count, and static key count.
- Add representative relay manifest fixtures for every current effect branch,
  including native and SPL `SWAP_REMIT`, native and SPL `SWAP_REVERT`,
  `RESERVE_READY`, and native and SPL `RESERVE_CREATE_CANCELLED`.
- Assert account metas, not just pubkeys: writable/readonly flags and
  non-signer status must match the Solana handler's needs.
- Make the fixture fail if WIRE's estimator is below the actual relay-built
  transaction or if the manifest omits an account required for effect success.

### Phase 2: WIRE Envelope Creation

- Add SVM destination detection to `sysio.msgch::buildenv`.
- Add named Solana terminal budget constants.
- Add deterministic per-attestation account-cost estimation with exhaustive
  coverage for every outbound-capable Solana attestation type.
- Extend prefix packing to stop before Solana budgets are exceeded.
- Preserve existing behavior for non-Solana destinations.
- Add contract tests for READY spillover and status cleanup.

### Phase 3: Solana Terminal Finalization

- Add dedicated zero-data `epoch_in_finalize` in `wire-solana`.
- Update Anchor IDL and client generation.
- Add Solana program tests for chunk upload plus zero-data
  `epoch_in_finalize`.
- Confirm finalization still processes consensus/effects inside the terminal
  `epoch_in` path.
- Add pinned Reserve custody state and make reserve-backed handlers use it
  instead of mutable `OutpostConfig.token_addresses_by_code` or
  `precision_by_token_code` rows.
- Enforce the canonical-ATA `RESERVE_CREATE_CANCELLED` SPL refund policy:
  `create_reserve` must accept only the creator's canonical ATA as the SPL
  source, and cancellation must derive and, when needed, recreate that same ATA
  before refund.
- Do not add temporary compatibility with the old final-data-chunk terminal
  mode; deployment is a coordinated cutover.

### Phase 4: Relay And Builder Guards

- Change the relay to submit all non-empty chunks before terminal finalization.
- Change dynamic terminal extras from pubkeys to `account_meta` values.
- Add full effect-account manifest extraction for every current Solana-side
  effect branch.
- Add actual transaction measurement before RPC submission.
- Add `libfc` index and packet-size guards.
- Add relay tests that prove oversized terminal transactions fail locally with
  clear diagnostics.

### Phase 5: Capacity SLO

- Derive the hard dynamic-account budget from measured constants.
- Set the launch SLO from the pessimistic WIRE estimator, not the relay's
  tighter actual manifest. With an example hard dynamic-account budget around
  `18`, reserve-backed swaps budgeted at `8` dynamic accounts cap at about
  `2` swaps per epoch, so a 50 percent SLO is about `1` swap per epoch.
- Alert on Solana-bound READY backlog age/count.
- Decide whether source admission or pacing is needed before production.

## Validation Plan

### WIRE Contract Tests

Test cases should prove:

- non-Solana outposts keep byte-only packing;
- Solana packing includes a prefix that fits all Solana budgets;
- the first omitted Solana attestation remains `READY`;
- included rows are marked `PROCESSED` before cleanup;
- consumed rows are cleaned up as today;
- ordering is preserved across epochs;
- a normal single attestation of every Solana-bound type fits by itself;
- estimator coverage is exhaustive for all current outbound-capable Solana
  attestation types, including explicit zero-cost rows;
- adding a new outbound-capable type without an estimator case fails tests;
- the estimator budgets every reserve-backed `SWAP_REMIT` and `SWAP_REVERT`
  candidate at the pessimistic `8`-account row in v1.

### Relay And `libfc` Tests

Test cases should prove:

- transaction builder rejects more than 256 legacy account keys;
- instruction account-index overflow is rejected before serialization;
- raw packet overflow is rejected before RPC submission;
- zero-data `epoch_in_finalize` transaction is built in the expected shape;
- measured relay account union is less than or equal to WIRE's estimator for
  representative native and SPL envelopes;
- dynamic extras preserve correct `account_meta` flags after deduplication;
- every current Solana-side effect branch has a manifest fixture;
- reserve-backed relay manifests derive native-vs-SPL mode and SPL account
  addresses from pinned Reserve custody state, not mutable token-binding rows;
- reserve-backed handlers convert depot-frame amounts using pinned Reserve
  custody decimals, not mutable precision rows;
- `RESERVE_CREATE_CANCELLED` derives/fetches the creator and native-vs-SPL
  state deterministically, derives the creator's canonical ATA, and fails
  locally if the required refund accounts cannot be identified.

### Solana Program Tests

Test cases should prove:

- non-final upload chunks still append data;
- zero-data `epoch_in_finalize` rejects incomplete buffers;
- zero-data `epoch_in_finalize` processes a complete buffer;
- consensus/effect processing remains inside terminal finalization;
- dynamic `remaining_accounts` are consumed exactly as expected;
- each current effect branch succeeds with the relay-generated account
  manifest, or reaches its documented non-throwing rejection path for invalid
  input;
- SPL `SWAP_REMIT` does not create recipient ATAs during terminal
  finalization; a missing or uninitialized recipient ATA queues the documented
  swap-rejected response instead of silently dropping value;
- every reserve creation path stores pinned custody mint/native mode and
  decimals;
- changing `set_token_address` after reserve creation does not change the
  custody mode or mint used by reserve-backed finalization;
- changing `set_token_precision` after reserve creation does not change the
  decimals used by reserve-backed finalization;
- `create_reserve` rejects non-canonical SPL source token accounts;
- `RESERVE_CREATE_CANCELLED` SPL refund uses the creator canonical ATA;
- `RESERVE_CREATE_CANCELLED` SPL refund recreates the canonical ATA when it was
  closed after reserve creation;
- buffer cleanup matches current final-chunk behavior.

### Cross-Repo CI Gate

The estimator duplicates knowledge across `wire-sysio`, the relay, and
`wire-solana`. Add a parity fixture to CI so any account-layout drift fails
before merge.

At minimum, CI should compare:

- WIRE-estimated terminal packet bytes vs relay-built actual bytes;
- WIRE-estimated dynamic accounts vs relay extracted account union;
- relay account-meta manifest vs Solana handler-required accounts and flags;
- WIRE static constants vs measured terminal no-extra transaction layout;
- current Anchor IDL account layout vs the constants used by WIRE.

## Rollout Notes

This is a coordinated change across WIRE contracts, the relay/client, and the
Solana outpost program.

Rollout is a coordinated cutover. Do not support mixed old/new terminal modes
as a temporary compatibility layer. Operators must run the matching Solana
program, relay/client, and WIRE contract versions for the SEC-94 release.
Deployment should include an IDL/program-version check in the relay so a
mismatched operator fails locally before submitting any chunks.

Rollout order:

1. land measurement fixtures and builder guards;
2. land dedicated zero-data `epoch_in_finalize` in `wire-solana`;
3. land relay support for the new terminal flow;
4. land Solana-aware envelope creation in `sysio.msgch`;
5. run cross-repo parity tests;
6. set launch SLOs and backlog alerts;
7. coordinate operator deployment to the matching versions;
8. enable in production only after measured constants are pinned and all
   operators have the cutover build.

Budget constants are consensus parameters once they affect `buildenv` packing.
Changing them after deployment requires coordinated contract, relay, and
program updates.

## PR Sign-Off Items

Before the implementation PR is accepted, reviewers should explicitly sign off
on two bundled decisions:

1. Scope:
   SEC-94 includes both the transaction-budget liveness fix and the
   reserve-custody/precision immutability correctness fix. The latter touches
   value-moving reserve/refund paths and needs focused review.

2. Capacity:
   Launch capacity planning uses the pessimistic WIRE estimator. For
   reserve-backed swaps, v1 budgets `8` dynamic accounts per swap even when the
   relay's exact manifest is smaller.

## Remaining Production Inputs

1. Measurement fixture ownership:
   The canonical terminal-layout fixture lives in `wire-sysio` because the
   relay/client builds the actual Solana transaction submitted to RPC.
   `wire-solana` owns companion program tests and the Anchor IDL/account-layout
   source that the fixture consumes.

2. Production Solana-bound capacity:
   Set the launch SLO from the measured hard dynamic-account budget and the
   pessimistic WIRE estimate. For reserve-backed swaps, v1 should assume `8`
   dynamic accounts per swap. With an example `18`-account hard budget, that is
   about `2` swaps per epoch hard cap and about `1` swap per epoch at a 50
   percent SLO. If expected demand exceeds that SLO, add source pacing or an
   ALT throughput track before production.

3. Future solo-over-budget policy:
   Current valid attestation shapes should fit solo. If a future shape cannot,
   define source-level admission or explicit remediation before enabling it.

## Non-Goals

- Do not change OPP protobufs for this first version.
- Do not change local Solana account indices to `uint16_t`.
- Do not add post-consensus crank or paging.
- Do not create multiple outbound envelopes per outpost/epoch in the first
  implementation.
- Do not rely on relay-side sharding to reinterpret an already committed
  envelope.
- Do not add generic automatic refund behavior in `sysio.msgch`.
- Do not add the production governance recovery path for a drift-committed
  unrepresentable envelope in the first dev-phase implementation.

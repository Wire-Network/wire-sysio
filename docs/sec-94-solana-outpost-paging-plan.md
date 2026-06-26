# SEC-94 Solana Outpost Finalization Paging Plan

Status: design plan, not implementation

Related issue: SEC-94 / WSA-212

Primary repositories:

- `Wire-Network/wire-solana`: Solana outpost program protocol and Anchor IDL
- `Wire-Network/wire-sysio`: native Solana outpost relay/client

Reference inspected for this plan:

- `Wire-Network/wire-solana` `origin/master` at
  `65190fc74c39a2a77ec9dd76dd7bc70ba420803a`
- Important caveat: that revision does not contain the OPP outpost program or
  `epoch_in` instruction. It contains batching/cranking precedents in
  `liqsol-core` and `validator-leaderboard`, which this plan uses as the
  Solana-side pattern to apply to the OPP outpost implementation branch.

## Problem Description

The current `wire-sysio` Solana relay sends an outbound WIRE OPP envelope to the
Solana outpost by calling `outpost_solana_client::deliver_outbound_envelope`.
That method splits the envelope bytes into `epoch_in` chunks. Non-final chunks
only append chunk data to the per-operator chunk buffer. The final chunk also
passes every dynamic Solana account that the outpost program may need while
finalizing the envelope.

The final chunk currently carries all dynamic accounts at once:

- recipient/operator SOL wallets for `WITHDRAW_REMIT`;
- depositor SOL wallets for `DEPOSIT_REVERT`;
- recipient SOL wallets for `SWAP_REMIT`;
- per-reserve PDAs derived from `(token_code, reserve_code)`;
- SPL reserve vault PDAs;
- SPL recipient associated token accounts;
- SPL mint accounts;
- the SPL Token Program id when SPL transfers are present.

Those accounts are appended as Anchor `remaining_accounts` after the IDL-declared
`epoch_in` accounts. This makes the final chunk fundamentally different from the
earlier chunks: it is not just writing bytes, it is also triggering
`finalize_envelope` and any inline inbound/outbound side effects on the Solana
program.

The failure mode is account growth, not envelope byte growth alone. The envelope
has a byte cap and chunk size, but the final transaction can still grow beyond
Solana transaction constraints because the dynamic `remaining_accounts` list is
derived from the contents of the envelope. The `fc::network::solana` transaction
builder compiles instruction account indices as `uint8_t`, so any transaction
that needs more than 256 unique account keys cannot be represented correctly.
The raw transaction packet size can also exceed Solana's packet limit before the
account index limit is reached.

When that final transaction fails today, the earlier chunk transactions may
already have succeeded. The batch operator catches the Solana/RPC exception,
logs the failure, and does not advance its local outbound epoch cursor. That
means the same epoch is retried, but without a protocol path that can make the
large finalization smaller, the retry can hit the same final transaction limit
again.

## Why A Protocol Change Is Needed

A client-side guard that rejects `> 256` account keys would prevent malformed
transactions and make the failure earlier and clearer, but it would not recover
the epoch. It would turn an opaque Solana transaction failure into a deterministic
operator-visible error.

SEC-94 needs stronger liveness: finalization must be able to process a large
envelope across multiple Solana transactions. That requires the Solana program
to understand partial finalization progress, because Solana transaction
atomicity only covers one page at a time. The program must store enough state to
know which envelope is being finalized, which page is expected next, and whether
completion has already emitted the outbound result.

The proposed fix is protocol-aware sharding/paging:

- upload envelope chunks as before;
- record consensus/delivery as before;
- when consensus is reached, initialize a durable finalization session;
- process finalization items in multiple ordered pages;
- advance an on-chain cursor only after a page succeeds;
- emit/commit the final outbound result only after all pages are complete.

## Protocol Layers And Protobuf Impact

There are two different protocol layers involved:

1. OPP envelope protocol: the protobuf schema under
   `libraries/opp/proto/sysio/opp/`, including `Envelope`, `Message`, and
   attestation payloads.
2. Solana outpost execution protocol: Anchor instructions, IDL-declared
   accounts, PDAs, and `remaining_accounts` contracts used by the Solana outpost
   program and the `wire-sysio` relay.

This plan changes the second layer. The envelope bytes and attestation meanings
do not need to change simply to page Solana finalization. The current OPP
`Envelope` already contains the full ordered message stream, and the Solana
program can derive page items from that canonical envelope after consensus.

Therefore, the expected implementation does not require an OPP protobuf change
if finalization paging remains an execution detail of the Solana outpost
program. In that design:

- `libraries/opp/proto/sysio/opp/opp.proto` remains unchanged;
- `libraries/opp/proto/sysio/opp/attestations/attestations.proto` remains
  unchanged;
- the page cursor/session lives in Solana account state, not inside the OPP
  envelope;
- the new page instruction is represented in the Anchor IDL, not in OPP
  protobuf.

A protobuf change would be required only if WIRE needs the paging lifecycle to
be part of the cross-chain OPP contract. Examples:

- adding an OPP attestation that reports Solana finalization page progress;
- adding an OPP control message that tells a remote chain which page to process;
- changing the `Envelope` format to carry page metadata or page hashes;
- making page completion a fact that the depot or other outposts must verify
  from OPP messages rather than from Solana program state.

Those options are heavier because they alter generated C++/CDT/TypeScript/
Solana models and the semantic envelope shared by every OPP participant. For
SEC-94, the narrower and safer change is to keep OPP protobuf stable and add
page-aware execution to the Solana outpost program plus the `wire-sysio` relay.

## Existing Solana Paging Patterns

Although `wire-solana` `origin/master` does not include the OPP outpost program,
it already uses the right pattern in other programs:

- `BatchOrchestrator` stores durable cursors and epoch pins for batch work:
  [`programs/liqsol-core/src/states/batch_orchestrator.rs`](https://github.com/Wire-Network/wire-solana/blob/65190fc74c39a2a77ec9dd76dd7bc70ba420803a/programs/liqsol-core/src/states/batch_orchestrator.rs#L14-L109)
- `reset_cursor_if_stale` protects cursor-bearing work from stale epoch state:
  [`batch_orchestrator.rs`](https://github.com/Wire-Network/wire-solana/blob/65190fc74c39a2a77ec9dd76dd7bc70ba420803a/programs/liqsol-core/src/states/batch_orchestrator.rs#L269-L299)
- stake and unstake instructions validate the `remaining_accounts` batch shape,
  verify that the accounts are contiguous from the current cursor, process the
  batch, and then advance progress:
  [`stake_operations.rs`](https://github.com/Wire-Network/wire-solana/blob/65190fc74c39a2a77ec9dd76dd7bc70ba420803a/programs/liqsol-core/src/instructions/stake_controller/stake_operations.rs#L84-L170)
  and
  [`stake_operations.rs`](https://github.com/Wire-Network/wire-solana/blob/65190fc74c39a2a77ec9dd76dd7bc70ba420803a/programs/liqsol-core/src/instructions/stake_controller/stake_operations.rs#L1211-L1222)
- `syncValidatorStakes.ts` shows the client loop: read cursor, build a bounded
  `remainingAccounts` slice, add compute budget instructions, send, repeat:
  [`syncValidatorStakes.ts`](https://github.com/Wire-Network/wire-solana/blob/65190fc74c39a2a77ec9dd76dd7bc70ba420803a/scripts/stake-controller/syncValidatorStakes.ts#L149-L238)
- `setup_validator_pdas_batch` verifies derived PDAs for a contiguous range and
  advances `infra_next_index`:
  [`selection_operations.rs`](https://github.com/Wire-Network/wire-solana/blob/65190fc74c39a2a77ec9dd76dd7bc70ba420803a/programs/liqsol-core/src/instructions/validator_registry/selection_operations.rs#L489-L607)
- `crank_update_scores` documents an ordered `remaining_accounts` layout and
  stores `crank_next_index`:
  [`crank_update_scores.rs`](https://github.com/Wire-Network/wire-solana/blob/65190fc74c39a2a77ec9dd76dd7bc70ba420803a/programs/validator-leaderboard/src/instructions/crank_update_scores.rs#L22-L33)

The OPP outpost fix should follow the same shape: persistent cursor, exact
remaining-account verification, page-at-a-time progress, and restartable client
orchestration.

## Proposed Protocol Change

### Goals

1. Keep the chunk-upload protocol compatible where possible.
2. Avoid putting all dynamic finalization accounts in the final `epoch_in`
   transaction.
3. Make finalization resumable after RPC errors, process restarts, or page
   transaction failures.
4. Preserve consensus semantics: the outpost must not emit/commit the final
   outbound result until the agreed envelope has been fully processed.
5. Make account order and account validation deterministic enough that bad,
   missing, duplicated, or out-of-order accounts are rejected before cursor
   advancement.
6. Keep the native relay responsible for deriving page accounts, but make the
   Solana program the authority that validates those accounts against the
   canonical envelope and on-chain cursor.

### New Or Updated Program State

Add a durable finalization state account, or extend an existing outpost state
account if the implementation branch already has the right storage boundary.
The state should be shared per finalized envelope, not per uploading operator.

Suggested PDA:

```text
["finalization", epoch_index_le, envelope_hash]
```

Suggested fields:

```text
epoch_index: u32
envelope_hash: [u8; 32]
total_bytes: u32
total_items: u32
next_item_index: u32
phase: u8
completion_emitted: bool
started_slot: u64
started_epoch: u64
bump: u8
```

The session also needs access to the canonical envelope bytes while pages are
processed. There are two viable storage approaches:

1. Reuse the existing canonical inbound-envelope storage if the outpost program
   already persists the agreed envelope bytes outside the per-operator chunk
   buffer.
2. Introduce a shared finalization-envelope data account seeded by epoch and
   envelope hash. On consensus reach, copy the agreed bytes from the triggering
   operator's chunk buffer into this shared account and keep it until
   finalization completes.

The per-operator chunk buffer should not be the only source of truth after
consensus. If it is closed before all pages are processed, later page
transactions would have no deterministic on-chain source from which to derive
expected accounts and effects.

### Instruction Flow

#### 1. `epoch_in` non-final chunks

Non-final chunks remain unchanged:

- write the chunk into the per-operator chunk buffer;
- do not pass dynamic finalization accounts;
- do not process envelope effects.

#### 2. `epoch_in` final chunk

The final chunk should stop doing full envelope finalization inline.

Instead, after reconstructing and validating the envelope, it should:

1. compute the canonical `envelope_hash`;
2. record this operator's delivery for `(epoch_index, operator, envelope_hash)`;
3. check whether the outpost consensus threshold has been reached for the same
   envelope hash;
4. if consensus is not reached, return success after recording delivery;
5. if consensus is reached and no finalization session exists, initialize the
   finalization session and canonical envelope data;
6. if consensus is reached and a session already exists, leave it unchanged;
7. close only the caller's per-operator chunk buffer when it is safe to do so.

The final `epoch_in` transaction may still need fixed accounts, but it should
not require the full dynamic account set. It may require no dynamic
`remaining_accounts` at all.

#### 3. `process_finalization_page`

Add a page/crank instruction, with a name such as:

```text
process_finalization_page(epoch_index, envelope_hash, start_item_index, item_count)
```

Required fixed accounts should include the same state accounts used by the
current finalization path, plus the finalization session and canonical envelope
data account. Dynamic effect accounts are passed through `remaining_accounts`.

The instruction should:

1. load the finalization session;
2. require `phase == Processing` or equivalent;
3. require `start_item_index == next_item_index`;
4. decode or scan the canonical envelope to the requested item range;
5. derive the exact expected accounts for every item in the page;
6. consume `ctx.remaining_accounts` in deterministic order;
7. require every supplied account to match the expected pubkey, mutability, and
   ownership constraints for that item;
8. process the page effects;
9. require that no extra remaining accounts were supplied;
10. advance `next_item_index` by the number of successfully processed items.

The page transaction should be atomic. If account validation or processing fails,
the cursor does not advance and the same page can be retried.

#### 4. Completion

Completion can be a separate instruction, such as:

```text
complete_finalization(epoch_index, envelope_hash)
```

or it can be the final branch of `process_finalization_page` when
`next_item_index == total_items`.

Completion should:

1. require all items have been processed;
2. require `completion_emitted == false`;
3. emit or commit the final outbound result once;
4. set `completion_emitted = true`;
5. mark the epoch/envelope finalized;
6. close temporary chunk/finalization data accounts when safe.

The important semantic change is that "consensus reached" and "finalization
effects completed" become separate states. Outbound emission should move to the
second state.

### Finalization Item Stream

The program and relay need a shared deterministic definition of a "finalization
item." The safest definition is the canonical traversal order of decoded inbound
OPP envelope actions/attestations, including entries that do not require dynamic
Solana accounts.

Each item has a known account layout. Example layouts based on the current
`wire-sysio` dynamic account extraction are:

| Item kind | Dynamic accounts for that item |
| --- | --- |
| `WITHDRAW_REMIT` | operator/recipient SOL wallet |
| `DEPOSIT_REVERT` | depositor SOL wallet |
| native `SWAP_REMIT` | recipient SOL wallet, reserve PDA |
| SPL `SWAP_REMIT` | recipient SOL wallet if still required by the handler, reserve PDA, reserve vault PDA, recipient ATA, SPL mint, SPL Token Program |
| reserve lifecycle item requiring a reserve PDA | reserve PDA |
| item with no Solana side effect | no dynamic accounts |

The exact table should live in the Solana outpost program as the source of truth.
The `wire-sysio` relay should mirror that table only to build candidate pages.
The Solana program must still verify every account from the canonical envelope
before executing side effects.

### Remaining Account Contract

The current implementation uses order-independent lookup helpers for some
remaining accounts. Paging should move to a stricter page-local contract:

- accounts are supplied in canonical item order;
- each item consumes a deterministic account subsequence;
- duplicate pubkeys are allowed when two items legitimately reference the same
  account, because the instruction account-index list can reference the same
  key more than once;
- extra accounts are rejected;
- missing accounts are rejected;
- accounts for future pages are rejected;
- accounts for prior pages are rejected by the cursor check.

This mirrors the `wire-solana` batching examples, where the program verifies
that the batch accounts match the expected contiguous cursor range.

### Error And Retry Semantics

The intended recovery behavior is:

- If a page transaction fails, Solana rolls back the page, and the cursor remains
  unchanged.
- The same batch operator can retry the same page.
- If the process restarts, the relay reads the finalization session cursor and
  resumes from `next_item_index`.
- If another operator/cranker races and advances the cursor first, the retrying
  client rereads state and continues from the new cursor.
- If a duplicate old page is submitted after success, the program rejects it
  because `start_item_index != next_item_index`.
- If the session is already complete, the relay treats the epoch as delivered
  rather than re-uploading chunks.

The batch operator should keep its existing outer error handling: if
`deliver_outbound_envelope` throws, it logs the failure and does not advance
`_last_outbound_epoch`. The change is that retries now have an on-chain cursor
from which they can resume instead of retrying one oversized final transaction.

## `wire-solana` Change Plan

Because `origin/master` does not contain the OPP outpost program, these changes
must be applied to the branch/repo revision that actually owns
`programs/opp-outpost`. If the OPP outpost is later merged into `origin/master`,
the same plan applies there.

### Program State

Add finalization state near the existing outpost envelope/chunk/delivery state:

- `FinalizationSession` account;
- optional `FinalizationEnvelopeData` account if the current program does not
  already persist the canonical envelope bytes;
- constants for PDA seeds and account size limits;
- an enum-like phase field for `Idle`, `Processing`, and `Complete`, or the
  closest style already used by the program.

The account should have reserved bytes for future migrations, following the
`BatchOrchestrator` style in `liqsol-core`.

### Instruction Changes

Update `epoch_in`:

- keep chunk writes unchanged;
- on final chunk, reconstruct the envelope and record operator delivery;
- initialize or find the finalization session after consensus;
- stop requiring full dynamic effect accounts on the final chunk;
- stop processing all effects inline;
- stop emitting outbound inline until finalization pages are complete.

Add `process_finalization_page`:

- require the finalization session and canonical envelope data;
- require the start index equals the stored cursor;
- validate exact page-local `remaining_accounts`;
- execute only the page's effects;
- advance the cursor;
- complete and emit when the cursor reaches the end, or leave that to
  `complete_finalization`.

Optionally add `complete_finalization`:

- useful if the final page can process effects but still needs a small,
  separately bounded completion transaction;
- also useful if outbound emission has a distinct account set from effect
  processing.

Add cleanup behavior:

- close per-operator chunk buffers after they are no longer needed;
- close finalization envelope/session accounts only after completion has been
  recorded and emitted exactly once;
- consider an admin or permissioned cleanup instruction for stale sessions if an
  epoch is abandoned by governance or migration.

### Account Validation

For each finalization item, the program should derive expected pubkeys from the
canonical envelope and known PDA seeds. The page instruction should validate:

- expected pubkey;
- expected writable/read-only role;
- program ownership where relevant;
- SPL mint/address relationship for SPL transfers;
- associated token address derivation for SPL recipients;
- reserve PDA and reserve vault PDA seeds;
- no remaining accounts left unconsumed after the requested page.

The program must not trust the relay's page construction. The relay chooses a
page size for transaction limits; the program validates the page against the
canonical envelope.

### Tests

Add Solana program tests for:

- final `epoch_in` initializes finalization but does not require all dynamic
  accounts;
- `process_finalization_page` advances the cursor for a valid first page;
- later pages resume from the stored cursor;
- out-of-order pages are rejected;
- wrong recipient/reserve/ATA/mint accounts are rejected;
- extra accounts are rejected;
- missing accounts are rejected;
- duplicate page after cursor advancement is rejected or treated as an explicit
  no-op, whichever behavior is chosen and documented;
- SPL and native paths;
- completion emits once and cannot be repeated;
- page failure leaves cursor unchanged;
- large envelopes that previously exceeded one transaction now complete across
  multiple pages.

## `wire-sysio` Change Plan

The `wire-sysio` side should become a page orchestrator. It should not make the
Solana program trust client-side derivation, but it must derive the same accounts
to build valid page transactions.

### Client API Shape

Extend `opp_solana_outpost_client` with methods along these lines:

```cpp
std::string epoch_in(..., std::vector<solana_public_key> extra_remaining_accounts);

std::optional<finalization_session>
get_finalization_session(uint32_t epoch_index, const envelope_hash_t& hash);

std::string process_finalization_page(
   uint32_t epoch_index,
   const envelope_hash_t& hash,
   uint32_t start_item_index,
   uint32_t item_count,
   std::vector<account_meta> page_remaining_accounts);

std::string complete_finalization(uint32_t epoch_index, const envelope_hash_t& hash);
```

The exact C++ types should follow the existing `fc::network::solana` client
style. If the Solana program exposes an Anchor account for the session, add a
small decoder for that account so the relay can read `next_item_index`,
`total_items`, and completion state.

### `deliver_outbound_envelope` Flow

Update `outpost_solana_client::deliver_outbound_envelope` to:

1. compute the envelope hash locally;
2. check whether a finalization session already exists for the epoch/hash;
3. if no session exists and this operator has not already delivered, upload
   chunks through `epoch_in` as today, but pass no all-envelope dynamic accounts
   on the final chunk;
4. after final chunk, read the session state;
5. if consensus is not reached and no session exists, return success for this
   operator's delivery;
6. if a session exists and is incomplete, loop over pages until completion or
   deadline;
7. before each page, reread or refresh the cursor if another cranker may have
   advanced it;
8. build a page from `next_item_index` that fits Solana account-key and packet
   constraints;
9. send `process_finalization_page`;
10. repeat until the session reports complete;
11. return the signature of the last page/completion transaction.

If a retry happens after final chunk success but before all pages complete, the
method should skip any chunk upload that the Solana program already recorded and
resume from the finalization session cursor. This is essential because the retry
boundary in `outpost_opp_job` is the whole `deliver_outbound_envelope` call.

### Page Builder

Refactor the existing full-envelope dynamic account extraction into a page-aware
planner:

- parse the envelope once into a deterministic finalization item stream;
- for each item, derive that item's dynamic account metas in canonical order;
- build a page by appending whole items until adding the next item would exceed
  a conservative transaction limit;
- never split one item across pages;
- if one item alone exceeds the limit, throw a clear protocol error because the
  program/account model for that item must change;
- preserve duplicate account metas where the page contract requires them;
- use the SPL mint cache for SPL `SWAP_REMIT` page planning;
- keep the deadline check before chunk upload and before each page transaction.

The page size should be selected by actual transaction shape, not by a fixed
number of attestations. A good first implementation is:

1. build fixed accounts and compute-budget pre-instructions;
2. append candidate item accounts;
3. estimate or build the transaction;
4. require `account_keys.size() <= 256`;
5. require serialized transaction bytes stay under Solana's packet limit with a
   safety margin;
6. remove the last item if it would exceed the limit.

Even with protocol paging, the generic Solana transaction builder should still
reject transactions with more than 256 unique account keys before narrowing
indices to `uint8_t`. That guard is a safety net for all Solana client callers,
not the primary SEC-94 recovery mechanism.

### Likely Files

Expected `wire-sysio` files:

- `libraries/opp/proto/sysio/opp/*.proto`
  - no change expected for the preferred design;
  - change only if page progress/completion becomes an OPP-level message rather
    than Solana execution state.
- `plugins/outpost_solana_client_plugin/src/outpost_solana_client.cpp`
  - replace all-at-once remaining-account collection with page planning;
  - orchestrate finalization-session resume;
  - keep SPL mint cache integration.
- `plugins/outpost_solana_client_plugin/include/sysio/outpost_solana_client_plugin.hpp`
  - add program client lambdas for reading finalization state and processing
    pages;
  - add PDA derivations for finalization session/data accounts.
- `plugins/outpost_solana_client_plugin/include/sysio/outpost_solana_client_plugin/outpost_solana_client.hpp`
  - add helper structs for finalization items/pages and account derivation.
- `libraries/libfc/src/network/solana/solana_client.cpp`
  - add a hard guard before writing `uint8_t` account indices;
  - preferably add a reusable transaction size/account-count validation helper.
- `libraries/libfc/src/network/solana/solana_types.cpp`
  - validate serialization boundaries if needed.
- `plugins/batch_operator_plugin/src/outpost_opp_job.cpp`
  - likely no semantic change, but logs may need to distinguish chunk delivery,
    awaiting consensus, page progress, and completion.
- tests under `plugins/outpost_solana_client_plugin/test/` and
  `libraries/libfc/test/network/solana/`.

### `wire-sysio` Tests

Add focused tests for:

- page planner keeps every generated page under the configured account-key
  limit;
- page planner keeps pages under a serialized transaction-size cap where a
  transaction estimate is available;
- final chunk no longer receives all dynamic accounts;
- native and SPL `SWAP_REMIT` page account derivation;
- duplicate dynamic accounts within one page;
- resume from a mocked finalization session cursor;
- retry after page failure does not restart from page zero;
- `solana_client::create_transaction` rejects more than 256 unique account keys
  instead of truncating indices;
- `deliver_outbound_envelope` returns after chunk delivery when consensus is not
  reached, but loops pages when finalization is ready.

The existing `test_fc` Solana client tests are the natural place for the generic
transaction-builder guard. The outpost plugin tests are the natural place for
the page planner and relay orchestration.

## Behavior After The Fix

For ordinary small envelopes, behavior should be almost unchanged except that
the final chunk starts a finalization session and the relay may submit one
additional page transaction. If desired, the Solana program can allow the final
chunk to immediately process page zero only when it still fits, but that is an
optimization and should not be required for correctness.

For large envelopes, operators should see bounded page progress instead of one
oversized final transaction:

```text
epoch_in chunks uploaded
consensus reached
finalization session initialized
page 0 processed
page 1 processed
...
completion emitted once
batch operator advances epoch
```

For failures:

- malformed or oversized single-item pages produce explicit protocol errors;
- transient RPC failures retry from the stored cursor;
- wrong accounts fail the current page without corrupting progress;
- already-completed sessions are treated as complete rather than replayed.

## Compatibility And Rollout

This is a Solana outpost instruction/IDL change. It is not expected to be an OPP
protobuf wire-format change unless page progress/completion is deliberately made
part of the cross-chain OPP message contract. `wire-sysio` and the Solana
outpost program must be rolled out together or gated by program/IDL version.

Recommended rollout:

1. Add the Solana program state and instructions.
2. Update the Anchor IDL.
3. Update `wire-sysio` to detect the new IDL/instruction set.
4. Keep a clear error if the relay is configured with an old outpost program
   that only supports all-at-once finalization.
5. Deploy the Solana program.
6. Roll out batch operators with the paged relay.
7. Monitor page progress logs and completion counts.

Backward compatibility options:

- hard cutover by program id/IDL version;
- feature flag in outpost config;
- dual client path that uses paging when `process_finalization_page` exists and
  otherwise rejects large finalizations with an explicit old-program error.

The hard cutover is simpler and safer if the OPP outpost is not yet deployed to
production. Dual path is safer if existing deployments must stay live during a
rolling upgrade.

## Open Questions

1. Where does the current Solana outpost program persist canonical inbound
   envelope bytes after consensus? If it only uses the per-operator chunk buffer,
   add a shared finalization data account.
2. Should completion be folded into the last page or kept as a separate
   instruction? This depends on the outbound emission account set and transaction
   size.
3. Should page planning include items with no dynamic accounts, or should the
   program skip those while advancing to the next account-requiring item? The
   simpler, more auditable design is to page over the full canonical item stream.
4. What is the exact finalization item/account table in the current
   `programs/opp-outpost` branch? The table in this document is inferred from
   the current `wire-sysio` relay and must be reconciled with the Rust handler.
5. Should any account lookup tables be introduced later? They may help account
   capacity, but paging should still be implemented because program heap/CU and
   transaction atomicity remain bounded.
6. What operator is allowed to process pages? The simplest model is any valid
   operator/cranker can advance the shared session after consensus; if the
   outpost wants stricter permissions, encode that in the page instruction.

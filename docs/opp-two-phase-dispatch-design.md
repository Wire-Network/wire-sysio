# Two-phase OPP inbound dispatch on the Solana outpost

Status: design, approved 2026-08-10. Supersedes the fused terminal-call design
currently on `feat/resumable-opp-dispatch` (wire-sysio #552, wire-solana #419).

## Problem

A Solana transaction cannot carry the effect accounts for every attestation in an
OPP envelope. The packet limit is 1232 bytes serialized and every dynamic account
costs 32 bytes in the message, so a large envelope — many SPL swap remits across
distinct reserves — needs more accounts than one transaction can declare. Only
the relay can size a batch, because only the relay supplies the accounts and
therefore knows the transaction's real byte cost.

The depot previously solved this by modelling Solana's transaction capacity
inside a WIRE consensus contract (`estimate_svm_dynamic_accounts`, the
`svm_terminal_budget_fits` gate in `buildenv`, and a `queueout` admission check
that refused to queue any Solana-bound attestation the estimator did not
recognise). That coupling was deleted in `c0233d208` for good reasons: it put
another chain's MTU inside depot consensus, and adding an attestation type meant
editing a depot contract's estimator or the depot rejected it outright. This
design replaces it.

## Why the fused design fails

The current implementation makes the terminal `epoch_in` call do two jobs at
once:

1. **Record delivery and tip consensus** — consensus-critical, must always
   succeed, fixed and tiny.
2. **Dispatch effects** — resource-bounded, account-hungry, can legitimately fail
   to fit.

Fusing them means every way dispatch can fail becomes a way consensus can wedge.
The four defects found in review of #552 are all faces of this one choice:

| Defect | Site | Mechanism |
|---|---|---|
| Cursor never seeded from chain | `outpost_solana_client.cpp:930` | `settled = 0`; the cursor is first read at `:963`, *after* the first terminal call. Re-entry packs accounts for `[0, batch_end)` while the program settles from its own `dispatched_count` → `EffectAccountMissing` → throw → delivery never recorded. Permanent wedge on exactly the multi-round case the feature exists for. |
| Zero-attestation assert | `:950` | `count_inbound_attestations` returns 0 on `ParseFromArray` failure (`:411-416`), so `dispatch_limit == 0` and `FC_ASSERT` fires *before any terminal call*. Chunks are already staged; delivery is never recorded; consensus can never tip. Regression vs. the merge base, which sent the terminal finalize unconditionally. |
| Silent round exhaustion | `:931` | After `MAX_TERMINAL_ROUNDS` the loop falls through with no log and no error, returns `last_sig` as success, and strands the cursor with `_last_outbound_epoch` advanced. |
| Retry chunk re-upload | `batch_operator_plugin/src/outpost_opp_job.cpp:120-134` | `_last_outbound_epoch` advances only on a clean return of the whole fused operation, so any dispatch failure re-sends **all** data chunks from offset 0 every 15s, unbounded, with the re-upload eating the same delivery budget. |

Patching these individually leaves the shape that produced them.

## The design

Split the two jobs into two instructions.

### `epoch_in` terminal call — delivery and consensus only

The terminal call records this operator's delivery, runs the consensus predicate,
and stops. It carries **no effect accounts and no `dispatch_limit`**. It is
fixed-size and cannot fail on account budget, so consensus can never wedge on a
dispatch concern.

Data-chunk calls are unchanged.

### `dispatch_attestations(epoch_index, dispatch_limit)` — permissionless crank

A new instruction that settles `[dispatched_count, dispatched_count +
dispatch_limit)` using the account manifest the caller supplies.

- **Permissionless.** Any caller may crank. This is what lets a wedged outpost be
  unstuck by someone other than the operator whose relay gave up. It is not a
  user-discretion surface (see "Relationship to `outpost-remit-is-immediate`").
- **The cursor is the only input.** The instruction reads `dispatched_count` from
  `EpochDeliveries` and settles from there. The caller's `dispatch_limit` bounds
  how far, never where from. This makes the unseeded-cursor defect
  unrepresentable rather than fixed.
- **Requires `consensus_reached`.** Cranking before consensus tips is an error,
  not a silent no-op.
- Needs a new instruction discriminator, and any new failure mode gets an error
  code appended after the existing range — the numeric table in
  `opp-outpost-technical-spec.md` and `scripts/opp/patch-idl-errors.js` both key
  on stable numbering.

Per `standard-names-not-invented.md`, the instruction is named
`dispatch_attestations` (author-specified, 2026-08-10) — not
`dispatch_effects`, and the relay-side helper derives from it (`drain_dispatch`
below is the loop; the per-call wrapper is `dispatch_attestations`).

### The completion invariant — the load-bearing part

The program in wire-solana #419 already gates epoch closure on full drain
(`epoch_in.rs:618-645`):

```rust
epoch_deliveries.dispatched_count = end as u32;

if epoch_deliveries.dispatched_count < total_attestations {
    return Ok(());                       // epoch does NOT close
}

config.next_epoch_index = envelope.epoch_index.checked_add(1)?;
config.previous_epoch_hash = digest;
config.current_epoch_started_at = now_ts;
emit_outbound_inner(...)?;               // the depot's only completion signal
```

**This block must move into `dispatch_attestations`, firing on the call where the
cursor reaches `total_attestations`.** Leaving it in `epoch_in` would close the
epoch and emit outbound at consensus time with attestations still pending — the
depot would believe remits landed when they had not. That is a worse defect than
any this design fixes, and it is the single most important thing to get right in
the split.

Four things ride the draining call, all of them today gated the same way:

1. `config.next_epoch_index` advances.
2. `previous_epoch_hash` / `current_epoch_started_at` are stamped.
3. `emit_outbound_inner` fires — the outpost→depot envelope.
4. The chunk buffer self-closes (`epoch_in.rs:335-343`), already deliberately
   gated on `next_epoch_index` moving past the epoch because a partially
   dispatched envelope needs those bytes again. The crank reads the envelope from
   that buffer, so it must stay open until drain.

Because the depot learns of resolution *only* via the outbound envelope, and that
emit is gated on drain, the invariant "the OPP flow does not resolve until every
inbound attestation has dispatched" is enforced structurally by placement, not by
an assertion anyone can forget.

### Consequence: drain is a liveness requirement

`next_epoch_index` advancing is gated on drain, and Phase A turns away calls
whose epoch does not match. So an undrained cursor does not merely delay remits —
**it blocks the outpost from processing any later epoch.** Two things follow:

- Round exhaustion must **log and alarm**, never return success. Under
  `epoch-stall-is-fatal.md` a stalled epoch is terminal, and silently returning
  success converts a recoverable stall into an unattributed one.
- The permissionless crank is not a convenience. It is the recovery path.

### Relay driving

Placement redirect (2026-08-11, architecture review): the relay carries NO new
plugin interface. See
`docs/superpowers/specs/2026-08-11-contained-resumable-dispatch-relay-design.md`.

The crank runs behind the existing `outpost_client` SPI, entirely inside
`outpost_solana_client`:

- `deliver_outbound_envelope` = chunks + a zero-account terminal call, then a
  BEST-EFFORT tail drain (log-and-drop) so the operator whose delivery tips
  consensus dispatches immediately. Delivery success is decided by the
  terminal call alone, so `_last_outbound_epoch` advances on clean delivery
  and chunks are never re-uploaded.
- `read_inbound_envelope` = drain-then-read. `run_inbound` already calls it
  every tick for exactly as long as the epoch's outbound envelope is missing
  -- and that envelope only exists once the cursor drains -- so the read path
  is the standing recovery driver. `within_epoch_window()` stays open when the
  epoch is overdue, and every elected operator ticks it, so any group
  operator's tick can unstick a wedged cursor. The envelope bytes come from a
  client-side delivered-envelope memo, repopulated after a restart by the next
  outbound tick's redelivery.

`_last_outbound_epoch` semantics are unchanged from master: it advances on
successful **delivery**, which is its actual job (do not re-upload chunks).
Dispatch progress is tracked separately by the on-chain cursor. This decouples
the two failure domains and removes the chunk re-upload storm without needing a
chunk-buffer probe.

Per `never-swallow-rpc-errors-ts.md`'s C++ analogue in this plugin, every crank
failure path logs the chain-side reason (`custom program error: 0x…`) rather than
being swallowed by the tick loop.

### Test flows

The crank loop has ONE implementation -- `drive_dispatch_rounds`, internal to
`outpost_solana_client` and factored over its RPC touchpoints for the unit
tests. Flows exercise dispatch through the ordinary delivery/read cycle: the
tail drain settles the tipping operator's epoch inline, and the read path's
drain covers everything else, so a flow observes the same envelope round-trip
the fused design produced.

### Cursor read commitment

`execute_tx_and_confirm` returns at `processed` commitment
(`libfc solana_client.hpp:45`); `get_account_info` defaults to `confirmed`
(`:586`). The cursor read must pin `processed`, or poll until the read catches up
to the write, or a lagging confirmed bank returns pre-transaction state and the
drain loop takes a false break. `read_inbound_envelope` already pins its read
commitment deliberately — follow that precedent.

### Batch sizing

`MAX_TERMINAL_DYNAMIC_ACCOUNTS = 16` is currently an unmeasured hand estimate:
the budget counts only the extras, not the ~13 IDL-declared accounts or the
compute-budget pre-instruction. The deleted SEC-94 fixture measured exactly this.
Restore a terminal-shape equivalent asserting static accounts + N extras ≤ 1232
bytes, and derive the constant from it rather than asserting it. Per the "no
magic literals" invariant in `wire-sysio/CLAUDE.md`, the derived bound lives
behind a named `constexpr`.

The always-take-one rule stays (a single oversized attestation must make progress
rather than wedge), but its justifying comment at `:936-937` is wrong twice — an
oversized attestation's accounts are all included, and a genuinely missing account
now aborts rather than logs-and-skips. The real failure is
`transaction::serialize`'s 1232-byte assert throwing locally. Rewrite the comment
to say that, and treat an unsatisfiable single attestation as an alarming
condition rather than something the depot re-drives.

## Relationship to `outpost-remit-is-immediate`

The standing rule states that every remit-class attestation triggers its transfer
inline, in the same transaction that processes the envelope, and that "the
dispatch loop IS the release."

This design does not satisfy the rule's letter. It does satisfy its intent. The
rule guards against two things: a **user-controlled** delay (a `claim()` timed to
dodge a slash) and a divergence window a party can exploit. A permissionless
crank with no user discretion, whose completion gates the epoch itself, preserves
both — a remit cannot be selectively withheld, and the epoch cannot close around
an undispatched one.

It is worth stating plainly that the *current* fused design also fails the rule's
letter, undocumented and wedge-prone. The choice is between an explicit, bounded,
recoverable deferral and an implicit one.

The rule doc and wire-solana's normative `opp-outpost-technical-spec.md` must be
updated in the same landing. The spec still codifies log-and-skip, 672-byte
chunks, and the 5-argument `epoch_in` (lines 150, 838-842, 882, 915), all of
which this work changes.

## Scope

### Resolved by the design

- Unseeded cursor — unrepresentable; the cursor is the instruction's contract.
- Zero-attestation assert — the delivery call has no manifest to build, so a
  decode failure cannot block delivery recording.
- Round-exhaustion stranding — degrades to "crank again next tick".
- Retry chunk re-upload — delivery and dispatch become independent facts.

### Remaining as explicit work

All landed on `feat/resumable-opp-dispatch` (contained placement):

- Cursor read pinned at `processed` (`read_epoch_dispatch_progress`).
- Terminal-shape packet-limit measurement restored;
  `MAX_TERMINAL_DYNAMIC_ACCOUNTS` validated against it.
- Round exhaustion elogs and returns -- the log is the alarm; the next tick
  resumes from the on-chain cursor (see the placement spec).
- The state machine is tested through its seam (`drive_dispatch_rounds`
  callbacks): packing, `dispatch_limit` sizing, cursor resume, the
  consensus/stall/exhaustion exits.
- The five whole-envelope extractors are DELETED; `extract_inbound_effects` is
  the single decode and its tests absorbed the per-view coverage.
- Stale comments corrected alongside the split.

Still open:

- Update `outpost-remit-is-immediate.md` and `opp-outpost-technical-spec.md`.

### Unrelated, fix separately

`queueout` lost its `chain_code` registration assert in `c0233d208`. The commit
deliberately deleted the SEC-94 estimator allow-list, which was correct; the
`chains_tbl.get(..., "chain_code is not registered")` lookup existed only as
scaffolding to learn `chain.kind`, so it fell out as collateral rather than by
decision. Restore the bare registration assert with no estimator and no `kind`
branch. Separately, `buildenv`'s chains fetch now uses the bare `.get()` overload
and aborts with `"key not found"`; give it the message overload. Three stale
references describe the deleted guard and must be corrected or a future reader
will conclude msgch is the backstop: `sysio.reserv.cpp:93-95`,
`sysio.msgch_tests.cpp:164-170`, `sysio.reserv_tests.cpp:411-414`.

Any change under `contracts/**` runs `contracts_unit_test -- --sys-vm` before the
commit lands, and follows the rebuild + WASM/ABI copy sequence
(`post-contract-refactor-rebuild.md`, `test-wire-sysio-contracts-before-committing.md`).

## Landing sequence

The manifest pins wire-solana `next`, on which none of this exists — the pinned
program has `MAX_CHUNK_BYTES = 672` with an exact-size requirement for non-final
chunks, no `dispatch_limit` argument (Anchor 0.31.1's
`BorshDeserialize::deserialize` tolerates trailing bytes, so a stray argument is
*silently dropped* and dispatch runs unbounded), no `dispatched_count`, and
log-and-skip on missing effect accounts. Landing wire-sysio #552 against that
program stalls SOL OPP in both directions.

Order:

1. wire-solana program change — `dispatch_attestations`, completion block moved,
   error codes appended. The abort-on-missing-account contract is **already on #419**
   (`require_remaining_account` in `instructions/opp/mod.rs`, 17 call sites in
   `inbound.rs`), so nothing needs pulling forward from #409; #409's remaining content is
   the collateral asset-identity work (SOL-375/379/380), a separate concern.
   **Decision (2026-08-10):** #409 stays an independent PR; none of its work comes into
   this branch. The ONLY overlap is error-code numbering — the two branches assign
   6064/6065 to different variants. This branch keeps `InvalidDispatchLimit` = 6064 and
   `EffectAccountMissing` = 6065; **#409 renumbers its collateral variants after this work
   lands**, updating `opp-outpost-technical-spec.md`'s numeric table and
   `scripts/opp/patch-idl-errors.js` in the same change. Do not let a merge auto-resolve
   this — git will silently produce a wrong table.
2. Merge to `next`; refresh the manifest pin.
3. wire-sysio #552 — relay split contained in `outpost_solana_client.{hpp,cpp}`:
   bare terminal + tail drain, drain-then-read recovery, commitment pin,
   measurement tests; zero plugin-interface changes.
4. Rule and spec doc updates, in the same landing as (3).

## Testing

- Program: unit coverage for cursor arithmetic, the completion gate (epoch does
  **not** close below `total_attestations`, does close at it), permissionless
  caller acceptance, and rejection before `consensus_reached`.
- Plugin: drive the packing loop through a seam — multi-round drain, resume from a
  nonzero cursor, stall break, round exhaustion alarming, and the terminal-shape
  packet-limit measurement.
- Contracts: `contracts_unit_test -- --sys-vm` for the `queueout` change.
- Flow: a SOL swap-remit flow whose envelope needs ≥ 2 dispatch rounds, asserting
  the epoch does not advance until the cursor drains. Run via the canonical pair
  (`run-flow.mjs` + `flow-heartbeat-monitor.mjs`) per
  `run-flows-via-canonical-scripts.md`.

## Open items

- Whether the batch-operator group carries a *duty* to crank (monitoring/alerting
  on an undrained cursor) in addition to the crank being permissionless.
- Sizing `dispatch_limit` per call: derived from the measured packet budget, but
  whether the relay adapts on failure or uses a fixed conservative bound.

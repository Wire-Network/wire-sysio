# System-contract upgrade order

The system contracts are not independent deployables. `sysio.epoch::advance`
inlines actions into six other contracts, and the emissions gate reads two more
contracts' tables, so a release's contract builds are only correct **as a set**.
Upgrading them one at a time creates windows in which a new caller meets an old
callee.

**The rule: deploy each compatibility-coupled SET in ONE transaction.** A single
`sysio.msig` proposal carrying that set commits or fails as a unit, so the set
has no mixed-version window. The coupled set is usually smaller than the
release — see [The deployment recipe](#the-deployment-recipe) for what goes in
it, how to deploy it, and the size ceiling that decides whether it fits.
Contracts outside the coupled set stage separately, in any order.

## The deployment recipe

"Just `setcode` everything in one transaction" fails twice over — once on which
action deploys each account, once on transaction size — and both bite this
release specifically.

**1. The deploy action depends on WHICH account holds the contract — the
transaction mixes both.**

*Separate system accounts* (`sysio.epoch`, `sysio.reserv`, `sysio.opreg`,
`sysio.uwrit`, …) deploy through **`sysio.roa::setsyscode(account, vmtype,
vmversion, code)` / `sysio.roa::setsysabi(account, abi)`**. Those set the
code/abi AND reconcile the account's gifted RAM to its exact new usage out of
sysio's pool via `giftram`, measured after the write (re-callable: a smaller
re-deploy reclaims). A raw `setcode` here skips that reconciliation and leaves
the account paying for the new size out of a finite quota it does not have —
not theoretical for this release, where `sysio.reserv.wasm` grows **6,040
bytes** and `sysio.opreg.wasm` grows **2,447 bytes** against a system account's
small creation allowance.

*The root `sysio` account* — which holds `sysio.system` and **is the RAM pool
itself** — deploys with the **native `setcode` / `setabi`**. `giftram` cannot
self-target: with `account == sysio` and a positive delta it moves the delta
from sysio's reslimit row into `sysio.acct`, then calls
`set_resource_limits(sysio, sram - delta)` and `add_system_resources(sysio,
delta)` — both against the same account, so the chain quota is restored while
the ROA ledger has already been decremented. The result is a ledger that reads
lower than the actual quota, and it fails silently: sysio's limit is finite, so
`giftram`'s own `cur_ram >= 0` guard does not catch it. This is why the
production bootstrap deploys `system` on `sysio` natively (the harness encodes
the same split as `ContractSteps.DeployMode.raw` for bios/system/roa).

So the atomic transaction for this release's trio carries **native
`setcode`/`setabi` for `sysio`** plus **ROA `setsyscode`/`setsysabi` for
`sysio.epoch` and `sysio.reserv`**.

**2. The whole release does not fit in one transaction.** The five changed
WASMs total **579,408 bytes**, already past the default
`max_transaction_net_usage` of **524,288** (`config::default_max_block_net_usage
/ 2`) before ABIs or action wrapping. `sysio.msig::propose` receives the
complete inner transaction before it chunks storage, so proposing through msig
does not dodge that input NET.

What fits is the **compatibility-coupled trio** — `sysio.reserv`,
`sysio.epoch`, `sysio.system` — the three the edges below actually couple:

| | code | abi | total |
|---|---|---|---|
| `sysio.epoch` | 75,579 | 6,758 | 82,337 |
| `sysio.system` | 174,138 | 61,916 | 236,054 |
| `sysio.reserv` | 84,026 | 24,647 | 108,673 |
| **trio** | **333,743** | **93,321** | **427,064** |

That leaves ~97 KB against the 524,288 ceiling — enough, but not by so much
that it can be assumed. **Preflight the packed size of the actual proposal**
rather than trusting this table, which is a snapshot of one release.

`sysio.opreg` and `sysio.uwrit` are uncoupled (they gain no action another
contract inlines, and no other contract reads their new tables), so they stage
in their own transaction, before or after the trio.

When a future release's coupled set does NOT fit, the options are: split off
whatever is genuinely uncoupled and stage it, or raise
`max_transaction_net_usage` via `setparams` first — as a **tested** path, with
the raise proposed and confirmed before the deploy proposal, never assumed to
work on the day.

## Why a mixed version is not merely degraded

The CDT-generated dispatcher ends every contract's `apply` with

```c
default:
  if (r != "sysio"_n.value) sysio_assert_code(false, 1);
```

so an inline action a stale callee does not implement **asserts**, and an assert
inside `advance` aborts the whole transaction. That is a chain-wide epoch stall:
no epoch advances, no envelope is built, no emission accrues, and every
`sysio.msgch::chkcons` retry takes the same path. The contracts are written so
`advance` cannot abort (`feedback_opp_handlers_never_throw.md`); a version skew
defeats that discipline from outside the contract.

`sysio` itself is the exception in that dispatcher, and it is the worse one: an
action `sysio.system` does not implement is **silently ignored** rather than
asserted. A skew there halts nothing — it drops the effect. An `accrueepoch`
that never runs looks exactly like a chain that is working.

Table reads carry no dispatcher at all, so they fail differently again — and
the failure mode depends on whether the ROW exists, not on whether the FIELD
does:

- **The whole KV key is absent** → `get_or_default` yields the default. This is
  the only case that is quietly correct, and it is the case the
  `payclaimtot` transition below actually lands in: the new singleton does not
  exist yet under an old `sysio.system`.
- **The row exists in an older, shorter encoding** → the new reader still
  decodes those bytes. The streaming path underflows; the fixed-serializable
  `kv::global` path copies `sizeof(T)` from a short read. A field ADDED to an
  EXISTING row therefore does not "default to zero" — it needs a
  layout-compatible encoding or an explicit migration, decided per change.

## The cross-contract edges

Everything `sysio.epoch::advance` inlines, directly:

| Callee | Actions |
|---|---|
| `sysio.reserv` | `sweepclaims` |
| `sysio.uwrit` | `chklocks`, `pruneuwreqs`, `drainfwq` |
| `sysio.opreg` | `recorddel`, `termcheck`, `flushwtdw` |
| `sysio.chalg` | `slashop` |
| `sysio.msgch` | `queueout`, `buildenv` |
| `sysio` | `accrueepoch`, `rcrdbatch`, `payepoch` |

Those callees inline further (`drainfwq` → `sysio.reserv::refundwire`,
`termcheck` → the `sysio.opreg` remit path, `payepoch` → `sysio.token::transfer`),
so the transitive subtree — not just the table above — sits inside `advance`'s
abort surface.

Independently of inlines, the emissions readiness gate in `sysio.epoch` **reads**
`sysio.system`'s `emitcfg`, `t5state` and `payclaimtot`, and `sysio.token`'s
`accounts`.

### WIRE-343 pre-launch activation

Activate WIRE-343 in one quiesced maintenance window, with no
`sysio.epoch::advance` between contract deployments. This remains the normal
pre-launch procedure because a complete immutable roster history is required
to credit the batch-operator payout:

1. Set `pay_cadence_epochs` to a value in `[1, 10]` before the window. New
   writes are bounded to that range; an older stored value outside it is read
   with an effective clamp until it is rewritten.
2. Prefer a completed payment period and verify that `t5state` has
   `pending_emission_amount == 0`, all `batch_group_epochs` counters are zero,
   and `batchepochs` is empty.
3. Quiesce epoch advancement and deploy the new `sysio.system` and
   `sysio.epoch` contracts together before allowing the next `advance`.

The deployed contracts also have a bounded recovery path for an accidental
mixed or mid-period deployment. `rcrdbatch` prunes the exact oldest retained
roster, and `payepoch` removes at most 20 history rows per payment, so even a
malformed gapped table cannot turn cleanup into an unbounded `advance`.
If `payepoch` sees missing, stale, non-contiguous, or over-cap history, it
retains that period's batch-emission and swap-fee slices in the treasury,
records the exact retained amounts and incomplete-history status in `epochlog`,
and drains stale history monotonically before resuming batch payouts. Producer,
capex, and governance processing still completes. The runtime also shortens a
legacy cadence when necessary to keep batch credits at or below 100 per payout.
This is a recovery path, not a replacement for the normal quiesced deployment.
Do not downgrade while `batchepochs` is non-empty. T5 must also be initialized
before its first successful epoch advance.

## The two rules for future changes

1. **A contract that gains an action `advance` inlines deploys BEFORE
   `sysio.epoch`.** Otherwise the new caller reaches an old callee that cannot
   dispatch it, and every advance aborts.
2. **A contract whose new state the gate must reserve deploys AFTER
   `sysio.epoch`.** Otherwise the new writer commits state the old gate does not
   know to reserve, and the gate authorizes what the treasury cannot cover.

**The two edges point in OPPOSITE directions along the dependency arrow, so they
cannot be collapsed into one inequality.** State them separately:

- **Call edge — `callee_version >= caller_version`.** The contract that RECEIVES
  an inlined action upgrades first, because the new caller emits an action the
  old callee cannot dispatch.
- **Table edge — `reader_version >= writer_version`.** The contract that READS
  the new state upgrades first, because the new writer commits state the old
  reader does not know to account for.

A sentence that says "the caller must never be older than the callee" inverts
the first one, and following it produces exactly the epoch-new / callee-old
state that aborts every advance.

## Staged rollout (when one transaction is not possible)

For the SEC-150 claimable-payout release the order is:

```
sysio.reserv  ->  sysio.epoch  ->  sysio.system
```

`sysio.opreg` and `sysio.uwrit` are free to land anywhere in the sequence: they
gain no action any other contract inlines (`claimremit` is user-initiated), and
no other contract reads their new tables.

| Edge | Why |
|---|---|
| `sysio.reserv` before `sysio.epoch` | The new `advance` inlines `sysio.reserv::sweepclaims`, guarded only on the account existing. An old `sysio.reserv` build has the account and not the action, so the inline asserts and every advance aborts. |
| `sysio.epoch` before `sysio.system` | The new `payepoch` retains WIRE in `payclaims` and reserves it in `payclaimtot`. The old gate counts that backing as spendable, so a later pay period can double-commit it and leave credited claims underfunded. |

Every intermediate state of that order is safe. A new `sysio.reserv` under an old
`sysio.epoch` is simply never asked to sweep — the retention deadline then rests
on `credit_wire_claim`'s opportunistic sweep until epoch catches up. A new
`sysio.epoch` under an old `sysio.system` reads a `payclaimtot` whose KV key does
not exist yet, so `get_or_default` yields a zero reserve — the correct answer
while nothing is credited, and the absent-key case rather than the
short-decode one (see [above](#why-a-mixed-version-is-not-merely-degraded)).

## Downgrades

**A downgrade is not the upgrade run backwards. It is a data-migration problem
first, and a code-ordering problem second** — because by the time you want to
roll back, all three claim tables may hold value that only the NEW code can pay
out:

| Table | Contract | Paid out by | Stranded when that contract rolls back |
|---|---|---|---|
| `payclaims` | `sysio.system` | `claimpay` | earned epoch pay |
| `wireclaims` | `sysio.reserv` | `claimwire` | swap payouts + refunds already withheld from recipients |
| `remitclaims` | `sysio.opreg` | `claimremit` | debited operator collateral |

Every one of those balances is value already taken from someone's spendable
position and parked behind an action the old build does not have. Rolling back
with rows present does not degrade — it strands.

There is also a live-writer hazard with no upgrade counterpart: rolling
`sysio.epoch` back while the new `sysio.system` is still deployed lets `payepoch`
keep CREATING claims that the reverted gate does not reserve, so the treasury
resumes double-committing while the pile of unreachable claims grows.

The safe procedure is therefore:

1. **Quiesce the credit writers** so no new claim rows appear.
2. **Drain or migrate all three claim tables** — claimants pull, or the balances
   are migrated. This step is the one that actually gates the rollback, and it
   cannot be completed unilaterally: a claimant who never claims holds it open.
3. **Roll back the coupled trio in the order `sysio.system` → `sysio.epoch` →
   `sysio.reserv`** — the mirror of the upgrade order, so the writer is retired
   before the reader that accounts for it, and the caller before the callee it
   would otherwise inline into.
4. **Roll `sysio.opreg` back only once its remits are handled.**

**A live chain with uncooperative claimants is not safely downgradeable** by
code order alone. If a rollback has to happen anyway, the outstanding balances
are a liability to settle deliberately, not a detail the deploy sequence
absorbs.

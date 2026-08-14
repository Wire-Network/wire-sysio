# System-contract upgrade order

The system contracts are not independent deployables. `sysio.epoch::advance`
inlines actions into six other contracts, and the emissions gate reads two more
contracts' tables, so a release's contract builds are only correct **as a set**.
Upgrading them one at a time creates windows in which a new caller meets an old
callee.

**The rule: deploy a release's system contracts in ONE transaction.** A single
`sysio.msig` proposal carrying every `setcode`/`setabi` action commits or fails
as a unit, so no mixed-version window exists at all. Ordering only becomes a
question when a staged rollout is forced on you; the fallback order for the
current release is in [Staged rollout](#staged-rollout-when-one-transaction-is-not-possible).

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

Table reads carry no dispatcher at all, so a reader that meets a stale writer
sees the field's default rather than an error. That direction is always silent.

## The cross-contract edges

Everything `sysio.epoch::advance` inlines, directly:

| Callee | Actions |
|---|---|
| `sysio.reserv` | `sweepclaims` |
| `sysio.uwrit` | `chklocks`, `pruneuwreqs`, `drainfwq` |
| `sysio.opreg` | `recorddel`, `termcheck`, `flushwtdw` |
| `sysio.chalg` | `slashop` |
| `sysio.msgch` | `queueout`, `buildenv` |
| `sysio` | `accrueepoch`, `payepoch` |

Those callees inline further (`drainfwq` → `sysio.reserv::refundwire`,
`termcheck` → the `sysio.opreg` remit path, `payepoch` → `sysio.token::transfer`),
so the transitive subtree — not just the table above — sits inside `advance`'s
abort surface.

Independently of inlines, the emissions readiness gate in `sysio.epoch` **reads**
`sysio.system`'s `emitcfg`, `t5state` and `payclaimtot`, and `sysio.token`'s
`accounts`.

## The two rules for future changes

1. **A contract that gains an action `advance` inlines deploys BEFORE
   `sysio.epoch`.** Otherwise the new caller reaches an old callee that cannot
   dispatch it, and every advance aborts.
2. **A contract whose new state the gate must reserve deploys AFTER
   `sysio.epoch`.** Otherwise the new writer commits state the old gate does not
   know to reserve, and the gate authorizes what the treasury cannot cover.

Both rules say the same thing from opposite ends: the *reader/caller* must never
be older than the *writer/callee* it depends on.

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
`sysio.epoch` under an old `sysio.system` reads a `payclaimtot` that has no row
yet; the KV read is contract-side, so it does not consult the stale ABI and
`get_or_default` yields a zero reserve — the correct answer while nothing is
credited.

## Downgrades

A downgrade is an upgrade with the versions swapped, so it inherits both rules in
reverse: roll back `sysio.epoch` **before** `sysio.reserv`, or the surviving new
`advance` will inline `sweepclaims` into a contract that no longer implements it.
Rolling back `sysio.system` while `payclaims` rows exist strands them — the rows
outlive the code that can pay them.

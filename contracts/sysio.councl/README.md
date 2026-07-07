# sysio.councl

Council election contract. Fills 21 council seats — one per tier-1 node owner — by electing a
candidate from a shared pool. The right to *propose* a seat's slate of 3 candidates starts with
that seat's tier-1 owner and **escalates** through tier-2 and tier-3 node owners if the earlier
tier fails to elect anyone, so every seat is guaranteed to fill. See
[DESIGN.md](DESIGN.md) for the full model and rationale.

> Implemented against the modern KV table stack (`kv::table` / `kv::scoped_table` / `kv::global`)
> and the `sysio.roa` / `sysio.system` cross-contract read pattern used by `sysio.chalg`. The pure
> election arithmetic lives in a dependency-free header
> ([`council_math.hpp`](include/sysio.councl/council_math.hpp)) and is unit-tested host-side in
> [`contracts/tests/council_math_tests.cpp`](../tests/council_math_tests.cpp).

## Responsibility

- Registers council **candidates** (a sysio account + a short handle).
- Snapshots the tiered **node-owner** sets from `sysio.roa::nodeowners` (the tier-1 roster in a
  governance-chosen order; the full tier-2 and tier-3 sets for escalation).
- Runs, per seat, a **strict-priority slate vote**: candidates are considered in submission order;
  candidate *i+1* is only considered once candidate *i* is mathematically eliminated.
- **Escalates** a failed seat T1 → T2 → T3, choosing tier-2/3 proposers pseudo-randomly from an
  in-contract entropy accumulator, and falls back to a governance assignment if tier-3 is exhausted.

## Election rules

| Quantity | Rule |
|----------|------|
| Win threshold | `floor(N·2/3) + 1` YES, where `N` is the tier's electorate size |
| Elimination | a candidate is out at `ceil(N/3)` NO (can no longer reach the threshold) |
| Tier-1 | `N = 20` (the other tier-1 owners); the seat owner does not vote, no auto-yes |
| Tier-2 / Tier-3 | `N =` full tier size; the proposer auto-yes counts for all 3 candidates; all other tier members vote |
| Timing | one `time_slot_sec` per attempt for both the nomination window and the voting window |
| Randomness | SHA-256 entropy accumulator over contract activity (block number excluded — "Variant B"); folds in seat + round for collision-free retries |

## Actions

| Action | Auth | Purpose |
|--------|------|---------|
| `addcandidate(account, handle)` | `account` | Self-register as a candidate (registration phase). |
| `rmcandidate(account)` | contract | Remove a candidate before init. |
| `startinit(time_slot_sec, ordered_owners[21])` | contract | Freeze the tier-1 roster; close registration. |
| `loadtier(tier, max_rows)` | contract | Batch-load the tier-2/3 snapshot from roa (resumable). |
| `finalizeinit()` | contract | Verify snapshots vs `sysio.system::nodecount`; open seat 0. |
| `reset()` | contract | After DONE: bump the generation and reopen registration. |
| `repcandidate(proposer, c1, c2, c3)` | `proposer` | The active proposer nominates a 3-candidate slate. |
| `vote(voter, v1, v2, v3)` | `voter` | Independent yes/no on each slate candidate. |
| `settle()` | anyone | Push a timed-out attempt forward; stir entropy. |
| `forceassign(member)` | contract | Governance backstop when tier-3 is exhausted. |
| `stir()` | anyone | Advance the entropy accumulator. |

## Tables (KV)

| Table | Type | Scope | Contents |
|-------|------|-------|----------|
| `config` | global | — | init progress, `time_slot_sec`, generation, tier sizes, load cursors |
| `state` | global | — | live cursor (seat/tier/phase/proposer), current slate + tallies, entropy accumulator |
| `candidates` | scoped | generation | `account`, `handle`, `elected` |
| `roster` / `tier2` / `tier3` | scoped | generation | frozen ordered node-owner snapshots (by-owner secondary index) |
| `ballots` | scoped | (generation, round) | one row per voter per attempt |
| `tried3` | scoped | (generation, seat) | tier-3 proposers already attempted for a seat |
| `council` | scoped | generation | the 21 filled seats (owner, tier, proposer, member) |

## Lifecycle

`addcandidate*` → `startinit` → `loadtier*` → `finalizeinit` → per seat
`repcandidate` + `vote*` (with `settle` cranks) escalating T1→T2→T3→`forceassign` as needed →
all 21 seats filled → `DONE` → optional `reset` for a new generation.

## Build / status

Compiled with the Wire CDT toolchain; `sysio.councl.wasm` / `sysio.councl.abi` are committed
alongside the source (like every other system contract), so `BUILD_SYSTEM_CONTRACTS=OFF` builds
consume the prebuilt artifacts. Rebuild the artifacts with `BUILD_SYSTEM_CONTRACTS=ON` (targeting
the Wire CDT), then copy `.wasm`/`.abi` back to this directory and regenerate client types per the
root `CLAUDE.md`. Whenever the contract changes, regenerate the affected reference data as noted in
`CLAUDE.md` (system-contract WASM changes shift action merkle roots).

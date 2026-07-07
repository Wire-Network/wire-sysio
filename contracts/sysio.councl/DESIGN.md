# sysio.councl — Design & Implementation Plan (v1)

> Status: **confirmed design.** Supersedes the discarded `multi_index` prototype and the v0
> draft. Every decision below is **[CONFIRMED]** with the maintainer unless explicitly tagged
> **[GROUNDED]** (dictated by existing on-chain code) or **[NOTE]** (a consequence to be aware of).

## 1. Purpose

Fill a **21-seat council**. There is one seat per **tier-1 node owner** (from `sysio.roa`).
Each seat is filled by electing one **candidate** from a shared candidate pool. The right to
*propose* a seat's slate of 3 candidates starts with that seat's tier-1 owner and **escalates**
through tier-2 and then tier-3 node owners if the earlier tier fails to elect anyone — so every
seat is guaranteed to fill. When all 21 seats are filled the election is complete and the
council is set.

Per seat, the flow is:

1. The seat's **tier-1 owner** proposes a slate of 3 candidates (`repcandidate`); the other 20
   tier-1 owners vote; strict-priority resolution (§6) either elects a candidate or fails.
2. On failure, one **tier-2** account is chosen pseudo-randomly (§5) and gets the same
   opportunity, voted on by all other tier-2 accounts.
3. On failure, one **tier-3** account is chosen, voted on by all other tier-3 accounts; if that
   fails, a *new* tier-3 account is chosen and it repeats.
4. If tier-3 is exhausted without a winner, a governance backstop fills the seat (§7).

## 2. Grounding in existing contracts [GROUNDED]

- **Tier source of truth: `sysio.roa`.** `nodeowners` is a public KV `scoped_table` scoped by
  `network_gen`, indexed `bytier`, row = `{owner, tier(1/2/3), ...}`. No registration order or
  timestamp is stored — only `owner.value` is a free deterministic key.
- **Read precedent: `sysio.chalg`.** `roa::roastate_t(ROA_ACCOUNT)` → `network_gen`, then
  `roa::nodeowners_t(ROA_ACCOUNT, network_gen)`; point-lookup a voter and check
  `tier == NODE_OWNER_TIER_T1`; iterate the `bytier` index to enumerate a tier. We reuse this.
- **Tier counts / caps: `sysio.system`.** Owns a `nodecount` KV global (`t1_count`, `t2_count`,
  `t3_count`); caps are `T1_MAX = 21`, `T2_MAX = 84`, `T3_MAX = 1000`. Used at init as a
  completeness cross-check for the tier snapshots (§9).
- **Block entropy is limited [GROUNDED].** An ordinary action can call
  `current_block_number()` and `current_time_point()` and `get_active_producers()`, but **cannot
  read a block id/hash** — `sysio.system`'s `blockinfo` table stores only `{version,
  block_height, block_timestamp}`, never `previous_block_id`. This is why randomness uses an
  in-contract entropy accumulator (§5), not a block hash.
- **Table engine: KV** (`kv::table`, `kv::scoped_table`, `kv::global`), not `multi_index`. Enum
  handling via the protobuf `NodeOwnerTier` and `magic_enum` per repo rules; hashing via the
  `sysio::sha256` intrinsic (deterministic, no FP).

## 3. Confirmed decisions

| # | Decision | Choice |
|---|----------|--------|
| 1 | Slot/seat ordering | `init` receives explicit `ordered_owners[21]`; asserted to be a permutation of exactly the roa tier-1 set. |
| 2 | Resolution | **Strict priority** over the 3 candidates (§6), not first-past-the-post. |
| 3 | Timing | Event-driven; single `time_slot_sec` is both the propose-deadline and the voting window, per attempt. |
| 4 | Tier-1 membership | Snapshot the 21 owners + order at init; ignore later roa churn; fail init if `t1 != 21`. |
| 5 | Seat outcome | **Every seat fills** via the T1→T2→T3→governance escalation ladder (§7). No empty seats. |
| 6 | Settlement | Lazy settle at top of every action + a permissionless `settle` action (no on-chain timers). |
| 7 | Candidate exclusivity | An elected candidate leaves the pool; losers may be re-proposed. Floor `MIN_CANDIDATES = 23`. |
| 8 | Candidate registration | Self-register (`require_auth(account)`) before init; `init` closes registration and asserts `>= 23`. |
| 9 | Randomness | In-contract **entropy accumulator**, **Variant B** (block number excluded from the stir), §5. |
| 10 | Proposer auto-yes | Tier-2/3 proposer's auto-yes counts for **all three** slate candidates. Tier-1 has no auto-yes. |
| 11 | Tier-2/3 selection set | **Full ordered tier-2 and tier-3 owner lists frozen at init**; nth-pick indexes those. |
| 12 | Admin auth / re-runs | `init*` = `require_auth(get_self())`; all tables scoped by `election_gen`. |

## 4. Constants

```cpp
namespace councl {
   inline constexpr uint8_t  SEATS          = 21;   // tier-1 owners == council seats
   inline constexpr uint8_t  T1_VOTERS      = 20;   // SEATS - 1 (seat owner never votes on own seat)
   inline constexpr uint8_t  SLATE_SIZE     = 3;    // candidates per repcandidate
   inline constexpr uint8_t  MIN_CANDIDATES = SEATS + 2; // 23: <=20 elected before the last seat, +3 available
   inline constexpr size_t   MAX_HANDLE_LEN = 32;   // twitter handle cap
   constexpr auto            ROA_ACCOUNT    = "sysio.roa"_n;
   constexpr auto            SYSTEM_ACCOUNT = "sysio"_n;         // owner of sysio.system tables (nodecount)

   // Thresholds are computed per round from the electorate size N (never hard-typed):
   //   win(N)  = floor(2N/3) + 1   ==  (2*N)/3 + 1        // YES needed to elect
   //   elim(N) = ceil(N/3)         ==  N - (2*N)/3        // NO that makes a candidate impossible
   // Duals: win(N) + (elim(N) - 1) == N. Examples:
   //   N=20 -> win 14, elim 7      (tier-1: "14 of the other 20", "7 no kills a candidate")
   //   N=84 -> win 57, elim 28     (tier-2 at cap)
   constexpr uint64_t win_threshold (uint64_t n) { return (2*n)/3 + 1; }
   constexpr uint64_t elim_threshold(uint64_t n) { return n - (2*n)/3; }
}
```

## 5. Randomness — entropy accumulator (Variant B) [CONFIRMED]

A rolling hash of contract activity supplies the pseudo-random seed for tier-2/3 selection. It
is fully deterministic (all nodes replay the same actions in the same order) and needs no block
hash.

```cpp
// in `state` (KV global)
checksum256 acc;         // entropy accumulator
uint64_t    stir_count;  // monotonic; guarantees acc advances even for identical (tag,actor)

// called at the TOP of EVERY action handler (init*, addcandidate, rmcandidate, repcandidate,
// vote, settle, stir, forceassign). Variant B: block_number is intentionally NOT folded in.
void stir(name action_tag, name actor) {
   acc = sysio::sha256(pack(acc, action_tag, actor, ++stir_count));
}

// pure, unit-testable
uint64_t seed_u64(const checksum256& s);                      // fold 256->64 bits (first 8 bytes, LE)
uint64_t select_index(const checksum256& acc,                 // deterministic index into [0, m)
                      uint64_t seat, uint64_t round_id, uint64_t m) {
   return seed_u64(sysio::sha256(pack(acc, seat, round_id))) % m;   // m = available (untried) count
}
```

- **`seat` + `round_id` are folded into the selection seed**, so two re-selections that land in
  the same block (e.g. successive tier-3 retries) still get distinct seeds. `round_id` is a
  globally monotonic attempt counter (§8) and doubles as the "attempt number."
- **Selection over the untried set:** `m = tierN_size - tried_count`; walk the frozen tier list
  skipping tried entries to the `select_index`-th survivor. [NOTE] tier-3 can be up to 1000, so
  this walk is O(tier size) KV reads — acceptable because it happens only on a tier-3 escalation,
  not on the hot path.

### Security / grinding [NOTE]
Variant B removes the cheapest manipulation (free per-block resampling by choosing which block to
call `settle` in): with `block_number` out of the stir, a caller must *inject an extra stirring
transaction* to change `acc`, which is visible, costs CPU/RAM, and can be perturbed by any other
participant's action. This is **not** manipulation-proof — true unpredictability against the
party controlling the trigger needs a future-block VRF/beacon this chain doesn't expose. For a
council of node owners the documented accumulator is the accepted trade-off. The permissionless
`stir` action lets honest participants add entropy and advance liveness.

## 6. Strict-priority resolution [CONFIRMED]

One round has a proposer, an electorate size `N`, a slate `c[0..2]`, and per-candidate `yes[i]`
and `no[i]` tallies. `T = win_threshold(N)`, `E = elim_threshold(N)`.

- **One ballot per voter**, carrying an independent yes/no for **each** of the 3 candidates
  (needed because per-candidate `no` drives elimination). A voter never re-votes as priority
  advances.
- **Tier-2/3 only:** the proposer's auto-yes seeds `yes[i] = 1` for **all three** candidates at
  round start (they advocated the whole slate). **Tier-1:** no auto-yes; the seat owner is not a
  voter.
- A candidate is **eliminated** when `no[i] >= E` (it can no longer reach `T`).
- The **active** candidate is the lowest index `i` with `no[i] < E`.

Resolution, re-evaluated after every `vote` and inside `settle`:

- **WIN** the round for `c[active]` as soon as `yes[active] >= T`. (Because priority is strict, a
  higher-index candidate at/above `T` cannot win while a lower-index one is still alive; it only
  becomes eligible the instant the lower one is eliminated, which this same check catches.)
- **FAIL** the round the moment **all three** are eliminated.
- Otherwise **pending** until `now >= vote_deadline` or every eligible voter has voted; then WIN
  if `yes[active] >= T`, else FAIL.

Electorate sizes / voter sets:

| Tier | `N` | Eligible voters | Auto-yes |
|------|-----|-----------------|----------|
| 1 | `T1_VOTERS = 20` | the other 20 tier-1 owners (`roster` minus the seat owner) | none |
| 2 | `n2` (tier-2 snapshot size) | all tier-2 owners except the proposer (`n2 - 1`) | proposer, all 3 |
| 3 | `n3` (tier-3 snapshot size) | all tier-3 owners except the proposer (`n3 - 1`) | proposer, all 3 |

[NOTE] Degenerate tiers: if a tier's snapshot size is `0`, that tier is **skipped** during
escalation (you can't run a round with no electorate). If `n == 1`, `T = win(1) = 1` and the lone
proposer's auto-yes wins instantly — acceptable and bounded.

## 7. Escalation ladder (per seat)

For seat `k` (owner `roster[k]`), attempts run until one elects a candidate:

```
T1 attempt:  proposer = roster[k]                         (exactly 1 attempt)
   └ fail ─> T2 attempt: proposer = random tier-2 acct    (exactly 1 attempt)
        └ fail ─> T3 loop: proposer = random tier-3 acct  (repeat, excluding already-tried
                          this seat, until WIN or tier-3 exhausted)
             └ tier-3 exhausted ─> BACKSTOP (governance forceassign)
```

- An **attempt fails** if the proposer does not `repcandidate` within `time_slot_sec`
  (propose-deadline), or the voting round FAILS (§6).
- **T1 and T2 get exactly one attempt each.** **T3 loops**, choosing a fresh untried tier-3
  account (via §5) each time.
- **Exclusion:** a tier-3 account tried for seat `k` is not chosen again for seat `k` (tracked in
  `tried3`, scoped per seat). Guarantees the T3 loop makes progress and terminates.
- **Backstop:** when every tier-3 account has been tried for seat `k` with no winner (or tier-3 is
  empty and T2 failed), the seat enters `BACKSTOP`; governance calls `forceassign(member)` to seat
  an un-elected candidate. This is the termination guarantee for a seat whose electorate simply
  never produces a super-majority.
- On **WIN** (or `forceassign`): mark the candidate `elected`, write the `council` row, advance to
  seat `k+1`. When `k+1 == SEATS`, the election is `DONE`.

## 8. Tables (KV), all scoped by `election_gen`

- **`config`** (`kv::global`): `initialized`, `reg_open`, `phase_init {REG, LOADING, READY}`,
  `time_slot_sec`, `network_gen`, `election_gen`, `start_ts`, `n2`, `n3`, and load cursors
  (`t2_loaded`, `t3_loaded`).
- **`state`** (`kv::global`): the live election cursor —
  `active_seat` (0..20), `tier {1,2,3}`, `phase {AWAIT_REP, VOTING, BACKSTOP, DONE}`,
  `proposer`, `round_id` (monotonic attempt counter), `round_open_ts`, `rep_ts`, `vote_deadline`,
  `elect_N`, `votes_cast`, `seats_filled`, the current slate `c[3]`, `yes[3]`, `no[3]`,
  and the entropy fields `acc`, `stir_count`.
- **`roster`** (`kv::table`, key = seat index → `{owner}`): frozen ordered tier-1 (21).
- **`tier2`** (`kv::table`, key = index → `{owner}`): frozen ordered tier-2 snapshot.
- **`tier3`** (`kv::table`, key = index → `{owner}`): frozen ordered tier-3 snapshot.
- **`candidates`** (`kv::table`, key = `account.value` → `{account, handle, elected}`).
- **`ballots`** (`kv::scoped_table`, scope = `round_id`, key = `voter.value` → `{voter, v[3]}`):
  one row per voter per attempt; existence enforces single-vote; scoping by `round_id` keeps
  retries isolated.
- **`tried3`** (`kv::scoped_table`, scope = `active_seat`, key = tier-3 index → `{}`): tier-3
  accounts already attempted for this seat.
- **`council`** (`kv::table`, key = seat index → `{seat_owner, filled_tier, proposer, member}`):
  the 21 outputs (`seat_owner = roster[k]`, `proposer`/`filled_tier` record who actually
  succeeded, `member` is the elected candidate).

## 9. Actions

### Registration (phase_init == REG)
- **`addcandidate(name account, string handle)`** — `require_auth(account)`. `reg_open`.
  `handle.size() <= MAX_HANDLE_LEN`. Insert if absent (else "already a candidate").
- **`rmcandidate(name account)`** — `require_auth(get_self())`. `reg_open`, candidate not yet
  frozen.

### Init (multi-step, to bound per-transaction work) [NOTE]
Tier-3 can hold up to 1000 rows, too many to read+write in one transaction, so init is staged:
- **`startinit(uint64 time_slot_sec, std::vector<name> ordered_owners)`** —
  `require_auth(get_self())`.
  1. `check(!config.initialized || state.phase == DONE)` (re-run bumps `election_gen`, wipes gen scope).
  2. `check(time_slot_sec > 0)` and `check(count(candidates) >= MIN_CANDIDATES)`.
  3. Read `roa::roastate` → `network_gen`. Enumerate roa tier-1 via `bytier`; `check(size == 21)`
     and `check(ordered_owners` is a permutation of that set`)`. Freeze `roster[0..20]`.
  4. `phase_init = LOADING`; reset load cursors; `reg_open = false`.
- **`loadtier(uint8 tier, uint32 max_rows)`** — `require_auth(get_self())`, `LOADING` only.
  Appends up to `max_rows` of roa's tier-2 or tier-3 owners (resuming from the cursor) into the
  `tier2`/`tier3` snapshot in `bytier` order. Idempotent; call repeatedly until the cursor
  reaches the tier's end.
- **`finalizeinit()`** — `require_auth(get_self())`, `LOADING` only.
  1. `check` the tier-2/tier-3 snapshots are complete vs `sysio.system::nodecount`
     (`n2 == t2_count`, `n3 == t3_count`) — the completeness cross-check.
  2. Initialize `state`: `active_seat = 0`, `tier = 1`, `phase = AWAIT_REP`,
     `proposer = roster[0]`, `round_id = 0`, `round_open_ts = now`, `acc = sha256(election_gen)`,
     `seats_filled = 0`. `phase_init = READY`, `initialized = true`.

### Election
- **`repcandidate(name proposer, name c1, name c2, name c3)`** — `require_auth(proposer)`.
  `stir("repcandidate", proposer)`, then `settle_if_elapsed()`.
  `check(phase == AWAIT_REP && proposer == state.proposer)`;
  `check(now <= round_open_ts + time_slot_sec)` (propose-deadline);
  validate slate (3 distinct, each exists, each `!elected`).
  Open the round: set `c[]`, `no[] = {0,0,0}`, `yes[] = tier==1 ? {0,0,0} : {1,1,1}` (auto-yes),
  `elect_N`, `votes_cast = 0`, `phase = VOTING`, `rep_ts = now`,
  `vote_deadline = now + time_slot_sec`. Immediately `try_resolve()` (covers `n==1`).
- **`vote(name voter, bool v1, bool v2, bool v3)`** — `require_auth(voter)`.
  `stir("vote", voter)`, then `settle_if_elapsed()`. `check(phase == VOTING)`;
  `check(voter` is eligible for the current tier and `!= proposer)`;
  `check(no ballot exists for voter in round_id)`. Record ballot; for each true `v`, `++yes[i]`;
  for each false, `++no[i]`; `++votes_cast`. `try_resolve()`.
- **`settle()`** — no auth (permissionless). `stir("settle", get_self())` then
  `settle_if_elapsed()`. Lets anyone push a timed-out attempt forward and inject entropy.
- **`forceassign(name member)`** — `require_auth(get_self())`, `phase == BACKSTOP` only. `member`
  must be an un-elected candidate. Fills the seat and advances.

### Internal helpers (callable from multiple actions per the "many states" requirement)
- **`settle_if_elapsed()`**: if `phase == AWAIT_REP && now > round_open_ts + time_slot_sec` →
  `fail_attempt()`. If `phase == VOTING` → `try_resolve()` (which no-ops unless a resolve
  condition is met).
- **`try_resolve()`**: apply §6. WIN → `win_attempt(c[active])`. FAIL (all eliminated, or deadline
  reached / all voted without `yes[active] >= T`) → `fail_attempt()`. Else return (pending).
- **`win_attempt(candidate)`**: mark `elected`; write `council[active_seat]`; `++seats_filled`;
  `advance_seat()`.
- **`fail_attempt()`**: escalate per §7 —
  T1→T2 (select), T2→T3 (select), T3→next untried T3 (select) or `BACKSTOP` if exhausted; skip
  empty tiers. Each new attempt: `++round_id`, `phase = AWAIT_REP`, `round_open_ts = now`,
  set `proposer`, `elect_N`, clear slate/tallies.
- **`select_proposer(tier)`**: uses §5 over the untried members of `tier2`/`tier3`; records the
  pick in `tried3` for tier-3.
- **`advance_seat()`**: `++active_seat`; if `== SEATS` → `phase = DONE`; else start a fresh T1
  attempt for `roster[active_seat]` (`tier = 1`, `++round_id`, `AWAIT_REP`, timers reset).

## 10. State machine

```
REG ──startinit──> LOADING ──loadtier*──> finalizeinit ──> READY
                                                             │
                    ┌──────────────── seat k ────────────────┘
                    ▼
   ┌────────> [AWAIT_REP] ──repcandidate──> [VOTING] ──try_resolve──┐
   │  (propose-deadline miss: settle → fail_attempt)                │
   │                                                                │
   │  fail_attempt escalates tier:            WIN ──win_attempt──> advance_seat
   │   T1→T2→T3(loop, exclude tried)→BACKSTOP        │                   │
   └──────────────────── (next attempt) ────────────┘         active_seat==21?
                    BACKSTOP ──forceassign──> win_attempt          │  yes → DONE
                                                                   └─ no → seat k+1 (AWAIT_REP)
```

All timing is relative: an attempt's clock starts at `round_open_ts` (the moment the prior
attempt/seat resolved). Nothing is on an absolute `init + k*slot` schedule, so skips and early
resolutions compound forward naturally.

## 11. Invariants, determinism, termination

- **Termination:** each seat runs T1 (1) + T2 (1) + T3 (≤ `n3`, strictly shrinking untried set) +
  an unconditional governance `BACKSTOP`. So a seat always resolves in bounded steps, and the
  election always completes exactly `SEATS` seats. No unbounded loop.
- **Exactly one candidate per seat**, and a candidate occupies at most one seat (`elected` gate at
  `repcandidate` + set on win). `MIN_CANDIDATES = 23` guarantees ≥ 3 un-elected candidates remain
  for the last seat (≤ 20 elected before it).
- **One ballot per voter per attempt** (row existence in `ballots` scoped by `round_id`); the
  proposer is never a voter; retries are isolated by `round_id` scope.
- **Determinism / consensus safety:** integer arithmetic only; time via
  `current_time_point().sec_since_epoch()`; hashing via `sysio::sha256`; no floats, no UB, no
  block-hash dependence. `acc` evolves identically on every replaying node (Variant B excludes
  the only non-replay-safe temptation, and it never reads a block id).
- **Cross-contract reads** are read-only point/index lookups against roa's and sysio.system's
  public KV tables. The snapshots at init make the running election immune to mid-election roa
  churn.
- **Enum discipline:** tier comparisons use `opp::types::NodeOwnerTier` + `magic_enum` /
  `NodeOwnerTier_Name`, never raw `int`, per repo rules.

## 12. Test plan

Split by binary per CLAUDE.md; the seed math is deliberately isolated for cheap maintenance.

**Pure unit tests (fast, in `test_fc` or a contract unit target) — the maintainable seed layer:**
- `win_threshold`/`elim_threshold`: table of `N ∈ {1,2,3,20,21,84,1000}` → expected `(T,E)`;
  assert the dual `T + (E-1) == N` for all `N` in a range (property test).
- `seed_u64` / `select_index`: **property tests** (output always in `[0,m)`; deterministic for
  identical inputs; changes when `seat`/`round_id`/`acc` change — avalanche) **plus a small,
  regeneratable golden-vector table** (≤ ~10 rows). Because the seed formula "will be tweaked,"
  keep exact-value assertions ONLY in this golden table (one command to regenerate); everywhere
  else assert *properties*, so a formula change doesn't cascade edits.
- Strict-priority resolver as a pure function over `(N, yes[], no[], votes_cast, deadline_hit)`:
  candidate-1-wins-outright; candidate-2-only-after-1-eliminated; candidate-3 path;
  all-eliminated fail; deadline/all-voted fail; `n==1` auto-win; auto-yes seeding for tier-2/3.

**On-chain integration tests (contract test harness):**
- Registration bounds (`MAX_HANDLE_LEN`, dup, auth, `< 23` fails init).
- Staged init: `startinit` permutation/`t1==21` checks; `loadtier` batching + resume; `finalizeinit`
  completeness cross-check vs `nodecount`.
- Tier-1 happy path (14 yes → seat filled; strict priority: 1 wins despite 2 having more yes).
- Elimination: candidate 1 gets 7 no → candidate 2 becomes active and wins.
- Early termination: all 20 voted; deadline path via `settle`.
- Propose-deadline miss → escalate to T2.
- T2 flow with auto-yes and `n2`-based threshold; T2 fail → T3.
- T3 loop with `tried3` exclusion; distinct proposers across retries (seed uses `seat`+`round_id`).
- T3 exhaustion → `BACKSTOP` → `forceassign` fills seat.
- Empty-tier skip (`n2==0` and/or `n3==0`).
- Full 21-seat run to `DONE`; re-run bumps `election_gen` and isolates state.
- Determinism spot-check: same action sequence in a replay yields identical selections.

## 13. Implementation plan

1. Scaffold `contracts/sysio.councl/`: `include/sysio.councl/sysio.councl.hpp`,
   `src/sysio.councl.cpp`, `CMakeLists.txt`; add the subdir to `contracts/CMakeLists.txt`
   (currently absent) and register in `contracts_project`.
2. Constants + `win/elim_threshold` + the pure seed/resolver helpers (header-only, so the unit
   tests link them without the WASM).
3. KV table structs (§8) and the roa/sysio.system cross-contract read helpers (mirror
   `sysio.chalg`).
4. Actions in dependency order: `addcandidate`/`rmcandidate` → `startinit`/`loadtier`/
   `finalizeinit` → `repcandidate` → `vote` → `settle` → `forceassign`, with the internal
   `settle_if_elapsed`/`try_resolve`/`win_attempt`/`fail_attempt`/`select_proposer`/`advance_seat`.
5. Ricardian contracts for every action under `ricardian/`.
6. Tests per §12; wire into `contracts/tests/`.
7. Rewrite `README.md` (the current one is a v0 placeholder describing the old flow) to match this
   design.
8. Build via `contracts_project`, copy `.wasm`/`.abi` back to the source tree per CLAUDE.md, and
   regenerate client types.

## 14. Notes deferred / minor

- **Candidate ↔ owner overlap:** a tier-1/2/3 node owner may also be a registered candidate; no
  special rule. (An account has exactly one roa tier, so a proposer is never in the electorate of
  its own round anyway.)
- **`forceassign` scope:** governance-only escape hatch; expected to be rarely if ever used. Its
  existence is the formal termination guarantee, not the normal path.
- **`time_slot_sec` reuse:** one parameter serves both the propose-deadline and the voting window
  for every attempt at every tier. Split into two params only if operational experience wants it.

# sysio.councl — Design and implementation

> Status: **implemented design.** Supersedes the discarded `multi_index` prototype and the v0
> draft. **[GROUNDED]** marks behavior dictated by existing on-chain code and **[NOTE]** calls out
> an operational consequence.

## 1. Purpose

Fill a **21-seat council**. There is one seat per **tier-1 node owner** (from `sysio.roa`).
Each seat is filled by electing one **candidate** from a shared candidate pool. The right to
*propose* a seat's slate of 3 candidates starts with that seat's tier-1 owner and **escalates**
through tier-2 and then tier-3 node owners if the earlier tier fails to elect anyone. Every seat
has a bounded path to a governance backstop; governance must call `forceassign` there to guarantee
eventual completion. When all 21 seats are filled the election is complete and the council is set.

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
- **Tier counts / caps: `sysio.roa`.** Generation-scoped `nodeowners` rows are authoritative for
  both membership and counts. ROA enforces the shared caps (`T1_MAX = 21`, `T2_MAX = 84`,
  `T3_MAX = 1000`) before inserting a row. `sysio.system::nodecount` remains an optional emissions
  mirror and is deliberately not an election authority because pre-`setemitcfg` registrations are
  absent from it.
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
| 3 | Timing | Event-driven; one inclusive `time_slot_sec` window (maximum 30 days) is used for both nomination and voting. Settlement starts only after the deadline. |
| 4 | Tier-1 membership | Snapshot the 21 owners + order at init; ignore later roa churn; fail init if `t1 != 21`. |
| 5 | Seat outcome | **Every seat fills** via the T1→T2→T3→governance escalation ladder (§7). No empty seats. |
| 6 | Settlement | Lazy settlement in election interactions plus authenticated `settle(caller)` and `stir(caller)` cranks (no on-chain timers). A stale unbound nomination/vote commits settlement only; optional `expected_round` makes clients fail loudly instead. |
| 7 | Candidate exclusivity | An elected candidate leaves the pool; losers may be re-proposed. Floor `MIN_CANDIDATES = 23`. |
| 8 | Candidate registration | Self-register and pay the row RAM before init; registration is capped at 1,000 and init asserts `>= 23`. |
| 9 | Randomness | In-contract **entropy accumulator**, **Variant B** (block number and timestamp excluded), §5. |
| 10 | Proposer auto-yes | Tier-2/3 proposer's auto-yes counts for **all three** slate candidates. Tier-1 has no auto-yes. |
| 11 | Tier-2/3 selection set | **Full ordered tier-2 and tier-3 owner lists frozen at init**; nth-pick indexes those. |
| 12 | Admin auth / re-runs | Initialization and cleanup require contract auth. `reset` aborts `LOADING` while preserving candidate registration, aborts an active `READY` election while deleting partial results, or retires `DONE` while retaining history. READY cleanup advances `election_gen`. |
| 13 | Recovery | Once an active attempt is past its deadline, governance may call `forceback` to enter `BACKSTOP` immediately instead of waiting through every remaining tier-3 attempt. |

## 4. Constants

```cpp
namespace councl {
   inline constexpr uint8_t  SEATS          = 21;   // tier-1 owners == council seats
   inline constexpr uint8_t  T1_VOTERS      = 20;   // SEATS - 1 (seat owner never votes on own seat)
   inline constexpr uint8_t  SLATE_SIZE     = 3;    // candidates per repcandidate
   inline constexpr uint8_t  MIN_CANDIDATES = SEATS + 2; // 23: <=20 elected before the last seat, +3 available
   inline constexpr size_t   MAX_HANDLE_LEN = 32;   // candidate-handle byte cap
   inline constexpr uint32_t MAX_CANDIDATES = 1000; // registration/RAM bound
   inline constexpr uint32_t MAX_TIME_SLOT_SEC = 30 * 24 * 60 * 60;
   constexpr auto            ROA_ACCOUNT    = "sysio.roa"_n;
   constexpr auto            SYSTEM_ACCOUNT = "sysio"_n;         // system RAM payer

   // Thresholds are computed per round from the electorate size N (never hard-typed):
   //   win(N)  = floor(2N/3) + 1   ==  (2*N)/3 + 1        // YES needed to elect
   //   elim(N) = ceil(N/3)         ==  N - (2*N)/3        // NO that makes a candidate impossible
   // Duals: win(N) + (elim(N) - 1) == N. Examples:
   //   N=20 -> win 14, elim 7      (tier-1: "14 of the other 20", "7 no kills a candidate")
   //   N=84 -> win 57, elim 28     (tier-2 at cap)
   constexpr uint64_t win_threshold (uint64_t n) { return (2*n)/3 + 1; }
   constexpr uint64_t elim_threshold(uint64_t n) { return n - (2*n)/3; }
}

static_assert(councl::T1_VOTERS == councl::SEATS - 1);
static_assert(win_threshold(councl::T1_VOTERS) + 1 == win_threshold(councl::SEATS));
static_assert(elim_threshold(councl::T1_VOTERS) == elim_threshold(councl::SEATS));
```

## 5. Randomness — entropy accumulator (Variant B)

A rolling hash of authenticated election activity supplies the pseudo-random seed for tier-2/3
selection. Candidate registration and initialization do not stir because election state does not
exist yet. Nomination, voting, settlement, recovery, assignment, and explicit stirring do.

```cpp
// In `state` (KV global).
checksum256 acc;
uint64_t    stir_count;

void stir(name action_tag, name actor) {
   acc = sha256(pack(acc, action_tag, actor, ++stir_count));
}

uint64_t select_index(const checksum256& acc,
                      uint64_t seat, uint64_t round_id, uint64_t available) {
   const auto seed = sha256(pack(acc, seat, round_id));
   return seed_u64(seed) % available;
}
```

`seed_u64` reads the first eight checksum bytes in **big-endian** order. Golden vectors make this
wire-level choice explicit. Seat and monotonic `round_id` distinguish retries. Tier-3 uses a
persistent lazy Fisher-Yates swap remap: each selection performs constant KV work, removes one
virtual slot, and cannot repeat an owner within the seat. A new seat starts with the full tier-3
set, so an owner may be selected again for a later seat.

`settle(caller)` and `stir(caller)` both require `caller` authorization. Neither block number nor
`current_time_point()` is hashed. Excluding the timestamp is deliberate: the transaction's timing
should determine whether settlement is allowed, not provide a caller-chosen extra resampling
input. This remains pseudo-random, deterministic, and grindable—not a cryptographic beacon. A
future-block VRF would be required to remove trigger-party manipulation. Authenticated cranks make
each resampling attributable while allowing any account to advance liveness, but cheap identities
mean authentication does not make multi-account grinding economically expensive. More sharply, a
single actor controlling several eligible identities can simulate the deterministic post-action
accumulator and selected proposer for every identity *offline*, then submit only the favorable
identity in that same transition-triggering action. This same-call choice is an accepted limitation
of Variant B, not merely a choice of transaction timing.

## 6. Strict-priority resolution [CONFIRMED]

One round has a proposer, an electorate size `N`, a slate `c[0..2]`, and per-candidate `yes[i]`
and `no[i]` tallies. `T = win_threshold(N)`, `E = elim_threshold(N)`.

- **One vote per voter**, carrying an independent yes/no for **each** of the 3 candidates.
  A bounded bitmap indexed by the frozen tier snapshot prevents duplicates; individual ballots
  are not retained because the aggregate tallies are sufficient for resolution.
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
- Otherwise **pending** until `now > vote_deadline` or every eligible voter has voted; then WIN
  if `yes[active] >= T`, else FAIL.

Electorate sizes / voter sets:

| Tier | `N` | Eligible voters | Auto-yes |
|------|-----|-----------------|----------|
| 1 | `T1_VOTERS = 20` | the other 20 tier-1 owners (`roster` minus the seat owner) | none |
| 2 | `n2` (tier-2 snapshot size) | all tier-2 owners except the proposer (`n2 - 1`) | proposer, all 3 |
| 3 | `n3` (tier-3 snapshot size) | all tier-3 owners except the proposer (`n3 - 1`) | proposer, all 3 |

[NOTE] Degenerate tiers: if a tier's snapshot size is `0`, that tier is **skipped** during
escalation (you can't run a round with no electorate). If `n == 1`, `T = win(1) = 1` and the lone
proposer's auto-yes wins instantly. Node owners may also register as candidates, so the lone
proposer can nominate and immediately seat themselves; the contract intentionally does not impose
an electorate-size floor or proposer/candidate exclusion.

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
- **Exclusion:** the per-seat lazy Fisher-Yates remap removes each selected tier-3 owner from that
  seat's available set. This guarantees progress and termination without an O(n3) survivor scan.
- **Backstop:** when every tier-3 account has been tried for seat `k` with no winner (or tier-3 is
  empty and T2 failed), the seat enters `BACKSTOP`; governance calls `forceassign(member)` to seat
  an un-elected candidate. This is the termination guarantee for a seat whose electorate simply
  never produces a super-majority.
- **Recovery:** after the current nomination or voting deadline has elapsed, governance may call
  `forceback()` to enter `BACKSTOP` directly. Without intervention the normal retry bound remains
  T1 + T2 + up to 1,000 T3 attempts; with intervention, a stalled election needs only the current
  slot plus governance response time.

At the 30-day maximum slot, the protocol-controlled path to `BACKSTOP` is bounded by at most
`21 seats × 1,002 attempts × 2 windows × 30 days = 1,262,520 days`; two windows accounts for a
nomination arriving at its deadline followed by a voting timeout. This deliberately conservative
bound is operationally impractical, which is why `forceback` exists. Final completion still
depends on a governance `forceassign`, so the contract cannot promise a wall-clock completion time
when governance itself is unavailable.
- Governance can also call `reset` at any time in `READY` to abandon a misconfigured or
  operationally impractical election immediately. Batched purge deletes partial council results,
  advances the generation, and reopens registration; this is distinct from `forceback`, which
  preserves the election and fills only the current seat.
- On **WIN** (or `forceassign`): mark the candidate `elected`, write the `council` row, advance to
  seat `k+1`. When `k+1 == SEATS`, the election is `DONE`.

## 8. Tables (KV)

- **`config`** (`kv::global`): `init_phase {REG, LOADING, READY, CLEANING}`, `time_slot_sec`,
  `network_gen`, `election_gen`, tier sizes/loaded-row counts, persistent ROA scan cursors and
  completion flags, candidate count, cleanup mode, and cleanup position.
- **`state`** (`kv::global`): live seat/tier/phase/proposer, round timing, 32-bit electorate and
  tally counts, current slate, bounded duplicate-vote bitmap, remaining tier-3 count, and entropy.
- **`roster`**, **`tier2`**, **`tier3`** (`kv::scoped_table`, generation scope): frozen ordered
  owner snapshots with a by-owner index.
- **`candidates`** (`kv::scoped_table`, generation scope): account, restricted handle, elected
  flag. The candidate pays its row RAM.
- **`tier3remap`** (`kv::scoped_table`, generation/seat scope): only non-identity Fisher-Yates
  virtual-to-actual mappings required by constant-time selection.
- **`council`** (`kv::scoped_table`, generation scope): the 21 historical results. These rows are
  intentionally retained after reset.

Rows mirror their primary key in the serialized value (`idx`, `account`, or `seat`) consistently.
This makes ABI-decoded rows self-identifying and secondary-index/debug tooling clearer despite the
small duplication.

## 9. Actions

### Registration (init_phase == REG)
- **`addcandidate(name account, string handle)`** — `require_auth(account)`. Require 1–32 bytes
  drawn from ASCII alphanumeric, `@`, `_`, `-`, and `.`; insert with `account` as RAM payer while
  the count is below `MAX_CANDIDATES`.
- **`rmcandidate(name account)`** — `require_auth(get_self())`; remove during `REG`.

### Init (multi-step, to bound per-transaction work) [NOTE]
Tier-3 can hold up to 1000 rows, too many to read+write in one transaction, so init is staged:
- **`startinit(uint64 time_slot_sec, std::vector<name> ordered_owners)`** —
  `require_auth(get_self())`.
  1. Require `init_phase == REG`; a completed prior generation must first use `reset`/`purge`.
  2. Require `0 < time_slot_sec <= MAX_TIME_SLOT_SEC` and at least `MIN_CANDIDATES`.
  3. Read `roa::roastate` → `network_gen`. Enumerate roa tier-1 via `bytier`; `check(size == 21)`
     and `check(ordered_owners` is a permutation of that set`)`. Freeze `roster[0..20]`.
  4. Set `init_phase = LOADING` and reset the loaded-row counts, persistent source cursors, and
     scan-completion flags.
- **`loadtier(uint8 tier, uint32 max_rows)`** — `require_auth(get_self())`, `LOADING` only.
  Inspects at most `max_rows` ROA owner rows in deterministic primary-owner order and appends the
  matching tier-2 or tier-3 identities into the snapshot. The persistent last-inspected cursor
  bounds reads as well as writes, including calls where most source rows belong to another tier.
  Reaching end sets that tier's scan-complete flag. A subsequent call starts a new deduplicating
  pass, allowing owners inserted earlier than the prior cursor to be absorbed after a failed
  completeness check without quadratic wrap scans.
- **`finalizeinit()`** — `require_auth(get_self())`, `LOADING` only.
  1. Require both source scans complete, then count generation-scoped ROA tier-2/tier-3 rows and
     check them against the deduplicated snapshot sizes. This works even when emissions were never
     configured and detects membership inserted during a staged scan.
  2. Initialize `state`: `active_seat = 0`, `tier = 1`, `phase = AWAIT_REP`,
     `proposer = roster[0]`, `round_id = 1`, `round_open_ts = now`,
     `acc = sha256(pack((ACC_SEED_TAG, election_gen)))`, where `ACC_SEED_TAG` is the
     `name` value `"councilseed"_n`,
     `seats_filled = 0`, `tier3_available = n3`. Set `init_phase = READY`.

### Generation cleanup
- **`reset()`** — contract auth in `LOADING` or `READY`. Enter `CLEANING`; do not advance the
  generation yet. A LOADING abort preserves candidate rows and returns to `REG` in the same
  generation after removing roster/snapshots. An active READY abort also removes partial council
  rows and advances the generation. A DONE reset advances the generation but retains council
  history.
- **`purge(max_rows)`** — contract auth. Delete at most `max_rows` rows across resumable,
  mode-specific cleanup stages. Unknown stage values abort explicitly instead of spinning.
  Completion removes the global election state before reopening `REG`.

### Election
- **`repcandidate(name proposer, name c1, name c2, name c3, optional<uint64> expected_round)`** —
  `require_auth(proposer)`.
  `stir("repcandidate", proposer)`, then `resolve_or_settle()`.
  `check(phase == AWAIT_REP && proposer == state.proposer)`;
  `check(now <= round_open_ts + time_slot_sec)` (propose-deadline);
  validate slate (3 distinct, each exists, each `!elected`).
  Open the round: set `c[]`, `no[] = {0,0,0}`, `yes[] = tier==1 ? {0,0,0} : {1,1,1}` (auto-yes),
  `elect_N`, `votes_cast = 0`, `phase = VOTING`,
  `vote_deadline = now + time_slot_sec`. Immediately `try_resolve()` (covers `n==1`).
- **`vote(name voter, bool v1, bool v2, bool v3, optional<uint64> expected_round)`** —
  `require_auth(voter)`.
  `stir("vote", voter)`, then `resolve_or_settle()`. `check(phase == VOTING)`;
  `check(voter` is eligible for the current tier and `!= proposer)`;
  `check` and set the voter's frozen-snapshot bitmap bit; for each true `v`, `++yes[i]`;
  for each false, `++no[i]`; `++votes_cast`. `try_resolve()`.
- For both nomination and voting, a supplied `expected_round` must match before settlement and
  causes a deadline-crossing transition to fail/roll back. Omitting it preserves the permissionless
  settlement-only behavior. Each action also asserts its own inclusive deadline after settlement,
  so future state-machine changes cannot accidentally accept a late payload.
- **`settle(name caller)`** — `require_auth(caller)`. Stir, then resolve or advance elapsed state.
- **`stir(name caller)`** — `require_auth(caller)`. Stir and perform the same lazy settlement.
- **`forceback()`** — contract auth. After the active nomination/vote deadline has elapsed, move
  directly to `BACKSTOP` as the governance recovery policy.
- **`forceassign(name member)`** — `require_auth(get_self())`, `phase == BACKSTOP` only. `member`
  must be an un-elected candidate. Fills the seat and advances.

### Internal helpers (callable from multiple actions per the "many states" requirement)
- **`resolve_or_settle()`**: if `phase == AWAIT_REP && now > round_open_ts + time_slot_sec` →
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
- **`select_tier3_proposer()`**: uses §5's mutating Fisher-Yates remap and decrements the available
  count, making the tier-3-only side effect explicit.
- **`advance_seat()`**: if `seats_filled == SEATS` → `phase = DONE` while `active_seat` remains 20;
  otherwise increment it and start a fresh T1 attempt (`++round_id`, `AWAIT_REP`, timers reset).

## 10. State machine

```
REG ──startinit──> LOADING ──loadtier*──> finalizeinit ──> READY
                         │
                         └──reset──> CLEANING ──purge*──> REG
                                                             │
                    ┌──────────────── seat k ────────────────┘
                    ▼
   ┌────────> [AWAIT_REP] ──repcandidate──> [VOTING] ──try_resolve──┐
   │  (propose-deadline miss: settle → fail_attempt)                │
   │                                                                │
   │  fail_attempt escalates tier:            WIN ──win_attempt──> advance_seat
   │   T1→T2→T3(loop, no repeats)→BACKSTOP           │                   │
   └──────────────────── (next attempt) ────────────┘         seats_filled==21?
       elapsed attempt ──forceback──> BACKSTOP ──forceassign──> win_attempt
                                                                  │  yes → DONE
                                                                  └─ no → seat k+1 (AWAIT_REP)

READY(active) ──reset──> CLEANING ──purge*──> REG (next generation, partial results removed)
DONE ──────────reset──> CLEANING ──purge*──> REG (next generation, history retained)
```

All timing is relative: an attempt's clock starts at `round_open_ts` (the moment the prior
attempt/seat resolved). Nothing is on an absolute `init + k*slot` schedule, so skips and early
resolutions compound forward naturally.

## 11. Invariants, determinism, termination

- **Termination:** each seat runs T1 (1) + T2 (1) + T3 (≤ `n3`, strictly shrinking untried set) +
  an unconditional transition to governance `BACKSTOP`. The automated state machine reaches that
  backstop in bounded steps with no unbounded loop; the election completes exactly `SEATS` seats
  provided governance performs any required `forceassign`.
- **Exactly one candidate per seat**, and a candidate occupies at most one seat (`elected` gate at
  `repcandidate` + set on win). `MIN_CANDIDATES = 23` guarantees ≥ 3 un-elected candidates remain
  for the last seat (≤ 20 elected before it).
- **One vote per voter per attempt** (bounded bitmap reset for every attempt); the proposer is
  never a voter.
- **Determinism / consensus safety:** integer arithmetic only; time via
  `current_time_point().sec_since_epoch()`; hashing via `sysio::sha256`; no floats, no UB, no
  block-hash dependence. `acc` evolves identically on every replaying node (Variant B excludes
  the only non-replay-safe temptation, and it never reads a block id).
- **Cross-contract reads** are read-only point/index lookups against ROA's public KV tables. The
  snapshots at init make the running election immune to mid-election ROA
  churn. `finalizeinit` also verifies that the captured ROA generation is still current; if a
  generation or count divergence prevents finalization, governance can rescan or abort LOADING
  without making candidates repay registration RAM.
- **Enum discipline:** election state uses typed contract enums. The action-boundary tier byte is
  checked with `magic_enum`; protobuf ROA tiers use their generated enum names/helpers.
- **Storage bound:** at most 1,000 candidate-paid rows; 21 roster + 84 tier-2 + 1,000 tier-3
  system-paid snapshot rows; at most 125 bytes in the vote bitmap; and at most 1,000 remap rows
  per seat (21,000 across a worst-case generation). Remap rows are sparse in typical operation.
  All ephemeral rows are purged before the next generation. Council history grows by exactly 21
  retained rows per completed generation; partial rows from an active abort are deleted.

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
- Registration bounds (handle length/characters, duplicate/auth, candidate cap, `< 23` fails init).
- Staged init: `startinit` permutation/`t1==21` checks; source-read-bounded `loadtier` batching and
  churn-safe rescan; ROA-authoritative finalization without emissions config; LOADING abort that
  preserves paid candidate registration; ROA cap enforcement when `nodecount` lags.
- Tier-1 happy path (14 yes → seat filled; strict priority: 1 wins despite 2 having more yes).
- Elimination: candidate 1 gets 7 no → candidate 2 becomes active and wins.
- Early termination: all 20 voted; partial-turnout voting deadline via `settle`; exact inclusive
  boundary and immediately-after behavior.
- Propose-deadline miss → escalate to T2.
- T2 flow with auto-yes and `n2`-based threshold; T2 fail → T3.
- T3 Fisher-Yates exclusion; distinct proposers across retries and reuse on a later seat.
- T3 exhaustion → `BACKSTOP` → `forceassign` fills seat.
- Empty-tier skip (`n2==0` and/or `n3==0`).
- Candidate 3 win after candidates 1 and 2 are eliminated; elected candidate cannot be reused.
- One-member tier auto-yes; empty tier combinations; maximum tier-3 retry/storage behavior.
- Full 21-seat run to `DONE`; bounded reset/purge; second generation isolation, retained completed
  council history, removed global state, and active-abort deletion of partial results.
- Late nomination/vote settlement-only behavior plus fail-loud `expected_round`; authenticated
  stir/settle; forceback authorization.
- Determinism spot-check: same action sequence in a replay yields identical selections.

## 13. Validation and artifacts

The contract is built only through the repository's `contracts_project` target with Wire CDT.
Both `council_math_tests` and `sysio_councl_tests` belong to `contracts_unit_test`. The generated
ABI and WASM are copied back beside the source, and their hashes are compared with a clean rebuild
before release. Ricardian contracts are sourced from `ricardian/sysio.councl.contracts.md`; every
action parameter placeholder must match the ABI.

## 14. Notes deferred / minor

- **Candidate ↔ owner overlap:** a tier-1/2/3 node owner may also be a registered candidate; no
  special rule. (An account has exactly one roa tier, so a proposer is never in the electorate of
  its own round anyway.)
- **`forceassign` scope:** governance-only escape hatch; expected to be rarely if ever used. Its
  existence is the formal termination guarantee, not the normal path.
- **`time_slot_sec` reuse:** one parameter serves both the propose-deadline and the voting window
  for every attempt at every tier. Split into two params only if operational experience wants it.

# Council elections

WIRE-418 replaces sequential single-seat attempts with simultaneous ordered flights and shared voting across the frozen T1, T2, and T3 electorates. This is the direct pre-launch contract interface; there is no compatibility adapter for the former proposer/escalation ladder.

## Registration and initialization

There are 21 seats, in the stable order supplied by governance to `startinit`. The roster must be a permutation of the 21 authoritative ROA T1 owners. Candidates self-register an account and a validated 1–32 byte handle, paying their registration RAM. Governance may remove candidates only before initialization. The pool contains 23–1,000 candidates and stays closed throughout the election.

`startinit(time_slot_sec, ordered_owners)`, `loadtier(tier, max_rows)`, and `finalizeinit()` require contract authority. Initialization retains the existing bounded staged ROA snapshot model, source cursors, network-generation checks, and owner caps. Both tier scans must finish before finalization. The frozen T1/T2/T3 snapshots persist across every continuation round; subsequent ROA registrations do not affect voting. The time slot is positive and at most 30 days. Source scans and purge calls accept 1–1,000 rows.

## Lifecycle and public cranks

```text
NOMINATING -> GENERATING -> VOTING -> TABULATING -> CONTINUING
     ^                                                 |
     +-------------------------------------------------+
                                     all 21 filled -> DONE
```

Any authenticated caller can invoke `settle(caller, election_gen, round_id, max_steps)`. A call checks the required generation and round, then advances only the current phase. It never uses a submitted nomination or ballot as an implicit settlement request.

- NOMINATING lasts one configured time slot. At the exact deadline, submissions remain valid; only a later crank freezes the round seed and enters GENERATING.
- GENERATING processes up to `max_steps` original seats (1–21) from its persisted cursor. After every missing flight is processed, it computes the ordered flight commitment and opens one common voting window.
- VOTING remains open through its exact deadline, even with full turnout or enough approvals. A later crank enters TABULATING.
- TABULATING processes up to `max_steps` seats in original ascending order. Neither caller nor batch size chooses the next seat.
- CONTINUING opens a fresh nomination round on the next crank, preserving winners and snapshots. DONE is reached only when all 21 seats are filled; repeated completed cranks are harmless.

`stir(caller)` contributes to the existing public entropy accumulator. It does not advance election phases or alter a frozen generation seed. There are no on-chain timers: callers must crank eligible transitions.

## Flights and reservations

`repcandidate(proposer, c1, c2, c3, election_gen, round_id)` requires the frozen T1 owner's authorization and a vacant seat. All vacant owners can nominate concurrently. A, B, and C are ordered preferences and must be distinct, registered, unelected candidates.

Each (candidate, position) can belong to one seat per round. A candidate may occur in different positions in other flights. First accepted nominations reserve positions. Owners may atomically replace or reorder their flights while nominations remain open: validation permits their own retained claims, rejects conflicts with other seats, and releases old claims only in a successful transaction. Failed replacements preserve the previous flight and reservations.

Claims live on candidate rows as a round identity and three seat values. A newer round logically releases old claims without scanning the candidate pool. Flights are immutable after generation completes.

Missing flights are generated in ascending seat order with the same constraints. One pool scan reads at most 1,000 candidates into memory per generation transaction. Each position selects among registered, unelected, unclaimed candidates excluding those already chosen within that flight. Draws use the frozen round seed plus election generation, round, seat, and position; later callers, entropy contributions, and batch sizes cannot reshuffle them. The inherited accumulator/modulo selection remains influenceable and is not cryptographically unbiased.

An incomplete defensive draw leaves the seat vacant. Under normal invariants the minimum pool guarantees complete flights: with R remaining seats there are at least R+2 unelected candidates, and at most R-1 other flights reserve any position. There are at least three A options, two distinct B options after choosing A, and one distinct C option after A/B.

## Signed ballots and public tallies

`vote(voter, election_gen, round_id, flight_hash, votes)` requires the voter's authorization. Every identity field is mandatory. The flight hash commits to generation, round, and each original seat's finalized candidate vector (empty for unavailable seats), folded in ascending seat order. Clients read it from state after VOTING opens. A signature cannot silently refer to another generation, continuation round, or flight set.

A ballot lists `{seat, v1, v2, v3}` for every eligible available flight in ascending order. All three choices are independent; zero through three YES approvals per flight are valid. Missing, duplicate, unordered, or extra flight entries reject the entire transaction. A T1 owner cannot vote on their own seat's flight, including automatic flights. T2/T3 owners vote on every flight with no proposer exclusion or automatic YES credit. An owner whose T1 seat is filled votes on all remaining flights.

One immutable ballot per owner per round is stored publicly. Membership is resolved from the frozen snapshots. Each flight stores separate T1/T2/T3 vote counts and three YES counts; NO for an occurrence is the tier's vote count minus that occurrence's YES count. Approvals are never pooled across occurrences of the same candidate.

Thresholds are `floor(2*B/3)+1`, where B is the total accepted ballots submitted by that tier in the current round. State exposes `t1_ballots`, `t2_ballots`, and `t3_ballots`; the same tier denominator applies to every flight. Frozen membership determines who may submit, not the threshold. Per-flight vote counts remain audit data and are not denominators. Invalid or duplicate ballots change neither counters nor tallies.

All 21 T1 owners submitting means B=21 and 15 YES are required, although each flight receives at most 20 T1 decisions. Ten T1 submissions require 7 YES; 30 T2 submissions require 21; 100 T3 submissions require 67. A tier with zero submissions cannot elect. There is no additional participation quorum: one submitted ballot with one YES can qualify a candidate. If only a T1 owner's own seat remains vacant, their complete ballot is empty and still counts once toward B. These turnout consequences are intentional; operators and clients must not substitute a per-flight denominator or exclude empty valid ballots.

## Final tabulation

After the common window closes:

1. Process each vacant original seat in ascending order.
2. For that seat, consider T1, then T2, then T3.
3. Within a tier, scan A, then B, then C.
4. Skip already elected candidates and select the first candidate meeting that tier's submitted-ballot YES threshold.
5. Record the seat immediately, mark the member elected for the entire election, and stop processing that seat.

There is no explicit-NO elimination gate. With 14 submitted tier ballots, A at 9 YES/5 NO and B at 14 YES/0 NO elects B at closure (threshold 10). Skipping an elected candidate does not transfer votes or change denominators.

Seat priority is evaluated before tier priority globally: an earlier seat's T3 winner can remove a candidate from a later seat with T1 support. This greedy algorithm can leave vacancies that a different assignment would avoid; maximizing filled seats is not its objective.

## Continuation and reset

Normal continuation retains all results, elected flags, registration, and snapshots. Every vacant seat receives fresh nominations in each subsequent round until all 21 seats are filled. Losing candidates can be nominated again. Ballots, tallies, and submitted-ballot counters start fresh; stale round requests fail. Original seat IDs remain unchanged.

There is no manual assignment or reservation of a vacancy for governance, and no round limit that switches to a fallback. Repeated rounds do not guarantee completion if candidates never receive enough approvals. Recorded partial results do not add partial-council powers or a new term-activation protocol.

`reset()` is an authorized abort/retirement operation, never continuation. During LOADING it preserves candidate registration and the generation while cleaning staged snapshots. During an unfinished election it deletes partial council results; after DONE it retains completed council history. `purge(max_rows)` performs bounded staged cleanup of candidates, snapshots, flights, ballots, and (for an abort) results, then advances the generation and reopens registration. Generation and round counters reject overflow.

## Storage and integration

| Table | Scope and retention |
|---|---|
| config, state | Singleton configuration and bounded phase/cursor/entropy state |
| candidates | Generation; elected status and lazy per-round position claims |
| roster, tier2, tier3 | Generation; original frozen owner identities |
| flights | Generation/seat key; latest flight and its per-tier running totals |
| ballots | Generation/voter key; latest public ballot and its round/flight commitment |
| council | Generation/seat key; member, original owner, filling tier, proposer, and winning round |

Ephemeral storage does not grow with the number of continuation rounds. Latest flights/ballots remain visibly labeled by round until overwritten; clients must filter by the active round. On-chain action history carries earlier submissions; this contract does not retain an unbounded ballot archive. Completed council history remains after retirement.

The actual CDT build produces the tracked council ABI/WASM. `contracts/tools/generate-sysio-contract-types.py` produces the matching `wire-libraries-ts` SDK interfaces and registry. SDK tests verify typed action preparation and ABI serialization. Hub's legacy council UI requires a separate integration follow-up. Rich profiles, new candidate eligibility policy, stronger randomness, and migrations are outside WIRE-418. See [OPERATIONS.md](OPERATIONS.md) for window announcements, crank ownership, and client state handling.

## Validation

The focused `council_math_tests` and `sysio_councl_tests` suites cover final qualification, reservations and rollback, minimum/maximum candidate pools, shared ballots, snapshot/owner churn, caps, deadlines, batching equivalence, ordered duplicate elimination, repeated continuation, and cleanup. Run contract behavior against the matching generated artifacts with explicit `--sys-vm-jit`. Broader runtime/OS regression coverage belongs to remote CI.

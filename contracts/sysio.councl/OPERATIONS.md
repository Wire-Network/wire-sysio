# Running a council election

The election operator coordinates announcements and monitors progress. Contract authority controls initialization and explicit abort/retirement. Any authenticated account can advance the public election with `settle`; the operator should assign a primary crank caller and a backup. Contract execution has no autonomous timers.

## Registration and initialization

1. Announce the intended registration closing time, the 21-seat roster, the nomination/voting time slot, and where participants can read live state. Candidate accounts register with `addcandidate`. The contract accepts 23–1,000 candidates and stores their account and short handle.
2. At the announced time, the contract-authorized operator calls `startinit(time_slot_sec, ordered_owners)`. This transaction closes registration. An announcement alone does not close it: if initialization is delayed, registration remains open. Communicate the actual transaction and revised schedule rather than implying a timestamp gate exists.
3. Repeatedly call authorized `loadtier(2, max_rows)` and `loadtier(3, max_rows)` with 1–1,000 rows per call until both source scans finish. Monitor the config snapshot counts/cursors. Resolve a reported ROA generation or owner-count mismatch before proceeding; do not assume incomplete snapshots will finalize.
4. Call authorized `finalizeinit()`. Its execution time opens round 1 nominations. Read `config.election_gen`, `state.round_id`, and the nomination deadline (`state.round_open_ts` plus `config.time_slot_sec`) and announce them. Retain the frozen owners for the entire election; later ownership changes do not add or remove voters.

## Every round

Read state before preparing any action. Actions carry the current `election_gen` and `round_id`; never resend an old signed action with a different identity. The `settle` caller signs as that account, not as the contract owner.

| State | Operator action and participant behavior |
|---|---|
| NOMINATING | Vacant T1 seat owners nominate or replace their three candidates. Submissions at `round_open_ts + time_slot_sec` are accepted. After that deadline, call `settle` to freeze the automatic-generation seed. |
| GENERATING | Repeatedly call `settle` with `max_steps` between 1 and 21. The persisted cursor controls original seat order; reduce batch size when resource headroom is limited. Voting is not yet open. |
| VOTING | Announce `vote_deadline` and `flight_hash` from state after all flights are finalized. Owners submit one complete, immutable public ballot. At the exact deadline submissions remain valid. After it, call `settle` to enter tabulation. |
| TABULATING | Repeatedly call `settle` until all seats have been considered. Show recorded results as they appear, but do not describe a partial council as a completed election. |
| CONTINUING | Call `settle` to open the next nomination window for every remaining vacancy. Announce its new round and actual deadline. Preserve winners, registration, and frozen membership. |
| DONE | Verify all 21 results and unique elected members. Repeated `settle` calls are harmless. Archive generation-tagged public results before any later retirement. |

A delayed crank delays subsequent windows. In particular, `vote_deadline` is based on when automatic generation finishes, not on the planned registration or nomination schedule. Re-read on-chain deadlines after every transition; a website countdown or operator announcement must follow that state. An authenticated backup caller can continue from the persisted cursor without coordinating a new batch offset. Refresh state after a stale-round rejection and rebuild only an action appropriate to that state.

If a round fills 16 seats, the next round has five vacant seats and fresh ballots/counters. Continue until all 21 seats fill, even after a round with no winners. There is no round limit, governance reservation, or manual seat assignment. Insufficient approvals can keep an election incomplete indefinitely.

## Ballot and turnout display

Fetch flights only after VOTING opens and keep their order by original seat ID. Filter flights and ballot rows by the active generation and round: storage retains the latest row per seat/voter, so older rows may still be visible. Sign the state-provided `flight_hash` together with the generation and round.

- T1 owners omit their own seat's flight, whether manual or automatic. With 21 vacant seats, this is 20 flights and 60 YES/NO choices. An owner whose seat is filled votes on all remaining flights.
- T2/T3 owners cover every available flight: up to 21 flights and 63 independent decisions. There is no implicit YES vote.
- Use `t1_ballots`, `t2_ballots`, or `t3_ballots` as B for `floor(2*B/3)+1`. The tier's denominator is shared by every flight; each flight's `votes_cast` only describes that flight's received decisions. Thresholds can rise while voting remains open; live totals are provisional.
- All 21 T1 submissions require 15 YES, despite each flight having only 20 possible T1 decisions. Ten submissions require 7. A tier with no submissions cannot elect; one submission can elect with one YES. No extra participation quorum applies.
- If only a T1 owner's own seat is vacant, their valid complete ballot has an empty `votes` list. It still increments the tier counter and may raise the threshold for that flight.

Read the submitted ballot row back after a transaction succeeds. A rejected, incomplete, duplicate, unauthorized, stale, or late ballot changes neither the stored ballot nor counters/tallies. Public YES totals do not combine repeated occurrences of a candidate across flights. Results are determined after closure in seat order, then T1/T2/T3, then A/B/C, skipping already elected candidates.

## Abort, retirement, and cleanup

Use authorized `reset()` only for an explicit abort or retirement, never to continue ordinary voting. Aborting during LOADING preserves candidates and the current generation while clearing staged snapshots. Aborting an unfinished election deletes its partial results. Retiring DONE preserves completed council history.

After reset, repeatedly call authorized `purge(max_rows)` with 1–1,000 rows until cleanup finishes and registration reopens. Start with smaller batches when resource use is uncertain. Cleanup is staged and resumable; one successful call does not imply every table is empty. A new election generation must use new signed identities. Do not infer voting eligibility or an active result from an old generation's retained rows.

The focused contract resource test measures maximum-pool/membership execution and payer RAM changes; it is a local JIT measurement, not a promise about production latency or every runtime. Re-evaluate batch sizing on the deployment's actual chain configuration and monitor failed transactions instead of automatically increasing resource limits.

The generated SDK provides the contract actions and table types. A working Hub election UI, rich candidate profiles, stronger randomness, and npm release/consumer adoption require their separate delivery steps.

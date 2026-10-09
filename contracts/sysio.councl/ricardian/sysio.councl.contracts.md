<h1 class="contract">addcandidate</h1>

---
spec_version: "0.2.0"
title: Add Council Candidate
summary: '{{nowrap account}} registers as a council candidate.'
---

{{account}} registers as a candidate for the council election with the short handle {{handle}} and
pays the RAM for that candidate row. A generation accepts at most 1,000 candidates.

## Preconditions
- The caller must be authorized as {{account}}.
- Candidate registration must be open (before the election has started).
- {{account}} must not already be a candidate.
- The handle must use the contract's allowed 1–32-byte ASCII character set.

<h1 class="contract">rmcandidate</h1>

---
spec_version: "0.2.0"
title: Remove Council Candidate
summary: 'Remove candidate {{nowrap account}} before the election starts.'
---

The contract owner removes {{account}} from the candidate pool. Allowed only while registration is open.

<h1 class="contract">startinit</h1>

---
spec_version: "0.2.0"
title: Start Election Initialization
summary: 'Freeze the tier-1 roster and begin an election.'
---

The contract owner begins an election: registration closes, the ordered list of 21 tier-1 node
owners is frozen as the seat roster, and the roa network generation is captured.

## Preconditions
- The caller must have contract-owner authorization.
- At least 23 candidates must be registered.
- `time_slot_sec` must be between one second and 30 days, inclusive.
- `ordered_owners` must be a permutation of exactly the 21 tier-1 node owners in sysio.roa.

<h1 class="contract">loadtier</h1>

---
spec_version: "0.2.0"
title: Load Tier Snapshot
summary: 'Append tier-{{nowrap tier}} node owners into the frozen snapshot.'
---

The contract owner inspects at most {{max_rows}} node-owner rows from sysio.roa while appending
tier-{{tier}} identities into the frozen voting snapshot. A persistent source cursor bounds
reads as well as writes, and identity deduplication allows a later pass to absorb newly observed
owners. Called repeatedly until the tier scan is complete.

<h1 class="contract">finalizeinit</h1>

---
spec_version: "0.2.0"
title: Finalize Election Initialization
summary: 'Verify the tier snapshots and open simultaneous nominations.'
---

The contract owner finalizes initialization: the tier-2 and tier-3 source scans must be complete,
their snapshot sizes are verified against authoritative generation-scoped sysio.roa rows, and the
shared nomination window opens for all 21 seats.

<h1 class="contract">reset</h1>

---
spec_version: "0.2.0"
title: Begin Election Reset
summary: 'Abort initialization or an election, or clean a completed generation.'
---

The contract owner starts cleanup while initialization is loading or while an election is active
or complete. A loading abort preserves candidate registrations and reopens registration in the
same generation. An active-election abort advances the generation and removes partial council
results. A completed election advances the generation while retaining its council history. The
contract owner must call `purge` until cleanup completes.

<h1 class="contract">purge</h1>

---
spec_version: "0.2.0"
title: Purge Election State
summary: 'Delete up to {{max_rows}} ephemeral rows from the completed generation.'
---

The contract owner deletes at most {{max_rows}} rows from the mode-specific candidate, roster,
tier-snapshot, flight, ballot, and optional partial-council cleanup stages. Completion removes live election
state and reopens registration. Completed council history is retained; partial results from an
aborted active election are not.

<h1 class="contract">repcandidate</h1>

---
spec_version: "0.2.0"
title: Nominate or Replace a Council Flight
summary: '{{nowrap proposer}} submits an ordered flight for their vacant seat.'
---

{{proposer}} nominates {{c1}}, {{c2}}, and {{c3}} in priority order for their frozen T1 seat in election {{election_gen}}, round {{round_id}}. Candidates must be distinct, registered, and unelected. Each candidate-position pair can belong to only one flight. During the shared nomination window, a successful replacement releases prior claims and acquires the new claims atomically. A rejected replacement preserves the previous flight. The proposer must authorize this action. Stale identities or elapsed nominations fail; this action never opens voting or acts as a crank.

<h1 class="contract">vote</h1>

---
spec_version: "0.2.0"
title: Submit a Council Round Ballot
summary: '{{nowrap voter}} submits one immutable ballot for the finalized round.'
---

{{voter}} submits independent YES/NO decisions in {{votes}} for every eligible flight in ascending seat order. Election {{election_gen}}, round {{round_id}}, and finalized flight commitment {{flight_hash}} must match the active voting window. The voter must authorize the action and belong to a frozen electorate. T1 owners omit their own seat's flight, including automatically generated flights; T2/T3 owners vote on every available flight without implicit YES credit. Ballots cannot be revised or duplicated. Votes and running totals are public. Submissions at the exact deadline are accepted. Results are tabulated only after the shared window closes. The YES threshold is floor(2*B/3)+1, where B is all accepted ballots from that tier in this round, identical for every flight. A valid empty T1 ballot still counts toward B. Zero submissions cannot elect; there is no additional turnout quorum.

<h1 class="contract">settle</h1>

---
spec_version: "0.2.0"
title: Advance the Council Round
summary: '{{nowrap caller}} advances bounded election work.'
---

Authenticated {{caller}} advances election {{election_gen}}, round {{round_id}}, processing at most {{max_steps}} seats (1–21) in the persisted original order. After nominations expire, generation freezes one seed; voting opens only after all flights are finalized. After the shared voting deadline, tabulation processes each seat through T1, T2, T3 and A, B, C, immediately excluding elected members from later seats. A subsequent continuation starts fresh nominations, tallies, and ballot counters for every vacancy while retaining winners and snapshots. Rounds repeat until all 21 seats are filled; there is no manual assignment fallback. Stale identities fail. Exact deadlines do not trigger settlement. This action also contributes public entropy.

<h1 class="contract">stir</h1>

---
spec_version: "0.2.0"
title: Contribute Council Entropy
summary: '{{nowrap caller}} contributes to the public entropy accumulator.'
---

Authenticated {{caller}} contributes to the election's deterministic public entropy accumulator. This does not change an already frozen automatic-generation seed or advance election phases. Use settle to drive eligible transitions.

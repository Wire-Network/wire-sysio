<h1 class="contract">addcandidate</h1>

---
spec_version: "0.2.0"
title: Add Council Candidate
summary: '{{nowrap account}} registers as a council candidate.'
---

{{account}} registers as a candidate for the council election with the short handle {{handle}}.

## Preconditions
- The caller must be authorized as {{account}}.
- Candidate registration must be open (before the election has started).
- {{account}} must not already be a candidate.

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
- `ordered_owners` must be a permutation of exactly the 21 tier-1 node owners in sysio.roa.

<h1 class="contract">loadtier</h1>

---
spec_version: "0.2.0"
title: Load Tier Snapshot
summary: 'Append tier-{{nowrap tier}} node owners into the frozen snapshot.'
---

The contract owner appends up to {{max_rows}} tier-{{tier}} node owners from sysio.roa into the
frozen escalation snapshot, resuming from the load cursor. Called repeatedly until complete.

<h1 class="contract">finalizeinit</h1>

---
spec_version: "0.2.0"
title: Finalize Election Initialization
summary: 'Verify the tier snapshots and open the first seat.'
---

The contract owner finalizes initialization: the tier-2 and tier-3 snapshots are verified complete
against the live node counts, and voting opens for the first council seat.

<h1 class="contract">reset</h1>

---
spec_version: "0.2.0"
title: Reset For New Election
summary: 'Start a new election generation and reopen registration.'
---

The contract owner starts a fresh election generation and reopens candidate registration. Only
valid once the previous election is complete.

<h1 class="contract">repcandidate</h1>

---
spec_version: "0.2.0"
title: Nominate a Candidate Slate
summary: '{{nowrap proposer}} nominates a slate of three candidates.'
---

{{proposer}} nominates a slate of three distinct, un-elected candidates ({{first_c}} is c1,
{{second_c}} is c2, {{third_c}} is c3) for the current seat, opening the voting round.

## Preconditions
- The caller must be authorized as {{proposer}}.
- {{proposer}} must be the active proposer for the current seat.
- The nomination window must not have elapsed.
- The three candidates must be distinct, registered, and not already elected.

<h1 class="contract">vote</h1>

---
spec_version: "0.2.0"
title: Vote on the Current Slate
summary: '{{nowrap voter}} votes on the three current-slate candidates.'
---

{{voter}} casts an independent yes/no vote on each of the three candidates in the current slate.

## Preconditions
- The caller must be authorized as {{voter}}.
- Voting must be open for the current slate.
- {{voter}} must be an eligible voter for the active tier and must not be the proposer.
- {{voter}} must not have already voted in this round.

<h1 class="contract">settle</h1>

---
spec_version: "0.2.0"
title: Settle Election State
summary: 'Advance a timed-out attempt and stir entropy.'
---

A permissionless crank that resolves an elapsed attempt (a missed nomination window or a closed
voting window) and advances the election, while mixing entropy into the accumulator.

<h1 class="contract">forceassign</h1>

---
spec_version: "0.2.0"
title: Governance Seat Assignment
summary: 'Assign {{nowrap member}} to the current seat.'
---

The contract owner seats {{member}} for the current seat. Valid only when tier-3 has been exhausted
without electing a candidate (the governance backstop).

<h1 class="contract">stir</h1>

---
spec_version: "0.2.0"
title: Stir Entropy
summary: 'Advance the entropy accumulator.'
---

A permissionless crank that advances the entropy accumulator used for pseudo-random tier-2/tier-3
proposer selection.

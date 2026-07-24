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

The contract owner appends up to {{max_rows}} tier-{{tier}} node owners from sysio.roa into the
frozen escalation snapshot. Resume is identity-based, so newly observed owners are appended without
duplicating owners already loaded. Called repeatedly until complete.

<h1 class="contract">finalizeinit</h1>

---
spec_version: "0.2.0"
title: Finalize Election Initialization
summary: 'Verify the tier snapshots and open the first seat.'
---

The contract owner finalizes initialization: the tier-2 and tier-3 snapshots are verified complete
against the live node counts, and the first council seat's nomination window opens.

<h1 class="contract">reset</h1>

---
spec_version: "0.2.0"
title: Begin Election Reset
summary: 'Begin staged cleanup of the completed election generation.'
---

The contract owner starts cleanup of ephemeral rows from a completed election. Council result rows
remain as permanent history. The contract owner must call `purge` until cleanup completes and
candidate registration reopens.

<h1 class="contract">purge</h1>

---
spec_version: "0.2.0"
title: Purge Election State
summary: 'Delete up to {{max_rows}} ephemeral rows from the completed generation.'
---

The contract owner deletes at most {{max_rows}} candidates, roster entries, tier snapshots, and
tier-3 remap entries from the completed generation. When all ephemeral rows have been removed, the
generation advances and registration reopens. Council result rows are retained.

<h1 class="contract">repcandidate</h1>

---
spec_version: "0.2.0"
title: Nominate a Candidate Slate
summary: '{{nowrap proposer}} nominates a slate of three candidates.'
---

{{proposer}} nominates a slate of three distinct, un-elected candidates ({{c1}}, {{c2}}, and
{{c3}}) for the current seat, opening the voting round.

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
summary: '{{nowrap caller}} advances timed-out election state and stirs entropy.'
---

{{caller}} authorizes a public crank that resolves an elapsed attempt and advances the
election while mixing the authenticated caller into the accumulator.

<h1 class="contract">forceback</h1>

---
spec_version: "0.2.0"
title: Governance Recovery Backstop
summary: 'Move an elapsed active attempt to governance backstop.'
---

The contract owner moves an elapsed nomination or voting attempt directly to BACKSTOP. This is an
exceptional recovery path for an operationally stalled election and cannot be used before the
active attempt's inclusive deadline has passed.

<h1 class="contract">forceassign</h1>

---
spec_version: "0.2.0"
title: Governance Seat Assignment
summary: 'Assign {{nowrap member}} to the current seat.'
---

The contract owner seats {{member}} for the current seat. Valid only at the governance backstop,
reached either after tier-3 exhaustion or through `forceback` recovery of an elapsed attempt.

<h1 class="contract">stir</h1>

---
spec_version: "0.2.0"
title: Stir Entropy
summary: '{{nowrap caller}} advances entropy and settles elapsed election state.'
---

{{caller}} authorizes a public crank that mixes the authenticated caller into the entropy
accumulator and also advances an elapsed election attempt. Block number and block timestamp are not
entropy inputs.

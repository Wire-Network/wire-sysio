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
tier-{{tier}} identities into the frozen escalation snapshot. A persistent source cursor bounds
reads as well as writes, and identity deduplication allows a later pass to absorb newly observed
owners. Called repeatedly until the tier scan is complete.

<h1 class="contract">finalizeinit</h1>

---
spec_version: "0.2.0"
title: Finalize Election Initialization
summary: 'Verify the tier snapshots and open the first seat.'
---

The contract owner finalizes initialization: the tier-2 and tier-3 source scans must be complete,
their snapshot sizes are verified against authoritative generation-scoped sysio.roa rows, and the
first council seat's nomination window opens.

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
tier-snapshot, remap, and optional partial-council cleanup stages. Completion removes live election
state and reopens registration. Completed council history is retained; partial results from an
aborted active election are not.

<h1 class="contract">repcandidate</h1>

---
spec_version: "0.2.0"
title: Nominate a Candidate Slate
summary: '{{nowrap proposer}} nominates a slate of three candidates.'
---

{{proposer}} nominates a slate of three distinct, un-elected candidates ({{c1}}, {{c2}}, and
{{c3}}) for the current seat, opening the voting round. Optional {{expected_round}} binds the
request to a specific round; if the round is stale or elapses during lazy settlement, the action
fails and rolls back instead of acting as a settlement-only crank. Omitting it preserves the
settlement-only behavior.

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
Optional {{expected_round}} binds the ballot to a specific round and makes a stale or
deadline-crossing ballot fail instead of silently settling the election.

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

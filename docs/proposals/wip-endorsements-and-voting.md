# WIP endorsements and Council decisions

Status: proposed executable implementation for policy review; not enabled by bootstrap.

## Gap and evidence

The user-story acceptance brief requires **Tier-2 owners to endorse WIPs** and
**Council members to vote on WIPs**. Existing `sysio.councl::vote` selects election
flights; it cannot identify a WIP. Hub's WIP list currently reads GitHub issues and
labels, which are not signed governance decisions.

Evidence: [election actions at the reviewed baseline](https://github.com/Wire-Network/wire-sysio/blob/41e8afd50baeca39a3bbff3bd76a03b33b0c7cbb/contracts/sysio.councl/include/sysio.councl/sysio.councl.hpp),
[ROA owner eligibility](https://github.com/Wire-Network/wire-sysio/blob/41e8afd50baeca39a3bbff3bd76a03b33b0c7cbb/contracts/sysio.roa/sysio.roa.hpp),
and [Hub content reader](https://github.com/Wire-Network/wire-hub-webapp/blob/4894ca4c123f709088d209d4d415ffb67a32f2e3/src/app/_services/wips.service.ts).

## Proposed contract

All five actions belong to `sysio.councl`. Existing election actions, tables and
cleanup are unchanged. The new tables are `wips`, `wipcounter`, `wipendorses` and
`wipballots`; ballots and endorsements are scoped by the monotonically allocated
proposal ID. No proposal executes code or changes `sysio` authority.

| Action | Required signer and effect |
|---|---|
| `openwip(uri, content_hash, council_gen, policy)` | Council contract authority, following existing election administration. Allocates and returns a proposal ID. Freezes the revision, policy, current ROA Tier-2 owners and 21 unique elected members from the explicitly selected completed Council. |
| `endorsewip(owner, proposal_id, content_hash)` | Frozen Tier-2 owner. One immutable endorsement before the endorsement deadline. |
| `votewip(voter, proposal_id, content_hash, choice)` | Frozen Council member. One immutable Yes/No/Abstain ballot during the voting window, only if the endorsement threshold was reached. |
| `settlewip(caller, proposal_id)` | Any authenticated caller, after the relevant deadline. Records a terminal result; no payment, execution or authority mutation. |
| `cancelwip(proposal_id, reason)` | Council contract authority. Cancels an undecided proposal with a bounded, retained reason. |

The ROA Tier-2 cap is 84 and the Council has 21 seats. Opening copies these bounded
sets atomically, using ROA's canonical by-tier index and typed Council readers.
There is no extra staged snapshot workflow and no dependency on retaining an old
election's temporary electorate. Later registrations and election reset/cleanup
cannot alter an open WIP's membership or remove its votes.

Selecting `council_gen` is explicit governance approval of that electorate for this
proposal. The contract requires all 21 distinct elected members; it does **not**
infer that an election transferred system authority or install a global Council
term. Governance must select the intended adopted Council, not an arbitrary stale
term. This selection remains a policy review point.

## Policy proposed for discussion

No mainnet thresholds or time windows are silently configured. Each opening must
supply reachable positive absolute counts for `minimum_endorsements`, `quorum`
and `minimum_yes`, and positive `endorsement_seconds`/`voting_seconds` bounded to
30 days each. Bounds are operational limits, not recommended voting durations.

- One endorsement per frozen owner account; one ballot per frozen Council account.
- Endorsement window is `[opened_at, endorse_until)`. Voting starts exactly at
  `endorse_until` and ends exclusively at `vote_until`; no keeper delay shifts it.
- Insufficient endorsements expires at the first deadline. Otherwise settlement
  waits for the voting deadline. Participation counts Yes + No + Abstain; quorum
  failure expires. With quorum, the absolute Yes threshold determines Passed or
  Rejected. There is no hidden percentage, tie-break, or early approval.
- Ballots cannot be replaced. A revised proposal receives a new ID/hash and starts
  with zero decisions. Governance can cancel before an outcome becomes finalizable.
- Membership is frozen for that proposal even if elections turn over. Governance
  may cancel during the open window if a new term should reconsider it.
- Only governance can open proposals, bounding public RAM growth. Proposal and
  voter rows use the existing privileged system RAM model. Terminal evidence is
  retained; no automatic history deletion or mutable tally-reset path is added.

The group should approve these choices, particularly absolute thresholds,
abstention/quorum treatment, immutable ballots, proposal opening/cancellation
authority, selected Council generation, turnover behavior and retention. Deployment
alone enables no proposal and chooses none of this policy for mainnet.

## Content and client integration

Proposal prose stays off-chain. `content_hash` is SHA-256 of the **exact UTF-8
bytes** of the reviewed, immutable revision; do not normalize whitespace, line
endings or Unicode after hashing. `uri` is bounded printable HTTPS, intended to
point to a pinned revision. The contract neither fetches a URL nor trusts issue
labels. Clients fetch the bytes, verify the digest, and bind every signed
endorsement/ballot to both ID and hash. On unavailable or mismatched content they
must refuse signing, not fall back to a mutable issue body.

The ABI exposes typed policies, statuses, choices and public records. Regenerate
system-contract SDK bindings with the producer's generator. Hub can then add the
ordinary-owner/Council review-sign-reload journeys and display recorded deadlines,
counts, each account's decision and transaction evidence. This backend PR does not
claim those not-yet-wired Hub buttons are implemented.

## Compatibility and acceptance

Existing serialized rows and action arguments remain byte-compatible; no migration,
election-policy change, protocol execution or new authority is involved. New WIP
state must be retained when upgrading or rolling back; old code cannot process it.
Do not erase retained decisions to make a downgrade appear supported.

Contract tests exercise the normal signed journey, election cleanup independence,
wrong signers/revisions/tiers, new owners excluded from the snapshot, duplicates,
closed windows, missing Council results, unreachable/zero policies, URI/hash
validation, exact quorum/approval boundaries, cancellation, immutable terminal
outcomes and new revisions without inherited endorsements. Run the existing full
Council/election suite as well as the complete contract suite before release.

For live acceptance, use real ordinary T2 accounts and elected Council accounts
through Hub, verify irreversible receipts and reload/reconnect behavior, and let
cluster failures fail the run. Unit fixtures do not establish live election or WIP
readiness. No Sandbox/Testnet deployment is part of this PR.

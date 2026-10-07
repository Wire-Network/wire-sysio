# `sysio.synd`

`sysio.synd` is the depot's syndication contract. It owns every rule between an outpost's
`SYNDICATE_LIQ` or `LIQ_YIELD` report and the shadow LIQ a holder receives, and every rule for
sending shadow LIQ back to an outpost with `DESYNDICATE_LIQ`. It is the only account that may mint and
burn in `sysio.liq`. The contract is `contracts/sysio.synd`; the specification is §4 of the
syndication underwriting spec. Underwriting itself is `sysio.bond`'s job (see
[`sysio-bond-integration.md`](sysio-bond-integration.md)).

## The queue

Nothing an outpost reports is paid out on arrival. Every syndication waits in a queue until the
envelope that carried it has been underwritten.

### Intake

`sysio.msgch` decodes an accepted inbound envelope and, for each syndication and yield report in it,
sends `onsynd` or `onyield`, then `closeenv` after the last one. All three carry the outpost, the depot
epoch and the envelope's canonical digest.

- `onsynd` mints the amount into `sysio.synd`'s own `sysio.liq` holder row and adds a SYNDICATION item
  for the syndicating pubkey, linked or not. The item starts earning WIRE at once.
- `onyield` adds a YIELD item. It mints nothing.
- Both add to the `(outpost, token, epoch)` row in `envelopes` and to the `(outpost, token)` running
  sums in `ledger`.
- Both compare the outpost custody the message carries with the depot's outstanding shadow first
  ([The solvency check](#the-solvency-check)); a shortfall is recorded and pulls the cord, and the
  message is held all the same.
- `closeenv` moves the envelope's rows from OPEN to WAITING and runs one step of the queue, with a
  small fixed budget (`CLOSEENV_QUEUE_LIMIT`, 16 units of work).

These three run inside `sysio.msgch`'s consensus transaction and never throw. A message that breaks a
rule (a pubkey of the wrong key family, a token with no shadow or of another outpost, an amount out of
range or past the shadow's headroom, a closed envelope, a replayed sequence) is dropped with a `DROP`
line on the console and consumes no sequence. So is a message that would leave its envelope impossible
to underwrite: one that takes the envelope's syndicated plus yield total, rounded up to `sysio.bond`'s
increment, past the asset range, and a yield report that takes the envelope's yield total past the
shadow's headroom. An envelope with no syndication or yield report writes nothing.

### One step

`closeenv` runs a bounded step inline; anyone may run `crank(limit)`. Each crank runs three
independent bounded phases:

1. **Synchronize outcomes.** `sync(limit)` visits at most `limit` envelopes from a persistent cursor,
   including DONE envelopes whose funds released before finality. `syncenv(chain_code, token_code,
   epoch_index)` targets one envelope directly. Both record the terminal result and its settlement
   amounts before acknowledging bond; neither releases, burns nor claims funds. They work while frozen.
2. **Release and settle.** The FIFO release walk retains `ledger.queue_epoch`, its independent pair
   cursor, `limit` work budget and `4 * limit` examination cap. Existing release gates, yield accounting,
   burns and challenge settlements apply. The phase refreshes reached requests before moving funds.
3. **Advance underwriting.** A separate per-pair `underwriting_epoch` and global pair cursor advance
   past fully bonded or ruled requests. The oldest WAITING envelope is issued only after those before
   it meet that gate. This phase has its own `limit` work budget and `4 * limit` examination cap; an
   empty release bucket never consumes it.
   Running issuance after settlement preserves the existing rule that fees earned in this crank can
   fund the next request's bounty. Its budget and cursor remain independent of release.

Syndication uses bond's durable `requestkeep` interface. A terminal request cannot be pruned before
syndication records and acknowledges its outcome, even when all payouts were claimed and seven days
have passed. There is no polling deadline. Delayed synchronization delays progress, not recoverability.
An INVALID outcome or pending challenger share discovered after DONE rewinds settlement to that envelope.

The stable request identity is `(issuer, schema, statement)`, resolved through bond's public accessor.
An inline `syncenv` after creation records the actual numeric ID; syndication never predicts bond's
counter. Admission validation is shared with bond. The statement remains the packed
`(chain_code, epoch_index, digest, token_code)` under `oppenvelope`; coverage is the envelope's totals
rounded to the bond increment and the bounty is capped by the fee pot.

### The gates of a syndication

In this order: envelope consensus, then held, then its request's turn, then BONDED, APPROVED or VALID,
then the bucket in item id order, then the fee, then release. Nothing releases from an envelope whose
request is OPEN, HELD or INVALID.

- **Bucket.** The pair's syndication bucket is ticked lazily against the depot's epoch:
  `level = min(burst, level + refill * epochs elapsed)`, not counting an epoch during which the
  `sysio.andon` cord was pulled (see The emergency stop); a new bucket starts full. The tranche is
  `min(remaining, level)`, so no single release exceeds the burst. An empty bucket stops the pair's
  syndications until it refills; an item larger than the level releases in tranches over several
  epochs.
- **Fee.** `tranche * synd_fee_bps / 10000`, floored, into the fee pot.
- **Release.** The rest goes to the account the pubkey has AuthX-linked at release time. The WIRE the
  item earned while held is credited to that account's own `sysio.liq` row with `sysio.liq::creditowed`
  and claimed there with `sysio.liq::claim`; nothing is pushed. WIRE the yield pool cannot cover yet
  stays on the item, and once the item is released whole it moves to the pubkey's parked row, which a
  later `sweep` pays. LIQ is credited by `sysio.liq::settle` without notifications, including to contract accounts.
  Only a pubkey with no link needs parked principal.

A YIELD item is released whole into `sysio.liq`'s pending yield by `mintyield`. It pays no fee and uses
no budget; it waits only when the shadow's headroom cannot take it.

### Challenge and ruling

`challenge(challenger, chain_code, token_code, epoch_index)` holds a REQUESTED, RELEASABLE or DONE
envelope whose request is OPEN or BONDED: an envelope released whole while its request is still only
bonded can be challenged, since the bond exists for exactly that case. The challenger pays the
request's hold bond plus the pair's `challenge_extra`; a charge of zero is refused (`challenge charge is
zero; configure challenge_extra`). The extra stays in the fee pot, and `sysio.synd` holds the request
through `sysio.bond::hold` naming the challenger as beneficiary. The envelope is HELD until `sysio`
rules, and the pair's `queue_epoch` is rewound to it so the ruling is processed. One challenge per
request; contract accounts may challenge. Any row the challenge creates bills `sysio`.

A challenge of a request that is not yet fully bonded freezes the whole pair until `sysio` rules: a
held request is neither bonded nor ruled, so no later envelope of the pair is requested. That costs the
challenger only the hold bond and `challenge_extra`. Launch every pair with a non-zero
`challenge_extra` sized to make such a freeze expensive.

- **VALID:** release resumes, and the share of the hold bond `sysio.bond` awards the issuer is
  forwarded to the challenger. The step that first sees the ruling snapshots that share and the
  challenger on the envelope (`hold_share`, `hold_beneficiary`, `share_pending`) and forwards it from the
  snapshot, so the forward does not depend on the request row `sysio.bond` may prune.
- **INVALID:** the step that first sees the ruling snapshots the forfeit (the bonded amount) and the
  bounty returned for want of a hold on the envelope (`forfeit`, `bounty_returned`). Every unreleased
  syndication of the envelope is burned and its yield items dropped, within the step's budget. The
  step that finishes pulls what `sysio.bond` still owes with `sysio.bond::claim` (anyone may have
  claimed it for `sysio.synd` already, and a request pruned since was fully paid), burns the amount
  already released out of the forfeit, puts the rest and the returned bounty in the fee pot, and marks
  the envelope INVALID. Holders already paid keep what they received; supply is whole again.

### Dropping an envelope

`dropenv(chain_code, token_code, epoch_index)`, signed by `sysio`, is the escape hatch for an envelope
the queue can never request. It takes an OPEN or WAITING envelope with no request issued, burns its
held syndications out of `sysio.synd`'s row with `sysio.liq::burn`, drops its yield reports and marks
it INVALID; the next step moves `queue_epoch` past it. A late message of a dropped OPEN envelope is
refused as closed.

### The parked hold

A parked balance sits in `sysio.synd`'s own `sysio.liq` row with a shadow-custody position, so it keeps
earning WIRE. It is delivered, balance and banked WIRE together, when the pubkey is linked
(`sysio.authex::createlink` sends `linkswept` inline) or by the permissionless `sweep(account,
chain_kind)` for a link that already exists. The balance arrives by `sysio.liq::settle`, which invokes no recipient code;
a rejecting transfer handler cannot block linking or delivery. The banked WIRE is credited with `sysio.liq::creditowed`, which
notifies no one. WIRE the yield pool cannot cover yet is kept: the parked row stays at balance 0 with it
banked, and a later delivery pays it. Parked funds have already paid the fee and used the budget.

## Desyndication

`desyndicate(holder, quantity)`, signed by the holder, sends shadow LIQ back to its outpost.

1. The holder must be AuthX-linked for the token's chain; the outpost pays that pubkey.
2. The pair must have a `syndconfig` row (`desyndication budget is not configured`).
3. The pair's desyndication bucket is ticked, as above. A quantity above its level is refused
   (`desyndication exceeds the current budget`). The bucket drops by the whole quantity.
4. The quantity moves from the holder's own `sysio.liq` row to `sysio.synd`. Only that row can be
   desyndicated: shadow held in an item or in `parked` sits in `sysio.synd`'s row and is not the
   holder's to send.
5. The fee, `quantity * desynd_fee_bps / 10000` floored, stays in the fee pot. A fee that rounds to
   zero leaves the whole quantity to burn; a fee of the whole quantity (10000 bps) is refused, since
   nothing would reach the outpost.
6. The net rest is burned with `sysio.liq::burn` and queued to the outpost as `DESYNDICATE_LIQ`, with
   the next request id from `syndcounters`. `returns` retains the original holder, destination key,
   outpost, token and exact net amount until its external outcome is established. The attestation
   carries `total_syndicated`, the live outstanding shadow after the burn; no historical total is stored.

The burn is final. An outpost either pays the release, stores it and pays it later, or skips it:

- **Stored, paid later.** Once the recipient resolves, an outpost never loses a release. Solana stores it
  as a `PendingPayout` at `["pending_desyndication", request_id_le8]` for any of four reasons
  (wire-solana `programs/liqsol-core/src/states/pending_payout.rs:33-52` at bb134198): `OutpostFrozen`,
  `LegacyUserRecord`, `CustodyShortfall` and `SettlementRefused`. Ethereum stores it in
  `SyndicationPool.pendingDesyndications(requestId)` with reason `OUTPOST_FROZEN`, `SETTLEMENT_REFUSED`
  or `CUSTODY_SHORTFALL` (wire-ethereum `contracts/outpost/SyndicationPool.sol:155-159`, `:182` at
  d4fbb8f8). The permissionless crank (`pay_pending_desyndication` on Solana,
  `payPendingDesyndication` on Ethereum) pays each stored release exactly once, after its cause clears.
- **Skipped.** A release the outpost refuses on its content or configuration is logged and dropped. On
  Solana these are: the chain code, an unresolvable user, a non-positive amount, a user that is one of
  the pool's own custody authorities, an absent `GlobalState` or `DistributionState`, an outpost that is
  not PostLaunch, and a token code that does not map to the pool's mint (wire-solana
  `programs/liqsol-core/src/instructions/opp/inbound.rs:5121-5241`,
  `desyndication_config_refusal` `:5449-5459`). Each logs `opp_outpost: DesyndicateLIQError ...
  err_data=<reason>` (`:4986-4994`). On Ethereum they are `wrong chain`, `wrong token`,
  `amount out of range` and `unresolvable recipient` (`SyndicationPool.sol:594-618`), each a
  `DesyndicationDropped(requestId, depotAmount, reason)` event (`:220`).

**Return resolution.** Both actions below require `sysio` and an existing outstanding request:

- `finishreturn(request_id)` attests the external obligation was paid/completed and removes it without
  any depot credit.
- `refundreturn(request_id)` attests irreversible external rejection/cancellation: nothing was paid and
  no payable external obligation remains. It restores the exact stored net amount to the original
  holder, then consumes the obligation atomically. The fee stays charged. A failed mint rolls back the
  removal. Repeating either action on an already resolved ID fails; IDs are never reused.

These are trusted governance determinations based on external evidence, not on-chain verification of
external finality. A queued or pending payout, timeout, transient refusal, or duplicate-request response
never justifies a refund: the external obligation may still pay. Governance must establish irreversible
rejection/cancellation, including no stored payout and no prior payment. In particular an `already paid`
or `already pending` response is not rejection of the underlying obligation.

No receipt protocol, automatic timeout refund, or permanent return history is introduced. The original
holder and destination are retained while needed; governance does not reconstruct them from old traces.
`sysio.liq::recredit(holder, quantity)` remains its existing exceptional privileged supply-repair facility,
with unchanged authorization. It is not the normal return recovery interface and has no request-level
deduplication; operators must use `refundreturn` for these outstanding obligations.

## The solvency check

The invariant is that each outpost holds at least what the depot has issued against it. No side keeps a
counter for it: each side reads the balance it already holds at the moment it builds or checks a
message.

Line numbers below are at wire-sysio c85c05118b, wire-solana bb134198 (`programs/liqsol-core/src/`) and
wire-ethereum d4fbb8f8 (`contracts/outpost/SyndicationPool.sol`). The function or symbol is named with
each, so a cite survives a line shift.

### The readings

| Reading | Where it is read |
|---|---|
| **Depot outstanding** of a shadow symbol | `liq::outstanding_of`: the `sysio.liq` supply plus the yield parked in `liqpending` (`contracts/sysio.liq/include/sysio.liq/sysio.liq.hpp:240-245`). Pending yield counts because the permissionless `queueyield` mints it into supply later (`contracts/sysio.liq/src/sysio.liq.cpp:188-210`): it is committed once `mintyield` parks it (`:138-150`). `headroom_of` is the asset range net of the same figure (`sysio.liq.hpp:252-256`). |
| **Solana custody** | The `liqsol_pool_ata` balance. `SYNDICATE_LIQ` reads it after the syndication's transfer (`syndicate_post_launch`, `instructions/wire_syndication/syndicate_liqsol.rs:338-339`); `LIQ_YIELD` after the claim into the pool (`report_liq_yield`, `instructions/opp/report_liq_yield.rs:181`, `:188`). The typed account is reloaded first because it is stale after the CPI. On a `DESYNDICATE_LIQ` it is the balance `settle_desyndication` reads before anything moves (`instructions/opp/inbound.rs:5580`). liqSOL amounts cross the wire unscaled, so both sides count in one unit. |
| **Ethereum custody** | `SyndicationPool.poolBalanceDepot()`, the pool's liqETH balance floored into the depot frame (`SyndicationPool.sol:801-803`), the frame `syndicate` credits in. `SYNDICATE_LIQ` reads it after the transfer (`syndicate`, `:451`); `LIQ_YIELD` carries the balance the fold measured (`realizeYield`, `:497`, `:510`). |
| **`DESYNDICATE_LIQ.total_syndicated`** | The depot outstanding after this burn: `outstanding_of` read before the inline burn, less the net amount (`contracts/sysio.synd/src/sysio.synd.cpp:1424`). |

Every `SYNDICATE_LIQ` and `LIQ_YIELD` carries the outpost's custody reading in `total_syndicated`;
every `DESYNDICATE_LIQ` carries the depot's outstanding in the same field.

### The two inequalities

**On the depot**, as `sysio.synd` admits a `SYNDICATE_LIQ` or `LIQ_YIELD`:

```
onsynd:   total_syndicated >= outstanding_of(st) + amount      (sysio.synd.cpp:1106-1107)
onyield:  total_syndicated >= outstanding_of(st)               (sysio.synd.cpp:1147-1148)
```

- **Only an admitted message is compared**, after `admit_sequence` has consumed its sequence
  (`sysio.synd.cpp:1100`, `:1142`). A replayed or dropped message can carry a stale, lower total, and
  comparing it would be a false alarm.
- **`onsynd` compares with the outstanding after its own mint** (`+ amount`): the outpost locked the
  syndication before it read its custody. `sysio.msgch` sends an envelope's messages as inline actions,
  which run depth-first, so each message sees the mints of the ones before it.
- **`onyield` compares with the outstanding as it stands**: the report is held and mints nothing yet.

**On an outpost**, as it settles a `DESYNDICATE_LIQ` from the envelope:

```
pool_balance - amount >= total_syndicated      (a pool below amount is a shortfall too)
```

- Solana: `settle_desyndication` with `solvency_floor = Some(total_syndicated)` (`inbound.rs:5580-5591`).
  It runs after every account the payout needs is present and before every chain-state refusal, so a
  payout that would also be refused still raises the alarm. `pay_pending_desyndication` passes `None`
  (`inbound.rs:5822-5833`): the total stored with a shortfall record is a snapshot, and clearing the
  cord is the operator's decision.
- Ethereum: the same rule in `_handleDesyndicateLIQ` (`SyndicationPool.sol:629-633`). It runs after
  the content checks, `already paid` and the recipient, and after the pause check: a paused pool stores
  the release as `OUTPOST_FROZEN` with no comparison (`:620-627`), as a frozen Solana outpost does. The
  `already pending` check comes after it (`:638-641`), so a re-emit of a stored id still raises the
  alarm. `payPendingDesyndication` applies no floor (`:751-773`).

The check is one-sided, custody >= outstanding. Every state in flight moves custody up or the
outstanding down, so none of them raises a false alarm:

- a syndication is locked on the outpost before the depot mints it;
- a desyndication is burned on the depot before the outpost pays it, or while the outpost stores the
  payout as pending (the tokens stay in the custody account until paid);
- yield is claimed into custody before it is reported, and reported before it is released;
- an envelope ruled INVALID or dropped with `dropenv` is burned on the depot (`sysio.synd.cpp:819`,
  `:1260`) while the outpost keeps the custody;
- a `recredit` of a skipped desyndication returns shadow the outpost never paid out (see
  [The recredit rule](#desyndication));
- anyone can send tokens to a custody account.

Every writer of the depot outstanding is matched by custody, or is a launch or governance act:

| Writer | Line (`sysio.liq.cpp`) | Matched by |
|---|---|---|
| `mint` (from `sysio.synd::onsynd` and `importsynd`) | `:105` | the syndication the outpost locked before it sent the message; the pre-launch import (below) |
| `mintyield` (pending) | `:138-150` | the yield the outpost claimed into custody before it reported it |
| `queueyield` | `:205` | moves pending into supply; the outstanding is unchanged |
| `burn` | `:118` | lowers the outstanding: a desyndication, an INVALID ruling, `dropenv` |
| `recredit` | `:91` | governance: custody the outpost never paid out (the recredit rule) |
| `regliqpool` | `:355` | launch: the LCO liquidity, "already in outpost custody" (`:353-354`) |

### What happens on each side

| Depot result | What happens |
|---|---|
| custody = outstanding | Nothing. |
| custody > outstanding | `sysio.synd::<path>: EXCESS -- reported <r> outstanding <o>` is printed, and nothing else. |
| custody < outstanding | Record a `mismatch` and send `sysio.andon::pull` with `sysio.andon@active` if the cord is clear; otherwise record the shortfall without repeating the pull. |

With the required Andon deployment in place, intake is still held and minted and the envelope
continues. Privileged syndication bypasses inline authorization but declares the existing
Andon active permission. There is no contract-maintained puller list. A missing Andon account or incompatible deployed action fails the transaction atomically. A total is not a slashable fact.

On an outpost a shortfall moves nothing, pulls that outpost's own cord and stores the payout:

- Solana: `handle_desyndicate_liq` sets `GlobalState.frozen`, logs `opp_outpost:
  DesyndicateLIQCustodyShortfall ... pool_balance=<b> depot_outstanding=<o> -- outpost frozen, payout
  stored`, and stores a `PendingPayout` with reason `CustodyShortfall { pool_balance, depot_outstanding }`
  (`inbound.rs:5269-5289`, `:5307-5314`; `states/pending_payout.rs:42-47`).
- Ethereum: `_recordCustodyShortfall` emits `CustodyShortfall(requestId, poolBalanceDepot,
  totalSyndicated)` (`SyndicationPool.sol:236`), pauses the pool if it is not paused already, and stores
  the payout in `pendingDesyndications` with reason `CUSTODY_SHORTFALL` (`:660-675`; enum `:155-159`). A
  release with request id 0 still emits the event and pauses the pool, then is dropped
  (`request id 0 cannot be deferred`, `:670-673`). A request id already stored still emits the event and
  pauses the pool, then is dropped as `already pending`, keeping the first record (`:687-690`).

The runbook for a shortfall-triggered pull on each chain is
[`emergency-stop-playbook.md`](emergency-stop-playbook.md) (A shortfall-triggered pull).

**Recovery must wait for post-repair admission.** A `SyndicateLIQ` or `LIQYield` built before a custody
repair still carries the pre-repair custody. `check_custody` checks every admitted message, even while
frozen; clearing the depot cord before those messages arrive lets an old shortfall pull it again.
Repair custody, clear the repaired outpost when safe, and have it emit a message after the repair (or
wait for its next one). Keep the depot cord pulled until the depot admits that message. Read:

```bash
clio -u "$DEPOT_URL" get table sysio.synd syndcursors -S sysio.synd
clio -u "$DEPOT_URL" get table sysio.synd mismatch -S sysio.synd
```

For that `chain_code`, `syndcursors.last_sequence` must reach the post-repair message's `sequence`,
and the latest `(chain_code, token_code)` incident must be reconciled explicitly; read all result pages. Record the
message's emission after repair and its actual admission. `admit_sequence` drops any sequence at or
below the cursor, so a cursor already past the message plus no incident does not alone prove that
message was admitted: verify its admission trace or observe a later post-repair message being admitted.
Only after this check passes for every repaired outpost may the depot cord clear. Mismatch rows for
earlier pre-repair sequences reporting the known shortfall are expected and must be recorded as part
of the existing incident, not treated as a new incident. A mismatch on the post-repair message still
requires investigation before clearing.

### What counts as slack

Custody above the outstanding is slack, and slack hides a shortfall up to its size. The real slack is:

- **Yield spent on pretokens** (Solana, PreLaunch only). `purchase_pretokens_from_yield` decrements
  `yield_accumulated_liqsol` (`instructions/wire_pretokens/purchase_pretokens_from_yield.rs:193-198`)
  but moves no liqSOL out of the pool, and no counter records it.
- **Flooring dust** (Ethereum). `poolBalanceDepot()` floors the pool balance into the depot frame
  (`SyndicationPool.sol:801-803`); the remainder below one depot unit stays in the pool. Solana has none:
  its custody reading is passed unscaled (`syndicate_liqsol.rs:339-346`).
- **Donations**: anyone can send tokens to a custody account.
- **Custody left behind by burned envelopes**: an INVALID ruling or `dropenv` burns shadow on the depot
  (`sysio.synd.cpp:819`, `:1260`) while the outpost keeps the tokens.
- **Custody behind a dropped report, permanently.** A `SYNDICATE_LIQ` or `LIQ_YIELD` the depot drops at
  intake mints nothing, but the outpost has already counted it: Solana has taken the syndication into the
  pool (`syndicate_liqsol.rs:254`, `:282-287`) or advanced its yield watermark
  (`report_liq_yield.rs:213-214`); Ethereum has added the syndication to principal
  (`SyndicationPool.sol:446`) or folded the yield into it (`:505`). Neither side re-reports it, so that
  custody stays in the pool unowned and is slack for good. The depot's `sysio.synd::<path>: DROP --
  <reason>` console line (`drop`, `sysio.synd.cpp:174-176`) is its only audit trail.

The pretoken purchase proceeds and the claimed pool yield are not slack: they back the LCO shadow
(below), and the yield unreported at the flip is reported as `LIQ_YIELD` later.

The check has two further limits. It does not detect an outpost that locked funds the depot never
released; that shows up as held envelopes, not as a `mismatch` row. And a shortfall on the depot is
found only when the outpost next sends a `SYNDICATE_LIQ` or `LIQ_YIELD`; a shortfall on an outpost only
when the depot next sends it a `DESYNDICATE_LIQ`.

**A negative liqETH rebase is a true alarm.** liqETH rebases: when its yield oracle finalizes a negative
delta, `Yield.sol` calls `liqEth.decreaseIndex` (wire-ethereum `contracts/liqEth/Yield.sol:514-518`,
`contracts/liqEth/liqEth.sol:68-71`), which lowers the pool's balance with no depot burn behind it. The
custody no longer covers the outstanding, and both sides say so: the depot on the next `SYNDICATE_LIQ`
or `LIQ_YIELD` (a `mismatch` row and the cord), the pool on the next `DESYNDICATE_LIQ` (it pauses itself
and stores the release as `CUSTODY_SHORTFALL`). `realizeYield` sends nothing meanwhile: it reverts with
`WIRE_PoolUnderbacked` while the balance is below principal (`SyndicationPool.sol:499`). Recovery at v1
is a top-up: send liqETH to the pool until `poolBalanceDepot()` covers the outstanding plus every stored
release, then unpause the pool and pay the stored releases. Keep the depot cord pulled until a message
built after the repair has been admitted without a mismatch, as described above
([`emergency-stop-playbook.md`](emergency-stop-playbook.md), A shortfall-triggered pull). A governance
write-down of the depot's outstanding instead is pending the owner's ruling; nothing performs one at v1.

### Launch reconciliation

At the Solana flip the depot mints two things against the `liqsol_pool_ata`:

- the LCO shadow, `initial_chain_amount`, minted to `sysio` by `regliqpool` (`sysio.liq.cpp:332-356`,
  bootstrap window only `:337`);
- each pre-launch syndicated position, minted verbatim by `importsynd` (`sysio.synd.cpp:1473-1490`,
  through `credit_by_pubkey` `:1599-1611`).

At the flip the pool holds:

| Holding | Source |
|---|---|
| syndicated principal, `total_staked_liqsol` | `syndicate_liqsol.rs:254` (transfer), `:282-287` (counter) |
| pretoken purchase proceeds, `total_purchased_liqsol` | `instructions/wire_pretokens/purchase_pretoken.rs:247-254` (transfer), `:298-303` (counter); PreLaunch only (`:176-180`) |
| claimed pool yield not converted to pretokens, `yield_accumulated_liqsol` | the claims into the pool; all of it is unreported at the flip, since `liq_yield_reported` is 0 on every pre-launch path (`instructions/wire_config/admin_instructions.rs:110-116`) |
| yield spent on pretokens | stays in the pool with no counter (`purchase_pretokens_from_yield.rs:193-198`) |
| donations | anyone |

The pretoken proceeds and the yield are the protocol proceeds
(`instructions/opp/init_liqsol_reserve.rs:208-214`). They back the LCO shadow `regliqpool` mints
("already in outpost custody", `sysio.liq.cpp:353-354`); this is an inference from those two comments,
not a check the code makes.

**The trap.** The first PostLaunch `report_liq_yield` reports the whole unreported yield,
`yield_accumulated_liqsol - liq_yield_reported` (`unreported_liq_yield`,
`states/wire_deposit_state.rs:193-196`; `report_liq_yield.rs:163`), and advances the watermark
(`:213-214`). That yield is liqSOL the pool already held at the flip. If `initial_chain_amount` and the
import credits are sized from the raw pool balance, the remainder is minted twice, once at the import and
once as yield, and the depot reports a shortfall at the first message after that yield is released.

**The rule.** Read at the flip, while the outpost is Launching (nothing but a donation can move the pool
then: `admin_instructions.rs:134-139`):

```
initial_chain_amount + Σ import credits  <=  pool balance − (yield_accumulated_liqsol − liq_yield_reported)
```

The only cross-check the tooling makes is the optional `LiqPoolSpec.custody_total`
(`libraries/opp/proto/sysio/opp/bootstrap/bootstrap.proto:191-195`): validation V13 requires it to equal
`initial_chain_amount` plus the pool's `syndications` when it is non-zero, and skips it at 0
(`libraries/opp/test/test_bootstrap_platform_config.cpp:273-280`). V13 compares the config with itself,
not with the chain: it cannot tell a `custody_total` copied from the raw pool balance from a correct one.
Check the left-hand side against the right-hand side read from the chain first, then declare
`custody_total` as the left-hand side; never copy the raw pool balance into it. The snapshot tool that
fills the config is not in any repo.

**The Ethereum launch rule.** `SyndicationPool` seeds its principal once, at go-live, with
`initializeSyndication(..., _initialPrincipal)` (wire-ethereum `SyndicationPool.sol:288-309`), and the
depot mints the same two things against it. The seed must equal the depot's mint and fit the pool:

```
_initialPrincipal == initial_chain_amount + Σ import credits <= poolBalanceDepot()
```

A seed above the balance reverts (`WIRE_InitialPrincipalExceedsBalance`, `:303-306`). A seed below the
depot's mint is reported by the next `realizeYield` as yield (`:494-511`) and minted a second time; a
seed above it hides real yield. The `cast call` check is in wire-ethereum `docs/deploy.md` (Go-live
reconciliation).

**After the flip.** The only PostLaunch outflow from the pool is the depot-ordered desyndication:
`settle_desyndication`'s transfer (`inbound.rs:5739`), reached from the envelope and from
`pay_pending_desyndication`. The others are closed:

- `refund` requires the Refund state (`instructions/wire_config/refund.rs:131-134`), which is reachable
  only from PreLaunch (`WireState::can_transition_to`, `states/wire_deposit_state.rs:34-40`);
- the local `desynd` refuses PostLaunch (`instructions/wire_syndication/desyndicate_liqsol.rs:192-195`);
- `init_liqsol_reserve` is hard-disabled: `require_reserve_and_swap_enabled` always refuses
  (`lib.rs:982`, `instructions/opp/mod.rs:56-58`).

**Precondition on re-enabling `init_liqsol_reserve`.** It moves the protocol proceeds,
`total_purchased_liqsol + yield_accumulated_liqsol` (`init_liqsol_reserve.rs:209-214`), out of the pool
into a reserve vault, while the depot supply they back stays. Re-enabled without a matching depot burn,
the next `DESYNDICATE_LIQ` finds the pool short and freezes the Solana outpost, and the next
`SYNDICATE_LIQ` or `LIQ_YIELD` pulls the depot cord.

## The emergency stop

`sysio.andon` holds one flag for the whole depot (the Andon cord). While it is pulled nothing leaves
`sysio.synd` and nothing it holds is burned; everything that brings funds in, and every step that only
records state, runs. Nothing is skipped: work that falls due while the cord is pulled is done by the
first queue step (`crank` or `closeenv`) after it clears.

The runbook for pulling and clearing the cord on every chain, and for the work that waits on the
clear, is [`emergency-stop-playbook.md`](emergency-stop-playbook.md).

| While the cord is pulled | Behaviour |
|---|---|
| Intake (`onsynd`, `onyield`, `closeenv`) | Runs as usual; `sysio.msgch` accepts every envelope. |
| Queue step: refresh, outcomes, requests | Runs: requests are issued and bonded, APPROVED and INVALID outcomes are recorded with their snapshots. |
| Queue step: releases | None. The step prints `the andon cord is pulled; releases, deliveries and burns wait for the clear` once and leaves every item where it is; no bucket is ticked. |
| An INVALID ruling | Recorded, with its forfeit snapshot; the burn and the claim of the forfeit wait (`... the burn waits for the clear, the pair waits`). |
| A VALID ruling after a challenge | Recorded, with the challenger's hold share snapshotted on the envelope (`hold_share`, `hold_beneficiary`) and marked `share_pending`; the forward waits (`the challenger's hold share waits for the andon cord to clear`). The first step after the clear claims the award if `sysio.bond` still owes it and pays the share from the snapshot, once -- even if `sysio.bond` pruned the request meanwhile. |
| `challenge` | Runs: the charge moves into `sysio.synd` and the hold bond into `sysio.bond`. Challenge windows keep counting. |
| `linkswept` | Delivers nothing and returns, so the user's `createlink` still succeeds; `sweep` delivers after the clear. |
| `sweep`, `desyndicate`, `dropenv`, `sweepyield` | Refused: `the andon cord is pulled: funds cannot leave custody`. |
| `setconfig`, `importsynd` | Run. |

Frozen queue steps never spend capacity or release funds. The first tick after clear refills
for all elapsed depot epochs, including the freeze, capped at the configured burst.
Repeated freezes cannot raise capacity beyond that cap.

## Tables

| Table | Key | What it holds |
|---|---|---|
| `syndconfig` | outpost, token | the pair's rules (below) |
| `envelopes` | outpost, token, depot epoch | digest, syndicated and yield totals, item count, state, `sysio.bond` request id, released, burned, the request's recorded outcome, the forfeit and returned bounty snapshotted on INVALID, and the challenger's hold share, its beneficiary and whether its forward is pending, snapshotted on VALID |
| `items` | envelope, then id (arrival order) | kind, beneficiary key family and pubkey, amount, remaining, yield position |
| `parked` | token, key family, pubkey | balance and yield position of an unlinked pubkey |
| `buckets` | outpost, token, direction | level and the depot epoch last ticked, one per direction (SYNDICATION, DESYNDICATION) |
| `ledger` | outpost, token | release and underwriting cursors; `retained_from_epoch` replay floor |
| `returns` | request id | original holder, destination family/key, outpost, token and exact net burned amount; erased upon explicit resolution |
| `feepot` | token | collected fees, with a yield position; bounties are paid from it, and its yield goes to `sysio` |
| `yieldpool` | token | the WIRE `sysio.synd`'s holder row earned, pulled from `sysio.liq` and credited out to held, parked and fee positions |
| `syndcursors` | outpost | the highest sequence admitted (replay guard) |
| `syndcounters` | singleton | the next item id and the next desyndication request id |
| `syndstate` | singleton | whether the launch import is closed; the pair the next queue step starts at |
| `mismatch` | outpost, token | latest unresolved shortfall evidence: epoch, sequence, kind, reported custody, expected outstanding, and time |

Envelope states: OPEN, WAITING, REQUESTED, RELEASABLE, HELD, INVALID, DONE.

## Configuration

`setconfig(chain_code, token_code, synd_fee_bps, desynd_fee_bps, synd_burst, synd_refill,
desynd_burst, desynd_refill, window_sec, bounty, challenge_extra)`, signed by `sysio`. It replaces the
pair's whole row.

| Knob | Default (no row) | Meaning |
|---|---|---|
| `synd_fee_bps` | 0 | fee on each released syndication tranche, at most 10000 |
| `desynd_fee_bps` | 0 | fee on each desyndication, at most 10000 |
| `synd_burst`, `synd_refill` | unset: nothing releases | syndication bucket size, and refill per depot epoch, in base units |
| `desynd_burst`, `desynd_refill` | unset: nothing desyndicates | desyndication bucket size and refill |
| `window_sec` | 10800 | challenge window of each request; above 0 |
| `bounty` | 0 | bounty posted on each request, paid from the fee pot |
| `challenge_extra` | 0 | charged to a challenger on top of the hold bond; set it above 0 at launch (see Challenge and ruling) |

The bucket, bounty and challenge amounts must each fit an asset. A pair with no row still has its
envelopes requested (with the default window and no bounty), but releases nothing and accepts no
desyndication. The row must exist, with the buckets set, before the pair carries traffic.

## What a holder sees

- A syndication from the outpost does not arrive at once. It waits for its envelope's request to be
  bonded, then for its turn in the bucket. Until then it earns WIRE in `items`, and that WIRE is
  credited to the account when the syndication arrives.
- It arrives in the account the pubkey is linked to at release, less the syndication fee. With no
  link it is parked, still earning, and arrives the moment the pubkey is linked (or on `sweep`).
- A large syndication may arrive in several tranches, one per bucket refill.
- An envelope ruled INVALID burns what had not yet been released; what had been released stays.
- The WIRE a held or parked syndication earned is owed on the holder's own `sysio.liq` row once it is
  delivered, and `sysio.liq::claim` pays it.
- `desyndicate` takes the whole quantity from the holder's balance and releases the quantity less the
  desyndication fee on the outpost. It is refused when the pair's desyndication budget is too low now;
  it refills every depot epoch.

## What an operator sees

- The console lines: `DROP` for a refused intake message, `sysio.synd::queue:` for anything a step
  left where it was, with the reason.
- `envelopes` shows where each envelope stands; `ledger.queue_epoch` is where each pair's queue
  starts; `buckets` shows each pair's remaining budget.
- `mismatch` lists the current unresolved incident for each affected outpost/token; a `SHORTFALL` line on the console
  goes with each row, and an `EXCESS` line with each message that reported more custody than the depot
  has outstanding.
- `crank(limit)` moves the queue when no envelope is arriving. It is permissionless and never throws.
- `sweepyield(token_code)` pulls the WIRE `sysio.synd`'s holder row has earned into the token's
  `yieldpool`, so held and parked positions can be paid out of it, and sends `sysio` what the fee pot
  earned: the pot is protocol revenue, and so is its yield.
- `dropenv` clears an envelope the queue can never request (below).

## Deployment order

- `sysio.synd` is deployed, privileged, before any liq token is activated: once a token is active,
  `sysio.msgch` routes its syndications to `sysio.synd`.
- `sysio.msgch`, `sysio.synd`, `sysio.liq`, `sysio.bond` and `sysio.authex` call each other's actions
  and read each other's rows. They deploy as one set, from the same commit (see
  [`contract-upgrade-order.md`](contract-upgrade-order.md)).
- `setconfig` for every pair before the pair carries traffic (see Configuration).
- Deploy `sysio.andon` privileged before `sysio.synd` carries traffic and configure its native
  pull/clear permissions (see the emergency-stop playbook). Its active permission exists from
  account creation and is declared by syndication's privileged inline pull. Redeploy Andon,
  swap, liq, bond and synd together when the shared cord layout changes. The simplified cord
  and bucket layouts require a fresh deployment; no in-place state migration is included.

## Misconfigurations that stall a pair for good

The queue never throws, so a pair that can never move just waits, printing its reason each step.
Later envelopes of the same pair wait behind it; other pairs are not affected.

- **A shadow token with a precision below 2.** `sysio.bond` bonds in increments of
  `10^(precision - 2)` base units and cannot bond such a token. Its envelopes stay WAITING
  (`sysio.bond cannot bond the token`), and nothing of the pair is ever released. `sysio` clears them
  one by one with `dropenv`, which burns what they hold.
- **An unset syndication bucket.** With no `syndconfig` row, nothing releases
  (`no syndconfig row: the syndication bucket is unset`). A row with `synd_burst` or `synd_refill` of
  0 behaves the same once the bucket is empty (`syndication bucket is empty until it refills`). Setting
  the row lifts the stall; the queue resumes on the next step.
- **An unset desyndication bucket.** With no row, or a zero `desynd_burst`, every `desyndicate` of the
  pair is refused. This refuses the signed action; it does not stall the queue.

## Operational retention and reconciliation

No historical reporting API or archive is maintained. Lifetime ledger totals are removed. Replay
sequences, counters, import completion, active custody/yield obligations and unresolved returns remain.

`pruneenv(chain_code, token_code, limit)` is permissionless and examines at most `limit` envelopes from
the pair's oldest retained row, stopping at the first unfinished one. It erases only DONE/INVALID rows
with a consumed terminal outcome (or a never-issued governance drop), no items or pending challenger
share, and an acknowledged bond result if that request still exists. DONE before finality is retained.
Settled rows behind a live earlier envelope remain temporarily; this prefix rule avoids a permanent
per-envelope tombstone table. The widened `retained_from_epoch` floor rejects all future admission below
it, including fresh sequences, different digests, and previously absent epochs in that prefix.

A later shortfall replaces the same pair's incident; a healthy report does not erase it.
`reconcile(chain_code, token_code, reported)` requires `sysio` and an existing incident, verifies the
attested custody covers current live outstanding supply, and erases only that incident. `reported` is a
governance attestation, not a fresh oracle measurement. The global Andon cord must be cleared separately
after every relevant incident and other cause has been resolved.

Existing deployed tables require a coordinated migration for the changed layouts and renamed return
table; no automatic migration or recovery from previously discarded data is included.

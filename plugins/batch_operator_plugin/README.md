# batch_operator_plugin

Cranks Depot and Outpost contracts, ferrying OPP message chains between the WIRE chain and external blockchains (Ethereum, Solana).

## Overview

Every batch operator runs this plugin. Each outpost chain runs its own consensus epoch:
`sysio.epoch` keeps, per chain, an epoch counter, a minimum duration between two envelopes
(`epochcfg`, default 6 minutes) and a sliding window of `batch_op_groups` groups of `operators_per_epoch`
operators (default 3 groups of 7) drawn from the operators serving that chain. The group
at the front of a chain's window is elected for that chain's current epoch and its
members execute the epoch cycle on that chain. Each poll the plugin also reads its own
`sysio.opreg` status (one exact lookup of its `operators` row):

| Status | Relay jobs | `chkcons` and depot cranks |
|--------|------------|----------------------------|
| `ACTIVE` | run | push |
| `UNKNOWN` (collateral under the minimum), or no row | stopped on every chain: `sysio.msgch::deliver` refuses any status but `ACTIVE` | keep their last state |
| `SLASHED`, `TERMINATED` | stopped on every chain | stop |

The relay jobs start on the first poll that reads `ACTIVE`, and stopped ones are rebuilt
on the first poll that reads `ACTIVE` again. A failed read changes nothing.

### Served chains

While its status lets the relay jobs run, the plugin relays a chain when its operator's
`sysio.opreg::servchains` row on it (its rows read as one range of the `byaccount` index) is:

| Status | Relayed |
|--------|---------|
| `SERVING` (`joinchain`) | yes |
| `LEFT` (`leavechain`) | while the operator still holds a seat in that chain's window, whose duties it still owes |
| `EVICTED` (the chain's miss rails) | no: `sysio.msgch::deliver` refuses its deliveries |

A chain with no row is not relayed. The relayed set is re-read every poll, so a
`joinchain` or `leavechain` takes effect without a restart.

## Epoch Cycle

Run by every member of a chain's elected group, on that chain, for the chain's own epoch. A
chain advances only once its traffic makes it due (`sysio.epoch` `due_at`), so nothing is held
back within an epoch: each of the chain's three relay jobs acts at every poll while the chain's
state calls for it.

**Outbound (WIRE → Outposts):**
1. Read the outpost's outbound envelope that `sysio.epoch::advance` built
   (`sysio.msgch::buildenv`) from the depot's `outenvelopes` table
2. Deliver it to the Outpost contract (ETH: one `OPPInbound.epochIn()` carrying the whole envelope,
   continued while its dispatch spills; SOL: `epoch_in`, chunked)
3. The outpost reaches consensus on the delivery that completes a strict majority of the group

The job calls the outpost client every poll until the depot records this operator's inbound
delivery of the epoch. A delivery the outpost already recorded is not sent again: it awaits the
rest of the group. The outpost's per-epoch crank rides the first call of an epoch that returns.

**Inbound (Outposts → WIRE):**
1. The outpost emits its outbound envelope once it has dispatched the inbound one (ETH: in the `epochIn`
   that finishes dispatch, a continuation if it spilled; SOL: in the consensus-reaching `epoch_in`)
2. Read the latest outbound envelope from Outpost storage
3. Deliver its raw protobuf bytes to Depot (`sysio.msgch::deliver`)
4. Depot accepts the first strict majority of the group's deliveries; right after its own delivery
   lands, the operator pushes `sysio.msgch::chkcons` if the chain is due, which advances it

**Backlog (Outposts → WIRE):**
1. Once the depot has decided the chain's current epoch N and the outpost's latest emitted epoch is N,
   read the outpost's outbound backlog in one read (ETH `OPP.pendingOutbound()` at the read
   confirmation; SOL `OutboundMessageBuffer` and `LatestOutboundEnvelope` at `confirmed`)
2. With messages queued, report the count and the backlog's age to the depot
   (`sysio.msgch::reportqueue`); a strict majority of the group's reports records the chain's traffic
3. Report once per epoch, and again while this operator's report for N stands and the chain stays idle
   (`due_at` zero) one chain minimum after the last one, which re-runs the depot's tally
4. While nothing is reported for N, read the outpost at most a quarter of the chain minimum apart, not
   every poll

The backlog job keeps its state under its own lock, so its outpost read and depot push never hold
up the chain's outbound or inbound job.

## Depot cranks

The plugin also pushes the depot actions nothing on chain schedules.
`sysio.msgch::chkcons(chain_code)` — which re-tallies a chain's undecided epoch, and
advances a decided one once the chain is due (`sysio.epoch` `due_at`: it has traffic and its
minimum has passed) — is pushed for each relayed chain:

| Pusher | When |
|--------|------|
| An elected operator, from the epoch tick | The epoch is decided (`sysio.msgch::outpcons` records it) and `due_at` has passed |
| An elected operator, right after its own delivery of the current epoch | `due_at` has passed (the delivery may itself have decided the epoch) |
| An elected operator, from the epoch tick | The epoch has stayed undecided one chain minimum past its start, at most once per chain minimum from this operator: its eligible group can shrink with no new delivery |
| Any ACTIVE operator serving the chain, from the epoch tick | The epoch is decided and has stayed due one chain minimum past `due_at`, so its elected group is not advancing it |

None goes to a chain before its genesis advance, to a paused chain, or to an idle decided
chain. The
rest come every `--batch-epoch-poll-ms` from their own cron job, from every ACTIVE
operator, because the elected one may be the operator that is offline, and each is a
cheap no-op once its work is done:

| Action | When | Why nothing else drives it |
|--------|------|----------------------------|
| `sysio.chalg::chkdispute(dispute_id)` | every OPEN envelope dispute | a dispute pauses `sysio.epoch::advance`, so no inline poke can reach it |
| `sysio.swap::tickyield(pair_token)` | every yield pool whose reservoir has a queued balance, at most once per `--batch-yield-tick-interval-ms` per pool from this operator | the reservoir is sold into the pool as time passes; only a tick moves the clock |
| `sysio.liq::queueyield(sym)` | every shadow with yield pending from an outpost `LIQ_YIELD` report | the report lands in `sysio.liq`'s pending balance; queuing it into the swap is a separate, permissionless step |
| `sysio.kicker::kick(sym)` | every kicker pool that could pay, at most ten pools per poll from the crank's own cron job, resuming the next page each poll; an attempt that does not pay is retried after `--batch-kick-interval-ms`, doubling to an hour | only a kick pays accrued gifts; TWAP, catch-up and daily caps are enforced on chain (`contracts/sysio.system/EMISSIONS.md`) |

The two yield cranks stay idle, without logging a read failure per poll, until both
`sysio.swap` and `sysio.liq` are deployed on the depot; the kick crank likewise until
`sysio.kicker` is deployed, and it pushes nothing before `setconfig`. The kick crank
pushes only when the rows say `kick` could pay: the pool's `min_interval_sec` has passed
since `last_kick`, the token's `sysio.liq` supply is not zero, its price observation
(restarted by every payment) spans `twap_window_sec`, and `budget_remaining` and the pool's unspent UTC-day ceiling each cover
`min_gift`. For the supplies it looks up the `sysio.liq::stat` row of each token on the
page, one exact read each, so the page bounds every read in a poll. A failed read of
`kickcfg` or of a supply ends the poll without pushing. Between payments it reads the pools and pushes nothing. A kick that passes those
checks and still does not pay — an Andon hold, a gift below `min_gift`, a treasury reserve,
spot past the contract's spot-versus-TWAP guard —
leaves `last_kick` where it was; the next attempt waits `--batch-kick-interval-ms`, and the
wait doubles per unpaid attempt up to an hour, starting over once `last_kick` moves. The
wait is spent on every attempt, failed or not. The crank runs as its own cron job, so its
pushes, each of which can wait out its 30 s expiration, never delay the epoch tick.
A failed pool-page read ends the poll and is retried from the same page; the plugin prunes
removed symbols from the retry state only at the end of a complete sweep.
`--batch-kick-crank=false` turns the kick crank off on this node, and the relay's
permission then need not be linked for `sysio.kicker::kick`.
`sysio.synd::crank` is pushed by the underwriter every poll (see [Underwriter](#underwriter)).

## Configuration

| Option | Default | Description |
|--------|---------|-------------|
| `--batch-operator-account` | — | WIRE account name for this operator. Configuring it enables the relay |
| `--batch-epoch-poll-ms` | 15000 | Longest interval between the relay's polls of a chain (see [Cadence](#cadence)), and the interval of the depot cranks and the underwriter's poll (ms) |
| `--batch-yield-tick-interval-ms` | 60000 | Minimum spacing between this operator's `sysio.swap::tickyield` pushes per yield pool (ms) |
| `--batch-underwriter-account` | none | `account` or `account@permission` (default `active`) the underwriter bonds from. Configuring it enables the underwriter |
| `--batch-underwriter-max-exposure` | none | Most the underwriter may have bonded at once in one shadow token, as a positive asset (`100.000000000 LIQETH`). Repeat per token; cannot name the same token as unlimited exposure |
| `--batch-underwriter-unlimited-exposure` | none | Allow exposure without a cap for one shadow token, as its symbol code (`LIQETH`, without amount or precision). Repeat per token; balance and verification checks still apply. Startup fails if max exposure names the same token |
| `--batch-kick-crank` | true | Push `sysio.kicker::kick` for every kicker pool that could pay. Off, the relay's permission need not authorize `kick` |
| `--batch-kick-interval-ms` | 60000 | First wait before this operator retries a `sysio.kicker::kick` that did not pay (ms); doubles per unpaid attempt, up to an hour. Must be 1 to 3600000; startup fails otherwise |

There is no separate enable flag: the relay runs when `--batch-operator-account`
is configured, the way `producer_plugin` keys off `--producer-name`, and the
underwriter when `--batch-underwriter-account` is; a node may run either or both,
and the poll option applies to both. The plugin must also be listed under
`plugin =` (or pulled in as a dependency by `external_debugging_plugin`), and
requires `read-mode = irreversible`.

Each role signs with the one operator-configured WIRE signature provider, of any key
type, whose key alone satisfies its authorization on chain: `<operator>@active` for
the relay, the configured permission for the underwriter. The provider may hold its
key remotely, as a `KMS:` WIRE K1 key does
([signature_provider_kms_plugin](../signature_provider_kms_plugin/README.md)). At startup the node also
checks that the key's signatures recover to it and that the authorization may
declare every action the role pushes (`linkauth`): for the relay `sysio.msgch::deliver`,
`chkcons` and `reportqueue`, `sysio.chalg::chkdispute`, `sysio.epoch::tickeconomy`,
`sysio.swap::tickyield`, `sysio.liq::queueyield`, and `sysio.kicker::kick` while the kick
crank is on. A role that fails either check, or matches no provider or more than one, stops
the node. A `reportqueue` the depot refuses is logged at info: the chain's epoch moved on, or
its group changed, since the poll that read it. A transient signing failure
during the key check, such as KMS throttling, is logged and passes the check.

The node exempts the accounts it signs as from subjective CPU billing: both roles
push on a schedule that loses some pushes (a race another operator won, an action
the irreversible view does not show done yet), and billed failures would soon have
the node refuse the account's transactions. The exemption also stops the node from
throttling anyone else's failing transactions that name those accounts, so an
operator node must not expose its transaction push API publicly.

Signers are chosen once, at startup. Each poll the node checks that each role's key
still satisfies its authorization; after `updateauth` rotates it, the role stops
pushing (the underwriter) or logs (the relay) until the node is restarted with the
new key.

### Cadence

Each chain is paced by its own epoch duration:

| | Rule | 360 s minimum | 10 s minimum |
|---|---|---|---|
| Poll interval | 1/24 of the chain minimum, no shorter than 1 s and no longer than `--batch-epoch-poll-ms` | 15 s | 1 s |
| Delivery timeout | 1/2 of the chain minimum, from 5 s to 60 s | 60 s | 5 s |

A chain's outbound, inbound and backlog jobs run at its own poll interval.
The delivery timeout bounds each call to the chain's outpost (an outbound delivery, every
transaction and confirmation wait included; an inbound read; a backlog read; the outpost
crank) and each depot push for the chain (`sysio.msgch::deliver`, `chkcons`, `reportqueue`); an
outbound delivery cut short resumes from the outpost's on-chain cursor at the next poll. Depot reads and the depot cranks'
pushes wait at most a push's 30 s expiration.
The epoch tick, which reads every chain's `epochstate` and `epochcfg`, the operator's
memberships and `sysio.chains`, fires every second and reads at the shortest poll
interval of the chains it relays (`--batch-epoch-poll-ms` when it relays none). A change
of a chain's duration reschedules its jobs on the next read.

### Outpost wiring

Nothing about an outpost is declared per node.

* **Which chains** — every active non-depot `sysio.chains` row the operator relays (see
  [Served chains](#served-chains)).
* **Where each one lives** — the row's own `outpost` struct (`opp_addr` /
  `opp_inbound_addr`), so every operator relays a chain through the same
  deployment.
* **How to reach it** — the RPC client registered under that chain's **own
  code**. `--outpost-ethereum-client` / `--outpost-solana-client` take the
  client id as their first field, and for an outpost that id must be the chain
  code (`ETHEREUM`, `SOLANA`, ...). The Ethereum client's verified `eth_chainId`
  is additionally asserted against the row's `external_chain_id`, so a client
  registered under the wrong code is rejected rather than relayed through.

A group member must deliver on **every** chain it serves, so a missing RPC client
for a relayed chain is fatal: the node logs the chains it cannot serve and shuts
down. A chain the operator does not relay needs no client and is ignored. The check
runs after the sync gate, where `sysio.chains` is readable, and again on every read
that adds a relayed chain. Missing contract *addresses* are not fatal — they are
governance state, fixable with `sysio.chains::setoutpost` without touching a node —
so such a chain is skipped fail-closed and picked up on a later tick. A `setoutpost`
redeploy is likewise picked up on the next epoch tick: the relay job is rebuilt
against the new address rather than left pointing at the old one.

## Underwriter

Any depot account can run the underwriter, with or without the relay, to bond the
requests `sysio.synd` issues for syndication envelopes. Each poll it:

1. **Verifies each statement against its outpost.** An OPEN request states the
   outpost, the depot epoch and the digest of the envelope the depot accepted. The
   underwriter first reads WIRE's latest irreversible `sysio.msgch::outpcons` tip,
   then independently reads the outpost's recorded digest for that same epoch
   (Ethereum `OPP.outboundEnvelopes`, Solana's outbound `EnvelopeLog`) at settled
   commitment (Solana `finalized`, the Ethereum client's read confirmation).
   Matching digests authenticate the accepted prefix: both chains commit each
   envelope's canonical bytes and its predecessor's digest. An older request must
   also match its original `sysio.synd::envelopes` row's request id, chain, token,
   epoch and immutable digest. This relies on finalized WIRE execution of the
   predecessor checks and syndication intake; it is not a standalone historical
   inclusion proof. No old envelope bodies or outpost records are needed. If a
   checkpoint cannot be matched, or any request sharing an epoch lacks its own
   accepted membership, a direct outpost record for that epoch still verifies it.
   The plugin logs matching checkpoints and reports a mismatched tip with both
   digests. Amounts, states and windows come from WIRE's tables.
2. **Bonds** each verified OPEN request `sysio.synd` issued under the `oppenvelope`
   schema, oldest first (`sysio.bond::accept`). Each contribution is the minimum of
   the uncovered remainder, the token's remaining exposure allowance (bonds not
   yet claimed count), and the account's `sysio.liq` balance, rounded down to a
   0.01-token increment. Explicit unlimited exposure removes only the exposure
   allowance limit; a token with neither exposure option is not underwritten.
   An account's first contribution to a request must be
   at least 1/64 of its coverage or take the whole remainder (`sysio.bond`
   refuses a smaller one); until it can, the request waits.
   Multiple underwriters can contribute to the same request;
   for example, three accounts with 2 LIQSOL of available capacity each can fully
   bond a 6 LIQSOL request. Each contribution stays locked while the request is
   OPEN: there is no withdrawal. Principal returns after full coverage, approval
   and claim, or a VALID governance ruling and claim. An INVALID ruling forfeits
   it. A lone provider
   whose cap is below the required coverage can therefore lock its whole cap
   while waiting for other providers.
   Submitted accepts reserve both exposure and wallet funds until the
   irreversible view accounts for them, the request leaves OPEN, or LIB time
   passes the signed transaction's expiration. This also covers a submission
   whose callback times out; the next poll cannot repeat its contribution.
   On startup, new accepts wait until LIB passes one transaction lifetime after
   the later of startup LIB time and wall time, so a previous process's unobserved
   transactions cannot be repeated even after failover to a node with an older
   LIB. Approvals and claims continue during that wait.
3. **Cranks** `sysio.synd::crank`, which releases what bonding allows and issues the
   next request: every poll while an envelope request is in play or has an outcome
   `sysio.synd` has not acknowledged, otherwise every 5 minutes.
4. **Approves** its requests once their challenge window has passed by chain time,
   and **claims** them once approved or ruled VALID, and again while yield the pool
   could not yet cover is owed. Every 10 minutes it claims what a forfeited bond
   earned, which pays it into `sysio.liq` so the request can be pruned, and prunes
   `sysio.bond` and `sysio.synd`. Its `sysio.bond` prunes sweep the requests issued
   before each sweep began, each starting after those the previous one examined.

What it cannot act on waits and is logged on every poll it holds; deduplicating
those lines is left to log tooling:

| Condition | Effect |
|-----------|--------|
| An outpost cannot be read | That chain's requests wait |
| Neither an authenticated accepted prefix nor a direct final outpost record proves the statement | The request waits |
| A synd bug produces a statement inconsistent with its immutable accepted row | `NOT_ACCEPTED`: defense in depth; never bonded through a checkpoint |
| The outpost recorded another digest for that epoch | Never bonded; an error log names both digests |
| The node cannot serve a chain (no RPC client, wrong chain id) | That chain's requests wait; the others go on |
| Neither exposure option for the token, or remaining finite exposure allowance or balance below one 0.01-token increment | The request waits |
| A first contribution would be below 1/64 of the request's coverage and not the whole remainder | The request waits |
| An accept is submitted but not yet accounted for by the irreversible view | Its funds stay reserved and that request waits |
| Startup finality wait | New accepts wait; approvals and claims continue |
| The `sysio.andon` cord is pulled | Nothing is bonded, approved or claimed until it clears |
| A request is challenged (HELD) | Reported, whether or not we bonded it: a judge rules it |
| A bond of ours is ruled INVALID while the node runs, even one since claimed or pruned | Bonding stops until a restart; approvals and claims go on. A forfeited bond's principal is not returned |
| A required funding/freeze table read fails, or a row behind one of our bonds does not decode | The pass does nothing |

### Underwriter setup

Use a separate account for each underwriter process: its exposure cap and pending
submission reservations are local to that process.

On the depot, the underwriter account needs only the shadow LIQ it bonds: no RAM,
CPU or NET. Its bond rows and the rows that hold the WIRE they earn bill the request's
issuer (the `sysio` RAM pool for `sysio.synd`'s requests), and its pushes name no
payer, so their CPU and NET bill the contract they call, `sysio.bond` or `sysio.synd`,
which have unlimited CPU and NET. With a dedicated permission, link it to exactly the
actions it pushes, so the node's key can move the inventory nowhere else:
`sysio.bond::accept`, `approve`, `claim`, `prune` and `sysio.synd::crank`,
`pruneenv`. The WIRE its bonds earn is recorded per issuer and withdrawn with
`sysio.bond::claimwire(account, issuer)` (`sysio.synd` for syndication requests),
which stays unlinked.

The node needs an outpost RPC client for every active chain (each client spec names
a signing key, though the underwriter only reads), the Ethereum OPP ABI files (which
must declare `outboundEnvelopes`) and the Solana IDL (which must declare
`EnvelopeLog`). Use RPC endpoints independent of the batch operators': the
verification is only as good as the outpost reads. Ethereum keeps 200 epochs by
default and Solana keeps its last 10 records; an older request remains verifiable
through a matching recent checkpoint and its accepted syndication row. Proofs are
rebuilt every pass, including after restart; there is no persistent verification
cache or expanded on-chain history. If neither a matching checkpoint nor a direct
record is available, the request waits. Before `sysio.chains::setoutpost` replaces an
active outpost's deployment, let that chain's `sysio.synd` envelopes drain, those
still waiting for a request included: the underwriter reads only the new deployment's
records. Run one underwriter node per account, and bond from that account by no
other means: a cap counts only the bonds its node makes.

Size each cap for the most the token syndicates in one challenge window
(`window_sec`, 3 hours by default) plus the time it takes to claim.

Alternatively, explicitly allow unlimited exposure for selected shadow symbols:

```sh
--batch-underwriter-account uw.acct \
--batch-underwriter-unlimited-exposure LIQETH \
--batch-underwriter-max-exposure '5.000000000 LIQSOL'
```

This permits LIQETH bonds up to the account's available balance while limiting
outstanding LIQSOL bonds to 5 LIQSOL. Repeat the unlimited option for additional
symbols. Neither option enables other tokens, and both require an underwriter
account to have any effect. Naming the same symbol in both options is a startup
error, regardless of argument order or whether it comes from the command line or
configuration file. Duplicate unlimited symbols are also rejected. Unlimited
exposure removes only the cap: contribution rounding and minimums, pending balance
reservations, startup finality, outpost verification, emergency-stop and
forfeiture-halt checks still apply.

A finite cap bounds the token inventory that can be bonded at once. Unlimited
exposure can bond the account's entire inventory of that token within one
challenge window, before a forfeiture triggers the halt. Fund an unlimited
underwriter account with only what you are prepared to have bonded at once.

## Dependencies

- `chain_plugin` — blockchain state access
- `cron_plugin` — irreversible block event subscription
- `signature_provider_manager_plugin` — signing key management
- `outpost_ethereum_client_plugin` — Ethereum outpost relay
- `outpost_solana_client_plugin` — Solana outpost relay

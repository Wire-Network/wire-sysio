# batch_operator_plugin

Cranks Depot and Outpost contracts, ferrying OPP message chains between the WIRE chain and external blockchains (Ethereum, Solana).

## Overview

Every batch operator runs this plugin. `sysio.epoch` keeps a sliding window of
`batch_op_groups` groups of `operators_per_epoch` operators (default 3 groups of 7, epochs
of 6 minutes); the group at the front of the window is elected for the current epoch and
its members execute the epoch cycle. Each poll the plugin also reads its own
`sysio.opreg` status and stops relaying once the operator is `SLASHED` or `TERMINATED`.

## Epoch Cycle

Run by every member of the elected group, for every active outpost chain:

**Outbound (WIRE → Outposts):**
1. Read the outpost's outbound envelope that `sysio.epoch::advance` built
   (`sysio.msgch::buildenv`) from the depot's `outenvelopes` table
2. Deliver it to the Outpost contract (ETH: one `OPPInbound.epochIn()` carrying the whole envelope,
   continued while its dispatch spills; SOL: `epoch_in`, chunked)
3. The outpost reaches consensus once enough group members have delivered identical bytes

**Inbound (Outposts → WIRE):**
1. The outpost emits its outbound envelope once it has dispatched the inbound one (ETH: in the `epochIn`
   that finishes dispatch, a continuation if it spilled; SOL: in the consensus-reaching `epoch_in`)
2. Read the latest outbound envelope from Outpost storage
3. Deliver its raw protobuf bytes to Depot (`sysio.msgch::deliver`)
4. Depot evaluates consensus across the group's deliveries

## Depot cranks

Every epoch poll (`--batch-epoch-poll-ms`) also pushes the depot actions nothing on
chain schedules. `sysio.msgch::chkcons` — which advances the epoch once every active
outpost has consensus and the boundary has passed — comes only from elected operators;
the rest come from every ACTIVE operator, because the elected one may be the operator
that is offline, and each is a cheap no-op once its work is done:

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
pushes, each of which can wait out `--batch-delivery-timeout-ms`, never delay the epoch tick.
A failed pool-page read ends the poll and is retried from the same page; the plugin prunes
removed symbols from the retry state only at the end of a complete sweep.
`--batch-kick-crank=false` turns the kick crank off on this node, and the relay's
permission then need not be linked for `sysio.kicker::kick`.
`sysio.synd::crank` is pushed by the underwriter every poll (see [Underwriter](#underwriter)).

## Configuration

| Option | Default | Description |
|--------|---------|-------------|
| `--batch-operator-account` | — | WIRE account name for this operator. Configuring it enables the relay |
| `--batch-epoch-poll-ms` | 15000 | How often to check epoch state (ms) |
| `--batch-delivery-timeout-ms` | 60000 | Max time for one outbound delivery, every transaction and confirmation wait included (ms). On Ethereum a single receipt wait may use whatever of it is left, so size it past the outpost chain's block time. |
| `--batch-yield-tick-interval-ms` | 60000 | Minimum spacing between this operator's `sysio.swap::tickyield` pushes per yield pool (ms) |
| `--batch-underwriter-account` | none | `account` or `account@permission` (default `active`) the underwriter bonds from. Configuring it enables the underwriter |
| `--batch-underwriter-max-exposure` | none | Most the underwriter may have bonded at once in one shadow token, as an asset (`100.000000000 LIQETH`). Repeat per token; a token without one is not underwritten |
| `--batch-kick-crank` | true | Push `sysio.kicker::kick` for every kicker pool that could pay. Off, the relay's permission need not authorize `kick` |
| `--batch-kick-interval-ms` | 60000 | First wait before this operator retries a `sysio.kicker::kick` that did not pay (ms); doubles per unpaid attempt, up to an hour. Must be 1 to 3600000; startup fails otherwise |

There is no separate enable flag: the relay runs when `--batch-operator-account`
is configured, the way `producer_plugin` keys off `--producer-name`, and the
underwriter when `--batch-underwriter-account` is; a node may run either or both,
and the poll and timeout options apply to both. The plugin must also be listed under
`plugin =` (or pulled in as a dependency by `external_debugging_plugin`), and
requires `read-mode = irreversible`.

Each role signs with the one operator-configured WIRE signature provider, of any key
type, whose key alone satisfies its authorization on chain: `<operator>@active` for
the relay, the configured permission for the underwriter. The provider may hold its
key remotely, as a `KMS:` WIRE K1 key does
([signature_provider_kms_plugin](../signature_provider_kms_plugin/README.md)). At startup the node also
checks that the key's signatures recover to it and that the authorization may
declare every action the role pushes (`linkauth`). A role that fails either check,
or matches no provider or more than one, stops the node. A transient signing failure
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

### Outpost wiring

Nothing about an outpost is declared per node.

* **Which chains** — every active non-depot `sysio.chains` row.
* **Where each one lives** — the row's own `outpost` struct (`opp_addr` /
  `opp_inbound_addr`), so every operator relays a chain through the same
  deployment.
* **How to reach it** — the RPC client registered under that chain's **own
  code**. `--outpost-ethereum-client` / `--outpost-solana-client` take the
  client id as their first field, and for an outpost that id must be the chain
  code (`ETHEREUM`, `SOLANA`, ...). The Ethereum client's verified `eth_chainId`
  is additionally asserted against the row's `external_chain_id`, so a client
  registered under the wrong code is rejected rather than relayed through.

An elected group must deliver on **every** active chain, so a missing RPC client
is fatal: the node logs the chains it cannot serve and shuts down. The check runs
after the sync gate, where `sysio.chains` is readable. Missing contract
*addresses* are not fatal — they are governance state, fixable with
`sysio.chains::setoutpost` without touching a node — so such a chain is skipped
fail-closed and picked up on a later tick. A `setoutpost` redeploy is likewise
picked up on the next epoch tick: the relay job is rebuilt against the new
address rather than left pointing at the old one.

## Underwriter

Any depot account can run the underwriter, with or without the relay, to bond the
requests `sysio.synd` issues for syndication envelopes. Each poll it:

1. **Verifies each statement against its outpost.** An OPEN request states the
   outpost, the depot epoch and the digest of the envelope the depot accepted. The
   outpost keeps its own write-once record of each envelope it emitted, the keccak256
   of its canonical bytes (Ethereum `OPP.outboundEnvelopes`, Solana's outbound
   `EnvelopeLog`); read at the outpost client's settled commitment (Solana `finalized`,
   the Ethereum client's read confirmation), that record must carry the statement's digest.
   Whether the statement is true is checked against the outpost alone; amounts,
   states and windows still come from the depot's tables.
2. **Bonds** each verified OPEN request `sysio.synd` issued under the `oppenvelope`
   schema, oldest first (`sysio.bond::accept`). Each contribution is the minimum of
   the uncovered remainder, the token's remaining exposure allowance (bonds not
   yet claimed count), and the account's `sysio.liq` balance, rounded down to a
   0.01-token increment. Multiple underwriters can contribute to the same request;
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
| The outpost holds no final record for the statement's epoch | The request waits |
| The outpost recorded another digest for that epoch | Never bonded; an error log names both digests |
| The node cannot serve a chain (no RPC client, wrong chain id) | That chain's requests wait; the others go on |
| No cap, or remaining exposure allowance or balance below one 0.01-token increment | The request waits |
| An accept is submitted but not yet accounted for by the irreversible view | Its funds stay reserved and that request waits |
| Startup finality wait | New accepts wait; approvals and claims continue |
| The `sysio.andon` cord is pulled | Nothing is bonded, approved or claimed until it clears |
| A request is challenged (HELD) | Reported, whether or not we bonded it: `sysio` rules it |
| A bond of ours is ruled INVALID while the node runs, even one since claimed or pruned | Bonding stops until a restart; approvals and claims go on. A forfeited bond's principal is not returned |
| A table read fails, or a row behind one of our bonds does not decode | The pass does nothing |

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
verification is only as good as the outpost reads. A request whose record has left
the outpost can no longer be verified and waits until `sysio` rules it. Ethereum keeps
200 epochs by default, but Solana keeps only its last 10 records, so a Solana request
must be verified within about 10 epochs. Before `sysio.chains::setoutpost` replaces an
active outpost's deployment, let that chain's `sysio.synd` envelopes drain, those
still waiting for a request included: the underwriter reads only the new deployment's
records. Run one underwriter node per account, and bond from that account by no
other means: a cap counts only the bonds its node makes.

Size each cap for the most the token syndicates in one challenge window
(`window_sec`, 3 hours by default) plus the time it takes to claim.

## Dependencies

- `chain_plugin` — blockchain state access
- `cron_plugin` — irreversible block event subscription
- `signature_provider_manager_plugin` — signing key management
- `outpost_ethereum_client_plugin` — Ethereum outpost relay
- `outpost_solana_client_plugin` — Solana outpost relay

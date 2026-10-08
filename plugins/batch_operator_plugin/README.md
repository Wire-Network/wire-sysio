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
2. Deliver it to the Outpost contract, chunked (ETH: `OPPInbound.epochIn()`, SOL: `epoch_in`)
3. The outpost reaches consensus once enough group members have delivered identical bytes

**Inbound (Outposts → WIRE):**
1. The consensus-reaching delivery emits the outpost's outbound envelope
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

The two yield cranks stay idle, without logging a read failure per poll, until both
`sysio.swap` and `sysio.liq` are deployed on the depot. `sysio.synd::crank` is pushed
by the underwriter every poll (see [Underwriter](#underwriter)).

## Configuration

| Option | Default | Description |
|--------|---------|-------------|
| `--batch-operator-account` | — | WIRE account name for this operator. Configuring it enables the relay |
| `--batch-epoch-poll-ms` | 15000 | How often to check epoch state (ms) |
| `--batch-delivery-timeout-ms` | 15000 | Max time to wait for chain delivery confirmation (ms) |
| `--batch-yield-tick-interval-ms` | 60000 | Minimum spacing between this operator's `sysio.swap::tickyield` pushes per yield pool (ms) |
| `--batch-underwriter-account` | none | `account` or `account@permission` (default `active`) the underwriter bonds from. Configuring it enables the underwriter |
| `--batch-underwriter-max-exposure` | none | Most the underwriter may have bonded at once in one shadow token, as an asset (`100.000000000 LIQETH`). Repeat per token; a token without one is not underwritten |

There is no separate enable flag: the relay runs when `--batch-operator-account`
is configured, the way `producer_plugin` keys off `--producer-name`, and the
underwriter when `--batch-underwriter-account` is; a node may run either or both,
and the poll and timeout options apply to both. The plugin must also be listed under
`plugin =` (or pulled in as a dependency by `external_debugging_plugin`), and
requires `read-mode = irreversible`.

Each role signs with the one operator-configured WIRE signature provider, of any key
type, whose key alone satisfies its authorization on chain: `<operator>@active` for
the relay, the configured permission for the underwriter. At startup the node also
checks that the key's signatures recover to it and that the authorization may
declare every action the role pushes (`linkauth`). A role that fails either check,
or matches no provider or more than one, stops the node.

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
   `EnvelopeLog`); read at finality, that record must carry the statement's digest.
   Whether the statement is true is checked against the outpost alone; amounts,
   states and windows still come from the depot's tables.
2. **Bonds** each verified OPEN request `sysio.synd` issued under the `oppenvelope`
   schema, oldest first and for its whole remainder (`sysio.bond::accept`), within
   the token's exposure cap (bonds not yet claimed count) and the account's
   `sysio.liq` balance.
3. **Cranks** `sysio.synd::crank`, which releases what bonding allows and issues the
   next request: every poll while an envelope request is in play or has an outcome
   `sysio.synd` has not acknowledged, otherwise every 5 minutes.
4. **Approves** its requests once their challenge window has passed by chain time,
   and **claims** them once approved or ruled VALID, and again while yield the pool
   could not yet cover is owed. It prunes `sysio.bond` and `sysio.synd` every 10
   minutes.

What it cannot act on waits and is logged on every poll it holds; deduplicating
those lines is left to log tooling:

| Condition | Effect |
|-----------|--------|
| An outpost cannot be read | That chain's requests wait |
| The outpost holds no final record for the statement's epoch | The request waits |
| The outpost recorded another digest for that epoch | Never bonded; an error log names both digests |
| The node cannot serve a chain (no RPC client, wrong chain id) | That chain's requests wait; the others go on |
| No cap, over the cap, or too little balance | The request waits |
| The `sysio.andon` cord is pulled | Nothing is bonded, approved or claimed until it clears |
| A request is challenged (HELD) | Reported, whether or not we bonded it: `sysio` rules it |
| A bond of ours is ruled INVALID while the node runs, even one since claimed or pruned | Bonding stops until a restart; approvals and claims go on. A forfeited bond is never claimed |
| A table read fails, or a row behind one of our bonds does not decode | The pass does nothing |

### Underwriter setup

On the depot, the underwriter account holds the shadow LIQ it bonds, RAM for its
bond rows, and CPU for a few pushes per poll. With a dedicated permission, link it to
exactly the actions it pushes, so the node's key can move the inventory nowhere
else: `sysio.bond::accept`, `approve`, `claim`, `prune` and `sysio.synd::crank`,
`pruneenv`. The WIRE its bonds earn is withdrawn with `sysio.bond::claimwire`, which
stays unlinked.

The node needs an outpost RPC client for every active chain (each client spec names
a signing key, though the underwriter only reads), the Ethereum OPP ABI files (which
must declare `outboundEnvelopes`) and the Solana IDL (which must declare
`EnvelopeLog`). Use RPC endpoints independent of the batch operators': the
verification is only as good as the outpost reads. A request whose epoch has left
the outpost's record window (200 epochs on Ethereum, 128 on Solana by default) can
no longer be verified and waits. Run one underwriter node per account.

Size each cap for the most the token syndicates in one challenge window
(`window_sec`, 3 hours by default) plus the time it takes to claim.

## Dependencies

- `chain_plugin` — blockchain state access
- `cron_plugin` — irreversible block event subscription
- `signature_provider_manager_plugin` — signing key management
- `outpost_ethereum_client_plugin` — Ethereum outpost relay
- `outpost_solana_client_plugin` — Solana outpost relay

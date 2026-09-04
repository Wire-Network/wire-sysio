# Becoming a block producer on WIRE

This guide is for an operator who wants to produce blocks on WIRE. Every step below is
**self-service**: you sign each action with your own account, and nothing on this path requires a
vote, a governance action, or anyone's approval. Once you have bonded collateral and registered
your keys, the chain schedules you automatically by rank.

There are two ways onto the schedule, and this guide covers the second:

- **Genesis producers** are placed in the schedule when a chain is bootstrapped. They are
  registered as *bootstrapped* operators, which only the registry contract itself can do.
- **Collateral-backed producers** post a bond on the outpost chains and earn a schedule position
  by rank. That is the open path, and it is the one described here.

## What you need before you start

| | |
|---|---|
| A WIRE account | The account that will produce blocks. Its `active` permission signs everything below. |
| An Ethereum wallet | Funded with the required bond plus gas. |
| A Solana keypair | Funded with the required bond plus fees. |
| A machine running `nodeop` | Reachable by the peer network, with your signing keys available to it. |

You must bond on **every** chain the network requires, not just one. The requirement lives in
`sysio.opreg`'s configuration as a per-chain minimum bond, and eligibility takes the **minimum**
across all of them. Posting extra on the cheapest chain buys you nothing.

## Step 1 — Link your outpost addresses

```
sysio.authex::createlink(chain_kind, account, sig, pub_key, nonce)
```

Sign this once per chain, with your Ethereum and Solana keys respectively. The link is what makes
a deposit you send on an outpost attributable to your WIRE account; without it the chain has no way
to know the bond is yours.

The `nonce` is a millisecond timestamp and is rejected if it is more than ten minutes old, so
generate it at signing time.

## Step 2 — Register as a producer operator

```
sysio.opreg::regoperator(account, OPERATOR_TYPE_PRODUCER, is_bootstrapped = false)
```

Signed by your own account. `is_bootstrapped` must be `false`; setting it `true` requires the
registry contract's own authority and is reserved for genesis producers.

You are now registered but not yet eligible. Your status stays `UNKNOWN` until the bond arrives.

## Step 3 — Post your collateral on each outpost

Deposit on the outpost chains themselves, signed by the wallets you linked in step 1:

- **Ethereum** — `OperatorRegistry.deposit(...)`
- **Solana** — the outpost program's `deposit` instruction

Each deposit travels to WIRE over the cross-chain protocol and credits your balance in
`sysio.opreg`. When every required chain is at or above its minimum, your operator status flips to
`ACTIVE` on its own and your rank score is computed from the bond you posted.

You can top up at any time. Every balance change rescores you, so additional collateral raises your
rank as soon as it lands.

## Step 4 — Register your block-signing key

```
sysio.system::regproducer(producer, producer_key, url, location)
```

Use `regproducer2` instead if you want a multi-key block-signing authority rather than a single
key. The `url` is where you publish information about your operation, and `location` is an
advisory number used for peer topology.

## Step 5 — Register a finalizer key

```
sysio.system::regfinkey(finalizer_name, finalizer_key, proof_of_possession)
```

This is a BLS key with its proof of possession, generated with `sys-util`:

```bash
sys-util bls create key --to-console
```

Two rules matter here. The key must be **globally unique**, so you cannot reuse another producer's
key or share one across accounts you control. And the first key you register is activated
automatically; if you later register additional keys, `actfinkey` chooses which one is active and
`delfinkey` removes one.

A producer without an active finalizer key can never be scheduled, because it could not take part
in finality.

## Step 6 — Run your node

Start `nodeop` with your producer name and both keys available to it, the block-signing key and the
finalizer key. The node must be peered into the network and caught up before its first slot
arrives, or it will simply miss the round.

## What happens next

Nothing. That is the point.

Rank is **position in a score-ordered index**, derived by iteration rather than assigned by any
action. The chain rebuilds the schedule at most once every 120 blocks; at that point the highest
ranked eligible producers become the active schedule, and the finalizer policy is rebuilt to match.
If your score puts you in the top 21 you are scheduled, and you begin producing in your slot.

To hold a position at all you need three things at once: an active producer row, an `ACTIVE`
producer operator registration backed by collateral, and an active finalizer key. Missing any one
of them means no position, no pay, and no schedule slot.

## How your rank is scored

The score is a weighted sum of normalised factors, ordered within a tier. Tier always beats score,
so no amount of collateral lifts a demoted producer above a healthy one.

| Factor | What it measures |
|---|---|
| Collateral | Your bond divided by the required minimum, taken as the **minimum** across every required chain. Linear and uncapped, so more collateral always outranks less. |
| Participation | Falls with each consecutive missed round and recovers when you produce. |
| Snapshot service | Snapshot attestations that reached quorum in the current pay period. Weighted at a tenth of collateral, so it separates producers the bond has left tied rather than outranking a larger bond. |

Three further factors, relay, API and benchmark service, exist in the configuration at zero weight.
They stay at zero until the chain can observe them; a self-declared factor would only be a source
of free points.

Snapshot service is optional. If you want it, register a snapshot provider account with
`regsnapprov` and vote snapshot hashes with `votesnaphash`. Only votes that reach quorum are
credited, so registering alone earns nothing.

## Getting paid

Producers are paid **per block produced**. Each block earns the same rate, computed as the active
share of the producer pool divided by the period's slot count. A block you miss is simply not paid,
and that pay stays in the treasury rather than being handed to whoever did produce.

Positions 22 and beyond, up to a configured end rank, are **standbys**. They draw a retainer from a
separate slice of the pool, decaying linearly with position, so the network keeps a ready bench.

Nothing is ever forfeited. If you are not payable when a payout runs, whether parked, demoted, or
temporarily under-collateralized, your block count is held rather than cleared, and it is paid at
the first payout after you are payable again. Unregistering right after producing and
re-registering before your next round costs you nothing.

Claim what you have earned with `claimpay`.

## Staying in the schedule

A **round** is your entire slot window. You are charged a missed round only when the whole window
goes unproduced, so a brief hiccup that costs you a block or two is not a miss.

Miss three rounds in a row and you are **demoted**. Demotion is categorical: it moves you into a
tier that no amount of collateral climbs out of, and the next rebuild drops you from the schedule.

There are two ways back:

1. **Produce a block while you are still scheduled.** Demotion and rescheduling are separate
   events, and the schedule floor can hold that gap open. A block is proof you are alive, so it
   clears the demotion outright, with no action on your part beyond being back online.
2. **Call `regproducer` again.** This is the way back once the schedule has actually dropped you.
   It re-supplies your signing key, which makes it a real statement that you are ready rather than
   a formality. There is no cooldown and no waiting period.

One subtlety worth planning around: even a single missed round short of demotion lowers your
participation factor, and if that drops you below the last scheduled position you stop being
scheduled. The miss counter only clears by producing, so a producer displaced this way holds the
penalty until it acts. Calling `regproducer` clears it, and so does posting enough additional
collateral to outrank the producer that displaced you.

## Leaving, voluntarily or otherwise

- **Park** with `unregprod`. Your bond is untouched and your operator status stays `ACTIVE`; you
  simply hold no schedule position. `regproducer` brings you back at the position your collateral
  earns.
- **Withdraw** with `sysio.opreg::withdraw`, which queues the request; `cancelwtdw` cancels it
  before it flushes. Once your balance falls below the minimum on any required chain you leave
  `ACTIVE` and the schedule drops you at the next rebuild.
- **Slashing** is punitive and permanent. A slashed operator's row is never pruned and the registry
  refuses to re-register it, so a slashed account cannot come back.

## A note on schedule size

The chain will not publish a schedule smaller than its safety floor. If demotions or withdrawals
would leave too few eligible producers, it keeps the last good schedule rather than concentrate
block production and finality onto too few nodes. During such a window a demoted producer may keep
its slot, which is exactly the case the "produce a block to recover" rule above exists for.

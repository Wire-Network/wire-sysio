# Integrating with `sysio.bond`

`sysio.bond` underwrites statements on the depot. An issuer registers a statement and the amount of a
depot-native token that must stand behind it. Underwriters bond that amount. A challenge window runs,
and the request is either approved or ruled by `sysio`. This guide is for a contract, or an account,
that issues requests: what it calls, how it learns what happened, and what it is paid. The contract is
`contracts/sysio.bond`; the specification is §3 of the syndication underwriting spec.

## Registering a request

`request(issuer, schema, statement, token_code, covered, bounty, window_sec)`, signed by the issuer.

- `schema` names how `statement` is to be read. `statement` is at most 1024 bytes. The issuer can
  register a given schema and statement once.
- `token_code` is WIRE or a shadow LIQ symbol registered on `sysio.liq`. Other tokens are refused.
- `covered` is what the underwriters must bond in total. It is a positive multiple of the increment.
- `bounty`, which may be zero, is pulled from the issuer into escrow. `addbounty` raises it later.
- `window_sec` is the challenge window. It starts when the request is fully bonded.

The contract is privileged: it pulls the issuer's tokens by an inline `transfer` under the issuer's
`active` authority. An issuer contract needs nothing extra.

An issuer contract that sends `request` inline, where an abort would take its own action down, can
check beforehand everything `request` checks, with the helpers `sysio.bond.hpp` exports:
`bond::increment_of(symbol)` for the increment, `bond::statement_digest_of(schema, statement)` and
`bond::statement_key_of(issuer, digest)` for the key its `bystatement` index refuses a duplicate on.
What a ruling awards an issuer can be computed the same way: `bond::hold_bond_of(covered, hold_bps)` is
the hold bond `hold` pulls (at `bondconfig.hold_bps`), `bond::share_of(amount, pot, covered)` the share
of a bounty or hold bond a stake earns, and `bond::escrow_key_of(request_id, kind)` the key of the
`escrows` row whose `payee` and `payout` record an award.
The id the request will take is `bondcounters.next_request_id`, read before sending; requests sent
from one action take consecutive ids from there. An issuer that must not depend on that ordering finds
its request afterwards through the `bystatement` key instead, as the syndication contract's queue does.

The issuer pays the RAM of its request's `requests` and `escrows` rows, and each underwriter the RAM
of its `bonds` row. A privileged issuer or underwriter is the exception: a system contract (the
syndication contract, issuing for its envelopes) has no RAM quota of its own, so the rows created on
its behalf bill the `sysio` RAM pool instead. Later writes by other signers (a ruling, a claim) keep
those payers. `prune` releases the RAM.

## The increment

Bonds move in steps of 0.01 token. For a token of precision `p` that is `10^(p - 2)` base units. A
token with a precision below 2 is refused. `covered` and every `accept` amount must be positive
multiples of it. An `accept` larger than what remains uncovered is reduced to the remainder, and only
the reduced amount is pulled.

## Following a request

`sysio.bond` notifies no one and sends nothing when a request changes state: approval and the
rulings only change the `requests` row and record what each party may claim. An account that could
refuse a notification or a transfer (a contract whose handler fails) can therefore block only its
own claim, never an approval, a ruling or another party's payout.

An issuer polls its `requests` rows (the syndication contract's crank does) and acts on `state`:
on INVALID it pulls its forfeit with `claim(request_id, issuer)`; on VALID after a hold of a request
that was only partly bonded it pulls the unbonded share of the hold bond the same way (a fully bonded
request's hold bond goes wholly to its underwriters, and the issuer is owed none of it). `prune` keeps
a ruled request for seven days (`PRUNE_RETENTION_SEC`), so an issuer that polls at least that often
sees every ruling. Anyone may send that `claim`, so an issuer accounts for what it is owed from
the row (on INVALID the forfeit is `bonded`), not from the transfer, and sends `claim` only while
`forfeit_pending` or an escrow's `payout` to it is not zero, since a claim with nothing owed is refused.
The syndication contract's queue does this.

## States

| State | Entered when | Leaves to |
|---|---|---|
| OPEN | `request` | BONDED, HELD, VALID, INVALID |
| BONDED | bonded reaches covered; the window starts | APPROVED, HELD, VALID, INVALID |
| APPROVED | `approve` after the window, with no hold | terminal |
| HELD | `hold(request_id, beneficiary)` by the issuer; posts the hold bond | VALID, INVALID |
| VALID | `rslvvalid` by `sysio` | terminal |
| INVALID | `rslvinvalid` by `sysio` | terminal |

The hold bond is `covered * hold_bps / 10000` (`hold_bps` is 1000 until `sysio` sets another). When
that rounds to zero, the request is held with no hold bond.

## Who is paid what

`s` is an underwriter's bond divided by `covered`. A share is `amount * pot / covered`, computed in
128 bits and floored; division remainders stay in the contract.

| Outcome | Bond | Bounty | Hold bond |
|---|---|---|---|
| APPROVED | returned | `s` of it to each underwriter | none |
| VALID, fully bonded | returned | `s` of it to each underwriter | `s` of it to each underwriter |
| VALID, partly bonded | returned | `s` to each underwriter; the unbonded share to `sysio` | `s` to each underwriter; the unbonded share to the issuer |
| VALID, nothing bonded | none | all to `sysio` | all to the issuer |
| INVALID after a hold | to the issuer | to the hold beneficiary | to the hold beneficiary |
| INVALID with no hold | to the issuer | returned to the issuer | none |

Every payment is pulled by `claim(request_id, account)`, one account per call, so no action loops
over the underwriters and no party's refusal blocks another. Anyone may call it; the payment goes to
`account`. A claim with nothing owed is refused with `nothing to claim`.

- **Underwriters** settle the bond and its shares with `claim`, then withdraw earned WIRE with `claimwire`.
- **The issuer** of an INVALID request claims the forfeit, in its own transfer (below), and the
  bounty back when there was no hold. On VALID after a hold of a partly bonded request it claims the
  unbonded share of the hold bond; the hold bond of a fully bonded request is all the underwriters'.
- **The hold beneficiary** of an INVALID request that was held claims the hold bond and the bounty.
- **`sysio`** claims the unbonded share of the bounty of a VALID request.

The rulings record these awards: the forfeit as `forfeit_pending` on the `requests` row, and the
rest as `payee` and `payout` on the `escrows` row of the bounty or the hold bond. Each drops to 0
when claimed.

## The emergency stop

While the depot's `sysio.andon` cord is pulled, `claim` and `claimwire` are refused (`the andon cord is pulled: funds
cannot leave custody`): every payout waits for the clear. Everything else runs -- `request`,
`addbounty`, `accept`, `hold`, `approve`, the rulings, `sweepyield` and `prune` -- and every transfer
into `sysio.bond` (a bounty, a bond, a hold bond) succeeds, so bonds can be placed and requests held
during a freeze. Nothing a ruling awards is lost: it stays recorded (`forfeit_pending`, `payout`, the
unpaid bond rows) until claimed, and `prune` never erases a request that still owes a claim. An issuer
contract that claims from a path that must not throw reads the cord first with
`andon::pulled(andon::ANDON_ACCOUNT)` (`sysio.andon.hpp`) and waits while it is pulled, as the
syndication contract does.

## The forfeit payment

The issuer's `claim` on an INVALID request pays its bonded forfeit through a LIQ ledger settlement,
or through an ordinary transfer with memo `sysio.bond::forfeit` for WIRE-denominated requests.
Clients read the request's recorded outcome; LIQ invokes no incoming-transfer handler.

What a client does with a forfeit depends on the token:

- a shadow LIQ symbol can be burned through `sysio.liq`: the syndication contract's burn (Part B of
  the syndication underwriting spec), which the syndication contract owns;
- WIRE cannot be burned by a client: `sysio.token::retire` needs the token's issuer, `sysio`. A
  client keeps forfeited WIRE or forwards it.

## Yield

Shadow LIQ held by `sysio.bond` earns WIRE on `sysio.liq`. Each bond and escrow row carries a custody
position (`docs/shadow-custody-integration.md`). `sweepyield(token_code)` pulls what the contract is
owed; `claim` also pulls before it pays.

- A returned bond is held until it is claimed, and earns until then: its position settles at the
  live index at the claim, and its WIRE is banked for the underwriter to withdraw with `claimwire`.
- A forfeited bond becomes the issuer's at `rslvinvalid`, which records the index in
  `resolved_index`; its position settles there. What the forfeit earns while it waits for the
  issuer's claim is slack. The bond's WIRE never goes to the underwriter: claiming it pays the WIRE
  into `sysio.liq` as bonus yield of the bond's symbol, through `addyield`. When the symbol has no
  supply left, `sysio.liq` cannot distribute, and the WIRE stays in `sysio.bond`.
- An escrow earns on what it still holds, including the shares and awards not claimed yet, and its
  WIRE goes to its funder, the issuer. The issuer may claim more than once. Its claim marks the
  escrow paid once no bond is owed a share of it and its award is claimed; the division remainder
  left in it is slack.
- A request in WIRE earns nothing.

## Pruning

`prune(from_id, limit)` walks the requests from id `from_id` up and acts on at most `limit` terminal
requests. It erases a request with its rows once they are all paid or owe nothing -- the forfeit and
every award claimed, with no residual owed yield -- and its ruling is at least `PRUNE_RETENTION_SEC`
(seven days) old. Durable requests also require issuer acknowledgement. Paid bond rows with no
residual yield may be erased independently; an unrelated unclaimed payout does not hold their RAM. A request whose token no longer resolves is skipped.
Anyone may call it.

## Durable outcome consumption

Polling contract issuers should use `requestkeep` (same arguments as `request`) and call `ack(request_id)`
after storing a terminal result. Only the issuer may acknowledge; acknowledgement is idempotent and
requires a terminal request. Rulings never notify the issuer, and claims do not require acknowledgement.
`prune` requires acknowledgement in addition to its existing retention and fully-paid checks for these
requests. Ordinary `request` retains its existing retention behavior.

The public `find_request`, `request_refusal`, and `settlement_due` accessors provide stable statement
lookup, shared admission rules, and outstanding settlement principal without client knowledge of counters
or escrow representation. Statement deduplication lasts while the request exists; durable delivery does
not add a permanent replay archive after acknowledged requests are pruned. Issuers must enforce their
own replay policy (syndication retains its envelope/sequence guards).

The durable request flag and syndication's independent cursors change table layouts. A deployment with
existing rows needs a coordinated table migration; fresh prelaunch state needs none. This change does
not provide a migration action or recover outcomes already pruned under the old contract.

## Ledger settlement and independent WIRE claims

LIQ principal, awards and forfeits use `sysio.liq::settle(custodian, beneficiary, quantity)`.
Only `sysio.synd` and `sysio.bond` can debit their own existing LIQ through this action.
It settles both yield positions, leaves supply unchanged, invokes neither account, and requires
Andon clear even for a custody recipient or self-settlement. Ordinary token transfers keep their
notifications and existing behavior.

`claim(request_id, account)` credits LIQ immediately and records earned WIRE in the account's
backed `wireclaims` balance. `claimwire(account)` withdraws that WIRE separately to the named
account. It is permissionless and respects Andon; a failed recipient transfer rolls back only the
WIRE withdrawal. The WIRE balance survives request pruning and never expires. Checked additions
reject overflow atomically. Yield the pool cannot cover remains on the original bond or escrow row;
that residual prevents pruning and can be retried without paying principal twice or earning yield
on returned principal. WIRE-denominated requests retain their ordinary, per-recipient claims.

Clients must read the recorded outcome and balances rather than infer a LIQ forfeit from a transfer
memo. LIQ settlement has no memo or transfer notification.

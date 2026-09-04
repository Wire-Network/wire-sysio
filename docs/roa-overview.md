# Resource Owner Allocation (ROA)

How Wire pays for CPU, NET, and RAM — and why end users don't.

---

## The short version

**A new account on Wire costs its owner nothing and can immediately use every provisioned
contract on the network.** It holds no CPU or NET allocation and needs none for ordinary
contract-paid calls — because the applications it calls pay instead of it.

That is the whole model in one line. The rest of this page is how it works and what it costs the
people who do pay.

**Applications pay, users don't.** When you sign an ordinary transaction, the CPU time and network
bandwidth it consumes are billed to the *contract each declared action calls*. Sending tokens,
swapping on a DEX, minting an NFT: the signing account is charged nothing and needs no allocation,
stake, or rental.

**Applications get paid for by node owners.** A contract cannot conjure its own capacity. It
receives a **policy** — a grant of CPU, NET, and RAM weight — from a **node owner**, who holds a
fixed share of the network's capacity determined by their tier and issues slices of it through the
`sysio.roa` contract. Only registered node owners can issue policies, and only out of their own
tier budget — so an ordinary account cannot grant itself bandwidth. (Privileged system paths
sit outside this: `setalimits` and the fixed account gift can set limits without a policy.) A node
owner may name itself as its own policy's recipient; registration does that for tier 1 only.

**Under default billing, a contract without a policy does not run when called directly.** Because
the contract is the payer for actions declared on it, an unprovisioned one has nothing to pay with
and those calls fail. Provisioning the contract — not the user — is what makes an application
usable.

For those calls there is one exception to who pays, covered in
[Who pays](#who-pays-the-payer-model): an account can volunteer to pay for itself, which takes the
contract's own CPU and NET out of the picture. It is opt-in, it requires signatures, and it is not
how ordinary traffic works.

---

## How this compares to Antelope-family chains

Antelope-family chains bill the signer, and over time offered three ways to fund that signer's
CPU and NET. Not every chain ran all three, and they arrived in sequence rather than as a set — so
what you have used depends on which chain and which era.

**Staking (2018).** Tokens locked with `delegatebw` bought a proportional share of CPU and NET, and
RAM was bought outright from a Bancor-curve market with `buyram`. `delegatebw` takes separate
`from` and `receiver` arguments, so a third party could stake on a user's behalf without that user
holding any token — but somebody still had to hold and lock tokens per user, keep track of the
`undelegatebw` timing to get them back, and understand three resource types to do it. A dApp
onboarding new users therefore built a funding or custodial layer of its own. RAM price moved with
the market, so contract deployment cost moved with it.

**REX (2019).** A lending market let token holders rent out their staked CPU/NET for yield instead
of leaving it idle. It improved capital efficiency and added a third system to understand alongside
staking and the RAM market.

**PowerUp (2021).** Replaced REX rentals with an expiring rental priced off a utilization curve.
Simpler to use — you paid for a fixed term of CPU/NET — and, like `delegatebw`, it separates the
fee `payer` from the resource `receiver`, so the user need not hold the token. The term is
configuration (`powerup_days`, 30 in the reference contract, and each order must match it), not
inherently daily. It is not stateless either: the system contract keeps a `powup_state` singleton
whose utilization drives the price, plus an order table that has to be expired as rentals lapse.
Somebody still pays per user, per term, at a price that moves with the curve.

In all three, the account that signs is the account that is *billed* — funding it is a separate
problem each of them leaves to the application.

On Wire the signer is billed only if it asks to be. By default the contract is, and it is
provisioned once by a node owner issuing a policy, after which every account that calls it
transacts without charge. A signer that names itself with `sysio.payer` takes the bill instead —
the exception, not the ordinary path.

| | EOS staking | REX | PowerUp | **Wire ROA** |
|---|---|---|---|---|
| Who is billed for a transaction | Signer | Signer | Signer | **Each declared action's contract, unless that action names an explicit payer** |
| End user needs native token | No — but someone does, per user | No — but someone does, per user | No — but someone does, per user | **No — the contract's one policy covers every caller** |
| CPU/NET acquired by | Locking tokens | Renting from a pool | Term fee on a curve | **A node owner's policy** |
| RAM acquired by | Bancor market purchase | same | same | **A node owner's policy** |
| Acquisition price set by | RAM market | Rental market | Utilization curve | **Off-chain, between issuer and recipient** ([what the weight then buys is on-chain](#how-weight-becomes-throughput)) |
| Reclaimable by | Unstake (3d) | Sell rex | Expires at term | **`reducepolicy` at or after `time_block`** |
| Cost to onboard a user | Per-user stake + RAM, held or funded by someone | same | Per-user recurring fee | **Nothing per user — the contract is already provisioned** |

---

## The three resources

Wire meters the same three resources Antelope does.

**CPU** — execution time, in microseconds. Metered per action the transaction declares and billed to
that action's payer; an inline action is timed inside the declared action that triggered it.

**NET** — transaction size on the wire, in bytes. Billed per declared action based on its serialized
billable size; inline actions add none.

**RAM** — persistent state: account rows, permissions, contract code, and every table row a
contract writes. Measured in bytes. Unlike CPU and NET, RAM is not a rate — it is an occupancy
level. It is consumed when state is written and released when state is deleted, or replaced by a
smaller value — `setcode` and `setabi` apply a signed `new_size - old_size` delta.

CPU and NET replenish continuously over an averaging window. RAM does not replenish; it is freed
only by deleting or shrinking the data holding it.

---

## Who pays: the payer model

This is the part that differs most from Antelope, and it is worth being precise about.

**The default: the called contract pays.** An ordinary transaction names no payer at all, so the
account billed for each action the transaction declares is the contract that action invokes.
Nothing in the transaction has to say so and no permission has to be added — this is what happens
when you do nothing special, which is the case for essentially all user traffic.

**The exception: an account can volunteer to pay for itself.** Wire reserves a permission name,
`sysio.payer`, for this. It is not a way to bill a stranger, and adding it is not a way to obtain
resources. On a regular action the protocol requires all three of the following together:

- the `sysio.payer` entry sits at **index 0** of the action's authorizations,
- the **same actor** also appears on that action under a real permission, and
- the transaction carries **signatures** satisfying that paired declared authorization.

The paired permission is not required to be `active`. `authorization_manager` looks for any entry
whose actor matches the payer and whose permission is not `sysio.payer` itself, then satisfies it
like any other declared authority — so `owner`, or a custom permission linked to that action, works
as well:

```cpp
// libraries/chain/authorization_manager.cpp — checking the explicit payer is paired
if (auth.actor == payer && auth.permission != config::sysio_payer_name) {
   foundPayer = true;   // any real permission of the payer, not `active` specifically
   break;
}
```

You can only volunteer yourself, or someone who co-signs. An account that names itself payer needs
its own allocation and fails without one.

Those two rules are the whole of `action::payer()`:

```cpp
// libraries/chain/action.cpp
account_name action::payer() const {
   if (!authorization.empty() && authorization[0].permission == config::sysio_payer_name)
      return authorization[0].actor;   // the exception: index 0 named a payer
   return account;                     // the default: the contract being called
}
```

```mermaid
flowchart TD
    A["Action arrives"] --> B{"authorization[0].permission<br/>== sysio.payer ?"}
    B -->|"No — ordinary traffic"| C["Payer = the contract being called"]
    B -->|"Yes — opt-in"| D{"Same actor also present<br/>with a real permission,<br/>and signed for?"}
    D -->|"No"| E["Rejected:<br/>unsatisfied authorization"]
    D -->|"Yes"| F["Payer = that actor"]
    C --> G["Contract needs a policy for<br/>actions declared on it.<br/>Signer not billed."]
    F --> H["Declared action: actor billed CPU and NET,<br/>contract not. Either way the marker<br/>authorizes RAM billed to the actor."]
```

The objective billing map is populated only from the actions a transaction *declares*, and is keyed
on `payer()` — so two declared actions sharing a payer aggregate into one entry. An authorizing
account that is not the payer never enters it, so consensus neither charges nor limits its CPU and
NET. Nor does a contract reached only inline: its work is timed inside the declared action that
triggered it and billed to that action's payer, so it needs no CPU or NET allocation for that work.
A producer running [subjective billing](#subjective-billing-meters-the-signer) does meter the
signer, but that is node-local rather than consensus.

RAM is separate: it is checked against the authorizations on the action *executing*, so a callee
that bills state to itself still needs the capacity — and an inline action carrying the marker for a
user who has granted the contract `sysio.code` bills that user's RAM while its CPU and NET stay with
the declared action's payer.

| Action authorizations | Payer | Notes |
|---|---|---|
| `{alice, active}` | `sysio.token` (the contract) | The default. Alice pays nothing. |
| `{alice, sysio.payer}, {alice, active}` | `alice` | Explicit self-pay. Alice needs her own allocation. |
| `{alice, sysio.payer}, {alice, active}, {bob, active}` | `alice` | Alice covers the whole action's cost. |

### Porting a contract from Antelope

On Antelope your contract works as soon as it is deployed, because the users calling it arrive with
their own staked or rented CPU and NET. On Wire they do not — every user account holds zero. Deploy
a ported contract, push a transaction the way you always have, and the call fails:

```
account <yourcontract> net usage is too high: 132 > 0
```

Nothing is wrong with the contract, the transaction, or the signer — the contract is the payer and
has no policy. It will look dead until a node owner issues it one.

The fix is not to ask users to acquire resources. It is a single `addpolicy` on the contract
account, and for a contract that bills its own state that is the whole of it — the same unmodified
transaction then succeeds, and every user transacts for free.

**One porting change is not optional, though.** A great many Antelope contracts name an
authorizing user as the RAM payer for rows they create — the `{user, "active"_n}` idiom, with
`user` passed to `emplace`. Wire rejects that transaction whatever policy the contract holds:
`validate_account_ram_deltas` requires a payer other than the receiver to carry the `sysio.payer`
permission on the action, and `active` does not satisfy it:

```
Requested payer alice did not authorize payment. Missing sysio.payer.
```

Two ways through, and the choice is a product decision:

- **Bill the rows to the contract** (`emplace(get_self(), ...)`). The contract's policy covers the
  storage, users stay free, and the caller's transaction is unchanged. This is the gasless path,
  and what most ports want.
- **Keep billing the user**, which requires the client to add `sysio.payer` at index 0 — making
  the user the CPU and NET payer too, so they need their own allocation.

A policy makes an unprovisioned contract callable; it does not make every Antelope contract
portable unchanged. Check where the contract bills its RAM first.

### Why the contract can afford it

The executable system contracts — `sysio.token`, `sysio.msig`, `sysio.wrap` and the rest — carry
unlimited CPU and NET, so their calls are covered by the system itself. That is established, not
inherited: native `newaccount` writes 0/0/0 and transfers the fixed RAM gift, then `setsyscode` →
`giftram` sets CPU and NET to `-1` while funding the code RAM. `sysio.roa` preserves it on any
later quota sync by passing `-1, -1` for a `sysio.`-prefixed owner. The prefix alone guarantees
nothing — `activateroa` deliberately creates
`sysio.acct` at **zero** CPU and NET, a pure RAM bucket for account creation.

> **What `-1` means here.** It is a **sentinel** meaning "no limit," not a quantity. The chain
> tests for it explicitly (`is_unlimited_cpu` returns `cpu_weight == -1`) and substitutes a large
> finite number where arithmetic is needed. Seeing `-1` in a `get_account` response is not an
> overflow.

A third-party contract is different. It is an ordinary account, and it is the payer for every action
a transaction declares on it without naming one explicitly — which is every ordinary call — so it
needs a real allocation. That is exactly what a ROA policy provides.

### What happens with no policy at all

An account created on Wire starts at **zero CPU, zero NET**, and 1,144 bytes of RAM — just enough
for its own account and permission rows:

```cpp
// contracts/sysio.system/src/sysio.system.cpp — native::newaccount
set_resource_limits( new_account_name, 0, 0, 0 );
transfer_ram( get_self(), new_account_name, sysiosystem::newaccount_ram );  // 1144 bytes
```

For a **user** account, consensus never consults those zeros. It signs, the contract pays, the
transaction succeeds.

For a **contract** account, those zeros are fatal under default billing. The contract is the payer,
so a transaction that declares an action on it with no policy fails outright:

```
account payloadless net usage is too high: 132 > 0
```

The transaction throws. Nothing is charged *objectively* — but this failure lands after
authorization, so the signer accrues subjective CPU and a failure count against the limiter.

That holds only while the contract is the payer. Because the billing map is keyed on `payer()` and
nothing else, an action carrying `{caller, sysio.payer}` puts the caller in the map and leaves the
contract out of it entirely — the contract's zero CPU and NET are never consulted. A provisioned
caller or relayer can therefore drive an otherwise unprovisioned contract, so long as the caller
has the capacity and any RAM the contract bills to *itself* is covered.

---

## Policies

A policy is a grant of resource weight from a node owner (the **issuer**) to an account (the
**owner**), recorded in `sysio.roa`.

| Field | Meaning |
|---|---|
| `net_weight` | SYS-denominated weight granted for NET |
| `cpu_weight` | SYS-denominated weight granted for CPU |
| `ram_weight` | SYS-denominated weight granted for RAM |
| `bytes_per_unit` | Bytes per 0.0001 SYS, **frozen at the moment the policy was created** |
| `time_block` | Block height before which the policy cannot be reduced or reclaimed |

RAM weight converts to bytes at `bytes_per_unit`. At the network's launch price of 104 bytes per
0.0001 SYS, **1 SYS of `ram_weight` ≈ 1.04 MB**.

CPU and NET weight do not convert to a fixed quantity — they buy a proportional share of network
throughput. See [How weight becomes throughput](#how-weight-becomes-throughput).

### Who can issue one

Any registered node owner, of any tier. The check is membership, not rank:

```cpp
require_auth(issuer);
check(nodeowners.contains(node_key), "Only Node Owners can issue policies for this generation.");
```

There is no tier gate on `addpolicy`, `expandpolicy`, `extendpolicy`, or `reducepolicy`. A tier-3
node owner has exactly the same policy powers as a tier-1 node owner. What differs is budget size.

### Node owner tiers

| Tier | Share of `total_sys` per owner | Max owners | Aggregate share when full |
|---|---|---|---|
| 1 | 4% | 21 | 84.0% |
| 2 | 0.15% | 84 | 12.6% |
| 3 | 0.003% | 1,000 | 3.0% |

Registration consumes part of an owner's own budget — 10% of the tier allocation set aside into
the network RAM pool, plus a flat personal policy for the owner's own account: 0.0080 SYS of RAM
and 0.0500 SYS each of NET and CPU, 0.1080 SYS in total — **tier 1 only**. Tiers 2 and 3 receive
no personal policy: managing policies costs an owner nothing, so they need no allocation of their
own and keep the whole remainder issuable. Using the launch configuration of 75,496 SYS
`total_sys`:

| Tier | Total allocation | Free to issue after registration | ≈ RAM if spent entirely on RAM |
|---|---|---|---|
| 1 | 3,019.8400 SYS | ~2,718 SYS | ~2.8 GB |
| 2 | 113.2440 SYS | ~102 SYS | ~106 MB |
| 3 | 2.2649 SYS | ~2.04 SYS | ~2.1 MB |

Every `addpolicy` and `expandpolicy` checks `total_new_allocation <= node.total_sys -
node.allocated_sys`. A node owner cannot issue more than they hold.

### What registration provisions

`regnodeowner` spends part of the tier allocation before the owner has issued anything:

| Component | Amount | Scales with tier |
|---|---|---|
| `sysio` RAM pool grant | 10% of the tier allocation | Yes — every tier |
| Personal RAM | 0.0080 SYS (8,320 bytes) | Tier 1 only |
| Personal NET | 0.0500 SYS | Tier 1 only |
| Personal CPU | 0.0500 SYS | Tier 1 only |

The 10% grant is not for the owner. It moves bytes into `sysio`'s RAM pool, which funds the
1,144-byte gift every new account on the network receives. It is written with
`time_block = UINT32_MAX` and is never reclaimable.

A tier-1 owner's three personal components land in a self-issued policy — `issuer == owner` —
carrying `time_block = 1`, so it can reshape or reclaim them immediately with `expandpolicy` or
`reducepolicy`. Tier 1 gets them because it is the only tier that can call `newuser`, whose
`sponsors` and `sponsorcount` rows are the only writes in the contract billed to a node owner. The
10% grant is not tier-gated.

### What a node owner needs to operate

Almost nothing. Managing policies costs a node owner no resources at all:

- `addpolicy`, `expandpolicy`, `extendpolicy`, and `reducepolicy` are actions on `sysio.roa`, so
  `payer()` resolves to `sysio.roa`, which carries unlimited CPU and NET.
- Every row the contract writes for registration and policy management is billed to `sysio.roa`
  as well — `policies`, `reslimit`, and `nodeowners` rows are all `emplace(get_self(), …)`.

An owner reduced to zero CPU, zero NET, and zero spare RAM can still issue a policy. Membership —
the `nodeowners` row — is what confers the ability to issue, not any allocation the owner holds.

The one exception is tier-1's `newuser`, which bills its `sponsors` and `sponsorcount` rows to
`creator`. Those are the only two writes in the contract charged to a node owner, so a tier-1
owner needs RAM headroom before its first `newuser` call.

### What differs between tiers

| | Tier 1 | Tier 2 | Tier 3 |
|---|---|---|---|
| Issue, expand, extend, reduce policies | Yes | Yes | Yes |
| Budget per owner | 3,019.8400 SYS | 113.2440 SYS | 2.2649 SYS |
| Max owners | 21 | 84 | 1,000 |
| `newuser` (sponsored accounts) | Yes | No | No |

Policy mechanics are identical across tiers — there is no tier check in any of the four policy
actions. `newuser` is the only tier-gated capability, guarded by
`check(node.tier == 1, "Creator is not a registered tier-1 node owner")`.

### Where network RAM comes from

`activateroa` splits the SYS left over after all tier allocations between two pools:

| Pool | Size | Funds |
|---|---|---|
| `sysio.roa` | ~157 MB at activation | The contract's own rows: policies, reslimits, node-owner records |
| `sysio` | ~157 MB at activation, ~7.98 GB once every node owner has registered | The 1,144-byte gift every new account receives |

`sysio`'s pool grows as owners register, because each registration deposits 10% of its tier
allocation into it. At 1,144 bytes per account, the funded pool supports roughly 6.97 million
accounts.

The gap between those two pools is why `newuser` bills its sponsorship rows to the sponsoring
tier-1 owner rather than to the contract. Billed to the contract, `sysio.roa`'s ~157 MB would cap
sponsorship near **1.09 million** users — well short of what `sysio`'s account pool can create. A
tier-1 owner's own free budget of ~2.83 GB is the only one that scales with how many users it
actually onboards.

A sponsorship costs **144 bytes** — a 16-byte key and 16-byte value over
`billable_size_v<kv_object>` of 112 — plus a one-time 144-byte `sponsorcount` row on a creator's
first `newuser`. Neither pool is a ceiling: `addpolicy` refuses CPU and NET to a `sysio.`-prefixed
account but not RAM, so a node owner can extend either with a RAM-only policy. The ~157 MB is
where `activateroa` starts them.

### The four policy actions

**`addpolicy`** — create a policy. Fails if this issuer already has one for this owner (use
`expandpolicy`). Weights must be in the core SYS symbol, non-negative, and at least one non-zero.

**`expandpolicy`** — add weight to an existing policy. Converts RAM at the *policy's* frozen
`bytes_per_unit`, not the current network price.

**`extendpolicy`** — push `time_block` further out. It can only move forward, never back, and never
to a block already in the past. A policy's term can be lengthened but not shortened.

**`reducepolicy`** — take weight back. Callable at or after `time_block`. Each weight is
capped at the stored policy weight, so an issuer can never withdraw more than they granted.

### Stacking policies from multiple issuers

The policy table is scoped per issuer, so the duplicate check is per issuer only. **An account can
hold policies from many node owners at once, and they sum into a single quota.**

Two node owners each granting `10.0000 SYS` of CPU and NET to the same account produce a combined
on-chain limit of `200000` weight units. Reduce one issuer's policy fully and the account drops to
`100000` — the other issuer's grant is untouched, and they can still expand it independently:

```
addpolicy    owner_A -> acct  10.0000 SYS cpu/net
addpolicy    owner_B -> acct  10.0000 SYS cpu/net
                              net = 200000   cpu = 200000

reducepolicy owner_A (full)   net = 100000   cpu = 100000    # B untouched
expandpolicy owner_B +5 SYS   net = 150000   cpu = 150000
```

This is how a contract gets provisioned by more than one sponsor: several node owners co-sponsor
the account, each on their own terms, each able to reduce its own policy without touching another
issuer's row.

Four things to know when stacking:

- **Each policy has its own `time_block`.** The lock is per issuer. An account's total capacity is
  only as stable as its shortest-committed policy.
- **Each policy freezes its own byte price.** After a network-wide `setbyteprice`, two policies with
  identical `ram_weight` can represent different byte counts.
- **RAM reclaim competes, and an exit is not guaranteed.** CPU and NET always unwind in full —
  they are rate limits with nothing to hand back. RAM can be *occupied*: `reducepolicy` reclaims
  `min(unused RAM on the whole account, requested)`, floored to a whole `bytes_per_unit`, and
  decrements the policy row and the issuer's `allocated_sys` by what was **reclaimed**, not
  requested. Grant spent on live state therefore stays counted against the issuer's budget until
  that state is deleted or shrunk, and the row is erased only once all three weights reach zero. Because the
  cap is the *account's* unused RAM rather than this policy's share, one issuer's grant can be
  pinned by state another issuer's grant paid for.
- **The ceiling is network-wide unallocated SYS,** not a per-account cap. There is no limit on how
  many policies one account may hold.

---

## How weight becomes throughput

CPU and NET use the same proportional-share formula Antelope uses. An account's share of the
network's virtual capacity over the averaging window is:

```
max_use_in_window = virtual_capacity_in_window × your_weight / total_weight_across_all_accounts
```

The denominator is `resource_limits_state`'s `total_cpu_weight` (or `total_net_weight`): the sum
of every account's positive weight, whatever set it. Accounts with unlimited (`-1`) limits
contribute zero. In normal production every positive weight comes from a ROA policy, so the two
are the same number — but privileged `setalimits` / `setacctcpu` / `setacctnet` can add weight
outside ROA, and bootstrap does.

The mechanism is unchanged from Antelope. **Only the source of the numerator changed.** In
Antelope the weight came from tokens staked with `delegatebw`. On Wire it comes from a policy a
node owner issued. Nothing about an account's token balance affects its resource share.

The practical consequence is the same as on Antelope: throughput is a share of a moving total. If
the network's total allocated weight grows and an account's does not, its slice shrinks. The only
remedy is more weight — `expandpolicy` from the existing issuer, or a policy from an additional
node owner.

`setbyteprice` does not help here. It rewrites `roastate.bytes_per_unit`, which governs how RAM
weight converts to bytes for policies struck afterwards, and touches neither CPU/NET weights nor
the total that divides them. That is also why each policy records the price it was struck at: so
its RAM conversion stays fixed when the network price later moves.

---

## Spam control

If the contract pays, what stops someone calling it in a loop for free? Two things, at two
different layers.

### The contract's own share is a hard cap

CPU and NET are rate limits over a window, and the payer's limit is the one that applies. Spam
aimed at a contract consumes that contract's own account quota, not another account's rolling
share — though every success it lands also draws on block-wide capacity. Once exhausted, further
calls fail with `tx_cpu_usage_exceeded` or `tx_net_usage_exceeded` naming the contract, and keep
failing until the window rolls forward.

That is the structural answer, and it bounds the *quota* damage: the spammer draws on the payer
account's own provisioned share — the sum of every policy stacked on it — never another account's.
Each issuer's exposure stays bounded by its own grant, reclaimable via `reducepolicy` at or after
`time_block`.

**It does not make the blast radius zero.** Every transaction that succeeds is billed twice over —
once to the payer's rolling window, and once to the block:

```cpp
// libraries/chain/resource_limits.cpp — add_transaction_usage
// account for this transaction in the block and do not exceed those limits either
rls.pending_cpu_usage += total_cpu_usage;
rls.pending_net_usage += total_net_usage;
```

`pending_*_usage` is what `get_block_cpu_limit()` / `get_block_net_limit()` subtract from to size
the *next* transaction's limits, so a heavily provisioned contract being hammered inside its own
quota still occupies block capacity and can delay unrelated traffic. What the quota guarantees is
that the spam stops when that contract's share runs out — not that nobody else notices while it
lasts. Bounding the shared-capacity effect is the second layer's job.

This is objective, part of consensus, and applies on every node with no configuration.

### Subjective billing meters the signer

The objective rules never look at the signer. `nodeop` can, and that is where a producer
distinguishes a spammer from a legitimate user of the same contract.

Before executing, `transaction_context::verify_init_subjective_billing` takes the first
authorizers that are **not** payers and checks each one:

```
available = subjective-account-cpu-allowed-us
          + the account's own objective CPU limit
          − its accumulated subjective bill
```

For an ordinary account with no policy the objective limit is `0`, so the budget is exactly
`subjective-account-cpu-allowed-us` — **300,000 µs (300 ms) by default**. When `available` reaches
zero the transaction is dropped before execution, with a message of the form:

```
Subjectively terminated trx <id>. Authorized account <name> exceeded
subjective CPU limit 300000us by <n>us with an objective cpu limit of 0us.
```

`update_billed_cpu_time` feeds it: when an action's first authorizer differs from its payer, the
CPU is recorded against the signer as well, so the signer accrues a running cost even though the
objective bill went to the contract.

Two properties keep this landing on abusers rather than on ordinary users:

- **Successful transactions do not accumulate.** A subjective bill is held as `pending_cpu_us` and
  removed when the transaction appears in a block. Traffic that lands costs the signer nothing over
  time.
- **Most failures accumulate and decay slowly.** A failed transaction moves its bill into a
  decaying accumulator whose window is `subjective-account-decay-time-minutes` — 24 hours by
  default. Repeatedly submitting transactions that fail is what burns the budget.

  Two classes are deliberately excluded, so a signer is not punished for conditions it did not
  cause: **duplicates** (`tx_duplicate`), and **block-level exhaustion** —
  `block_cpu_usage_exceeded`, `block_net_usage_exceeded`, `deadline_exception`,
  `interrupt_exception`, and the read-only VM-OC compile failure. Whether anything accumulates at
  all depends on how far the transaction got — see below.

A blunter limiter runs alongside it: `subjective-account-max-failures` (default `3`) per
`subjective-account-max-failures-window-size` blocks (default `1`). An account over the limit has
its transactions rejected outright until the window resets:

```
transaction <id> exceeded failure limit for account <name> until <time>
```

Failures are blamed on every per-action first authorizer, so a multi-action transaction shares
responsibility across all of them — but only once the chain has confirmed those accounts authorized
it. A transaction that fails authorization is not counted against the accounts it names, so naming
someone on a transaction you cannot sign does not throttle them.

### It is node-local, not consensus

A subjective drop never appears in a block. Two producers with different settings, different
traffic history, or different account exemptions can legitimately reach different decisions about
the same transaction. The recovery path for a dropped transaction is client resubmit, not
producer-side retry.

### Defaults

Subjective billing ships **on**, and the failure limiter with it — both are gated on the same
flag; setting the p2p and api flags both true disables it outright. The contract payer is excluded:
under contract-pays it is the account provisioned to absorb traffic, so billing it for its callers'
failures would point the cost at the wrong account. The signer still is.

| Option | Default | Effect |
|---|---|---|
| `disable-subjective-p2p-billing` | `false` | Set true to skip subjective enforcement for P2P transactions |
| `disable-subjective-api-billing` | `false` | Set true to skip subjective enforcement for API transactions |
| `subjective-account-cpu-allowed-us` | `300000` | Subjective CPU budget above an account's objective limit |
| `subjective-account-decay-time-minutes` | `1440` | Time to return a full subjective budget |
| `subjective-account-max-failures` | `3` | Failures allowed per account per window |
| `subjective-account-max-failures-window-size` | `1` | Window size in blocks for the failure limit |
| `disable-subjective-payer-billing` | `true` | Set false to also meter the payer (the contract) |
| `disable-subjective-account-billing <acct>` | — | Disables subjective billing for the **whole transaction** when any first authorizer matches — including the failure-limit precheck, and for every co-authorizer |

Independent of all of it, `incoming-transaction-queue-size-mb` (default `1024`) subjectively drops
transactions with a resource-exhaustion error when the incoming queue overflows.

---

## Scenarios

### A user sends tokens

Alice has an account, no ROA policy, zero CPU, zero NET, and 1,144 bytes of RAM. She sends 10 TOK
to Bob.

She signs `{alice, active}` on `sysio.token::transfer`. The payer is `sysio.token`, a system
account with unlimited limits. The transfer succeeds. Alice's CPU usage after the transaction:
`0`.

**RAM:** `sysio.token` sets `ram_payer = "sysio"_n`, so it bills every balance row to the `sysio`
account rather than to sender or receiver. Measured over two transfers — the first creating Bob's
balance row from scratch, the second modifying an existing one:

```
alice ram 944 -> 944   unchanged
bob   ram 944 -> 944   unchanged, even though a NEW row was created
sysio ram       +144   the new balance row, billed to sysio
```

The second transfer added nothing beyond the per-block charge `onblock` already bills to `sysio`.

For `sysio.token`, transfers are RAM-free for both parties whether or not new state is created.
Alice is charged nothing, for anything, on a token transfer.

> This is a property of `sysio.token`'s implementation, not a universal rule — but an ordinary
> third-party contract cannot unilaterally charge RAM to its users either. For a positive RAM delta
> billed to an account other than the receiving contract,
> `apply_context::validate_account_ram_deltas` requires that account to appear in the action's
> authorizations with the `sysio.payer` permission; without it the action fails with
> `Requested payer ... Missing sysio.payer`. And because `sysio.payer` must sit at index 0, adding
> it also makes that user the action's CPU and NET payer.
>
> So billing RAM to a user is an explicit opt-in by the user, not a choice the contract makes
> alone, and it opts them into paying for bandwidth at the same time. Contract authors who want the
> gasless experience should bill RAM to the contract account, which needs no such marker.
>
> **Scope: unprivileged contracts.** `validate_account_ram_deltas` bypasses the marker when the
> receiver is privileged **and** `sysio.`-prefixed (neither alone suffices — the
> `privileged_kv_payer_bypass` test pins that `privtest`, privileged but unprefixed, still fails),
> when a privileged receiver names `sysio` as payer, and when any authorization's actor is `sysio`.
> Privilege is granted by governance, so this describes the system contracts, not a route an
> application can take.
>
> **Compared with Antelope.** There, a contract names a RAM payer in the action body and the
> requirement is only that the named account authorized the action at all — CPU and NET follow a
> separate path entirely, funded by the signer's stake or rental. On Wire the two are welded
> together: naming a user as RAM payer requires the `sysio.payer` marker, and on a *declared* action
> that marker makes the same user the CPU and NET payer too. The split survives only on the inline
> path, where the marker bills the user's RAM and the declared action's payer keeps the bandwidth
> ([Who pays](#who-pays-the-payer-model)).

### A developer deploys a contract

The developer needs one thing: a policy on the account that will hold the contract.

RAM sizing at the launch price of 104 bytes per 0.0001 SYS:

| Item | Bytes | `ram_weight` |
|---|---|---|
| A 60 KiB contract WASM | ~614,400 | ~0.5908 SYS |
| One-time account metadata row | 152 | ~0.0002 SYS |
| ABI | ~4,000 | ~0.0039 SYS |
| 10,000 token holder rows @ ~144 B | ~1,440,000 | ~1.3847 SYS |

> **Code is billed at ten times its size.** `setcode` charges
> `code_size * setcode_ram_bytes_multiplier`, and that multiplier is **10** — for multiple copies
> and cached compilation. Size deployment from the ×10 figure or the `setcode` fails. It applies to
> code only: `setabi` bills the ABI at its actual size. The charge is levied even when the WASM is
> byte-identical to one already on chain, and a redeploy subtracts the old ×10 first, so an upgrade
> costs only the difference.

### Sizing CPU and NET

RAM has a direct conversion, so the table above can be exact. CPU and NET do not — a weight buys a
proportional share of network capacity, and the share depends on how much weight the whole network
has allocated. There is no protocol constant for "one transfer," and any document that gives you
one is guessing.

What you can do is measure and scale. Push the action once on testnet and read it back from the
transaction trace: `cpu_usage_us` for CPU, and the action's billable size for NET. Then work out
the rate you need — actions per averaging window, not actions in total — and ask a node owner for
weight that sustains it.

Two anchors to calibrate against:

- **NET is small and predictable, but it is not just the action.** Each action is billed its own
  serialized size *plus a share of the transaction's overhead* — a fixed 16 bytes, the signatures,
  the extensions and the header, divided across the actions and rounded up. A context-free action
  is additionally billed the bytes of its matching `context_free_data` entry:

  ```cpp
  // libraries/chain/transaction.cpp
  billable_net_per_action_overhead = (size / unpacked_trx.total_actions()) + 1;
  ```

  Two consequences: **signatures cost NET**, so a co-signed action is dearer than a solo one; and
  **batching amortizes the overhead**, so ten actions in one transaction cost less NET than ten
  transactions. A minimal no-argument action measures **132 bytes**; adding a second authorization
  took the same action to 148. A token transfer with a short memo is a few hundred.
- **CPU is the one to measure, not assume.** It depends on what the contract does and on the node's
  hardware and WASM runtime, so a figure from someone else's chain or from a debug build is not
  guidance. Measure yours.

For scale on the weight side: the personal allocation a tier-1 node owner's registration creates
is `0.0500 SYS` of each, and a routine test account is provisioned with `0.0010 SYS` of each.

Budget headroom for peaks above the average rate, not for failures: objective CPU and NET are
billed only on the success path, since `add_transaction_usage` runs from `finalize()` and a
throwing transaction has its session undone. A failed attempt costs the payer nothing *objectively*,
and a retry that lands is billed once. Nor subjectively: `disable-subjective-payer-billing` defaults
true, so `subjective_bill_failure` skips the payer and the cost lands on the *signer*, once an
action begins executing — see [Subjective billing](#subjective-billing-meters-the-signer).

The provisioning is a single `addpolicy` on the contract account. Because the contract is the payer
for any declared action that does not name one explicitly, ordinary users of that token are never billed — a
user is billed only if they opt in with `sysio.payer`, which the wallet or client would have to put
in the action deliberately. What the developer needs from the policy differs by resource, though:

- **CPU and NET are replenishing shares, not per-transaction payments.** They meter rate, not
  count, so a million transfers and ten transfers draw on the same weight. What the volume
  determines is whether that weight is a wide enough slice to sustain the rate; exceed it and
  transactions fail until the window rolls forward, rather than running down a balance.
- **RAM is occupancy, and it does accumulate.** Every transfer that creates a new holder row adds
  permanent state — the table above prices 10,000 of them at ~1.3847 SYS. A token expecting a
  million holders needs a policy sized for a million rows, or a contract that bills those rows
  elsewhere.

So the CPU/NET side is genuinely a one-time provisioning decision; the RAM side has to be sized for
the state the contract will hold.

### A trader swaps on a DEX

The trader signs swap actions on a DEX contract. Those actions do not name a payer, so the DEX
contract is billed for each of them and the trader is charged no CPU, no NET, and no RAM by the
protocol. A trader wanting throughput independent of the DEX's own share can opt into paying with
`sysio.payer`, but nothing requires it.

Any swap fee the trader pays is defined by the DEX contract's own economics, at the application
layer. There is no protocol-level resource charge underneath it.

### A contract exhausts its CPU

CPU and NET are rate limits over a window, not balances. A contract that saturates its share does
not run out of a balance — its transactions begin to fail with `tx_cpu_usage_exceeded` or
`tx_net_usage_exceeded` naming the contract account, and they succeed again as the window rolls
forward.

The fixes are ordinary capacity planning: `expandpolicy` from the existing issuer, or a second
policy from an additional node owner. Both take effect immediately and stack.

### A node owner onboards users

A **tier-1** node owner can call `roa::newuser`, which mints a sub-account under their own name
(`<owner>.<generated>`) with a supplied public key, funded with the standard 1,144-byte account
allocation drawn from the network pool.

The created account has **no CPU/NET policy** — and does not need one, because it transacts
against contracts that carry their own. A user gets an account at no cost to themselves and can
immediately use every provisioned contract on the network.

Two limits worth knowing:

- `newuser` is **tier-1 only**. Tiers 2 and 3 cannot use it.
- Ordinary accounts cannot call the native `newaccount` directly: Wire gates it on the declared
  creator being privileged, which replaced Antelope's name-suffix ownership rule. A privileged
  creator that authorized the action can. For applications that means `sysio.roa` — `newuser`, or
  the NFT-claim flow the OPP depot drives.

### Explicit self-pay

An account that bears its own cost — a relayer, an operator isolating its throughput from a shared
contract — puts `{self, sysio.payer}` first in the authorization list.

This requires CPU/NET allocation of its own. An ordinary account holding only its fixed gift fails
immediately:

```
account alice net usage is too high: 148 > 0
```

Self-pay is the exception, not the default. It exists so a party with its own provisioned capacity
can avoid competing for a shared contract's window.

---

## What ROA is not

Mechanisms an Antelope background might lead you to look for, which do not exist on Wire:

- **No RAM market.** `buyram` and `sellram` do not exist. RAM is not priced by a Bancor curve and
  is not tradeable. For applications it arrives only through a policy.
- **No REX, no PowerUp, no rentals.** None of these contracts or actions exist.
- **No `delegatebw`.** There is no way to convert a token balance into CPU, NET, or RAM. Beyond the
  fixed 1,144-byte gift every new account receives, application capacity comes from a node owner's
  policy. (Bootstrap and governance keep their own routes — `sysio.roa::giftram` and the privileged
  `setalimits` / `setacctcpu` / `setacctnet` — which are not open to applications.)
- **No consensus whitelist.** ROA has no allow-list of approved contracts. Holding a policy is
  what makes a contract callable by ordinary users, not a boolean gate on callability: a
  provisioned explicit payer can drive one without a policy, and privileged paths can set limits
  outside ROA. A *producer* may still refuse traffic:
  `nodeop`'s `actor-whitelist` / `contract-whitelist` and their blacklists are node-local
  configuration, checked during speculative execution, and can reject a fully provisioned
  contract.

---

## Capacity distribution

Tier 1 holds 84% of network capacity across its 21 owners. What the protocol enforces around that:

**Issuance is not gated on tier.** All 1,105 node owners across the three tiers issue policies with
identical mechanics. There is no tier check in `addpolicy`, `expandpolicy`, `extendpolicy`, or
`reducepolicy`. The tiers differ in capacity, not in permission.

**Sponsorship is not exclusive.** Because policies from different issuers stack additively into one
quota, an account is never limited to a single issuer. It can hold grants from any number of node
owners simultaneously, and any one of them can reduce their own policy — at or after their own
`time_block` — without touching another issuer's row. Reducing it is not the same as exiting
cleanly: see [Stacking policies](#stacking-policies-from-multiple-issuers) for what consumed RAM
does to the reclaim.

**Grants are time-committed.** `reducepolicy` is blocked until `time_block`, and `extendpolicy` can
only push that block further out, never shorten it. An issuer commits to a term when they issue and
cannot reclaim before it elapses.

**Caps are enforced on-chain** against the authoritative node-owner rows — 21 / 84 / 1,000 —
counted in `regnodeowner` at registration time.

What the protocol does not do is set the price or the terms of a policy. ROA defines the allocation
primitive and its constraints; the terms on which any issuer grants one are not represented
on-chain.

---

## Reference

### Selected actions on `sysio.roa`

The allocation surface. `giftram`, `setsyscode` and `setsysabi` are bootstrap/governance actions
and are omitted.


| Action | Authorization | Purpose |
|---|---|---|
| `addpolicy` | issuer (a node owner) | Create a policy for an account |
| `expandpolicy` | issuer | Increase an existing policy's weights |
| `extendpolicy` | issuer | Push `time_block` further out |
| `reducepolicy` | issuer, at or after `time_block` | Reclaim weight |
| `newuser` | tier-1 node owner | Create a sponsored sub-account |
| `newnameduser` | `sysio.roa` | Create a vanity account (NFT-claim flow) |
| `nodeownreg` | `sysio.roa` | Register a node owner from an OPP attestation |
| `forcereg` | `sysio.roa` | Register a node owner directly (governance/bootstrap) |
| `activateroa` | `sysio.roa` | Activate ROA and set the initial byte price |
| `setbyteprice` | `sysio.roa` | Change bytes-per-unit on network expansion |

### Selected tables

| Table | Scope | Contents |
|---|---|---|
| `roastate` | singleton | Activation flag, `total_sys`, `bytes_per_unit`, `network_gen` |
| `nodeowners` | `network_gen` | Node owner rows: tier, total and allocated SYS |
| `policies` | issuer | One row per (issuer, owner) pair |
| `reslimit` | global | Per-account totals summed across all its policies |

### Errors you may encounter

| Message | Meaning |
|---|---|
| `account X net usage is too high: N > 0` | X is the payer and has no NET allocation |
| `billed CPU time ... for the account X` | X exhausted its CPU share for the window |
| `Only Node Owners can issue policies for this generation.` | Issuer is not registered in that `network_gen` |
| `Not enough unallocated SYS for this policy.` | Issuer's budget is exhausted |
| `A policy for this owner already exists from this issuer.` | Use `expandpolicy` |
| `Cannot reduce policy before time_block` | The policy's committed term has not elapsed |
| `Cannot allocate CPU/NET to sysio accounts.` | ROA refuses CPU/NET policies for any `sysio.`-prefixed owner. It does not mean the account is unlimited — `sysio.acct` is at zero |
| `Only privileged accounts can create new accounts` | The declared creator is not privileged. Ordinary accounts cannot call native `newaccount`; `sysio.roa::newuser` is the supported flow, and only for a registered tier-1 sponsor |
| `Creator is not a registered tier-1 node owner` | `newuser` is tier-1 only |

---

*This document describes the implementation in the `wire-sysio` repository. The behavioural claims
above — contract-as-payer billing, the failure mode of an unprovisioned contract, RAM-free token
transfers, and multi-issuer policy stacking — were each verified against the code and confirmed
with executable tests. The transcript blocks reproduce actual test output.*

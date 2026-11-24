# sysio.opreg

The depot's operator registry: who the producers, batch operators, underwriters and challengers are,
the collateral each has bonded, and the outpost chains each batch operator serves.

## Responsibility

- Registers operators and keeps each one's global status (`UNKNOWN`, `ACTIVE`, `SLASHED`,
  `TERMINATED`). A role's status follows its collateral: an operator is `ACTIVE` while its available
  balance meets every requirement of its role (`opconfig`). One bond backs every chain an operator
  serves, and slashing and termination are global
- Custodies depot-native collateral (WIRE through `sysio.token`, shadow LIQ through `sysio.liq`).
  Withdrawals wait in `wtdwqueue` for an economic wait and a consensus wait on each chain they depend
  on; every payout is credited to `remitclaims` and pulled by the operator with `claimremit`
- Records the outpost chains each batch operator serves (`servchains`). A chain's schedule
  (`sysio.epoch`) seats only operators `SERVING` it
- Keeps a rolling delivery log per chain (`dellog`); `termcheck` evicts a batch operator that keeps
  missing its duty epochs on a chain from that chain alone

## Tables

| Table | Type | Description |
|-------|------|-------------|
| `operators` | `kv::table` | One row per operator, keyed by account: type, status, bootstrap flag, collateral balances, timestamps, status reason and the newest `MAX_RECENT_ACTIONS` audit entries. Indexed by type and status |
| `opconfig` | `kv::global` | Eviction rails (`terminate_max_consecutive_misses`, `terminate_max_pct_misses_24h`, `terminate_window_ms`), the prune delay, and each role's per-(chain, token) minimum bonds |
| `servchains` | `kv::table` | One row per (chain, batch operator), keyed chain first: `SERVING`, `LEFT` or `EVICTED`, the join time, the last change and the eviction reason. Indexed by account |
| `wtdwqueue` | `kv::table` | Pending withdrawals keyed by request id: the economic epoch each is eligible at and its per-chain consensus waits. Released by `flushwtdw`. Indexed by account |
| `dellog` | `kv::table` | One row per (chain, batch operator, duty epoch) the chain closed: delivered to that chain or missed. Keyed by chain, account and time; rows older than `terminate_window_ms` are pruned oldest-first across chains through the `bylogid` index |
| `remitclaims` | `kv::table` | Collateral and yield owed to an account, per token, until `claimremit` |
| `yielddebts` | `kv::table` | Shadow yield earned but not yet covered by the yield pool, kept across pruning and re-registration |
| `yieldpool` | `kv::table` | Per shadow token: WIRE received from `sysio.liq` and WIRE already credited |
| `opcounters` | `kv::global` | Next withdrawal and delivery-log ids, and the request id the next `flushwtdw` starts at |

## Actions

| Action | Auth | Description |
|--------|------|-------------|
| `setconfig` | `sysio.opreg` | Set the eviction rails, the prune delay and the per-role minimum bonds. `terminate_window_ms` must span `terminate_max_consecutive_misses + 1` duty rotations of every chain's epoch schedule |
| `regoperator` | the account, or `sysio.opreg` | Register an operator. Only `sysio.opreg` registers a bootstrapped one. An account registering itself needs a `sysio.authex` link for every active outpost, except a batch operator, whose links `joinchain` checks. A terminated account registers again once its balances are drained and no schedule seats it; the new registration serves no chain |
| `joinchain` | the operator, or `sysio.opreg` | Serve one active outpost: see below |
| `leavechain` | the operator | Stop serving one outpost: see below |
| `deposit` | the operator | Bond WIRE or a shadow LIQ token |
| `withdraw` | the operator | Queue a withdrawal; it stops counting toward the operator's available balance at once |
| `cancelwtdw` | the operator | Cancel a queued withdrawal |
| `flushwtdw` | `sysio.epoch` | Release matured withdrawals into `remitclaims` (from `tickeconomy`): see below |
| `available` | read-only | An operator's spendable balance for one (chain, token) |
| `processprod`, `processbatch`, `processuw` | `sysio.opreg` | Eligibility transitions after a balance change |
| `slash` | `sysio.chalg` | Seize the operator's collateral and mark it `SLASHED` |
| `recorddel` | `sysio.epoch` | Record one member's delivery result for a duty epoch one chain closed |
| `termcheck` | `sysio.epoch` | Evict a batch operator from one chain when its rolling log there breaches a rail: see below |
| `terminate` | `sysio.opreg` | Terminate an operator globally: its collateral is credited to `remitclaims` |
| `claimremit` | the operator | Pull one token's credited collateral or yield |
| `sweepyield` | permissionless | Pull the shadow yield `sysio.liq` owes the registry into its yield pool |
| `claimyield` | permissionless | Credit an operator's earned shadow yield to its WIRE claim |
| `prune` | permissionless | Erase settled operators terminated longer than the prune delay, with their chain memberships, and aged-out `dellog` rows |

## Withdrawals

`withdraw` queues a request with two waits. The economic wait ends `WITHDRAW_WAIT_ECONOMIC_EPOCHS`
economic epochs after the request. The consensus waits are per chain: for each chain the request waits
on, it records that chain's epoch index + `WITHDRAW_WAIT_EPOCHS` and is held until the chain reaches it.

- A batch operator waits on each chain it has a `servchains` row for (`SERVING`, `LEFT` or `EVICTED`).
  Any other operator type waits on every chain.
- Only a chain whose epoch has started at the request is waited on. With no chain to wait on, only the
  economic wait applies.

`flushwtdw` (from each `sysio.epoch::tickeconomy`) walks the queue in request order from where the last
flush stopped, ends at the first request whose economic wait has not passed, and skips a request a
consensus wait still holds. One flush releases at most `MAX_WTDW_FLUSH_PER_EPOCH` requests and skips at
most `MAX_WTDW_HELD_SKIPS_PER_FLUSH`; the next resumes at the request it stopped on.

## Served chains

A batch operator chooses the outpost chains it serves. Each chain runs its own schedule window, and
`sysio.epoch` fills a chain's window only from `ACTIVE` batch operators whose membership of that chain
is `SERVING`.

- **`joinchain(account, chain_code)`** needs a batch operator that is neither `SLASHED` nor
  `TERMINATED`, an active outpost, and the operator's `sysio.authex` link for the chain's kind, since
  the chain's roster carries the operator's address from it. With no row the operator becomes
  `SERVING`. A `LEFT` or `EVICTED` operator rejoins only once that chain's window holds none of its
  seats, and rejoining restarts the join time.
- **`leavechain(account, chain_code)`** turns `SERVING` into `LEFT`. The operator draws no new seat on
  the chain and still owes the duty of each seat it holds there.
- **`EVICTED`** removes the operator from one chain. `sysio.msgch::deliver` refuses its deliveries on
  that chain, and the chain's consensus threshold leaves it out. A `LEFT` member still delivers its
  remaining seats.

## Eviction

`sysio.epoch::advance(chain)` sends `recorddel` and `termcheck` for each member of the chain's group
whose epoch closed. `termcheck` reads that operator's rows on that chain recorded after both its
registration and its latest join of the chain, and evicts it from the chain when they hold more than
`terminate_max_consecutive_misses` consecutive misses, or a whole-percent miss rate above
`terminate_max_pct_misses_24h`. The membership becomes `EVICTED` with the rail as its reason; the
operator's status, its bond and its other chains are untouched. The window's groups are disjoint, so
the close that evicts a member also slides out the only seat it held on the chain. An `EVICTED` member
is not evaluated again, and it rejoins as a `LEFT` operator does, which restarts the rails.
Bootstrapped operators are never evicted. Governance's `terminate` is the global removal.
- A membership ends with its registration: `prune` erases a pruned operator's rows and `regoperator`
  erases them when the account registers again.

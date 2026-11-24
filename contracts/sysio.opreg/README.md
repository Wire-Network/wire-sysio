# sysio.opreg

The depot's operator registry: who the producers, batch operators, underwriters and challengers are,
the collateral each has bonded, and the outpost chains each batch operator serves.

## Responsibility

- Registers operators and keeps each one's global status (`UNKNOWN`, `ACTIVE`, `SLASHED`,
  `TERMINATED`). A role's status follows its collateral: an operator is `ACTIVE` while its available
  balance meets every requirement of its role (`opconfig`). One bond backs every chain an operator
  serves, and slashing and termination are global
- Custodies depot-native collateral (WIRE through `sysio.token`, shadow LIQ through `sysio.liq`).
  Withdrawals wait in `wtdwqueue` until both their economic wait and their consensus guard have passed;
  every payout is credited to `remitclaims` and pulled by the operator with `claimremit`
- Records the outpost chains each batch operator serves (`servchains`). A chain's schedule
  (`sysio.epoch`) seats only operators `SERVING` it
- Keeps the rolling delivery log (`dellog`) `termcheck` reads to remove a batch operator that keeps
  missing its duty epochs

## Tables

| Table | Type | Description |
|-------|------|-------------|
| `operators` | `kv::table` | One row per operator, keyed by account: type, status, bootstrap flag, collateral balances, timestamps, status reason and the newest `MAX_RECENT_ACTIONS` audit entries. Indexed by type and status |
| `opconfig` | `kv::global` | Termination rails (`terminate_max_consecutive_misses`, `terminate_max_pct_misses_24h`, `terminate_window_ms`), the prune delay, and each role's per-(chain, token) minimum bonds |
| `servchains` | `kv::table` | One row per (chain, batch operator), keyed chain first: `SERVING`, `LEFT` or `EVICTED`, the join time, the last change and the eviction reason. Indexed by account |
| `wtdwqueue` | `kv::table` | Pending withdrawals, released by `flushwtdw` |
| `dellog` | `kv::table` | One row per duty epoch a batch operator's group closed: delivered or missed. Rows older than `terminate_window_ms` are pruned |
| `remitclaims` | `kv::table` | Collateral and yield owed to an account, per token, until `claimremit` |
| `yielddebts` | `kv::table` | Shadow yield earned but not yet covered by the yield pool, kept across pruning and re-registration |
| `yieldpool` | `kv::table` | Per shadow token: WIRE received from `sysio.liq` and WIRE already credited |
| `opcounters` | `kv::global` | Next withdrawal and delivery-log ids |

## Actions

| Action | Auth | Description |
|--------|------|-------------|
| `setconfig` | `sysio.opreg` | Set the termination rails, the prune delay and the per-role minimum bonds. `terminate_window_ms` must span `terminate_max_consecutive_misses + 1` duty rotations of every chain's epoch schedule |
| `regoperator` | the account, or `sysio.opreg` | Register an operator. Only `sysio.opreg` registers a bootstrapped one. An account registering itself needs a `sysio.authex` link for every active outpost, except a batch operator, whose links `joinchain` checks. A terminated account registers again once its balances are drained and no schedule seats it; the new registration serves no chain |
| `joinchain` | the operator, or `sysio.opreg` | Serve one active outpost: see below |
| `leavechain` | the operator | Stop serving one outpost: see below |
| `deposit` | the operator | Bond WIRE or a shadow LIQ token |
| `withdraw` | the operator | Queue a withdrawal; it stops counting toward the operator's available balance at once |
| `cancelwtdw` | the operator | Cancel a queued withdrawal |
| `flushwtdw` | `sysio.epoch` | Release matured withdrawals into `remitclaims` (from `tickeconomy`) |
| `available` | read-only | An operator's spendable balance for one (chain, token) |
| `processprod`, `processbatch`, `processuw` | `sysio.opreg` | Eligibility transitions after a balance change |
| `slash` | `sysio.chalg` | Seize the operator's collateral and mark it `SLASHED` |
| `recorddel` | `sysio.epoch` | Record one member's delivery result for a duty epoch that closed |
| `termcheck` | `sysio.epoch` | Terminate a batch operator whose rolling log breaches a termination rail |
| `terminate` | `sysio.opreg` | Terminate an operator: its collateral is credited to `remitclaims` |
| `claimremit` | the operator | Pull one token's credited collateral or yield |
| `sweepyield` | permissionless | Pull the shadow yield `sysio.liq` owes the registry into its yield pool |
| `claimyield` | permissionless | Credit an operator's earned shadow yield to its WIRE claim |
| `prune` | permissionless | Erase settled operators terminated longer than the prune delay, with their chain memberships, and aged-out `dellog` rows |

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
- A membership ends with its registration: `prune` erases a pruned operator's rows and `regoperator`
  erases them when the account registers again.

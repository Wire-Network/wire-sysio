# sysio.uwrit

Underwriting ledger and swap lifecycle contract. Owns the underwriter COMMIT
race, the collateral lock vector, the swap-from-WIRE escrow queue, and the fee
rate every swap is charged.

## Responsibility

- Ingests `SWAP_REQUEST` attestations from outposts and opens an underwrite
  request (`uwreqs`) for each.
- Resolves the underwriter COMMIT race: verifies each candidate's signature
  against their WIRE account permissions, re-runs the LP variance check,
  re-checks available collateral, then picks a winner.
- Writes one collateral lock per required leg and holds it for the full
  wall-clock challenge window. **Locks are never released by delivery** — on the
  healthy path only `chklocks` sweeps them, once `expires_at_ms` passes. An
  UPHELD underwriter-fault challenge ends the window early instead: `sweeplocks`
  erases that commitment's locks where they stand, which can be well before
  `expires_at_ms`.
- Settles the winning swap against `sysio.reserv` and queues the outbound
  `SWAP_REMIT`.
- Escrows and drains swap-from-WIRE requests (`fwqueue`), charging a revert fee
  on caller-fault drain failures.
- Owns the swap fee RATE (`uwconfig.fee_bps`). The fee is charged and
  distributed by `sysio.reserv` — see "Fees" below.

## Fees

`uwconfig.fee_bps` (default 10 = 0.1%) is the **network fee**, taken out of the
**WIRE leg** of every swap. It is not the whole effective fee: each participating
non-WIRE leg's reserve independently charges its own `owner_fee_bps`
(`sysio.reserv::setrsvfee`, in `[MIN_OWNER_FEE_BPS, MAX_OWNER_FEE_BPS]` or zero).
A chain-to-chain swap therefore pays two owner fees plus the network fee; a swap
against a WIRE endpoint pays one plus the network fee. All of them come off the
same WIRE leg, so together they reduce what the recipient receives.

`sysio.reserv` routes each part as follows — the owner fees to their reserves,
and the network fee through a **two-stage** split:

| Part | Recipient | Path |
|---|---|---|
| Each reserve's `owner_fee_bps` | That **reserve's owner** | Accrues to the reserve row's `owner_fee_accrued`; drawn by `sysio.reserv::claimrsvfee` |
| 50% of the **network fee** | The swap's **winning underwriter** | Accrues to `sysio.reserv::uwfees`; drawn by that account's own `sysio.reserv::claimuwfee` |
| The other 50% — the **rewards pool** — less `fee_emissions_share_bps` | **Batch operators** | Accrues to `sysio.reserv::rewardbkt`; swept by `sysio.system::payepoch` into the batch-op distribution |
| `fee_emissions_share_bps` of the **rewards pool** | The `sysio` **emissions treasury** | Transferred out at settlement by `route_wire_fee` |

The 50/50 network split is the fixed `sysio.reserv::FEE_UNDERWRITER_SHARE_BPS`;
the stage-2 share is the governance dial `reserve_config.fee_emissions_share_bps`,
which **defaults to zero**. At that default every part stays in `sysio.reserv`'s
WIRE custody until claimed or drained and no part of a swap fee reaches the
emissions treasury — but a non-zero dial diverts that share of the rewards pool
to the treasury at settlement, so the custody statement holds only at the
default. Producers are never paid out of swap fees at any setting.

`uwconfig.fromwire_revert_fee_bps` is charged on the refunded escrow when a
queued from-WIRE swap reverts at drain for a cause the caller controls
(unpriceable target, variance tolerance exceeded). A revert has no winning
underwriter, so the whole revert fee becomes the rewards pool — reaching the
rewards bucket in full under the default zero `fee_emissions_share_bps`, less the
configured emissions share when that dial is set. Reverts caused
by system state changes after enqueue (reserve deactivated, flipped private,
chain deregistered) refund in full.

## Tables

| Table | Row type | Description |
|-------|----------|-------------|
| `uwconfig` | `uw_config` | Singleton: `fee_bps`, `collateral_lock_duration_ms`, `min_fromwire_amount`, `fromwire_revert_fee_bps`, `uwreq_pending_timeout_epochs`, `uwreq_retention_epochs` |
| `uwreqs` | `uw_request_t` | One row per swap intent — race state in `commits_by`, `winner`, lifecycle status, mirrored `variance_tolerance_bps`. Retained for `uwreq_retention_epochs` after ANY terminal transition — `COMPLETED` (once the last collateral lock is gone — normally when `chklocks` sweeps it at expiry, but earlier when `sweeplocks` erases it on an UPHELD challenge; both run the same `finalize_settled_uwreqs` tail. The reserve settlement itself already happened at winner selection, which is what made the row CONFIRMED), `REJECTED` (immediate failure via `reject_and_refund`), or `EXPIRED` (pending timeout, same path) — then erased by `pruneuwreqs` |
| `locks` | `lock_entry` | Flat per-leg lock vector consulted by `sysio.opreg::available()`. The `byexpire` secondary index lets `chklocks` sweep expired locks oldest-first, up to its per-epoch budget |
| `locksums` | `lock_sum` | Materialized Σ `lock_entry.amount` per `(underwriter, chain_code, token_code)` bucket — the "locked" half of `sysio.opreg::available()`, read O(1) instead of scanning `locks`. Written by exactly three paths, all in this contract: `try_select_winner` ADDS (on a win), `chklocks` DECREMENTS (healthy release at expiry), and `sweeplocks` DECREMENTS (erasing a commitment's held locks on an UPHELD challenge). A bucket's row is erased once its total reaches zero, so an absent row reads as zero. Any new erase path must decrement too — the rollup is authoritative for `available()`, so a bucket left positive after its last row is gone suppresses that collateral permanently |
| `fwqueue` | `fromwire_q` | Escrowed swap-from-WIRE requests awaiting drain. `byepoch` secondary index |
| `uwcounters` | `uw_counters` | Monotonic id allocators (uwreq ids, lock ids) |

## Actions

| Action | Auth | Description |
|--------|------|-------------|
| `setconfig` | `sysio.uwrit` | Set the fee rate, lock duration, from-WIRE floor, revert fee, and uwreq lifecycle windows |
| `createuwreq` | `sysio.msgch` | Open an underwrite request from an inbound `SWAP_REQUEST` attestation |
| `rcrdcommit` | `sysio.msgch` | Record an underwriter's per-leg `UNDERWRITE_INTENT_COMMIT` bytes; resolves the race once both legs are present |
| `swapfromwire` | `user` | Escrow WIRE and enqueue a swap-FROM-WIRE request |
| `drainfwq` | `sysio.epoch` or self | Drain the from-WIRE queue: settle what prices, revert the rest (charging the revert fee on caller-fault causes) |
| `chklocks` | `sysio.epoch` or self | Sweep collateral locks whose wall-clock window has expired, oldest-first, EXAMINING at most `max_rows` rows per call (`advance` passes `MAX_LOCK_RELEASE_PER_EPOCH`) — the budget counts rows examined rather than locks released, so held locks and the challenge pokes they generate are bounded by it too; an oversized expiry burst drains across later epochs rather than aborting `advance` |
| `pruneuwreqs` | `sysio.epoch` or self | Expire timed-out PENDING uwreqs and erase terminal rows past their retention window |
| `holdlocks` | `sysio.chalg` | Mark a commitment's winning locks as held by an OPEN underwriter-fault challenge (WIRE-297); held locks are skipped by `chklocks` instead of released |
| `freelocks` | `sysio.chalg` | Clear the hold after a REJECTED or LAPSED challenge, so the next `chklocks` releases the locks normally |
| `sweeplocks` | `sysio.chalg` | Erase a commitment's held locks after an UPHELD challenge — the underwriter is already SLASHED, so each `releaselock` takes its deferred-slash branch. Decrements `locksums` like `chklocks` does |
| `sumlocks` | read-only | Sum an underwriter's active locks for a `(chain, token)` bucket — the lock half of `sysio.opreg::available()` |

## Dependencies

- Receives inbound attestations via `sysio.msgch`; queues outbound `SWAP_REMIT`
  / `SWAP_REVERT` through it.
- Settles against `sysio.reserv` (`applyswap` / `applyfromwire` / `paywire` /
  `refundwire`), forwarding the winning underwriter so the fee accrues to them.
- Reads collateral from `sysio.opreg`; its `locks` table is the lock half of
  that contract's `available()` rollup.
- Lock sweeping and queue draining are inlined from `sysio.epoch::advance`.
- The off-chain counterpart is `wire-sysio/plugins/underwriter_plugin/`.

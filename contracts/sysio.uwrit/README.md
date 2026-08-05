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
  wall-clock challenge window. **Locks are never released by delivery** — only
  `chklocks` sweeps them once `expires_at_ms` passes.
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
| `uwreqs` | `uw_request_t` | One row per swap intent — race state in `commits_by`, `winner`, lifecycle status, mirrored `variance_tolerance_bps`. Retained for `uwreq_retention_epochs` after settlement for audit |
| `locks` | `lock_entry` | Flat per-leg lock vector consulted by `sysio.opreg::available()`. The `byexpire` secondary index lets `chklocks` sweep expired locks in one pass |
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
| `chklocks` | `sysio.epoch` or self | Sweep collateral locks whose wall-clock window has expired |
| `pruneuwreqs` | `sysio.epoch` or self | Expire timed-out PENDING uwreqs and erase terminal rows past their retention window |
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

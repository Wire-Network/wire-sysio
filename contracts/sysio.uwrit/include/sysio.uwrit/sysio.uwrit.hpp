#pragma once

#include <sysio/sysio.hpp>
#include <sysio/kv_global.hpp>
#include <sysio/kv_table.hpp>
#include <sysio/asset.hpp>
#include <sysio/crypto.hpp>
#include <sysio/system.hpp>
#include <fc-lite/crypto/chain_types.hpp>
#include <sysio/opp/types/types.pb.hpp>
#include <sysio.opp.common/slug_name.hpp>
#include <sysio.opp.common/opp_table_types.hpp>

namespace sysio {

   /**
    * @brief sysio.uwrit — underwriter race resolver + flat lock vector.
    *
    * Per the v6 data-model refactor (`load-context-and-follow-smooth-flame.md`
    * §3.13, §4.5, §4.6):
    *
    * - opreg owns the bond ledger (per-(operator, chain_code, token_code) aggregate
    *   balance). uwrit owns the **lock vector** — one row per leg of every
    *   in-flight UWREQ. opreg's `available()` rollup reads this table via a
    *   mirror to subtract active locks from the operator's spendable balance.
    *
    * - Identity has been rekeyed onto `sysio::slug_name` (uint64). Each
    *   `lock_entry` carries `(chain_code, token_code, reserve_code)`; the
    *   `reserve_code` records which specific reserve this leg is bound to
    *   so a slash-to-reserve hop on a same-(chain, token) pair with
    *   multiple reserves can route unambiguously. `uw_request_t` carries
    *   `src_*` and `dst_*` slug_name triples for the same reason.
    *
    * - The per-underwriter composite lock index can no longer fit in a
    *   `uint128_t` (3 × uint64 = 192 bits). It is split into two secondary
    *   indexes per the plan's B.2 design:
    *     * `byuwck`         — `checksum256(account || chain_code || token_code)`
    *                          for the per-(chain, token) rollup that opreg's
    *                          `available()` reads.
    *     * `byunderwriter` — uint64 split-index keyed on `underwriter.value`
    *                          for cheap per-operator scans (in-memory filter
    *                          on chain_code / token_code / reserve_code).
    *
    * - On `UNDERWRITE_INTENT_COMMIT` arrival (one per outpost; underwriters
    *   call `commit(...)` JSON-RPC on each side), `record_commit` registers
    *   the per-leg arrival in `uwreqs.commits_by`. When BOTH legs land for
    *   the same underwriter, `try_select_winner` checks the underwriter's
    *   `sysio.opreg::available(...)` for each chain; if both legs are
    *   covered, the underwriter wins, two rows are pushed onto `locks`, and
    *   a `REMIT` is queued to the destination outpost.
    *
    * - opreg.balance is **not** mutated when a lock is added — the lock
    *   simply reduces what `available()` rolls up. When a lock releases:
    *     * SLASHED underwriter   — opreg::releaselock decrements balance
    *                               and emits SLASH_OPERATOR (deferred-slash).
    *     * TERMINATED underwriter — opreg::releaselock decrements balance
    *                               and emits WITHDRAW_REMIT to authex
    *                               destination (deferred-remit).
    *     * Healthy underwriter   — no opreg call; freed amount naturally
    *                               reappears in `available()` once the lock
    *                               row is erased.
    */
   class [[sysio::contract("sysio.uwrit")]] uwrit : public contract {
   public:
      using contract::contract;

      // Well-known accounts
      static constexpr name EPOCH_ACCOUNT  = "sysio.epoch"_n;
      static constexpr name MSGCH_ACCOUNT  = "sysio.msgch"_n;
      static constexpr name AUTHEX_ACCOUNT = "sysio.authex"_n;
      static constexpr name OPREG_ACCOUNT = "sysio.opreg"_n;
      static constexpr name CHAINS_ACCOUNT = "sysio.chains"_n;
      static constexpr name CHALG_ACCOUNT = "sysio.chalg"_n;
      static constexpr name RESERVE_ACCOUNT = "sysio.reserv"_n;
      static constexpr name TOKEN_ACCOUNT = "sysio.token"_n;

      // Default number of epochs a TERMINAL (COMPLETED / REJECTED / EXPIRED)
      // UWREQ row lives before `pruneuwreqs` erases it. 10 epochs matches the
      // bootstrap doc's "losers retained 10 epochs for debugging" requirement.
      // Live value is `uwconfig.uwreq_retention_epochs` (retunable via
      // `setconfig`); this constant is only the fresh-chain default.
      static constexpr uint32_t DEFAULT_UWREQ_RETENTION_EPOCHS = 10;

      // Default number of epochs a PENDING UWREQ may wait for its underwriter
      // race to resolve before `pruneuwreqs` expires it (refund/revert + flip
      // to UNDERWRITE_REQUEST_STATUS_EXPIRED). A pending request only resolves
      // if underwriter commits arrive — an external, optional event — so
      // without this deadline an abandoned request is indistinguishable from a
      // slow one forever and its row (plus the user's escrowed/deposited
      // funds) lingers indefinitely (SEC-129 / WSA-223). Live value is
      // `uwconfig.uwreq_pending_timeout_epochs` (retunable via `setconfig`).
      static constexpr uint32_t DEFAULT_UWREQ_PENDING_TIMEOUT_EPOCHS = 10;

      // Ceiling `setconfig` accepts for BOTH epoch-count lifecycle knobs
      // (pending timeout + retention). Prevents a misconfigured value near
      // UINT32_MAX from wrapping the `current_epoch + knob` deadline stamp to
      // a tiny epoch index (instant expiry/prune). One million epochs is ~2
      // years at the 60s test cadence and far beyond any real retention need,
      // while `current_epoch + 1'000'000` stays astronomically below the
      // uint32 wrap point for any reachable epoch index.
      static constexpr uint32_t MAX_UWREQ_LIFECYCLE_EPOCHS = 1'000'000;

      // Rows `sysio.epoch::advance` passes to `pruneuwreqs` each epoch. Sized
      // like MAX_FWQ_DRAIN_PER_EPOCH (and for the same reason): the sweep runs
      // inline inside advance's hard, uncatchable transaction CPU deadline, so
      // per-epoch work must be bounded; a backlog simply drains across
      // subsequent epochs.
      static constexpr uint32_t MAX_UWREQ_PRUNE_PER_EPOCH = 32;

      // Locks `sysio.epoch::advance` passes to `chklocks` each epoch. Sized
      // like MAX_UWREQ_PRUNE_PER_EPOCH / MAX_FWQ_DRAIN_PER_EPOCH, and for the
      // same reason — but the exposure here is sharper than either, because
      // lock EXPIRY is inherently bursty. Every lock is stamped
      // `now + collateral_lock_duration_ms` at creation, so a burst of
      // settlements inside one epoch produces a burst of expiries inside one
      // epoch, exactly one lock-duration later. Sustained swap traffic
      // therefore presents `chklocks` with a whole epoch's settlements at
      // once, and per expired lock the sweep does an inline
      // `opreg::releaselock` dispatch plus an erase — all inside advance's
      // hard, uncatchable transaction CPU deadline.
      //
      // Unbounded, a large enough expiry burst aborts `advance`; and because
      // those same locks are still expired at the next advance, it aborts
      // identically every epoch thereafter — a PERMANENT chain-wide epoch
      // stall rather than a transient one. Bounded, an oversized burst is
      // just release latency that drains across subsequent epochs, which is
      // harmless: the challenge window has already closed, so a lock freed an
      // epoch or two late costs only a brief overstatement of the
      // underwriter's reserved collateral.
      static constexpr uint32_t MAX_LOCK_RELEASE_PER_EPOCH = 32;

      // ── UWREQ row-growth rails (SEC-129 / WSA-223) ─────────────────────────
      // Every uwreqs `modify` re-serializes the whole row inside the
      // never-throw evalcons / advance dispatch surfaces, so per-field byte
      // caps + a total-row guard keep the serialized row from approaching
      // database value limits (where a later consensus-dispatched modify
      // would abort and stall the chain). All are protocol backstops sized
      // generously above legitimate traffic — violations log + skip, never
      // check().

      // Per-candidate cap on `uw_request_t.commits_by`. rcrdcommit already
      // gates entries on ACTIVE underwriters; this bounds the row even if the
      // ACTIVE roster grows large.
      static constexpr uint32_t MAX_UWREQ_CANDIDATES = 32;

      // Per-leg cap on a stored verbatim `UnderwriteIntentCommit` payload
      // (`commit_entry.source_uic_bytes` / `dest_uic_bytes`). A real UIC is a
      // handful of ids + codes + a signature `verify_uic_signature` already
      // bounds to 1024 bytes — 2 KiB is ~2x the largest legitimate encoding.
      static constexpr uint32_t MAX_UIC_LEG_BYTES = 2048;

      // Cap on the stored inbound SwapRequest payload
      // (`uw_request_t.attestation_inbound_data`) at createuwreq.
      static constexpr uint32_t MAX_ATTESTATION_INBOUND_DATA_BYTES = 4096;

      // Cap on `SwapRequest.source_tx_id` (largest real id today is a 64-byte
      // Solana signature; 2x headroom for future chains).
      static constexpr uint32_t MAX_SOURCE_TX_ID_BYTES = 128;

      // Cap on the depositor address bytes (`SwapRequest.actor.address`).
      // Mirrors swapfromwire's existing 64-byte `recipient_addr` cap — the
      // platform's notion of a maximum chain address.
      static constexpr uint32_t MAX_DEPOSITOR_BYTES = 64;

      // Belt-and-braces ceiling on the projected serialized uwreqs row before
      // any growing `modify`. Structural worst case under the caps above
      // (32 candidates x 2 x 2 KiB legs + 4 KiB attestation + fixed fields)
      // is ~135 KiB, comfortably inside this guard; the guard exists so no
      // future field addition can silently push a row toward chain KV value
      // limits.
      static constexpr uint32_t MAX_UWREQ_ROW_BYTES = 262'144;

      // Safety rails on the depot-originated swap-from-WIRE queue (SEC-77 /
      // WSA-165). `swapfromwire` is public and escrows only the caller's WIRE,
      // so without bounds a caller could split real WIRE into an unbounded
      // number of system-paid `fwqueue` rows and force `drainfwq` to process
      // them all inside one `sysio.epoch::advance` transaction. That
      // transaction's CPU budget (~150 ms) is a hard, uncatchable deadline, so
      // an oversized queue would abort every advance and permanently stall
      // epoch progress chain-wide.
      //
      // MAX_FWQ_DRAIN_PER_EPOCH bounds the rows drained per advance; undrained
      // rows stay queued (escrow safe in reserve custody) and drain a later
      // epoch, converting a potential halt into bounded drain latency.
      // Ingress is already throttled economically — every `swapfromwire` is a
      // `require_auth(user)` transaction that bills the caller CPU/NET and
      // escrows real WIRE per row — so the drain bound alone is the liveness
      // rail; there is no per-caller row cap. Conservatively sized to stay well
      // under the transaction CPU ceiling shared with chklocks / buildenv /
      // emissions; raise it (contract upgrade) only if legitimate from-WIRE
      // throughput approaches the bound.
      static constexpr uint32_t MAX_FWQ_DRAIN_PER_EPOCH = 32;

      // Economic ingress rails on `swapfromwire`, complementing the drain
      // bound above. Both are live config (`uwconfig`) retunable via
      // `setconfig` without a contract upgrade; the values here are only the
      // defaults a fresh chain starts from.
      //
      // DEFAULT_MIN_FROMWIRE_AMOUNT floors the escrow per queued row (5 WIRE
      // at 9 decimals). Dust rows are what made queue spam nearly free: the
      // escrow comes back in full at drain, so 1-atomic rows could hold drain
      // slots while locking no meaningful capital — and sat below the Bancor
      // kernel's pricing floor, guaranteeing zero-quote refunds. With the
      // floor, sustaining a D-row backlog keeps D x floor WIRE locked for the
      // full queue latency the spammer themselves creates.
      //
      // DEFAULT_FROMWIRE_REVERT_FEE_BPS is charged on the refunded escrow
      // when a queued row reverts at drain for a cause the caller controls
      // (unpriceable target / variance tolerance exceeded). The fee is routed
      // through the same rewards/emissions split as settlement fees
      // (`opp::amm::split_wire_fee` + `sysio.reserv::route_wire_fee`), so
      // revert churn pays the system instead of recycling for free. Reverts
      // caused by system state changes after enqueue (reserve deactivated,
      // flipped private, chain deregistered) refund in full — the caller did
      // nothing wrong. Successful swaps are unaffected; they already pay
      // `fee_bps` at settlement.
      //
      // 500 bps (5%) is the launch default (Jonathan, 2026-08-04), replacing the
      // 0.1% placeholder that merely mirrored `fee_bps`. It has to be large
      // enough that cycling escrows through the queue is not free. It stays
      // governance-tunable via `setconfig` (capped at MAX_FEE_BPS), so this is a
      // starting point, not a commitment.
      static constexpr uint64_t DEFAULT_MIN_FROMWIRE_AMOUNT     = 5'000'000'000; // 5 WIRE @ 9 decimals
      static constexpr uint32_t DEFAULT_FROMWIRE_REVERT_FEE_BPS = 500;           // 5%

      // Maximum accepted swap fee, in basis points. A 100% fee (10000 bps)
      // would zero the post-fee WIRE leg of every swap (`net == 0` in
      // `opp::amm::split_wire_fee`), which let a from-WIRE or token-to-token
      // swap debit destination reserve liquidity while crediting zero WIRE —
      // draining the reserve at an arbitrary price (SEC-26 / WSA-042). Any fee
      // below 100% leaves a positive remainder for every positive input, so the
      // cap is 9999 (mirrors `sysio.reserv::MAX_CONNECTOR_WEIGHT_BPS`).
      //
      // This bounds THIS rate only. Reserve owner fees are charged off the same
      // WIRE leg and capped independently, so the TOTAL can still reach 100% —
      // 9999 here plus one owner fee at `sysio.reserv::MIN_OWNER_FEE_BPS` (1)
      // does exactly that. The settlement paths' `net > 0` checks are therefore
      // live rejections of a configured combination, not dead defense-in-depth;
      // see `opp::amm::split_wire_fee`.
      static constexpr uint32_t MAX_FEE_BPS = 9999;

      // Upper bound on the collateral lock duration. try_select_winner locks a
      // winning underwriter's collateral until `now_ms + collateral_lock_duration_ms`;
      // an unbounded duration near UINT64_MAX wraps that sum to a past timestamp,
      // releasing the lock immediately. 365 days is far beyond any real settlement
      // window yet leaves ~18 orders of magnitude of headroom below the wrap point.
      static constexpr uint64_t MAX_COLLATERAL_LOCK_DURATION_MS =
         365ull * 24ull * 60ull * 60ull * 1000ull;

      // -----------------------------------------------------------------------
      //  Actions
      // -----------------------------------------------------------------------

      /// Set underwriting fee + lock config. Fields:
      ///   * `fee_bps` — the NETWORK swap fee charged by the depot per spoke,
      ///     taken out of the WIRE leg of every swap (so the ETH/SOL the
      ///     recipient can receive is reduced). It is NOT the whole effective
      ///     fee: each participating non-WIRE leg's reserve independently charges
      ///     its own `owner_fee_bps` off the same leg (`sysio.reserv::setrsvfee`).
      ///     `sysio.reserv` splits the NETWORK component 50/50 between the swap's
      ///     winning underwriter (claimable via `sysio.reserv::claimuwfee`) and a
      ///     rewards pool (see `sysio.reserv::FEE_UNDERWRITER_SHARE_BPS`). That
      ///     pool then splits again by `reserve_config.fee_emissions_share_bps`:
      ///     that share is transferred to the `sysio` emissions treasury and the
      ///     remainder accrues to the rewards bucket, which
      ///     `sysio.system::payepoch` allocates to the batch-operator
      ///     distribution (paying only eligible shares; the rest stays in the
      ///     treasury). The dial defaults to zero, so by default the whole pool
      ///     is allocated to batch operators and no part of a fee leaves
      ///     `sysio.reserv`'s custody at settlement.
      ///   * `collateral_lock_duration_ms` — wall-clock milliseconds after
      ///     `lock_entry.created_at_ms` that the lock auto-expires (swept by
      ///     `sysio.epoch::advance -> chklocks`). This is the challenge
      ///     window: collateral stays locked for its full duration — it is
      ///     never released by delivery. Default 43,200,000 (12 hours);
      ///     test clusters shorten it via this action.
      ///   * `min_fromwire_amount` — minimum WIRE escrow (9-decimal atomic
      ///     units) `swapfromwire` accepts; rows below the floor are rejected
      ///     at enqueue. Must be positive. Default 5 WIRE.
      ///   * `fromwire_revert_fee_bps` — fee on the refunded escrow when a
      ///     queued from-WIRE swap reverts at drain for a caller-controlled
      ///     cause (see `drainfwq`); routed exactly like the settlement fee.
      ///     Capped at MAX_FEE_BPS so the post-fee refund stays positive.
      ///   * `uwreq_pending_timeout_epochs` — epochs a PENDING uwreq may wait
      ///     for its race to resolve before `pruneuwreqs` expires it
      ///     (refund/revert + EXPIRED). 1..MAX_UWREQ_LIFECYCLE_EPOCHS.
      ///   * `uwreq_retention_epochs` — epochs a terminal (COMPLETED /
      ///     REJECTED / EXPIRED) uwreq row is retained for audit before
      ///     `pruneuwreqs` erases it. 1..MAX_UWREQ_LIFECYCLE_EPOCHS.
      [[sysio::action]]
      void setconfig(uint32_t fee_bps,
                     uint64_t collateral_lock_duration_ms,
                     uint64_t min_fromwire_amount,
                     uint32_t fromwire_revert_fee_bps,
                     uint32_t uwreq_pending_timeout_epochs,
                     uint32_t uwreq_retention_epochs);

      /// Called inline from `sysio.msgch::dispatch` when a SWAP attestation
      /// arrives. Decodes the SwapRequest, prices the swap on the depot's live
      /// curve (`sysio.reserve::swapquote`'s kernel), runs the
      /// variance-tolerance check, and either:
      ///   * creates an OPEN UWREQ whose `dst_amount` is that QUOTE — never the
      ///     caller's `target_amount` (WNS-02) — or
      ///   * emits a SWAP_REVERT back to `chain_code` and skips UWREQ creation
      ///     when the gap between `target_amount` and the depot's current quote
      ///     exceeds `target_tolerance_bps` of that quote, or when every
      ///     required reserve is ACTIVE yet the curve cannot price the swap.
      ///
      /// `chain_code` is the source outpost the SWAP came from — needed so
      /// the SWAP_REVERT routes back to the user's depositing outpost on
      /// variance failure.
      [[sysio::action]]
      void createuwreq(uint64_t attestation_id,
                       opp::types::AttestationType type,
                       uint64_t chain_code,
                       std::vector<char> data);

      /// Called inline from `sysio.msgch::dispatch` when an
      /// UNDERWRITE_INTENT_COMMIT attestation arrives. Pre-validates the
      /// claimed underwriter's fixed-size recoverable signature before
      /// changing candidate evidence. Invalid claims are logged and ignored:
      /// they cannot replace stored bytes, change status/reason, or refresh
      /// arrival timestamps. A valid replay of an already-recorded leg never
      /// replaces the stored bytes: candidate legs are write-once. Once both
      /// legs are stored, an exact replay revalidates the preserved evidence
      /// and retries winner selection against current mutable depot state.
      /// When both legs
      /// land for the same underwriter, runs `try_select_winner` to resolve the
      /// race. For a dual-outpost request it revalidates only the older stored
      /// leg so a WIRE permission-key change between arrivals cannot authorize
      /// stale evidence; the just-verified incoming leg is not recovered twice.
      /// A complete candidate is evaluated immediately. Recoverable conditions
      /// (temporarily unavailable collateral, identity links, or reserves) keep
      /// its evidence on the PENDING row until an outpost replays one of the
      /// exact stored UICs; malformed or no-longer-authorized stored evidence is
      /// durably disqualified. Replayed UIC bytes never replace stored legs.
      ///
      /// `(from_chain_code, from_token_code, reserve_code)` together identify
      /// which leg of the swap this UIC covers. Same-chain swaps with
      /// multiple reserves of the same `(chain, token)` need all three
      /// codes to disambiguate src vs dst.
      ///
      /// `uic_bytes` is the raw zpp_bits-encoded `UnderwriteIntentCommit`
      /// payload — the action signature carries bytes, not the proto
      /// message itself, per `feedback_no_proto_messages_in_actions.md`.
      [[sysio::action]]
      void rcrdcommit(uint64_t uwreq_id,
                      name underwriter,
                      uint64_t chain_code,
                      sysio::slug_name from_chain_code,
                      sysio::slug_name from_token_code,
                      sysio::slug_name reserve_code,
                      std::vector<char> uic_bytes);

      /// Sweep all `locks` rows whose `expires_at_ms` has elapsed. Inlined
      /// from `sysio.epoch::advance` (as one of its FIRST steps — freshly
      /// freed collateral must be visible to the same advance's withdraw
      /// flushing); can also be invoked by `sysio.uwrit` itself for manual
      /// cleanup. The `byexpire` secondary index walks rows in ascending
      /// expiry, so the loop stops at the first unexpired row.
      ///
      /// For every expired lock the sweep inlines `opreg::releaselock`
      /// (deferred-slash for SLASHED underwriters, deferred-remit for
      /// TERMINATED, no-op for healthy) and erases the row. When the last
      /// lock of a CONFIRMED uwreq is swept, the uwreq flips to COMPLETED
      /// (delivery itself is implicit — there is no SWAP_REMIT ack; the
      /// lock window expiring IS the settlement horizon).
      ///
      /// This sweep is the only HEALTHY lock-release path: locks are a
      /// wall-clock challenge window (12h default) and are never released by
      /// delivery. It is not the only path that ERASES a lock — `sweeplocks`
      /// does too, on an UPHELD challenge (see `lock_sum`, whose rollup both
      /// must maintain).
      ///
      /// EXCEPTION (WIRE-297): a lock whose `challenge_id` is non-zero — an
      /// underwriter-fault challenge is OPEN against its commitment — is NOT
      /// released at expiry. The sweep skips it and instead pokes
      /// `sysio.chalg::chkuwchal`, whose resolution either sweeps the locks
      /// with the underwriter slashed (`sweeplocks`) or clears the hold
      /// (`freelocks`) so the NEXT sweep releases them normally. The epoch
      /// tick is thereby the challenge system's only cadence.
      ///
      /// Budget-bounded, mirroring `pruneuwreqs` / `drainfwq`: walks the
      /// `byexpire` index in ascending `expires_at_ms` and EXAMINES at most
      /// `max_rows` rows (`max_rows == 0` is a no-op). Inlined from
      /// `sysio.epoch::advance` with `MAX_LOCK_RELEASE_PER_EPOCH`; also
      /// invocable by `sysio.uwrit` itself with a caller-chosen budget for a
      /// manual backlog drain. NEVER throws past the auth gate: it runs inline
      /// inside `advance`, where an abort stalls epoch progress chain-wide.
      ///
      /// The budget counts rows EXAMINED, not locks released — held locks and
      /// the challenge pokes they generate cost real work too. Bounding only
      /// releases would let an arbitrary number of held rows be scanned and an
      /// arbitrary number of distinct challenges fan out an inline
      /// `chkuwchal` each, which is exactly the unbounded `advance` work this
      /// bound exists to remove. `open_challenges` is therefore bounded by
      /// `max_rows` as a consequence of the same counter.
      ///
      /// Ascending-expiry order makes the bound a FIFO drain, so an oversized
      /// burst simply spreads across subsequent epochs. Held locks are the one
      /// thing that can sit at the head of that queue without leaving it: they
      /// are skipped, not erased, so while more than `max_rows` challenges are
      /// open the rows behind them wait. That is bounded and self-clearing
      /// rather than a stall — each sweep pokes the challenges it can see, and
      /// a resolved challenge removes its locks (`sweeplocks`) or clears their
      /// hold (`freelocks`), letting the window advance on the next tick.

      [[sysio::action]]
      void chklocks(uint32_t max_rows);

      /// Mark the winning underwriter's locks for `uwreq_id` as held by the OPEN underwriter-
      /// fault challenge `chal_id` (WIRE-297). Auth: `sysio.chalg`, inlined from `openuwchal`.
      /// This is the AUTHORITATIVE liveness validation for a filing — every lock must still be
      /// inside its window and free of any other challenge, and at least one lock must exist —
      /// so a stale challenge aborts whole here, bond escrow included.
      [[sysio::action]]
      void holdlocks(uint64_t uwreq_id, name underwriter, uint64_t chal_id);

      /// Clear the challenge hold on the underwriter's locks for `uwreq_id` — the challenge was
      /// REJECTED by the council or LAPSED at the window's end. The locks then release on their
      /// next normal `chklocks` sweep: a healthy release, no collateral moves. Auth:
      /// `sysio.chalg`. Deliberately a silent no-op when nothing is held: it runs inline from
      /// the epoch tick (`chklocks` -> `chkuwchal` -> here), where an abort would stall epoch
      /// advancement chain-wide.
      [[sysio::action]]
      void freelocks(uint64_t uwreq_id, name underwriter);

      /// Release + erase the underwriter's locks for `uwreq_id` after an UPHELD challenge. The
      /// underwriter is already SLASHED (`chkuwchal` slashes before sweeping — inline actions
      /// run depth-first in send order), so every inlined `opreg::releaselock` takes its
      /// deferred-slash branch: the locked collateral is debited and the outbound SLASH
      /// attestation queued. Ends with the same COMPLETED-flip / evidence-clear / retention
      /// tail `chklocks` runs when a uwreq's last lock leaves. Auth: `sysio.chalg`; silent
      /// no-op when no locks remain (same never-throw-inside-advance reasoning as `freelocks`).
      [[sysio::action]]
      void sweeplocks(uint64_t uwreq_id, name underwriter);

      /// Bounded UWREQ lifecycle sweep (SEC-129 / WSA-223). Inlined from
      /// `sysio.epoch::advance` each epoch with `MAX_UWREQ_PRUNE_PER_EPOCH`;
      /// also invocable by `sysio.uwrit` itself with a caller-chosen budget
      /// for manual backlog drains. Walks the `byexpire` index over rows whose
      /// `expires_at_epoch` is non-zero and has elapsed, handling at most
      /// `max_rows` rows (`max_rows == 0` is a no-op):
      ///
      ///   * PENDING past its deadline — the underwriter race never resolved
      ///     inside `uwconfig.uwreq_pending_timeout_epochs`. Refund the source
      ///     side (SWAP_REVERT to the source outpost, or a full `refundwire`
      ///     of the from-WIRE escrow), flip the row to EXPIRED, clear its
      ///     heavy payloads, and re-stamp `expires_at_epoch` for the terminal
      ///     retention window.
      ///   * COMPLETED / REJECTED / EXPIRED past retention — erase the row.
      ///   * CONFIRMED never appears: winner selection zeroes the deadline
      ///     (the wall-clock lock window owns the row until `chklocks`
      ///     terminalizes it).
      ///
      /// NEVER throws past the auth gate — it runs inline inside `advance`,
      /// where an abort stalls epoch progress chain-wide. There are no timers
      /// anywhere in this lifecycle: `expires_at_epoch` is passive row data,
      /// and this action — a trigger fired by the epoch machinery — is the
      /// only thing that ever evaluates it.
      [[sysio::action]]
      void pruneuwreqs(uint32_t max_rows);

      /// Swap FROM WIRE (the depot is the source chain). `user` escrows
      /// `wire_amount` REAL WIRE into `sysio.reserv` custody NOW and the
      /// request is QUEUED — no uwreq is created in this transaction. The
      /// next `sysio.epoch::advance` drains the queue (`drainfwq`): the
      /// target reserve + variance are re-validated authoritatively and a
      /// PENDING uwreq with `src = (WIRE, WIRE)` is created for the normal
      /// single-leg underwriter race (target leg only).
      ///
      /// `wire_amount` must meet the configured `min_fromwire_amount` floor
      /// (default 5 WIRE) — the floor prices queue slots in locked escrow so
      /// dust rows cannot hold the drain hostage for free. Drain-time reverts
      /// the caller controls (unpriceable target, variance exceeded) refund
      /// the escrow minus `fromwire_revert_fee_bps`; reverts caused by system
      /// state changes after enqueue refund in full.
      ///
      /// `recipient_kind` / `recipient_addr` name the payout address on the
      /// target chain (flattened ChainAddress — proto messages never appear
      /// in action ABIs). The target reserve must be public: private
      /// reserves are excluded from WIRE-endpoint swaps.
      [[sysio::action]]
      void swapfromwire(name                  user,
                        uint64_t              wire_amount,
                        sysio::slug_name       dst_chain_code,
                        sysio::slug_name       dst_token_code,
                        sysio::slug_name       dst_reserve_code,
                        uint64_t              target_amount,
                        uint32_t              target_tolerance_bps,
                        opp::types::ChainKind recipient_kind,
                        std::vector<char>     recipient_addr);

      /// Drain the swap-from-WIRE queue. Inlined from `sysio.epoch::advance`
      /// each epoch (after the roster attestations are queued, before
      /// `buildenv`). Per row: re-validate the target reserve (exists,
      /// ACTIVE, not private) and the variance tolerance against the live
      /// quote — on failure, refund the user's escrowed WIRE via
      /// `reserv::refundwire` (minus `fromwire_revert_fee_bps` when the
      /// revert cause is caller-controlled) and drop the row; on success,
      /// emplace the
      /// PENDING uwreq (id = the queue row's depot-origin id) carrying a
      /// synthetic SwapRequest payload so the settlement tail
      /// (`emit_swap_remit`) can decode the recipient. NEVER throws.
      [[sysio::action]]
      void drainfwq();

      /// Read-only rollup of an underwriter's active lock total on a given
      /// `(chain_code, token_code)`. Used by off-chain consumers + (eventually)
      /// other contracts that don't rely on opreg's mirror.
      [[sysio::action, sysio::read_only]]
      uint64_t sumlocks(name underwriter,
                        sysio::slug_name chain_code,
                        sysio::slug_name token_code);

      // -----------------------------------------------------------------------
      //  Tables
      // -----------------------------------------------------------------------

      /// Auto-incrementing id-keyed primary key used by `uwreqs`.
      struct id_key {
         uint64_t id;
         uint64_t primary_key() const { return id; }
         SYSLIB_SERIALIZE(id_key, (id))
      };

      /// Per-leg lock row. Rows are pushed by `try_select_winner` and erased
      /// by `release`.
      ///
      /// The `(underwriter, chain_code, token_code)` triple is the indexing
      /// surface opreg's `available()` rollup uses (cross-contract read of
      /// `sysio::uwrit::locks_t` from `sysio.opreg`). 3 × uint64 = 192 bits
      /// exceeds `uint128_t`, so the composite is hashed into a `checksum256`
      /// via `by_underwriter_ck`. A separate `by_underwriter` split-index
      /// (uint64 keyed on `underwriter.value`) provides the cheap
      /// per-operator scan path for consumers that filter on chain / token
      /// / reserve in-memory (per plan §B.2).
      ///
      /// `reserve_code` records which specific reserve this leg covers; on
      /// a slash, the outpost routes seized collateral to that reserve via
      /// `ReserveAmount`, even when multiple reserves exist for the same
      /// `(chain_code, token_code)` pair.
      /// The `(account, chain_code, token_code)` collateral-bucket digest:
      /// the three uint64 identities packed little-endian into 24 bytes and
      /// hashed. 3 × uint64 = 192 bits does not fit `uint128_t`, so the triple
      /// is hashed to land in a `checksum256`.
      ///
      /// SINGLE SOURCE for that encoding, and it must stay that way.
      /// `lock_entry::by_underwriter_ck()` says which bucket a lock row
      /// belongs to; `lock_sum_key::primary_key()` addresses that bucket's
      /// materialized total. If the two derivations ever diverged, the rollup
      /// would be keyed differently from the rows it summarizes and every
      /// reader would silently observe zero locked — collateral already
      /// committed to a live lock would look spendable. Both call this, so
      /// they cannot diverge.
      static checksum256 compose_account_chain_token_ck(name account,
                                                        sysio::slug_name chain_code,
                                                        sysio::slug_name token_code) {
         std::array<uint8_t, 24> buf{};
         uint64_t acc_v = account.value;
         std::memcpy(buf.data() +  0, &acc_v,             8);
         std::memcpy(buf.data() +  8, &chain_code.value,  8);
         std::memcpy(buf.data() + 16, &token_code.value,  8);
         return sysio::sha256(reinterpret_cast<const char*>(buf.data()), buf.size());
      }

      struct lock_key {
         uint64_t lock_id;
         uint64_t primary_key() const { return lock_id; }
         SYSLIB_SERIALIZE(lock_key, (lock_id))
      };

      struct [[sysio::table("locks")]] lock_entry {
         uint64_t                lock_id          = 0;
         uint64_t                uwreq_id         = 0;
         name                    underwriter;
         sysio::slug_name         chain_code;
         sysio::slug_name         token_code;
         sysio::slug_name         reserve_code;
         uint64_t                amount           = 0;
         uint64_t                created_at_ms    = 0;
         /// `created_at_ms + uwconfig.collateral_lock_duration_ms`, computed
         /// at insert time in `try_select_winner`. Wall-clock — the lock is
         /// the 12h challenge window and is independent of epoch cadence
         /// (epochs can stretch when `advance` gate-blocks). Indexed via
         /// `byexpire` so `chklocks` sweeps expired locks in ascending order.
         uint64_t                expires_at_ms    = 0;

         /// Which collateral bucket this lock belongs to — see
         /// `compose_account_chain_token_ck`, the single source of that
         /// encoding, shared with `lock_sum_key::primary_key()`.
         checksum256 by_underwriter_ck() const {
            return compose_account_chain_token_ck(underwriter, chain_code, token_code);
         }
         /// Non-zero while an underwriter-fault challenge (the `sysio.chalg::uwchals` row id) is
         /// OPEN against this lock's commitment (WIRE-297). A held lock is NOT released at
         /// `expires_at_ms`: `chklocks` skips it and instead pokes the challenge's tally crank,
         /// so the collateral stays at risk until the council resolves or the challenge lapses.
         /// Stamped by `holdlocks`, cleared by `freelocks` (reject/lapse); an UPHELD challenge
         /// erases the row through `sweeplocks` instead.
         uint64_t                challenge_id     = 0;

         /// Split-index for cheap per-operator scans (plan §B.2). Callers
         /// pull all rows for a given underwriter and filter on
         /// chain_code / token_code / reserve_code in memory.
         uint64_t by_underwriter()   const { return underwriter.value; }
         uint64_t by_uwreq()         const { return uwreq_id; }
         uint64_t by_expires_at_ms() const { return expires_at_ms; }

         SYSLIB_SERIALIZE(lock_entry,
            (lock_id)(uwreq_id)(underwriter)(chain_code)(token_code)(reserve_code)
            (amount)(created_at_ms)(expires_at_ms)(challenge_id))
      };

      // Per plan §B.2: split-index approach — keep only uint64 secondary
      // indexes. `by_underwriter_ck` (checksum256) is computed on the row
      // when needed for cross-contract composite comparisons, but is NOT a
      // table-managed secondary index (Antelope KV's secondary-index
      // templates expect fixed-width integer keys). opreg's `available()`
      // rollup scans `byunderwriter` (uint64) and filters (chain_code,
      // token_code) in memory — cheap because underwriters hold O(1)
      // concurrent locks.
      using locks_t = sysio::kv::table<"locks"_n, lock_key, lock_entry,
         sysio::kv::index<"byuw"_n,
            sysio::const_mem_fun<lock_entry, uint64_t, &lock_entry::by_underwriter>>,
         sysio::kv::index<"byuwreq"_n,
            sysio::const_mem_fun<lock_entry, uint64_t, &lock_entry::by_uwreq>>,
         sysio::kv::index<"byexpire"_n,
            sysio::const_mem_fun<lock_entry, uint64_t, &lock_entry::by_expires_at_ms>>
      >;

      /// Primary key of `locksums`: one (underwriter, chain_code, token_code)
      /// collateral bucket, addressed by the SAME digest
      /// `lock_entry::by_underwriter_ck()` uses to say which bucket a lock row
      /// belongs to — both call `compose_account_chain_token_ck`.
      struct lock_sum_key {
         name             underwriter;
         sysio::slug_name chain_code;
         sysio::slug_name token_code;
         checksum256 primary_key() const {
            return compose_account_chain_token_ck(underwriter, chain_code, token_code);
         }
         SYSLIB_SERIALIZE(lock_sum_key, (underwriter)(chain_code)(token_code))
      };

      /// Materialized Σ `lock_entry.amount` for one (underwriter, chain_code,
      /// token_code) bucket — the "locked" half of `sysio.opreg::available()`.
      ///
      /// A CACHE of the `locks` table with exactly ONE writer: every code path
      /// that can change a bucket's total lives in this contract. There are
      /// THREE, and any new one inherits the same obligation — nothing
      /// structural enforces it:
      ///
      ///   * `try_select_winner` — ADDS, one lock per required leg, on a win.
      ///   * `chklocks`  — DECREMENTS, releasing locks at expiry.
      ///   * `sweeplocks` — DECREMENTS, erasing the held locks of a commitment
      ///     whose underwriter-fault challenge was UPHELD (WIRE-297). This one
      ///     runs OUTSIDE `chklocks`, which is exactly why it was missed once:
      ///     this block previously said `chklocks` was the sole erase path, and
      ///     `sweeplocks` erased rows without decrementing.
      ///
      /// Getting that wrong is permanent and silent rather than merely stale:
      /// the rollup is authoritative for `sysio.opreg::available()`, so a bucket
      /// left positive after its last row is gone suppresses that collateral
      /// forever — nothing decrements it again, because the rows that would
      /// have are already erased.
      ///
      /// A row is erased when its total reaches zero, so an absent row reads as
      /// zero and the table holds only live buckets.
      ///
      /// It exists because the derivation it replaces does not scale. Both
      /// `sum_locks_inline` rollups (here and in sysio.opreg) previously
      /// walked every lock row an underwriter held, each documenting the
      /// assumption that "per-underwriter lock counts are O(1)-ish so the scan
      /// is cheap". That is false under sustained swap traffic: locks are held
      /// for the full wall-clock challenge window
      /// (`collateral_lock_duration_ms`, 12h default) and are NEVER released
      /// by delivery, so a bucket's live lock count is
      /// (settlement rate × lock duration) — unbounded within the window. The
      /// scan ran per candidate inside `try_select_winner` (up to
      /// MAX_UWREQ_CANDIDATES of them per uwreq), i.e. inside the same
      /// consensus-dispatch CPU budget whose overrun stalls the chain.
      ///
      /// `sumlocks` reads this rollup, so it stays the cheap external answer
      /// to "how much of this bucket is locked"; the authoritative recompute
      /// is the `locks` table itself, which the contract tests scan and
      /// compare against this total.
      struct [[sysio::table("locksums")]] lock_sum {
         name             underwriter;
         sysio::slug_name chain_code;
         sysio::slug_name token_code;
         uint64_t         amount = 0;
         SYSLIB_SERIALIZE(lock_sum, (underwriter)(chain_code)(token_code)(amount))
      };

      using locksums_t = sysio::kv::table<"locksums"_n, lock_sum_key, lock_sum>;

      /// Per-underwriter race entry inside an UWREQ row. Tracks when each
      /// leg of a dual-COMMIT pair arrived so `try_select_winner` can
      /// resolve the race deterministically. Each leg's COMMIT is an
      /// independent attestation with its own chain_code + uw_ext_chain_addr
      /// (signed external-chain signer metadata for that leg) + signature over
      /// the whole UIC. The depot stores the full UIC bytes per leg so
      /// `try_select_winner` can reconstruct the signed digest verbatim and
      /// verify a canonical fixed-size recoverable K1, R1, EM, or ED signature
      /// against the underwriter's WIRE account `active` or `owner` permission.
      ///
      /// `commit_entry` does NOT carry codenames — the per-leg
      /// `(chain_code, token_code, reserve_code)` identity is on the
      /// surrounding `uw_request_t::src_*` / `dst_*` fields; the
      /// commit_entry slot is solely a race-tracker.
      struct commit_entry {
         name      underwriter;
         /// Source-leg COMMIT. `source_uic_bytes` is the verbatim zpp_bits
         /// serialization of the `UnderwriteIntentCommit` proto received from
         /// the source-side outpost; the bytes include the underwriter's
         /// signature in the `signature` field. Empty until the source-leg
         /// arrives.
         uint64_t          source_received_at_ms = 0;
         uint64_t          source_outpost_id     = 0;
         std::vector<char> source_uic_bytes;
         /// Destination-leg COMMIT. Same shape, populated when the dest-side
         /// outpost's relay arrives.
         uint64_t          dest_received_at_ms   = 0;
         uint64_t          dest_outpost_id       = 0;
         std::vector<char> dest_uic_bytes;
         /// Race outcome — INTENT_SUBMITTED (initial/retryable),
         /// INTENT_CONFIRMED (winner), DISQUALIFIED (durably invalid stored
         /// evidence, such as a signature invalidated by key rotation), or
         /// RELEASED (clean loser, retained for audit). A new matching
         /// `rcrdcommit` cannot rewrite or re-arm a DISQUALIFIED entry. An
         /// exact replay can re-evaluate only an INTENT_SUBMITTED entry. The
         /// reused protobuf enum also contains SLASHED, but commit entries
         /// never write that value; economic slash state belongs to lock and
         /// operator settlement.
         opp::types::UnderwriteStatus status = opp::types::UNDERWRITE_STATUS_INTENT_SUBMITTED;
         std::string reason;

         SYSLIB_SERIALIZE(commit_entry,
            (underwriter)
            (source_received_at_ms)(source_outpost_id)(source_uic_bytes)
            (dest_received_at_ms)(dest_outpost_id)(dest_uic_bytes)
            (status)(reason))
      };

      /// UWREQ row — one per inbound SWAP attestation. Tracks the swap's
      /// src/dst pairs, the underwriter race, and the eventual settlement.
      ///
      /// Each side of the swap carries a full `(chain_code, token_code,
      /// reserve_code)` triple per the v6 data-model refactor: identity
      /// is slug_name-keyed throughout, and `reserve_code` lets a same-
      /// `(chain, token)` swap target a specific reserve when multiple
      /// reserves exist for that pair.
      struct [[sysio::table("uwreqs")]] uw_request_t {
         uint64_t                                id;
         opp::types::AttestationType             type;
         opp::types::UnderwriteRequestStatus     status;

         /// Src / dst of the cross-chain swap. Populated by `createuwreq`
         /// from the decoded SwapRequest. Used by `try_select_winner` to
         /// validate per-leg bond coverage.
         ///
         /// `dst_amount` IS the AMM quote — the destination amount the
         /// underwriter must deliver and the amount `sysio.reserv` debits at
         /// settlement. It is **never** the caller's `SwapRequest.target_amount`
         /// (WNS-02): the target is an unauthenticated expectation carried over
         /// OPP, and paying it out let a caller name any figure and drain the
         /// destination reserve. `createuwreq` / `drainfwq` seed it with the
         /// quote at ingestion; `try_select_winner` re-prices on the live curve
         /// and overwrites it with the price the books actually move at, after
         /// checking the drift against `variance_tolerance_bps`.
         sysio::slug_name                         src_chain_code;
         sysio::slug_name                         src_token_code;
         sysio::slug_name                         src_reserve_code;
         uint64_t                                src_amount        = 0;
         sysio::slug_name                         dst_chain_code;
         sysio::slug_name                         dst_token_code;
         sysio::slug_name                         dst_reserve_code;
         uint64_t                                dst_amount        = 0;
         /// The destination amount the caller ASKED for — `SwapRequest.target_amount`
         /// (outpost-originated) or `fromwire_q::target_amount` (swap-from-WIRE).
         ///
         /// This is an expectation, never an instruction: it is the fixed
         /// reference the slippage check measures against, and it is NEVER paid
         /// out. Settlement uses `dst_amount` (the AMM quote) exclusively.
         ///
         /// It is retained on the row precisely so `try_select_winner` can
         /// compare the LIVE settlement quote against the user's ORIGINAL bound.
         /// Comparing instead against the previous quote (what `dst_amount`
         /// holds before the race re-prices) would compound the tolerance across
         /// the two checkpoints — a 10% tolerance accepting a 91 quote at
         /// ingestion and an 83 quote at settlement, 17% below a target of 100 —
         /// and would skip the bound entirely on a row created while no LP was
         /// provisioned (`dst_amount == 0`).
         uint64_t                                target_amount     = 0;
         /// Variance tolerance the user accepted at SWAP_REQUEST time, in
         /// basis points (50 = 0.5%). The allowance it produces is a fraction
         /// of the **AMM quote**, never of the user's `target_amount` (WNS-02)
         /// — see `variance_allowance` in the implementation — and is clamped
         /// to 100%. The depot's createuwreq path validates the ingestion quote
         /// against it; `try_select_winner` re-validates the live quote at
         /// race-resolution time so drift between ingestion and race doesn't
         /// silently move the settlement price past what the user accepted.
         uint32_t                                variance_tolerance_bps = 0;

         /// Source-chain id of the deposit transaction that funded this
         /// swap (ETH: 32-byte tx hash; SOL: 64-byte signature). Used by
         /// the off-chain underwriter plugin's `verify_source_deposit`
         /// step to confirm a real on-chain deposit backs the swap
         /// before committing collateral. `createuwreq` rejects any
         /// SwapRequest with an empty `source_tx_id` (emits SwapRevert
         /// for refund) — every outpost must populate this field at
         /// swap-emit time.
         std::vector<char>                       source_tx_id;

         /// Depositor's address on the source chain (decoded from
         /// `SwapRequest.actor.address`). ETH = 20 bytes (left-padded in
         /// 32-byte ABI slots when matched); SOL = 32-byte Ed25519
         /// pubkey. The underwriter plugin matches this against the
         /// `tx.from` (ETH) / fee-payer (SOL) of the source-deposit tx
         /// during verification.
         std::vector<char>                       depositor;

         /// Race state.
         std::vector<commit_entry>               commits_by;
         name                                    winner;
         uint64_t                                committed_at_ms   = 0;
         uint64_t                                settled_at_ms     = 0;
         /// The row's lifecycle deadline, evaluated (lazily, at trigger time)
         /// by `pruneuwreqs` via the `byexpire` index. Semantics by status:
         ///   * PENDING   — `creation_epoch + uwconfig.uwreq_pending_timeout_epochs`;
         ///                 past it the race is abandoned (refund + EXPIRED).
         ///   * CONFIRMED — 0 (no deadline): the wall-clock lock window owns
         ///                 the row; `chklocks` always terminalizes it.
         ///   * terminal  — `terminal_epoch + uwconfig.uwreq_retention_epochs`;
         ///                 past it the row is erased.
         /// 0 means "no deadline, never swept" (same zero-sentinel predicate
         /// as `sysio.dclaim::flushexpired`).
         uint32_t                                expires_at_epoch  = 0;

         /// Inbound attestation payload (zpp_bits-encoded protobuf).
         std::vector<char>                       attestation_inbound_data;

         /// Outbound attestation payload reserved for future flows where
         /// uwrit emits its own response (e.g. underwriter intent acks).
         /// Empty until that flow lands.
         std::vector<char>                       attestation_outbound_data;

         /// Non-zero while an underwriter-fault challenge (the `sysio.chalg::uwchals` row id)
         /// is OPEN against this request's commitment — stamped by `holdlocks`, cleared by
         /// `freelocks`, alongside the identical marker on each held lock.
         ///
         /// `pruneuwreqs` refuses to erase a row carrying one. The lock marker alone is not
         /// enough: the locks hold the COLLATERAL, but this row holds the EVIDENCE a challenge
         /// adjudicates against — `attestation_inbound_data` (the stored SwapRequest),
         /// `source_tx_id`, and `commits_by` with the per-leg UIC bytes. Terminal rows are
         /// erased at `terminal_epoch + uwreq_retention_epochs`, which is far shorter than the
         /// challenge window, so without this a challenge could outlive the record it exists
         /// to adjudicate and resolve against nothing.
         uint64_t                                challenge_id      = 0;

         uint64_t by_status() const { return magic_enum::enum_integer(status); }
         uint64_t by_winner() const { return winner.value; }
         uint64_t by_expires_at_epoch() const { return expires_at_epoch; }

         SYSLIB_SERIALIZE(uw_request_t,
            (id)(type)(status)
            (src_chain_code)(src_token_code)(src_reserve_code)(src_amount)
            (dst_chain_code)(dst_token_code)(dst_reserve_code)(dst_amount)
            (target_amount)(variance_tolerance_bps)(source_tx_id)(depositor)
            (commits_by)(winner)(committed_at_ms)(settled_at_ms)(expires_at_epoch)
            (attestation_inbound_data)(attestation_outbound_data)(challenge_id))
      };

      using uwreqs_t = sysio::kv::table<"uwreqs"_n, id_key, uw_request_t,
         sysio::kv::index<"bystatus"_n,
            sysio::const_mem_fun<uw_request_t, uint64_t, &uw_request_t::by_status>>,
         sysio::kv::index<"bywinner"_n,
            sysio::const_mem_fun<uw_request_t, uint64_t, &uw_request_t::by_winner>>,
         sysio::kv::index<"byexpire"_n,
            sysio::const_mem_fun<uw_request_t, uint64_t, &uw_request_t::by_expires_at_epoch>>
      >;

      /// Singleton holding the next-issued `lock_id` + the depot-origin
      /// swap-from-WIRE sequence. Keeps both auto-increments monotonic
      /// across action calls.
      struct [[sysio::table("uwcounters")]] uw_counters {
         uint64_t next_lock_id = 1;
         /// Sequence for depot-originated (swap-from-WIRE) uwreq ids. The
         /// issued id is `0x8000000000000000 | seq` — the high bit
         /// partitions the depot-origin id space away from inbound
         /// attestation ids (msgch's `mint_att_id` counts monotonically
         /// from 1 and can never reach 2^63), so `fwqueue` row ids double
         /// as collision-free uwreq ids.
         uint64_t next_fromwire_seq = 0;
         SYSLIB_SERIALIZE(uw_counters, (next_lock_id)(next_fromwire_seq))
      };

      using uwcounters_t = sysio::kv::global<"uwcounters"_n, uw_counters>;

      /// Queued swap-from-WIRE request. Written by `swapfromwire` (which
      /// escrows the user's WIRE in the same transaction), drained by
      /// `drainfwq` on the next `sysio.epoch::advance`. The row id is the
      /// depot-origin uwreq id the drained request will be created under.
      struct fw_key {
         uint64_t id;
         uint64_t primary_key() const { return id; }
         SYSLIB_SERIALIZE(fw_key, (id))
      };

      struct [[sysio::table("fwqueue")]] fromwire_q {
         uint64_t              id = 0;
         name                  user;
         uint64_t              wire_amount = 0;
         sysio::slug_name       dst_chain_code;
         sysio::slug_name       dst_token_code;
         sysio::slug_name       dst_reserve_code;
         uint64_t              target_amount = 0;
         uint32_t              variance_tolerance_bps = 0;
         opp::types::ChainKind recipient_kind = opp::types::CHAIN_KIND_UNKNOWN;
         std::vector<char>     recipient_addr;
         uint32_t              created_at_epoch = 0;

         uint64_t by_epoch() const { return created_at_epoch; }

         SYSLIB_SERIALIZE(fromwire_q,
            (id)(user)(wire_amount)
            (dst_chain_code)(dst_token_code)(dst_reserve_code)
            (target_amount)(variance_tolerance_bps)
            (recipient_kind)(recipient_addr)(created_at_epoch))
      };

      using fwqueue_t = sysio::kv::table<"fwqueue"_n, fw_key, fromwire_q,
         sysio::kv::index<"byepoch"_n,
            sysio::const_mem_fun<fromwire_q, uint64_t, &fromwire_q::by_epoch>>
      >;

      /// Fee + lock-duration configuration singleton. `fee_bps` is the per-spoke
      /// NETWORK swap fee, charged out of the WIRE leg — the reserve owner fees
      /// charged alongside it live on their own reserve rows
      /// (`sysio.reserv::reserve::owner_fee_bps`), not here. No fee-DISTRIBUTION
      /// shares live here either: the underwriter/rewards split of the network
      /// fee is fixed in `sysio.reserv` (FEE_UNDERWRITER_SHARE_BPS), and the
      /// rewards pool's own emissions share is that contract's governance dial
      /// (`reserve_config.fee_emissions_share_bps`).
      struct [[sysio::table("uwconfig")]] uw_config {
         uint32_t fee_bps                      = 10;           // 0.1% per spoke
         /// Wall-clock collateral lock duration — the challenge window.
         /// Locks are NEVER released by delivery; they expire this many ms
         /// after creation and are swept by `chklocks` at epoch advance.
         /// Default 12 hours.
         uint64_t collateral_lock_duration_ms  = 43'200'000;
         /// Minimum WIRE escrow accepted by `swapfromwire` (atomic units,
         /// 9 decimals) — the queue-slot price floor.
         uint64_t min_fromwire_amount          = DEFAULT_MIN_FROMWIRE_AMOUNT;
         /// Fee (bps of the escrow) on caller-controlled drain-time reverts,
         /// routed like the settlement fee. <= MAX_FEE_BPS.
         uint32_t fromwire_revert_fee_bps      = DEFAULT_FROMWIRE_REVERT_FEE_BPS;
         /// Epochs a PENDING uwreq may wait for its underwriter race before
         /// `pruneuwreqs` expires it (refund/revert + EXPIRED). The deadline
         /// is stamped per row at creation; retuning is not retroactive.
         uint32_t uwreq_pending_timeout_epochs = DEFAULT_UWREQ_PENDING_TIMEOUT_EPOCHS;
         /// Epochs a terminal (COMPLETED / REJECTED / EXPIRED) uwreq row is
         /// retained for audit before `pruneuwreqs` erases it. Stamped per
         /// row at the terminal transition; retuning is not retroactive.
         uint32_t uwreq_retention_epochs       = DEFAULT_UWREQ_RETENTION_EPOCHS;
         SYSLIB_SERIALIZE(uw_config, (fee_bps)(collateral_lock_duration_ms)
                                     (min_fromwire_amount)(fromwire_revert_fee_bps)
                                     (uwreq_pending_timeout_epochs)(uwreq_retention_epochs))
      };

      using uwconfig_t = sysio::kv::global<"uwconfig"_n, uw_config>;

   private:

      using UnderwriteRequestStatus = opp::types::UnderwriteRequestStatus;
      using UnderwriteStatus        = opp::types::UnderwriteStatus;
      using ChainKind               = opp::types::ChainKind;
      using AttestationType         = opp::types::AttestationType;
   };

} // namespace sysio

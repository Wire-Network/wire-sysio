#pragma once

#include <sysio/sysio.hpp>
#include <sysio/kv_global.hpp>
#include <sysio/kv_table.hpp>
#include <sysio/asset.hpp>
#include <sysio/crypto.hpp>
#include <sysio/system.hpp>
#include <sysio/privileged.hpp>
#include <sysio/opp/types/types.pb.hpp>
#include <sysio/opp/attestations/attestations.pb.hpp>
#include <sysio.opp.common/slug_name.hpp>
#include <sysio.opp.common/opp_table_types.hpp>

#include <limits>

#include <cstdint>
#include <limits>

namespace sysio {

   /**
    * @brief sysio.reserv — reserve registry with create→match→ready handshake.
    *
    * Per the v6 data-model refactor:
    *
    * - Reserve primary key is the triple `(chain_code, token_code, code)`
    *   (all codenames). Composite stored as `checksum256(chain || token || code)`.
    *
    * - `ReserveStatus` proto enum (`PENDING` / `ACTIVE` / `CANCELLED`) replaces
    *   the prior `active: bool`.
    *
    * - **Bootstrap path** (`current_epoch_index == 0`): `regreserve(...)` is
    *   priv-gated and inserts a row with `status=ACTIVE` inline. No `matchreserve`
    *   needed.
    *
    * - **Post-bootstrap path**: users call `create_reserve(...)` on outposts; the
    *   outpost queues a `RESERVE_CREATE` attestation; sysio.msgch dispatches
    *   `oncrtreserve(...)` which inserts a row with `status=PENDING`. Any WIRE
    *   account then calls `matchreserve(...)` putting up `requested_wire_amount`
    *   WIRE — `sysio.reserv` takes custody, status flips to `ACTIVE`, and a
    *   `RESERVE_READY` is queued back to the outpost.
    *
    * - **Cancel path**: creator calls `cancel_create_reserve(...)` on the outpost.
    *   `RESERVE_CREATE_CANCEL` flows; sysio.msgch dispatches `oncnclrsv(...)`. If
    *   `status == PENDING`, set `CANCELLED` + queue `RESERVE_CREATE_CANCELLED`.
    *   Else silent no-op (race lost — match landed first;
    *   feedback_opp_handlers_never_throw applies).
    */
   class [[sysio::contract("sysio.reserv")]] reserve : public contract {
   public:
      using contract::contract;

      // Well-known accounts
      static constexpr name MSGCH_ACCOUNT  = "sysio.msgch"_n;
      static constexpr name UWRIT_ACCOUNT  = "sysio.uwrit"_n;
      static constexpr name TOKEN_ACCOUNT  = "sysio.token"_n;
      static constexpr name EPOCH_ACCOUNT  = "sysio.epoch"_n;
      static constexpr name AUTHEX_ACCOUNT = "sysio.authex"_n;
      static constexpr name CHAINS_ACCOUNT = "sysio.chains"_n;
      /// The `sysio` account holds the WIRE emissions treasury; bootstrap
      /// `regreserve` drains the reserve's WIRE backing from it.
      static constexpr name TREASURY_ACCOUNT = "sysio"_n;

      // Bancor connector_weight is stored in basis points (10000 = 100%) and is
      // the WIRE-side weight of the token/WIRE pool; the token side gets the
      // remainder. weight = 5000 is the symmetric 50/50 case (pure constant
      // product). The weighted swap curve lives in `sysio.opp.common/amm_math.hpp`.
      //
      // Max is 9999, not 10000: both pool-side weights must be positive. At cw = 10000 the
      // token-side weight (10000 - cw) is 0, so amm_math::out_given_in returns 0 for every
      // token<->WIRE swap — a permanently unswappable (dead) reserve. Capping below the total
      // keeps the token side >= 1 bps.
      static constexpr uint32_t MAX_CONNECTOR_WEIGHT_BPS     = 9999;
      static constexpr uint32_t DEFAULT_CONNECTOR_WEIGHT_BPS = 5000;

      // The WIRE token's decimal precision — the depot frame's reference side,
      // and the cap for any source-token-side depot precision. WIRE custody is
      // an `asset{"WIRE", 9}`. The WIRE/target side is always this value, so it
      // is never carried per-reserve (only `source_token_precision` is stored);
      // this constant is the write/validation precision for every reserve.
      static constexpr uint32_t WIRE_PRECISION = 9;

      // Swap-fee split, stage 1. Every swap charges sysio.uwrit's `fee_bps` out
      // of the WIRE leg; this contract gives half of the collected fee to the
      // swap's WINNING UNDERWRITER (the party whose collateral lock let the swap
      // settle) and puts the other half into a REWARDS POOL. The fee RATE
      // (`fee_bps`) is owned by sysio.uwrit.
      static constexpr uint32_t FEE_UNDERWRITER_SHARE_BPS = 5000; // 50% underwriter / 50% rewards pool
      static constexpr uint32_t FEE_SPLIT_TOTAL_BPS       = 10000;

      // Swap-fee split, stage 2 — the rewards pool's own division, GOVERNANCE
      // CONFIGURABLE (`reserve_config.fee_emissions_share_bps`). This share of
      // the pool is transferred to the `sysio` emissions treasury; the remainder
      // accrues to `rewards_bucket` for batch operators via
      // `sysio.system::payepoch`.
      //
      // Nothing seeds the `reservcfg` row: bootstrap does not write it, and every
      // read goes through `get_or_default(reserve_config{})`, so the dial reads
      // as zero until the self-authorized `setconfig` first persists a row.
      //
      // The DEFAULT IS ZERO: the whole pool is allocated to the batch-operator
      // distribution and no part of a swap fee leaves this contract's custody at
      // settlement. (`payepoch` then pays only eligible shares — see
      // `drainrewards`.) A non-zero share re-opens a treasury inflow without
      // touching the underwriter half.
      static constexpr uint32_t DEFAULT_FEE_EMISSIONS_SHARE_BPS = 0;

      // Reserve OWNER fee bounds (WIRE-281). A reserve's owner fee is a second,
      // independent fee on the WIRE leg — it is NOT a share of the network fee.
      //
      // 0 means "this reserve charges nothing" and is always legal (every
      // reserve starts there, and an owner-less bootstrap reserve can never be
      // anything else — `setrsvfee` requires the owner's authority). A reserve
      // that DOES charge must land in [MIN_OWNER_FEE_BPS, MAX_OWNER_FEE_BPS]:
      // below the floor the fee floors to zero on ordinary amounts and is just a
      // misconfiguration, and the 99% ceiling keeps a positive remainder on the
      // leg (Jonathan, 2026-08-04).
      static constexpr uint32_t MIN_OWNER_FEE_BPS = 1;    // 0.01%
      static constexpr uint32_t MAX_OWNER_FEE_BPS = 9900; // 99%

      // Claimable-WIRE retention. `refundwire` / `paywire` credit an unbounded, caller-influenced
      // set of accounts, and the rows bill to the sysio RAM pool, so an abandoned dust row would
      // otherwise occupy system-paid RAM forever. A row not pulled within this window is swept and
      // its balance returns to the emissions treasury.
      //
      // One year is deliberately far longer than any plausible claim latency: forfeiture is a RAM
      // backstop, not an economic lever. (Contrast `sysio.dclaim::claim_window_sec`, which is
      // governance-tunable because staker windows ARE policy.) Promote this to config if that ever
      // changes; a constant keeps the swap-settlement surface unchanged in the meantime.
      static constexpr uint32_t WIRE_CLAIM_WINDOW_SEC = 365 * 24 * 60 * 60;

      // Rows swept per credit. Bounds the on-write retention sweep so a settlement action reached
      // from `sysio.epoch::advance` (via `drainfwq`) stays inside its CPU deadline, the same shape
      // `sysio.opreg::prune_dellog` uses.
      static constexpr uint32_t MAX_CLAIM_SWEEP_PER_CREDIT = 4;

      // Rows swept per epoch by `sweepclaims`, which `sysio.epoch::advance` inlines. Larger than
      // the per-credit budget because this is the trigger that has to actually drain a backlog:
      // the on-write sweep only fires while settlement traffic arrives, so if swaps stop, this is
      // the only thing that revisits an aged-out row. Sized like the other advance-inlined bounds
      // (`MAX_LOCK_RELEASE_PER_EPOCH`, `MAX_WTDW_FLUSH_PER_EPOCH`) to stay well inside advance's
      // hard, uncatchable CPU deadline; an oversized backlog simply drains across later epochs.
      static constexpr uint32_t MAX_CLAIM_SWEEP_PER_EPOCH = 32;

      // -----------------------------------------------------------------------
      //  Actions
      // -----------------------------------------------------------------------

      /// Bootstrap-window only. Insert a reserve row directly with
      /// `status=ACTIVE`. Priv-gated; rejects when `current_epoch_index > 0`.
      ///
      /// Custody: drains `initial_wire_amount` real WIRE from the `sysio`
      /// emissions treasury into this contract's balance so the row's
      /// `reserve_wire_amount` is backed from the moment it is usable.
      /// `is_private` / `owner` let the bootstrap seed privately-owned
      /// reserves directly (post-bootstrap creation derives ownership from
      /// the authex-linked matcher instead).
      [[sysio::action]]
      void regreserve(sysio::slug_name chain_code,
                      sysio::slug_name token_code,
                      sysio::slug_name reserve_code,
                      std::string     name,
                      std::string     description,
                      uint64_t        initial_chain_amount,
                      uint64_t        initial_wire_amount,
                      uint32_t        source_token_precision,
                      uint32_t        connector_weight_bps,
                      bool            is_private,
                      sysio::name     owner);

      /// Dispatched by sysio.msgch when a RESERVE_CREATE attestation arrives.
      /// Inserts a row with `status=PENDING`. **NEVER throws** —
      /// per feedback_opp_handlers_never_throw: duplicate / malformed records
      /// are silently logged + skipped.
      ///
      /// `external_token_amount` arrives in the depot's canonical 9-decimal
      /// frame — msgch unpacks it from the attestation's `ReserveAmount`
      /// carrier; the outpost converted its chain-native escrow units at the
      /// boundary (EVM `PrecisionLib.toDepot`, SVM `precision::to_depot`)
      /// exactly like the swap paths.
      ///
      /// Create gating: the creator must be `authex`-linked to a WIRE
      /// account. The creator's pubkey (EVM: `creator_pub_key`, 33-byte
      /// compressed, outpost-verified against msg.sender; SVM:
      /// `creator_chain_addr` already IS the 32-byte ed25519 key) is
      /// resolved against `sysio.authex::links.bypubkey`. No link → the
      /// request is rejected by queueing RESERVE_CREATE_CANCELLED back to
      /// the outpost (which refunds the creator) and NO row is inserted.
      [[sysio::action]]
      void oncrtreserve(sysio::slug_name       chain_code,
                        sysio::slug_name       token_code,
                        sysio::slug_name       reserve_code,
                        std::string           name,
                        std::string           description,
                        uint64_t              external_token_amount,
                        uint64_t              requested_wire_amount,
                        uint32_t              source_token_precision,
                        uint32_t              connector_weight_bps,
                        opp::types::ChainKind creator_chain_kind,
                        std::vector<char>     creator_chain_addr,
                        bool                  is_private,
                        std::vector<char>     creator_pub_key);

      /// Auth = matcher. Takes REAL WIRE custody of `wire_amount` (must
      /// exactly equal `reserve.requested_wire_amount`) via an inline
      /// `sysio.token::transfer(matcher → sysio.reserv)`, flips status to
      /// ACTIVE, records `owner = matcher`, queues RESERVE_READY outbound.
      ///
      /// Match gating: `matcher` MUST be the WIRE account `authex`-linked
      /// to the reserve's creator — `links.bynamechain[(matcher, chain
      /// kind)]`'s pubkey bytes must equal the creator pubkey recorded at
      /// create time. The match IS the WIRE deposit; the matcher becomes
      /// the reserve's owner (the ownership a private reserve's swap
      /// gating compares against).
      [[sysio::action]]
      void matchreserve(sysio::slug_name chain_code,
                        sysio::slug_name token_code,
                        sysio::slug_name reserve_code,
                        name            matcher,
                        uint64_t        wire_amount);

      /// Dispatched by sysio.msgch when a RESERVE_CREATE_CANCEL attestation
      /// arrives. If `status==PENDING`, flip to CANCELLED + queue
      /// RESERVE_CREATE_CANCELLED. Else: silent no-op (match won the race).
      /// **NEVER throws.**
      [[sysio::action]]
      void oncnclrsv(sysio::slug_name       chain_code,
                     sysio::slug_name       token_code,
                     sysio::slug_name       reserve_code,
                     opp::types::ChainKind creator_chain_kind,
                     std::vector<char>     creator_chain_addr);

      /// Read-only swap quote. Prices `from_amount` of the source reserve into
      /// the destination reserve along the depot's live curve — the SAME
      /// weighted-Bancor math (each reserve's `connector_weight_bps`) and the
      /// SAME post-fee reduction that settlement uses, so the quote equals what a
      /// swap would deliver.
      ///
      /// The fee priced here is the network fee (`sysio.uwrit::fee_bps`) PLUS
      /// every participating non-WIRE leg's reserve `owner_fee_bps` — both sides
      /// on a chain-to-chain swap, one side against a WIRE endpoint (a WIRE
      /// endpoint has no reserve and charges no owner fee). Callers drive
      /// ingestion and race-time variance checks off this value, so it must
      /// account for every rate settlement will charge; a quote that priced only
      /// `fee_bps` would drift from the books by each owner fee.
      /// Handles WIRE endpoints: a WIRE source/destination skips that leg's
      /// reserve (the depot IS the WIRE side). Returns 0 when a required reserve
      /// is missing or not ACTIVE (callers treat 0 as "no quote").
      [[sysio::action, sysio::read_only]]
      uint64_t swapquote(sysio::slug_name from_chain_code,
                         sysio::slug_name from_token_code,
                         sysio::slug_name from_reserve_code,
                         uint64_t        from_amount,
                         sysio::slug_name to_chain_code,
                         sysio::slug_name to_token_code,
                         sysio::slug_name to_reserve_code);

      /// Read-only: current rewards-bucket WIRE balance (the batch-operator share
      /// of collected swap fees, held in this contract's custody until
      /// `drainrewards` sweeps it to the emissions treasury for distribution to
      /// batch operators).
      [[sysio::action, sysio::read_only]]
      uint64_t rewardbal();

      /// Auth = `sysio` (the emissions treasury / system account). Sweep `amount`
      /// WIRE of accrued swap-fee rewards out of this contract's custody to the
      /// `sysio` treasury, where `sysio.system::payepoch` allocates it EXCLUSIVELY
      /// to the batch-operator distribution, on top of their emission share.
      /// Producers are not paid out of swap fees — the fee compensates the parties
      /// that carry an individual swap (the winning underwriter, whose share never
      /// enters this bucket, and the batch operators that relay it). Allocated is
      /// not the same as paid: payepoch pays only ELIGIBLE shares, and whatever it
      /// skips (no groups at all, an EMPTY group holding positive epochs,
      /// non-ACTIVE members, or the remainders of its two integer divisions) stays
      /// in the treasury. Called
      /// inline by payepoch with the amount it read from `rewardbal()`, so the
      /// swept WIRE lands in the treasury before payepoch's payout transfers
      /// execute (inline actions run depth-first, drain queued before payouts).
      ///
      /// Decrements `rewards_bucket.balance` by `amount`; `lifetime_accrued`
      /// (an audit total) is left untouched. A non-positive `amount` is REJECTED
      /// (throws — an internal sweep asking for <= 0 is a caller bug, not a
      /// no-op), as is an `amount` exceeding the live balance.
      [[sysio::action]]
      void drainrewards(int64_t amount);

      /// Auth=sysio.uwrit. Inline-debit at SWAP_REMIT emit time. Asserts the
      /// reserve is ACTIVE and balance is sufficient.
      [[sysio::action]]
      void debit(sysio::slug_name chain_code,
                 sysio::slug_name token_code,
                 sysio::slug_name reserve_code,
                 uint64_t        amount);

      // onreject was removed: no SwapRejected attestation exists — every
      // depot-initiated REMIT is paid by the destination outpost (no rejection,
      // so no reserve-ledger reconciliation is needed).

      // onreward was removed: the v6 STAKING_REWARD path credits the per-staker
      // reward to sysio.dclaim directly (already WIRE-denominated), so there is
      // no reserve leg and no reserve-side reward crediting.

      /// Auth=sysio.uwrit. Emit-time apply for a normal (outpost ↔ outpost)
      /// swap, fired from `try_select_winner` BEFORE the SWAP_REMIT is queued so
      /// every intervening quote prices post-swap. Computes the GROSS WIRE
      /// intermediate internally from the pre-mutation source row on the weighted
      /// curve (the source reserve's own `connector_weight_bps`) —
      /// `w_gross = amm::token_to_wire(src.chain, src.wire, src.cw, src_amount)`
      /// — takes the swap fee out of that WIRE leg, then:
      ///   src: chain += src_amount, wire -= w_gross
      ///   dst: wire  += w_net,      chain -= dst_amount   (w_net = w_gross - fee)
      /// `fee` is the TOTAL taken off that leg: BOTH reserves' `owner_fee_bps`
      /// (each accrued to its own reserve row's `owner_fee_accrued`) PLUS the
      /// network fee. Only the NETWORK component is split 50/50 to
      /// `underwriter`'s claimable accrual / the rewards pool — and any
      /// configured `fee_emissions_share_bps` of that pool is TRANSFERRED to the
      /// `sysio` treasury rather than accruing to `rewards_bucket`.
      /// Balances are checked BEFORE any mutation; a failed check
      /// aborts the surrounding race-resolution transaction (no half-state). `Σ
      /// reserve_wire_amount` drops by the whole fee; all of it stays in this
      /// contract's custody as the three accruals EXCEPT that emissions share,
      /// which is the only part that leaves.
      ///
      /// `underwriter` is the uwreq's winning underwriter, forwarded by
      /// `sysio.uwrit::try_select_winner`.
      [[sysio::action]]
      void applyswap(sysio::slug_name src_chain_code,
                     sysio::slug_name src_token_code,
                     sysio::slug_name src_reserve_code,
                     uint64_t        src_amount,
                     sysio::slug_name dst_chain_code,
                     sysio::slug_name dst_token_code,
                     sysio::slug_name dst_reserve_code,
                     uint64_t        dst_amount,
                     sysio::name     underwriter);

      /// Auth=sysio.uwrit. Emit-time apply for a swap-FROM-WIRE (the depot
      /// is the source; only the target outpost leg exists). The user's
      /// escrowed WIRE (already held by this contract since `swapfromwire`) has
      /// the swap fee taken out of it; only the post-fee remainder becomes the
      /// target reserve's WIRE-side liquidity:
      ///   dst: wire += w_net, chain -= dst_amount   (w_net = wire_in - fee)
      /// `fee` is the TOTAL taken off the leg: the DESTINATION reserve's
      /// `owner_fee_bps` (accrued to that row; there is no source reserve) PLUS
      /// the network fee. Only the NETWORK component is split 50/50 to
      /// `underwriter`'s claimable accrual / the rewards pool, and any configured
      /// `fee_emissions_share_bps` of that pool is TRANSFERRED to the `sysio`
      /// treasury. The escrowed `wire_in` splits into the new liquidity plus the
      /// routed fee; every part stays in custody except that emissions share, so
      /// custody balances once it is accounted for.
      ///
      /// `underwriter` is the uwreq's winning underwriter, forwarded by
      /// `sysio.uwrit::try_select_winner`.
      [[sysio::action]]
      void applyfromwire(sysio::slug_name dst_chain_code,
                         sysio::slug_name dst_token_code,
                         sysio::slug_name dst_reserve_code,
                         uint64_t        wire_in,
                         uint64_t        dst_amount,
                         sysio::name     underwriter);

      /// Auth=sysio.uwrit. Settlement for a swap-TO-WIRE (the depot is the
      /// target; only the source outpost leg exists). Pays the recipient exactly
      /// `wire_out` REAL WIRE from custody, and charges the swap fee on the gross
      /// weighted WIRE leg the source produces:
      ///   src: chain += src_amount, wire -= (wire_out + fee)
      ///   credit `wire_out` to the recipient's `wireclaims` row (pulled via `claimwire`)
      /// `fee` is the TOTAL taken off the leg: the SOURCE reserve's
      /// `owner_fee_bps` (accrued to that row; the recipient is paid in WIRE, so
      /// there is no destination reserve) PLUS the network fee. Only the NETWORK
      /// component is split 50/50 to `underwriter`'s claimable accrual / the
      /// rewards pool, and any configured `fee_emissions_share_bps` of that pool
      /// is TRANSFERRED to the `sysio` treasury. The source reserve keeps any
      /// surplus when the user targeted below the post-fee quote. `Σ
      /// reserve_wire_amount` drops by `wire_out + fee`; what leaves custody is
      /// that emissions share alone — `wire_out` stays in custody as a claim.
      ///
      /// `underwriter` is the uwreq's winning underwriter, forwarded by
      /// `sysio.uwrit::try_select_winner`.
      ///
      /// The payout is CREDITED, not transferred: this action is inlined from
      /// `sysio.uwrit::try_select_winner` inside the never-throw
      /// `deliver -> evalcons -> dispatch` chain, and `sysio.token::transfer`
      /// notifies `recipient`. A recipient whose notify handler asserts (or burns
      /// CPU) would otherwise abort the consensus-tipping delivery and stall
      /// dispatch. The WIRE stays in this contract's custody until claimed.
      [[sysio::action]]
      void paywire(sysio::slug_name src_chain_code,
                   sysio::slug_name src_token_code,
                   sysio::slug_name src_reserve_code,
                   uint64_t        src_amount,
                   sysio::name     recipient,
                   uint64_t        wire_out,
                   sysio::name     underwriter);

      /// Auth=sysio.uwrit. Refund escrowed WIRE to a swap-FROM-WIRE user
      /// whose queued request failed drain-time validation (reserve
      /// missing / not ACTIVE / private / variance drift) or whose uwreq was
      /// rejected. Touches no reserve row — the escrow was never credited to
      /// any `reserve_wire_amount`.
      ///
      /// `revert_fee_bps` is the caller-fault revert fee: the recipient gets
      /// `wire_amount` minus the fee, and the fee routes through the standard
      /// `route_wire_fee` path. A revert has no winning underwriter — nobody
      /// locked collateral for a swap that never settled — so the underwriter
      /// share is zero and the WHOLE revert fee becomes the rewards POOL. That
      /// pool is then split by `fee_emissions_share_bps` exactly like a
      /// settlement fee's: under the default zero dial the entire revert fee
      /// accrues to `rewards_bucket`, and with a configured dial that share is
      /// transferred to the `sysio` emissions treasury instead. Pass 0 for
      /// no-fault refunds (whole fee path no-ops). Callers keep it below 100%
      /// (`sysio.uwrit::MAX_FEE_BPS`) so the post-fee refund stays positive.
      ///
      /// The refund is CREDITED to the recipient's `wireclaims` row, not
      /// transferred. This action is inlined from the never-throw
      /// `sysio.uwrit::drainfwq` drain, which itself runs inline from
      /// `sysio.epoch::advance`; a pushed transfer notifies `recipient`, so a
      /// hostile notify handler would abort `advance` and halt epoch
      /// advancement chain-wide. Worse, the queue row erase would roll back
      /// with it, making the block permanent and retried every epoch.
      [[sysio::action]]
      void refundwire(sysio::name recipient,
                      uint64_t   wire_amount,
                      uint32_t   revert_fee_bps);

      /// Auth = the claiming account. Pull the caller's credited WIRE
      /// (swap-to-WIRE payouts and swap-from-WIRE refunds) out of this
      /// contract's custody in a single transfer.
      ///
      /// This is the ONLY place a `wireclaims` balance becomes a transfer to the
      /// claimant, and it carries the claimant's own authority — so a recipient
      /// whose transfer-notify handler aborts blocks nothing but its own claim.
      /// Throws when there is nothing to claim, which reaches only the caller.
      ///
      /// Also refuses a row past `expires_at_sec`. The sweep is bounded and
      /// best-effort, so an expired row can wait many epochs for its turn;
      /// without this check the retention deadline would mean "swept eventually"
      /// rather than "claimable until", and a forfeit balance would still pay
      /// out in the meantime.
      [[sysio::action]]
      void claimwire(sysio::name account);

      /// Auth = `sysio.epoch` or self. Erase up to `max_rows` `wireclaims` rows
      /// whose one-year retention window has closed and push their total to the
      /// emissions treasury.
      ///
      /// `sysio.epoch::advance` inlines this every epoch with
      /// `MAX_CLAIM_SWEEP_PER_EPOCH`. That is what makes the retention deadline
      /// real: `credit_wire_claim` also sweeps, but only as a side effect of a
      /// later credit, so with settlement traffic stopped no action would ever
      /// revisit an aged-out row — leaving an unbounded, system-funded table and
      /// the WIRE it reserves outstanding indefinitely.
      ///
      /// Bounded and never-throwing past the auth gate for the usual reason: it
      /// runs inline inside `advance`, where an abort stalls epoch progress
      /// chain-wide. An oversized backlog drains across later epochs.
      [[sysio::action]]
      void sweepclaims(uint32_t max_rows);

      /// Auth = self (`sysio.reserv`). Set the contract's fee-routing config.
      ///
      ///   * `fee_emissions_share_bps` — the share of each fee's REWARDS POOL
      ///     (the half left after the winning underwriter's cut) transferred to
      ///     the `sysio` emissions treasury; the remainder accrues to
      ///     `rewards_bucket` for batch operators. 0 (the default) allocates the
      ///     whole pool to the batch-operator distribution and keeps every fee
      ///     inside this contract's custody at settlement. Capped at
      ///     `FEE_SPLIT_TOTAL_BPS`.
      [[sysio::action]]
      void setconfig(uint32_t fee_emissions_share_bps);

      /// Auth = the reserve's `owner`. Set this reserve's owner fee — the
      /// independent, per-reserve fee its liquidity earns on every swap that
      /// draws from it (WIRE-281). `owner_fee_bps` is either 0 (charge nothing)
      /// or in `[MIN_OWNER_FEE_BPS, MAX_OWNER_FEE_BPS]`.
      ///
      /// Requires an ACTIVE reserve with a resolved `owner`: a bootstrap-seeded
      /// public reserve has no owner, so no account can authorize a fee on it
      /// and none can be stranded with no claimant. Re-callable — the fee is a
      /// live parameter, not a create-time constant.
      [[sysio::action]]
      void setrsvfee(sysio::slug_name chain_code,
                     sysio::slug_name token_code,
                     sysio::slug_name reserve_code,
                     uint32_t        owner_fee_bps);

      /// Read-only: one reserve's unclaimed owner-fee WIRE balance, held in this
      /// contract's custody until `claimrsvfee`. Zero for a reserve that has
      /// never charged.
      [[sysio::action, sysio::read_only]]
      uint64_t rsvfeebal(sysio::slug_name chain_code,
                         sysio::slug_name token_code,
                         sysio::slug_name reserve_code);

      /// Auth = the reserve's `owner`. Pay out this reserve's entire accrued
      /// owner fee as REAL WIRE from custody and zero the accrual;
      /// `owner_fee_lifetime` (an audit total) is untouched.
      ///
      /// Owner-authenticated and self-serve, exactly like `claimuwfee` — the
      /// depot never pushes these payouts. Throws when the reserve is missing,
      /// has no owner, or has nothing accrued: a claim with nothing to pay is a
      /// caller mistake, not a silent no-op.
      [[sysio::action]]
      void claimrsvfee(sysio::slug_name chain_code,
                       sysio::slug_name token_code,
                       sysio::slug_name reserve_code);

      /// Read-only: `underwriter`'s unclaimed swap-fee WIRE balance (the
      /// underwriter half of every fee their winning commits settled), held in
      /// this contract's custody until `claimuwfee`. Zero for an underwriter
      /// with no accrual row.
      [[sysio::action, sysio::read_only]]
      uint64_t uwfeebal(sysio::name underwriter);

      /// Auth = `underwriter` (the earner). Pay out the caller's entire accrued
      /// swap-fee balance as REAL WIRE from this contract's custody and zero the
      /// accrual. `lifetime_claimed` (an audit total) accumulates instead.
      ///
      /// Owner-authenticated and self-serve — the depot never pushes these
      /// payouts, so an underwriter claims on their own schedule and a dormant
      /// underwriter costs the chain no per-epoch inline transfers. Throws when
      /// the caller has no accrual row or a zero balance: a claim with nothing to
      /// pay is a caller mistake, not a silent no-op.
      [[sysio::action]]
      void claimuwfee(sysio::name underwriter);

      // -----------------------------------------------------------------------
      //  Tables
      // -----------------------------------------------------------------------

      /// Triple-slug_name primary key. Composite encoded as
      /// `checksum256(chain_code || token_code || reserve_code)`.
      struct reserve_key {
         sysio::slug_name chain_code;
         sysio::slug_name token_code;
         sysio::slug_name reserve_code;
         checksum256 primary_key() const {
            std::array<uint8_t, 24> buf{};
            std::memcpy(buf.data() +  0, &chain_code.value,   8);
            std::memcpy(buf.data() +  8, &token_code.value,   8);
            std::memcpy(buf.data() + 16, &reserve_code.value, 8);
            return sysio::sha256(reinterpret_cast<const char*>(buf.data()), buf.size());
         }
         SYSLIB_SERIALIZE(reserve_key, (chain_code)(token_code)(reserve_code))
      };

      struct [[sysio::table("reserves")]] reserve_row {
         sysio::slug_name             chain_code;
         sysio::slug_name             token_code;
         sysio::slug_name             reserve_code;
         std::string                 name;
         std::string                 description;
         opp::types::ReserveStatus   status                 = opp::types::RESERVE_STATUS_UNKNOWN;
         uint64_t                    reserve_chain_amount   = 0;
         uint64_t                    reserve_wire_amount    = 0;
         /// Depot-frame decimal precision of the source token side, recorded at
         /// creation so the reserve is self-describing — nothing assumes a
         /// precision. `source_token_precision = min(token native precision, 9)`
         /// (the outpost downscales anything above the 9-dec frame cap at its
         /// boundary). The WIRE/target side is always `WIRE_PRECISION` (9), so it
         /// is not carried (there is no target_token_precision). The AMM curve is
         /// precision-homogeneous, so this does NOT enter swap math — it exists
         /// for unambiguous amount interpretation off the curve.
         uint32_t                    source_token_precision = WIRE_PRECISION;
         uint32_t                    connector_weight_bps   = DEFAULT_CONNECTOR_WEIGHT_BPS;
         opp::types::ChainAddress    creator_addr;
         uint64_t                    requested_wire_amount  = 0;
         uint64_t                    external_token_amount  = 0;
         uint64_t                    registered_at_ms       = 0;
         uint64_t                    activated_at_ms        = 0;
         uint64_t                    cancelled_at_ms        = 0;
         /// Private reserves only swap against counterpart reserves owned
         /// by the same WIRE account (and are excluded from WIRE-endpoint
         /// swaps entirely). Immutable after create.
         bool                        is_private             = false;
         /// The WIRE account that matched (and therefore owns) this
         /// reserve — the authex-linked account of the creator. Empty for
         /// bootstrap-seeded public reserves unless `regreserve` named one.
         sysio::name                 owner;
         /// Canonical raw pubkey bytes of the creator (33-byte compressed
         /// secp256k1 for EVM, 32-byte ed25519 for SVM), normalized at
         /// create time. `matchreserve` compares the matcher's authex-link
         /// key against this.
         std::vector<char>           creator_pub_key;

         /// The owner's fee on this reserve's WIRE leg, in basis points — the
         /// reserve's own revenue, INDEPENDENT of the network `fee_bps`. Every
         /// swap that draws liquidity from this reserve pays it, so a
         /// chain-to-chain swap between two fee-charging reserves pays both plus
         /// the network fee. Set by the owner via `setrsvfee`; `0` (the default)
         /// means the reserve charges nothing. See `MAX_OWNER_FEE_BPS`.
         uint32_t                    owner_fee_bps          = 0;
         /// Unclaimed WIRE this reserve has earned from `owner_fee_bps`, held in
         /// this contract's custody until the owner calls `claimrsvfee`. Part of
         /// the custody invariant documented on `rewards_bucket`.
         uint64_t                    owner_fee_accrued      = 0;
         /// Audit total: WIRE this reserve has earned from `owner_fee_bps`.
         /// Monotonic — never decremented by a claim.
         ///
         /// SATURATES at `UINT64_MAX` (~18.45e9 WIRE at 9dp) rather than
         /// wrapping. Unlike `owner_fee_accrued` — a balance, bounded by what is
         /// actually in custody — this is an unbounded running total, so the
         /// ceiling is reachable in principle: it needs roughly 370 turnovers of
         /// the entire launch supply through THIS ONE reserve at a 5% owner fee
         /// (~19 at the 99% maximum). Past that the counter stops advancing
         /// while accrual and claims continue to work normally; only the audit
         /// history is truncated, never a balance.
         uint64_t                    owner_fee_lifetime     = 0;

         uint128_t by_chain_token() const {
            return (static_cast<uint128_t>(chain_code.value) << 64) | token_code.value;
         }
         uint64_t by_status() const { return magic_enum::enum_integer(status); }

         SYSLIB_SERIALIZE(reserve_row,
            (chain_code)(token_code)(reserve_code)(name)(description)
            (status)(reserve_chain_amount)(reserve_wire_amount)
            (source_token_precision)(connector_weight_bps)
            (creator_addr)(requested_wire_amount)(external_token_amount)
            (registered_at_ms)(activated_at_ms)(cancelled_at_ms)
            (is_private)(owner)(creator_pub_key)
            (owner_fee_bps)(owner_fee_accrued)(owner_fee_lifetime))
      };

      using reserves_t = sysio::kv::table<"reserves"_n, reserve_key, reserve_row,
         sysio::kv::index<"bychaintok"_n, sysio::const_mem_fun<reserve_row, uint128_t, &reserve_row::by_chain_token>>,
         sysio::kv::index<"bystatus"_n,   sysio::const_mem_fun<reserve_row, uint64_t,  &reserve_row::by_status>>
      >;

      /// Singleton accumulator for the rewards (batch-operator) half of swap
      /// fees. The WIRE stays in this contract's custody — it is NOT transferred
      /// out — so the custody invariant is `token_balance == Σ
      /// reserve_wire_amount + rewards.balance + Σ uw_fee_row.balance +
      /// Σ reserve_row.owner_fee_accrued + in-flight escrow +
      /// Σ wireclaims.balance`. A collected owner fee leaves
      /// `reserve_wire_amount` but stays in this contract until `claimrsvfee`,
      /// so it is a term in the equation, not an outflow; the last term likewise
      /// covers settled-but-unclaimed payouts and refunds, whose WIRE stays in
      /// custody until the recipient pulls it. `balance` is the portion
      /// earmarked for distribution (swept by `drainrewards` and folded into
      /// `sysio.system::payepoch`); `lifetime_accrued` is an audit total. (The
      /// underwriter half of each fee accrues to `uwfees` instead and is
      /// therefore not tracked here.)
      ///
      /// NOTE: sysio.system reads this row through a layout-compatible local
      /// definition (a `[[sysio::table]]`-attributed struct cannot be shared
      /// into sysio.system's translation unit — it corrupts that contract's
      /// read-only-action return codegen). The cross-contract read is exercised
      /// end-to-end by t5_emissions_tests/payepoch_folds_swap_fee_rewards, which
      /// fails if the two layouts ever diverge.
      struct [[sysio::table("rewardbkt")]] rewards_bucket {
         uint64_t balance          = 0;   // claimable WIRE held for distribution
         uint64_t lifetime_accrued = 0;   // audit: total WIRE ever routed to rewards
         SYSLIB_SERIALIZE(rewards_bucket, (balance)(lifetime_accrued))
      };
      using rewardbkt_t = sysio::kv::global<"rewardbkt"_n, rewards_bucket>;

      /// Claimable WIRE owed to an account by `paywire` (swap-to-WIRE settlement) or
      /// `refundwire` (swap-from-WIRE revert). Both credit here instead of transferring, because
      /// both are reached from never-throw paths where a recipient's transfer-notify handler could
      /// otherwise abort the enclosing transaction — stalling consensus dispatch in `paywire`'s
      /// case and halting epoch advancement chain-wide in `refundwire`'s.
      ///
      /// The backing WIRE never leaves this contract's custody at credit time, so it is a term of
      /// the custody invariant above.
      struct wireclaim_key {
         uint64_t account;
         SYSLIB_SERIALIZE(wireclaim_key, (account))
      };

      struct [[sysio::table("wireclaims")]] wire_claim {
         sysio::name account;
         uint64_t    balance        = 0;   // atomic WIRE units owed, not yet claimed
         uint32_t    expires_at_sec = 0;   // swept back to the treasury once past

         /// Expiry-major composite so the secondary index orders by expiry and the retention sweep
         /// can stop at the first live row. The account tail only breaks ties, keeping the key
         /// unique when many rows share an expiry second.
         uint128_t by_expiry() const {
            return (static_cast<uint128_t>(expires_at_sec) << 64) | account.value;
         }

         SYSLIB_SERIALIZE(wire_claim, (account)(balance)(expires_at_sec))
      };

      using wireclaims_t = sysio::kv::table<"wireclaims"_n, wireclaim_key, wire_claim,
         sysio::kv::index<"byexpiry"_n,
            sysio::const_mem_fun<wire_claim, uint128_t, &wire_claim::by_expiry>>
      >;

      /// Key for `uwfees` — one row per earning underwriter account.
      struct uw_fee_key {
         sysio::name underwriter;
         uint64_t primary_key() const { return underwriter.value; }
         SYSLIB_SERIALIZE(uw_fee_key, (underwriter))
      };

      /// Per-underwriter accrual of the underwriter half of swap fees. Credited
      /// by `route_wire_fee` at settlement (`applyswap` / `applyfromwire` /
      /// `paywire`) to the uwreq's winning underwriter, and drained by the
      /// owner-authenticated `claimuwfee`. The WIRE never leaves this contract's
      /// custody until a claim, so these balances are part of the custody
      /// invariant documented on `rewards_bucket`.
      ///
      /// `balance` is unclaimed WIRE. `lifetime_accrued` / `lifetime_claimed`
      /// are monotonic audit totals; a row is created on first accrual and
      /// RETAINED at zero balance after a claim so the audit trail survives.
      /// The two `lifetime_*` counters SATURATE at `UINT64_MAX` (~18.45e9 WIRE
      /// at 9dp) rather than wrapping — see `reserve_row::owner_fee_lifetime`
      /// for the reachability arithmetic. They are unbounded running totals, so
      /// unlike `balance` (bounded by custody) the ceiling is reachable in
      /// principle; past it the audit history is truncated while accrual and
      /// `claimuwfee` continue to work normally.
      struct [[sysio::table("uwfees")]] uw_fee_row {
         sysio::name underwriter;
         uint64_t    balance          = 0;   // unclaimed WIRE held in custody
         uint64_t    lifetime_accrued = 0;   // audit: WIRE accrued (saturating)
         uint64_t    lifetime_claimed = 0;   // audit: WIRE paid out (saturating)
         SYSLIB_SERIALIZE(uw_fee_row, (underwriter)(balance)(lifetime_accrued)(lifetime_claimed))
      };
      using uwfees_t = sysio::kv::table<"uwfees"_n, uw_fee_key, uw_fee_row>;

      /// Fee-routing configuration singleton. Holds only the governance dial
      /// that stage 2 of the fee split consults — the stage-1 underwriter share
      /// is the fixed `FEE_UNDERWRITER_SHARE_BPS` constant, and the fee RATE
      /// itself lives on `sysio.uwrit::uwconfig`.
      struct [[sysio::table("reservcfg")]] reserve_config {
         /// Share of each fee's rewards pool routed to the `sysio` emissions
         /// treasury; the remainder goes to `rewards_bucket`. Default 0.
         uint32_t fee_emissions_share_bps = DEFAULT_FEE_EMISSIONS_SHARE_BPS;
         SYSLIB_SERIALIZE(reserve_config, (fee_emissions_share_bps))
      };
      using reservcfg_t = sysio::kv::global<"reservcfg"_n, reserve_config>;

   private:
      using ReserveStatus = opp::types::ReserveStatus;
      using ChainKind     = opp::types::ChainKind;
   };

} // namespace sysio

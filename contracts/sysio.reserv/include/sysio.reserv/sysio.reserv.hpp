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

      // Bancor connector_weight is stored in basis points (10000 = 100%).
      // Pure constant-product corresponds to weight = 5000.
      static constexpr uint32_t MAX_CONNECTOR_WEIGHT_BPS     = 10000;
      static constexpr uint32_t DEFAULT_CONNECTOR_WEIGHT_BPS = 5000;

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

      /// Read-only quote across two reserves (src and dst). Requires both
      /// reserves ACTIVE with the same connector model. Returns 0 when any
      /// required reserve is missing or inactive.
      [[sysio::action, sysio::read_only]]
      uint64_t swapquote(sysio::slug_name from_chain_code,
                         sysio::slug_name from_token_code,
                         sysio::slug_name from_reserve_code,
                         uint64_t        from_amount,
                         sysio::slug_name to_chain_code,
                         sysio::slug_name to_token_code,
                         sysio::slug_name to_reserve_code);

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

      /// Auth=sysio.msgch. Credit STAKING_REWARD into the matching reserve.
      [[sysio::action]]
      void onreward(sysio::slug_name chain_code,
                    sysio::slug_name token_code,
                    sysio::slug_name reserve_code,
                    uint64_t        outpost_amount);

      /// Auth=sysio.uwrit. Emit-time four-leg apply for a normal
      /// (outpost ↔ outpost) swap, fired from `try_select_winner` BEFORE the
      /// SWAP_REMIT is queued so every intervening quote prices post-swap.
      /// Computes the WIRE intermediate internally from the pre-mutation
      /// rows — `w = cp_output(src.chain, src.wire, src_amount)` — then:
      ///   src: chain += src_amount, wire -= w
      ///   dst: wire  += w,          chain -= dst_amount
      /// All four legs balance-checked BEFORE any mutation; a failed check
      /// aborts the surrounding race-resolution transaction (no half-state).
      /// `Σ reserve_wire_amount` is unchanged (the `w` hop is internal).
      [[sysio::action]]
      void applyswap(sysio::slug_name src_chain_code,
                     sysio::slug_name src_token_code,
                     sysio::slug_name src_reserve_code,
                     uint64_t        src_amount,
                     sysio::slug_name dst_chain_code,
                     sysio::slug_name dst_token_code,
                     sysio::slug_name dst_reserve_code,
                     uint64_t        dst_amount);

      /// Auth=sysio.uwrit. Emit-time apply for a swap-FROM-WIRE (the depot
      /// is the source; only the target outpost leg exists). The user's
      /// escrowed WIRE (already held by this contract since `swapfromwire`)
      /// becomes the target reserve's WIRE-side liquidity:
      ///   dst: wire += wire_in, chain -= dst_amount
      /// `Σ reserve_wire_amount` rises by `wire_in`, matching the real WIRE
      /// balance bump that happened at escrow time.
      [[sysio::action]]
      void applyfromwire(sysio::slug_name dst_chain_code,
                         sysio::slug_name dst_token_code,
                         sysio::slug_name dst_reserve_code,
                         uint64_t        wire_in,
                         uint64_t        dst_amount);

      /// Auth=sysio.uwrit. Settlement for a swap-TO-WIRE (the depot is the
      /// target; only the source outpost leg exists). Pays the recipient
      /// REAL WIRE from this contract's custody and books the source side:
      ///   src: chain += src_amount, wire -= wire_out
      ///   inline sysio.token::transfer(sysio.reserv → recipient, wire_out)
      /// `Σ reserve_wire_amount` drops by `wire_out`, matching the real
      /// WIRE leaving custody.
      [[sysio::action]]
      void paywire(sysio::slug_name src_chain_code,
                   sysio::slug_name src_token_code,
                   sysio::slug_name src_reserve_code,
                   uint64_t        src_amount,
                   sysio::name     recipient,
                   uint64_t        wire_out);

      /// Auth=sysio.uwrit. Refund escrowed WIRE to a swap-FROM-WIRE user
      /// whose queued request failed drain-time validation (reserve
      /// missing / not ACTIVE / private / variance drift). Touches no
      /// reserve row — the escrow was never credited to any
      /// `reserve_wire_amount`.
      [[sysio::action]]
      void refundwire(sysio::name recipient,
                      uint64_t   wire_amount);

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

         uint128_t by_chain_token() const {
            return (static_cast<uint128_t>(chain_code.value) << 64) | token_code.value;
         }
         uint64_t by_status() const { return magic_enum::enum_integer(status); }

         SYSLIB_SERIALIZE(reserve_row,
            (chain_code)(token_code)(reserve_code)(name)(description)
            (status)(reserve_chain_amount)(reserve_wire_amount)(connector_weight_bps)
            (creator_addr)(requested_wire_amount)(external_token_amount)
            (registered_at_ms)(activated_at_ms)(cancelled_at_ms)
            (is_private)(owner)(creator_pub_key))
      };

      using reserves_t = sysio::kv::table<"reserves"_n, reserve_key, reserve_row,
         sysio::kv::index<"bychaintok"_n, sysio::const_mem_fun<reserve_row, uint128_t, &reserve_row::by_chain_token>>,
         sysio::kv::index<"bystatus"_n,   sysio::const_mem_fun<reserve_row, uint64_t,  &reserve_row::by_status>>
      >;

   private:
      using ReserveStatus = opp::types::ReserveStatus;
      using ChainKind     = opp::types::ChainKind;
   };

} // namespace sysio

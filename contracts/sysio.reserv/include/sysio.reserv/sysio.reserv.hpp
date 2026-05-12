#pragma once

#include <sysio/sysio.hpp>
#include <sysio/kv_global.hpp>
#include <sysio/kv_table.hpp>
#include <sysio/asset.hpp>
#include <sysio/system.hpp>
#include <sysio/opp/types/types.pb.hpp>
#include <sysio/opp/attestations/attestations.pb.hpp>

namespace sysio {

   /**
    * @brief sysio.reserv — per-chain reserve / quote management on WIRE.
    *
    * Every cross-chain reserve is paired with WIRE on the depot side: a swap
    * from `token_a` (chain A) to `token_b` (chain B) routes as
    * `token_a -> WIRE -> token_b`, hopping through this contract's
    * `reserves` table. v1 uses **constant-product** pricing (xy = k,
    * equivalent to a Bancor reserve at `connector_weight = 0.5`). The
    * `connector_weight_bps` field on `reserve_entry` is reserved for the
    * asymmetric Bancor extension; today's `swapquote(...)` ignores it.
    *
    * Quote formulas (uint128 fixed-point, no overflow on uint64 amounts):
    *
    *   token -> WIRE:   dW = (rW * dT) / (rT + dT)
    *   WIRE -> token:   dT = (rT * dW) / (rW + dW)
    *   token -> token:  dW_intermediate = swapquote(src, WIRE, src_amount)
    *                    dst_amount      = swapquote(WIRE, dst, dW_intermediate)
    *
    * @par Action surface
    * - `setreserve(chain, outpost_amount, wire_amount, connector_weight_bps)`
    *   — provision or overwrite a reserve row. Auth=self.
    * - `swapquote(from_amount, to_chain, to_token) -> TokenAmount` —
    *   read-only swap pricing endpoint. Returns 0-amount when any required
    *   reserve is missing; callers treat that as "no quote available; skip
    *   variance check".
    * - `onreward(chain, outpost_amount)` — credit the outpost-side reserve
    *   from a STAKING_REWARD attestation. Auth=sysio.msgch. The WIRE-side
    *   payout to the staker is a SEPARATE next-epoch action owned by the
    *   staking work stream; this action only grows
    *   `reserve_outpost_amount`.
    * - `onreject(rejected)` — destination outpost could not pay a
    *   SwapRemit and emitted SwapRejected back; re-add
    *   `unremitted_amount.amount` to the matching
    *   `reserve_outpost_amount` so the depot's accounting reconciles
    *   with the outpost's actual balance. Auth=sysio.msgch.
    *
    * @par Schema (post operator-collateral refactor)
    * The reserve row tracks BOTH the outpost-side and WIRE-side balances
    * as `TokenAmount` so the kind is explicit in storage and on the wire.
    * Renamed from the previous `lp_*` schema; the contract account name
    * `sysio.reserv` is unchanged.
    */
   class [[sysio::contract("sysio.reserv")]] reserve : public contract {
   public:
      using contract::contract;

      // Well-known accounts
      static constexpr name MSGCH_ACCOUNT = "sysio.msgch"_n;

      // Bancor connector_weight is stored in basis points (10000 = 100%).
      // Pure constant-product corresponds to weight = 5000.
      static constexpr uint32_t MAX_CONNECTOR_WEIGHT_BPS = 10000;
      static constexpr uint32_t DEFAULT_CONNECTOR_WEIGHT_BPS = 5000;

      // -----------------------------------------------------------------------
      //  Actions
      // -----------------------------------------------------------------------

      /// Provision or update a reserve. The (chain, outpost_token) pair is
      /// unique; re-calling `setreserve` for the same pair updates its
      /// amounts and connector weight in place.
      ///
      /// @param chain                  Outpost chain (ETH, SOL, etc).
      /// @param outpost_amount         TokenAmount on the outpost side
      ///                                (e.g. ETH or LIQETH on Ethereum).
      ///                                `outpost_amount.kind` identifies the
      ///                                non-WIRE side of the pair.
      /// @param wire_amount            TokenAmount on the WIRE side. `kind`
      ///                                must be TOKEN_KIND_WIRE — enforced
      ///                                with `check(...)`.
      /// @param connector_weight_bps   Bancor connector weight (5000 = 50%,
      ///                                pure constant product). Stored but
      ///                                unused by `swapquote` today.
      [[sysio::action]]
      void setreserve(opp::types::ChainKind     chain,
                      opp::types::TokenAmount   outpost_amount,
                      opp::types::TokenAmount   wire_amount,
                      uint32_t                  connector_weight_bps);

      /// Read-only swap quote. Returns a `TokenAmount` carrying the
      /// destination token kind + amount so callers don't have to track
      /// the destination kind separately. Returns
      /// `TokenAmount{ to_token, 0 }` when any required reserve is
      /// missing (caller's variance check treats 0 as "no quote
      /// available; skip variance check").
      ///
      /// @param from_amount   Source TokenAmount (kind + amount).
      /// @param to_chain      Destination chain.
      /// @param to_token      Destination TokenKind.
      [[sysio::action, sysio::read_only]]
      opp::types::TokenAmount swapquote(opp::types::TokenAmount   from_amount,
                                        opp::types::ChainKind     to_chain,
                                        opp::types::TokenKind     to_token);

      /// Credit an outpost-side reserve from a STAKING_REWARD attestation.
      /// Auth=sysio.msgch. Only `reserve_outpost_amount` grows; the
      /// WIRE-side payout to the staker is a separate next-epoch action
      /// owned by the staking work stream.
      ///
      /// @param chain            Outpost chain whose reserve received the reward.
      /// @param outpost_amount   Amount + kind credited. `kind` must
      ///                          match the reserve's
      ///                          `reserve_outpost_amount.kind`.
      [[sysio::action]]
      void onreward(opp::types::ChainKind     chain,
                    opp::types::TokenAmount   outpost_amount);

      /// Reconcile a failed SwapRemit. Destination outpost could not
      /// pay the recipient; the token stays in its reserve. Re-add the
      /// `unremitted_amount.amount` to `reserve_outpost_amount` so the
      /// depot's view of the reserve matches the outpost's actual
      /// balance. Auth=sysio.msgch.
      ///
      /// @param rejected   The decoded SwapRejected attestation.
      [[sysio::action]]
      void onreject(opp::attestations::SwapRejected rejected);

      // -----------------------------------------------------------------------
      //  Tables
      // -----------------------------------------------------------------------

      /// Composite primary key: pack chain (high 32 bits) + outpost-side
      /// TokenKind (low 32 bits) into a single uint64 so the
      /// (chain, outpost_token) pair is unique.
      struct reserve_key {
         uint64_t chain_token;
         uint64_t primary_key() const { return chain_token; }
         SYSLIB_SERIALIZE(reserve_key, (chain_token))
      };

      static constexpr uint64_t pack_chain_token(opp::types::ChainKind chain,
                                                  opp::types::TokenKind token) {
         return (static_cast<uint64_t>(chain) << 32)
              | static_cast<uint64_t>(token);
      }

      /// One reserve per (chain, outpost_token). The WIRE-paired side is
      /// implicit; every reserve holds the outpost-side token + WIRE.
      struct [[sysio::table("reserves")]] reserve_entry {
         opp::types::ChainKind     chain;
         /// Outpost-side reserve. `kind` identifies the non-WIRE side
         /// of the pair (e.g. TOKEN_KIND_ETH or TOKEN_KIND_LIQETH).
         opp::types::TokenAmount   reserve_outpost_amount;
         /// WIRE-side reserve. `kind` is always TOKEN_KIND_WIRE.
         opp::types::TokenAmount   reserve_wire_amount;
         uint32_t                  connector_weight_bps  = DEFAULT_CONNECTOR_WEIGHT_BPS;
         uint64_t                  last_updated_ms       = 0;

         /// Composite key matching `reserve_key::chain_token` (kept here too so
         /// secondary-index lookups by the same key work uniformly).
         uint64_t by_chain_token() const {
            return pack_chain_token(chain, reserve_outpost_amount.kind);
         }

         SYSLIB_SERIALIZE(reserve_entry,
            (chain)(reserve_outpost_amount)(reserve_wire_amount)
            (connector_weight_bps)(last_updated_ms))
      };

      using reserves_t = sysio::kv::table<"reserves"_n, reserve_key, reserve_entry>;

   private:
      using ChainKind   = opp::types::ChainKind;
      using TokenKind   = opp::types::TokenKind;
      using TokenAmount = opp::types::TokenAmount;
   };

} // namespace sysio

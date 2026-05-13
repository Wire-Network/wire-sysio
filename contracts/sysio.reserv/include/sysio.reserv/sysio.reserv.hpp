#pragma once

#include <sysio/sysio.hpp>
#include <sysio/kv_global.hpp>
#include <sysio/kv_table.hpp>
#include <sysio/asset.hpp>
#include <sysio/system.hpp>
#include <sysio/opp/types/types.pb.hpp>

namespace sysio {

   /**
    * @brief sysio.reserve — per-chain LP / reserve management on WIRE.
    *
    * Per `CLAUDE-WIRE-OPERATOR-COLLATERAL-IMPL-PLAN.md` §1 / Task 5: every
    * cross-chain LP is paired with WIRE on the depot side. A swap from
    * `token_a` (chain A) to `token_b` (chain B) routes as
    * `token_a -> WIRE -> token_b`, hopping through this contract's LP table.
    *
    * v1 implements **constant-product** quoting (xy = k, equivalent to a
    * Bancor LP at `connector_weight = 0.5`). The `connector_weight` field
    * on `lp_entry` is reserved for the asymmetric Bancor extension; today's
    * `quote(...)` ignores it and uses pure constant-product math. Quote
    * formulas (uint128 fixed-point, no overflow on uint64 reserves):
    *
    *   token -> WIRE:   dW = (rW * dT) / (rT + dT)
    *   WIRE -> token:   dT = (rT * dW) / (rW + dW)
    *   token -> token:  dW_intermediate = quote(src_chain, src_token, WIRE, src_amount)
    *                    dst_amount      = quote(WIRE, dst_chain, dst_token, dW_intermediate)
    *
    * Read-side consumers (uwrit's variance check, off-chain quote endpoints)
    * either call `quote(...)` directly or mirror the `lps` table and inline
    * the math. opreg's slash flow uses the default `ReserveTarget {KIND_LP,
    * paired_token=token_kind}` construction without consulting this contract
    * — `resolve_lp` is reserved for the path where the canonical mapping
    * needs to be overridden (e.g. emergency reroute).
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

      /// Provision or update an LP. The (chain, paired_token) pair is unique;
      /// re-calling `setlp` for an existing pair updates its reserves and
      /// connector weight in place. Reserves are denominated in the chain's
      /// canonical units (uint64).
      [[sysio::action]]
      void setlp(opp::types::ChainKind chain,
                 opp::types::TokenKind paired_token,
                 uint64_t reserve_paired,
                 uint64_t reserve_wire,
                 uint32_t connector_weight_bps);

      /// Read-only quote: how many destination tokens does `src_amount` of
      /// `(src_chain, src_token)` produce on `(dst_chain, dst_token)`?
      /// Returns 0 if any required LP is missing (caller's variance check
      /// should treat 0 as "no LP available; skip variance check").
      [[sysio::action, sysio::read_only]]
      uint64_t quote(opp::types::ChainKind src_chain,
                     opp::types::TokenKind src_token,
                     opp::types::ChainKind dst_chain,
                     opp::types::TokenKind dst_token,
                     uint64_t src_amount);

      /// Credit an LP's paired-token reserve from a STAKING_REWARD
      /// attestation. Auth=msgch. Currently unused; will be
      /// invoked once Task 4's dispatch wires those types in (today they
      /// fall through to no-op — see msgch's dispatch_attestation).
      [[sysio::action]]
      void creditlp(opp::types::ChainKind chain,
                    opp::types::TokenKind paired_token,
                    uint64_t paired_amount,
                    uint64_t wire_amount);

      // -----------------------------------------------------------------------
      //  Tables
      // -----------------------------------------------------------------------

      /// Composite primary key: pack chain (high 32 bits) + paired_token
      /// (low 32 bits) into a single uint64 so the (chain, token) pair is
      /// unique. Both enums fit comfortably in 32 bits each.
      struct lp_key {
         uint64_t chain_token;
         uint64_t primary_key() const { return chain_token; }
         SYSLIB_SERIALIZE(lp_key, (chain_token))
      };

      static constexpr uint64_t pack_chain_token(opp::types::ChainKind chain,
                                                  opp::types::TokenKind token) {
         return (static_cast<uint64_t>(chain) << 32)
              | static_cast<uint64_t>(token);
      }

      /// One LP per (chain, paired_token). The WIRE-paired side is implicit;
      /// every LP holds the paired token + WIRE.
      struct [[sysio::table("lps")]] lp_entry {
         opp::types::ChainKind  chain;
         opp::types::TokenKind  paired_token;
         uint64_t               reserve_paired           = 0;
         uint64_t               reserve_wire             = 0;
         uint32_t               connector_weight_bps     = DEFAULT_CONNECTOR_WEIGHT_BPS;
         uint64_t               last_updated_ms          = 0;

         /// Composite key matching `lp_key::chain_token` (kept here too so
         /// secondary-index lookups by the same key work uniformly).
         uint64_t by_chain_token() const {
            return pack_chain_token(chain, paired_token);
         }

         SYSLIB_SERIALIZE(lp_entry,
            (chain)(paired_token)(reserve_paired)(reserve_wire)
            (connector_weight_bps)(last_updated_ms))
      };

      using lps_t = sysio::kv::table<"lps"_n, lp_key, lp_entry>;

   private:
      using ChainKind = opp::types::ChainKind;
      using TokenKind = opp::types::TokenKind;
   };

} // namespace sysio

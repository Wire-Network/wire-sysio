#pragma once

/**
 * @file sysio.cap.hpp
 * @brief Capital staking contract.
 *
 * Receives lifecycle and yield messages from outposts (LIQ-NATIVE stakers),
 * tracks per-staker positions long-term, and pays WIRE rewards by drawing
 * from the capital-bucket budget that `sysio.system::payepoch` deposits to
 * the `sysio.cap` account each successful epoch.
 *
 * Status: scaffolding only. Inbound message handlers, escrow drain logic,
 * yield-pacing logic, and the pre-launch import path land in subsequent
 * PRs once the OPP designer resolves the staking-message stubs
 * (NATIVE_YIELD_REWARD body, STAKING_REWARD shape) and the warmup-fee
 * follow-ups (asset, sizing, mismatch handling).
 */

#include <sysio/sysio.hpp>
#include <sysio/kv_global.hpp>
#include <sysio/kv_table.hpp>
#include <sysio/asset.hpp>
#include <sysio/crypto.hpp>
#include <sysio/system.hpp>

#include <sysio/opp/types/types.pb.hpp>
#include <sysio.opp.common/opp_table_types.hpp>

namespace sysio {

   /**
    * @brief Capital staking contract on `sysio.cap`. The same account also
    * receives the capital-bucket portion of the per-epoch WIRE emission
    * (deposited inline by `sysio.system::payepoch`), so the contract
    * spends from its own balance rather than pulling from another account.
    */
   class [[sysio::contract("sysio.cap")]] cap : public contract {
   public:
      using contract::contract;

      // -----------------------------------------------------------------------
      //  Well-known accounts and constants
      // -----------------------------------------------------------------------

      static constexpr name AUTHEX_ACCOUNT = "sysio.authex"_n;
      static constexpr name MSGCH_ACCOUNT  = "sysio.msgch"_n;
      static constexpr name EPOCH_ACCOUNT  = "sysio.epoch"_n;
      static constexpr name TOKEN_ACCOUNT  = "sysio.token"_n;

      static constexpr symbol WIRE_SYM = symbol("WIRE", 9);

      // -----------------------------------------------------------------------
      //  Actions
      // -----------------------------------------------------------------------

      /**
       * @brief Set / update the capital-staking config singleton.
       *
       * Currently a placeholder: the config struct itself is empty. The
       * concrete fields land alongside the warmup-fee follow-ups (asset,
       * sizing, mismatch handling) and pacing tuning. Establishing the
       * singleton up front means adding fields later doesn't require a
       * data-migration path.
       */
      [[sysio::action]]
      void setconfig();

      // -----------------------------------------------------------------------
      //  Tables
      // -----------------------------------------------------------------------

      using StakeStatus  = sysio::opp::types::StakeStatus;
      using ChainKind    = sysio::opp::types::ChainKind;

      /**
       * @brief Position primary key — a monotonic id assigned on insert.
       *
       * Logical lookup by `(chain, native_address, outpost_position_id)`
       * goes through a secondary index that lands with the lifecycle
       * handler in a follow-up PR. A monotonic primary keeps insertions
       * cheap and avoids encoding variable-length address bytes into the
       * primary key path.
       */
      struct position_key {
         uint64_t id = 0;
         uint64_t primary_key() const { return id; }
         SYSLIB_SERIALIZE(position_key, (id))
      };

      /**
       * @brief A single staker's position. Long-lived: the row persists
       * for the full lifetime of the position (open through warmup,
       * active, cooldown, terminate or slash). Status walks
       * STAKE_STATUS_WARMUP -> ACTIVE -> COOLDOWN -> TERMINATED, with
       * SLASHED reachable from any state via a future inline call from
       * `sysio.chalg`.
       */
      struct [[sysio::table("positions"), sysio::contract("sysio.cap")]] position {
         uint64_t          id                  = 0;   ///< primary key

         /// External identity — what the outpost reports.
         ChainKind         chain_kind          = ChainKind::CHAIN_KIND_UNKNOWN;
         std::vector<char> native_address;            ///< raw outpost-side address bytes
         uint64_t          outpost_position_id = 0;   ///< id assigned by the source outpost

         StakeStatus       status              = StakeStatus::STAKE_STATUS_UNKNOWN;

         /// Wire account resolved via `sysio.authex::links` at open time.
         /// All WIRE payouts go here.
         name              wire_account;

         /// Principal in WIRE-equivalent. The outpost converts native
         /// principal (and any pre-launch tranche-priced warrants) to a
         /// WIRE amount before sending; wire-sysio never sees the tranche
         /// table or the underlying native principal.
         asset             principal           = asset{0, WIRE_SYM};

         /// Running total of WIRE ever paid out to this position. Pure
         /// audit trail — never decremented. Per-cycle accounting uses
         /// `pending_payout` and `fee_escrow_balance` instead.
         asset             accrued_yield       = asset{0, WIRE_SYM};

         /// WIRE owed to the staker but not yet emitted. Yield-reward
         /// inbound messages credit this; the per-epoch pacing logic
         /// drains it over the next 2 WIRE epochs (per
         /// `sysio.epoch::epoch_config::epoch_duration_sec`).
         asset             pending_payout      = asset{0, WIRE_SYM};

         /// WIRE held in escrow during WARMUP. Funded by the staker's
         /// pre-paid warmup fee at stake-open; drained as the staker's
         /// yield until the underlying native stake activates and ACTIVE
         /// payouts take over from the capital-bucket emissions.
         asset             fee_escrow_balance  = asset{0, WIRE_SYM};

         uint64_t          opened_at_ms        = 0;
         uint64_t          last_status_ms      = 0;

         uint64_t          primary_key()     const { return id; }
         uint64_t          by_wire_account() const { return wire_account.value; }
         uint64_t          by_status()       const { return static_cast<uint64_t>(status); }

         SYSLIB_SERIALIZE(position,
            (id)(chain_kind)(native_address)(outpost_position_id)
            (status)(wire_account)
            (principal)(accrued_yield)(pending_payout)(fee_escrow_balance)
            (opened_at_ms)(last_status_ms))
      };

      using positions_t = sysio::kv::table<"positions"_n, position_key, position,
         sysio::kv::index<"bywire"_n,
            sysio::const_mem_fun<position, uint64_t, &position::by_wire_account>>,
         sysio::kv::index<"bystatus"_n,
            sysio::const_mem_fun<position, uint64_t, &position::by_status>>
      >;

      /**
       * @brief Capital-staking config singleton. Currently empty; concrete
       * fields land with the warmup-fee, pacing, and import-window
       * resolutions.
       */
      struct [[sysio::table("capcfg"), sysio::contract("sysio.cap")]] cap_config {
         uint32_t reserved = 0;

         SYSLIB_SERIALIZE(cap_config, (reserved))
      };

      using capcfg_t = sysio::kv::global<"capcfg"_n, cap_config>;
   };

} // namespace sysio

#pragma once

#include <sysio/sysio.hpp>
#include <sysio/kv_global.hpp>
#include <sysio/kv_table.hpp>
#include <sysio/asset.hpp>
#include <sysio/crypto.hpp>
#include <sysio/system.hpp>
#include <sysio/opp/types/types.pb.hpp>
#include <sysio.opp.common/opp_table_types.hpp>

#include <algorithm>

namespace sysio {

   class [[sysio::contract("sysio.epoch")]] epoch : public contract {
   public:
      using contract::contract;

      // -----------------------------------------------------------------------
      //  Actions
      // -----------------------------------------------------------------------

      /// Set epoch configuration (privileged).
      [[sysio::action]]
      void setconfig(uint32_t epoch_duration_sec,
                     uint32_t operators_per_epoch,
                     uint32_t batch_operator_minimum_active,
                     uint32_t batch_op_groups,
                     uint32_t epoch_retention_envelope_log_count);

      /// Advance epoch if duration elapsed (permissionless crank).
      [[sysio::action]]
      void advance();

      /// Internal continuation after epoch-close operator mutations execute.
      [[sysio::action]]
      void finishadv(uint32_t epoch_index);

      /// Set the economic clock (privileged): the duration of one economic epoch and the time its grid
      /// starts. The grid may start in the past -- the clock then catches up through the elapsed epochs
      /// once it starts -- but no change is accepted while a boundary has passed without its tick, and a
      /// new start must be ahead while a collateral withdrawal is queued. The start is fixed once the
      /// clock has ticked, and a new duration spaces only the boundaries after the next one.
      [[sysio::action]]
      void seteconomy(uint32_t epoch_duration_sec, sysio::time_point_sec start_time);

      /// Open economic epoch `epoch_index` (permissionless crank). Refused unless `epoch_index` is the
      /// next index, its grid boundary has passed, and the consensus genesis advance has run. Never
      /// waits on outpost consensus. Runs the emissions gate, then matures withdrawals, accrues the
      /// emission share of the epoch this tick closes, pays on a pay epoch, and steps the batch-duty
      /// settlement. Once the clock has caught up to the grid (`economic_state::caught_up`), a pay epoch ticked
      /// while a later boundary has passed defers its payout to the tick that catches up, which pays every overdue
      /// period's pools in one payout, split over all the work counted since the last one. Until then each pay epoch's
      /// payout spends nothing and is never held for balance: its whole period stays in the treasury, and the blocks
      /// and duty counted meanwhile carry to the first payout after the clock has caught up.
      [[sysio::action]]
      void tickeconomy(uint32_t epoch_index);

      /// Group assignment — reads AVAILABLE batch ops from sysio.opreg.
      [[sysio::action]]
      void schbatchgps();

      /// Set global pause (only callable by sysio.chalg).
      [[sysio::action]]
      void pause();

      /// Clear global pause (only callable by sysio.chalg).
      [[sysio::action]]
      void unpause();

      // -----------------------------------------------------------------------
      //  Tables
      // -----------------------------------------------------------------------

      /// Global epoch configuration singleton.
      struct [[sysio::table("epochcfg")]] epoch_config {
         uint32_t    epoch_duration_sec = 360;   // 6 minutes
         uint32_t    operators_per_epoch = 7;
         uint32_t    batch_operator_minimum_active = 21;
         uint32_t    batch_op_groups = 3;          // rotation groups (21 / 7)

         /// Cap multiplier for the metadata-only `envelope_log` table on
         /// `sysio.msgch`. Effective row cap is
         /// `active_outposts * 2 * epoch_retention_envelope_log_count`
         /// (one inbound + one outbound record per active outpost per
         /// epoch). Default 200 — matches the SOL/ETH per-direction
         /// metadata-log cap. Each `evalcons` consensus-reach + `buildenv`
         /// emit reads this directly; runtime changes via `setconfig`
         /// take effect on the next write.
         uint32_t    epoch_retention_envelope_log_count = 200;

         SYSLIB_SERIALIZE(epoch_config,
            (epoch_duration_sec)(operators_per_epoch)
            (batch_operator_minimum_active)(batch_op_groups)
            (epoch_retention_envelope_log_count))
      };

      using epochcfg_t = sysio::kv::global<"epochcfg"_n, epoch_config>;

      /// Current epoch state singleton.
      struct [[sysio::table("epochstate")]] epoch_state {
         uint32_t                          current_epoch_index = 0;
         time_point                        current_epoch_start{};
         time_point                        next_epoch_start{};
         uint8_t                           current_batch_op_group = 0; // 0, 1, or 2
         std::vector<std::vector<name>>    batch_op_groups;           // 3 groups of 7
         checksum256                       last_consensus_hash;
         bool                              is_paused = false;

         SYSLIB_SERIALIZE(epoch_state,
            (current_epoch_index)(current_epoch_start)(next_epoch_start)
            (current_batch_op_group)(batch_op_groups)(last_consensus_hash)(is_paused))
      };

      using epochstate_t = sysio::kv::global<"epochstate"_n, epoch_state>;

      /// Economic clock configuration singleton, set by `seteconomy`. The economic clock drives
      /// emissions accrual and pay, collateral-withdrawal maturity and the producer pay slot count;
      /// it is independent of the consensus epoch above and never waits on outpost consensus.
      struct [[sysio::table("economycfg")]] economic_config {
         uint32_t              epoch_duration_sec = 360;   ///< length of one economic epoch
         sysio::time_point_sec start_time{};               ///< where the grid starts; may be in the past

         SYSLIB_SERIALIZE(economic_config, (epoch_duration_sec)(start_time))
      };

      using economycfg_t = sysio::kv::global<"economycfg"_n, economic_config>;

      /// Economic clock state singleton. Economic epoch N spans [current_epoch_start,
      /// next_epoch_start) on a fixed grid from `economic_config::start_time`: each boundary is the
      /// previous one plus the duration in force, never the time a crank happened to run.
      struct [[sysio::table("economystate")]] economic_state {
         uint32_t   current_epoch_index = 0;   ///< 0 until the first tick
         time_point current_epoch_start{};
         time_point next_epoch_start{};
         /// Set by the first tick that finds no later boundary passed, and never cleared. Until then the clock is
         /// catching up, through a grid started in the past or behind a first tick run more than one epoch late, and
         /// each pay epoch closes its own period without spending any of it (sysio.system::payepoch reads this flag);
         /// from then on a pay epoch ticked while a later boundary has passed defers its payout to the tick that
         /// catches up.
         bool       caught_up = false;

         SYSLIB_SERIALIZE(economic_state, (current_epoch_index)(current_epoch_start)(next_epoch_start)(caught_up))
      };

      using economystate_t = sysio::kv::global<"economystate"_n, economic_state>;

      /// Emissions readiness gate block log: at most one row, the open block episode, keyed by the
      /// first economic epoch the gate blocked. Each further blocked economic epoch refreshes the row;
      /// the first economic epoch whose emissions pass erases it. A blocked gate holds emissions only:
      /// the economic clock and the consensus epoch keep running.
      struct blocklog_key {
         uint64_t epoch_index;
         uint64_t primary_key() const { return epoch_index; }
         SYSLIB_SERIALIZE(blocklog_key, (epoch_index))
      };

      struct [[sysio::table("blocklog")]] blocklog_entry {
         uint32_t                              epoch_index        = 0; ///< first blocked economic epoch
         uint32_t                              last_epoch_index   = 0; ///< latest blocked economic epoch
         sysio::opp::types::EmissionsBlockReason reason           =
            sysio::opp::types::EMISSIONS_BLOCK_REASON_UNSPECIFIED;
         /// What the gate tried to authorize: the period total on a pay epoch, otherwise the epoch's share.
         int64_t                               attempted_emission = 0;
         /// T5 budget not yet paid out, accrued or settling (net headroom), not floored at 0.
         int64_t                               treasury_remaining = 0;
         int64_t                               sysio_balance      = 0;
         uint32_t                              first_blocked_at   = 0; // unix seconds
         uint32_t                              last_blocked_at    = 0; // unix seconds
         uint32_t                              blocked_epochs     = 0; ///< economic epochs blocked so far

         SYSLIB_SERIALIZE(blocklog_entry,
            (epoch_index)(last_epoch_index)(reason)(attempted_emission)(treasury_remaining)
            (sysio_balance)(first_blocked_at)(last_blocked_at)(blocked_epochs))
      };

      using blocklog_t = sysio::kv::table<"blocklog"_n, blocklog_key, blocklog_entry>;

      // Well-known accounts
      static constexpr name CHALG_ACCOUNT  = "sysio.chalg"_n;
      static constexpr name MSGCH_ACCOUNT  = "sysio.msgch"_n;
      static constexpr name EPOCH_ACCOUNT  = "sysio.epoch"_n;
      static constexpr name OPREG_ACCOUNT  = "sysio.opreg"_n;
      static constexpr name AUTHEX_ACCOUNT = "sysio.authex"_n;
      static constexpr name CHAINS_ACCOUNT = "sysio.chains"_n;

      /// True while the depot is in its epoch-0 bootstrap window: no epoch state yet, or the
      /// current epoch index is still 0. Never throws. The one window test `sysio.liq`'s
      /// `regliqpool` and `sysio.synd`'s `importsynd` gate their launch ingestion on.
      static bool in_bootstrap_window() {
         epochstate_t es(EPOCH_ACCOUNT);
         return !es.exists() || es.get().current_epoch_index == 0;
      }

      /// The depot's current consensus epoch index, or 0 before the first epoch state is written.
      /// Never throws. Read by `sysio.msgch` and by `sysio.opreg`'s withdrawal consensus guard.
      static uint32_t current_epoch_index() {
         epochstate_t es(EPOCH_ACCOUNT);
         return es.exists() ? es.get().current_epoch_index : 0;
      }

      /// The current economic epoch index, or 0 before the economic clock's first tick. Never throws.
      static uint32_t current_economic_epoch_index() {
         economystate_t es(EPOCH_ACCOUNT);
         return es.exists() ? es.get().current_epoch_index : 0;
      }

      /// The economic epoch the grid is in at `now`: the last one whose boundary has passed, counting
      /// boundaries `tickeconomy` has not yet processed. Equals `current_economic_epoch_index()` while
      /// the crank keeps up. 0 before the first boundary or when the clock is not configured. Never throws.
      ///
      /// `seteconomy` never moves a boundary that has passed, so the index never decreases as time
      /// passes; once the clock has ticked it also keeps the next boundary, so the index of every time
      /// before that boundary is fixed.
      static uint32_t economic_epoch_index_at(time_point now) {
         economycfg_t cfg_tbl(EPOCH_ACCOUNT);
         if (!cfg_tbl.exists()) return 0;
         const economic_config cfg   = cfg_tbl.get();
         const economic_state  state = economystate_t(EPOCH_ACCOUNT).get_or_default(economic_state{});
         const time_point next = state.current_epoch_index == 0 ? time_point{cfg.start_time} : state.next_epoch_start;
         if (now < next) return state.current_epoch_index;
         const int64_t duration_us = static_cast<int64_t>(cfg.epoch_duration_sec) * 1'000'000;
         return state.current_epoch_index + 1 + static_cast<uint32_t>((now - next).count() / duration_us);
      }

      /// Length of the economic epoch the next `tickeconomy` closes: from the last boundary ticked to the next one,
      /// spaced when that boundary opened. The first tick has no earlier boundary and closes one epoch of the
      /// configured duration. The tick accrues the emission share and the producer pay slots of this length.
      static uint32_t closing_epoch_duration_sec(const economic_config& cfg, const economic_state& state) {
         if (state.current_epoch_index == 0) return cfg.epoch_duration_sec;
         return static_cast<uint32_t>((state.next_epoch_start - state.current_epoch_start).to_seconds());
      }

      /// The longest economic epoch a tick can still close: the one the next tick closes, or a later one of the
      /// configured duration. Bounds one pay period's accrual (`sysio.system`'s period accrual check).
      static uint32_t longest_accruing_epoch_sec(const economic_config& cfg, const economic_state& state) {
         return std::max(cfg.epoch_duration_sec, closing_epoch_duration_sec(cfg, state));
      }

      /// Bounds on the consensus `epoch_duration_sec`. The floor keeps an epoch longer than the
      /// relay's round trip to the slowest outpost; the ceiling stops a governance typo from setting
      /// a multi-year epoch.
      static constexpr uint32_t MIN_EPOCH_DURATION_SEC = 60;
      static constexpr uint32_t MAX_EPOCH_DURATION_SEC = 30u * 24u * 60u * 60u;

      /// Bounds on the economic `epoch_duration_sec`. Floor is a typo-guard: below it a pay period
      /// holds fewer block slots than one producer rotation, so the per-block pay in
      /// sysio.system::payepoch degenerates to a handful of slots per producer, masking misconfig.
      /// Ceiling bounds the slot arithmetic and stops a multi-year economic epoch.
      static constexpr uint32_t MIN_ECONOMIC_EPOCH_DURATION_SEC = 60;
      static constexpr uint32_t MAX_ECONOMIC_EPOCH_DURATION_SEC = 30u * 24u * 60u * 60u;

      /// Upper bound on the number of batch-operator groups: Solana's operator registry seats at most
      /// `MAX_OPERATOR_GROUPS` (8), and a window an outpost cannot seat stalls that outpost.
      static constexpr uint32_t MAX_BATCH_OP_GROUPS = 8;

      /// Upper bound on `operators_per_epoch` (the size of one schedule group): Ethereum's OPPInbound is
      /// sized and tested for `OPERATORS_PER_GROUP` (7), under Solana's `MAX_GROUP_MEMBERS` (16). Each
      /// member of the expiring group also costs two inline actions in `advance` (opreg::recorddel +
      /// opreg::termcheck) and a payclaims credit in sysio.system::payepoch.
      static constexpr uint32_t MAX_OPERATORS_PER_EPOCH = 7;

      /// Largest schedule window (operators_per_epoch * batch_op_groups, which the product equality pins
      /// `batch_operator_minimum_active` to). The OPERATORS roster and the BatchOperatorGroups attestation
      /// list only the window, so this also bounds both (about 5.6 KB of roster at the ceiling). Ethereum
      /// applies the roster in one handler call, so it must fit one transaction: at 8 x 7, with 49 operators
      /// Ethereum has never seen, it takes about 9.3M gas (12.4M with the outbound queue full) of EIP-7825's
      /// 16,777,216, while 8 x 16 runs the handler out of gas (WIRE-342).
      static constexpr uint32_t MAX_SCHEDULED_BATCH_OPERATORS = MAX_BATCH_OP_GROUPS * MAX_OPERATORS_PER_EPOCH;

   private:

      // Namespace alias for OPP protobuf enum types
      using OperatorType   = sysio::opp::types::OperatorType;
      using OperatorStatus = sysio::opp::types::OperatorStatus;
   };

   /// Asserts the caller signed the transaction and is a member of the current
   /// resident batch-operator group. This is a group-membership check only: the
   /// group is a snapshot from the last schedule, so current operator eligibility
   /// (sysio.opreg ACTIVE status) must be enforced separately at the delivery
   /// call site (see msgch::deliver).
   inline void is_batch_operator_active(const name& batch_op_name) {
      require_auth(batch_op_name);
      epoch::epochstate_t epoch_tbl(epoch::EPOCH_ACCOUNT);
      check(epoch_tbl.exists(), "epoch state not initialized");
      auto state = epoch_tbl.get();

      auto cur_group = state.current_batch_op_group;
      check(cur_group < state.batch_op_groups.size(), "active group index out of range");
      auto& active_members = state.batch_op_groups[cur_group];
      check(
         std::find(active_members.begin(), active_members.end(), batch_op_name) != active_members.end(),
         "caller is not in the active batch operator group"
      );
   }

} // namespace sysio

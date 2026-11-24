#pragma once

#include <sysio/sysio.hpp>
#include <sysio/kv_global.hpp>
#include <sysio/kv_table.hpp>
#include <sysio/slug_name.hpp>
#include <sysio/asset.hpp>
#include <sysio/crypto.hpp>
#include <sysio/system.hpp>
#include <sysio/opp/types/types.pb.hpp>
#include <sysio.opp.common/opp_table_types.hpp>

#include <algorithm>
#include <optional>
#include <vector>

namespace sysio {

   class [[sysio::contract("sysio.epoch")]] epoch : public contract {
   public:
      using contract::contract;

      // -----------------------------------------------------------------------
      //  Actions
      // -----------------------------------------------------------------------

      /// Set outpost `chain_code`'s consensus epoch configuration (privileged). Each outpost chain runs
      /// its own consensus epoch: counter, pacing, schedule window and roster.
      [[sysio::action]]
      void setconfig(sysio::slug_name chain_code,
                     uint32_t epoch_duration_sec,
                     uint32_t operators_per_epoch,
                     uint32_t batch_operator_minimum_active,
                     uint32_t batch_op_groups,
                     uint32_t epoch_retention_envelope_log_count);

      /// Close outpost `chain_code`'s current consensus epoch once its duration has elapsed. Auth:
      /// sysio.msgch (`bootstrap` / `bootchain` for the genesis advance, `chkcons` afterwards) or
      /// sysio.epoch. The genesis advance (epoch 0 -> 1) requires the chain to be an active outpost with
      /// its schedule window seated (`schbatchgps`). The first genesis advance of any chain is the
      /// network genesis.
      [[sysio::action]]
      void advance(sysio::slug_name chain_code);

      /// Internal continuation of `advance(chain_code)` after its epoch-close operator mutations execute.
      [[sysio::action]]
      void finishadv(sysio::slug_name chain_code, uint32_t epoch_index);

      /// Set the economic clock (privileged): the duration of one economic epoch and the time its grid
      /// starts. `start_time` may lie in the past -- the clock then catches up through the elapsed
      /// epochs once it starts -- and is fixed once the clock has ticked.
      [[sysio::action]]
      void seteconomy(uint32_t epoch_duration_sec, sysio::time_point_sec start_time);

      /// Open economic epoch `epoch_index` (permissionless crank). Refused unless `epoch_index` is the
      /// next index, its grid boundary has passed, and the network genesis has happened. Never waits on
      /// outpost consensus. Runs the emissions gate, then matures withdrawals, accrues the epoch's
      /// emission share, pays on a pay epoch, and steps the batch-duty settlement.
      [[sysio::action]]
      void tickeconomy(uint32_t epoch_index);

      /// Fill outpost `chain_code`'s schedule window from the ACTIVE batch operators serving it
      /// (`SERVING` in `sysio.opreg::servchains`).
      [[sysio::action]]
      void schbatchgps(sysio::slug_name chain_code);

      /// Pause outpost `chain_code`'s epoch advancement (only callable by sysio.chalg).
      [[sysio::action]]
      void pause(sysio::slug_name chain_code);

      /// Resume outpost `chain_code`'s epoch advancement (only callable by sysio.chalg).
      [[sysio::action]]
      void unpause(sysio::slug_name chain_code);

      /// Forget the roster last queued to outpost `code`, so the next advance queues the roster to it
      /// again: the recovery for an outpost that lost the roster it held. Auth: sysio.epoch.
      [[sysio::action]]
      void resendroster(sysio::slug_name code);

      // -----------------------------------------------------------------------
      //  Tables
      // -----------------------------------------------------------------------

      /// Primary key of the per-outpost tables: the outpost's `sysio.chains` code.
      struct chain_key {
         uint64_t chain_code;
         uint64_t primary_key() const { return chain_code; }
         SYSLIB_SERIALIZE(chain_key, (chain_code))
      };

      /// One outpost chain's consensus epoch configuration, set by `setconfig`.
      struct [[sysio::table("epochcfg")]] epoch_config {
         uint64_t    chain_code = 0;
         uint32_t    epoch_duration_sec = 360;   // 6 minutes
         uint32_t    operators_per_epoch = 7;
         uint32_t    batch_operator_minimum_active = 21;
         uint32_t    batch_op_groups = 3;          // rotation groups (21 / 7)

         /// Epochs of history this outpost keeps in the metadata-only `envelope_log` table on
         /// `sysio.msgch`: at most `2 * epoch_retention_envelope_log_count` rows (one inbound + one
         /// outbound record per epoch). Default 200 — matches the SOL/ETH per-direction
         /// metadata-log cap. Each log write reads this directly; runtime changes via `setconfig`
         /// take effect on the next write.
         uint32_t    epoch_retention_envelope_log_count = 200;

         SYSLIB_SERIALIZE(epoch_config,
            (chain_code)(epoch_duration_sec)(operators_per_epoch)
            (batch_operator_minimum_active)(batch_op_groups)
            (epoch_retention_envelope_log_count))
      };

      using epochcfg_t = sysio::kv::table<"epochcfg"_n, chain_key, epoch_config>;

      /// One outpost chain's consensus epoch state.
      struct [[sysio::table("epochstate")]] epoch_state {
         uint64_t                          chain_code = 0;
         uint32_t                          current_epoch_index = 0;
         time_point                        current_epoch_start{};
         time_point                        next_epoch_start{};
         uint8_t                           current_batch_op_group = 0; // 0, 1, or 2
         std::vector<std::vector<name>>    batch_op_groups;           // 3 groups of 7
         bool                              is_paused = false;

         SYSLIB_SERIALIZE(epoch_state,
            (chain_code)(current_epoch_index)(current_epoch_start)(next_epoch_start)
            (current_batch_op_group)(batch_op_groups)(is_paused))
      };

      using epochstate_t = sysio::kv::table<"epochstate"_n, chain_key, epoch_state>;

      /// Network genesis marker, written by the first genesis advance of any outpost chain and never
      /// erased. Its presence closes the bootstrap window and lets the economic clock tick.
      struct [[sysio::table("netgenesis")]] network_genesis {
         time_point genesis_at{};   ///< when the first chain's genesis advance ran

         SYSLIB_SERIALIZE(network_genesis, (genesis_at))
      };

      using netgenesis_t = sysio::kv::global<"netgenesis"_n, network_genesis>;

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

         SYSLIB_SERIALIZE(economic_state, (current_epoch_index)(current_epoch_start)(next_epoch_start))
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

      /// The last OPERATORS roster queued to each outpost, keyed by the outpost's `sysio.chains` code.
      /// `advance` queues the roster to an outpost only when its hash differs from the row's. An
      /// outpost activated after genesis has no row, so its first advance queues the roster to it.
      struct [[sysio::table("rostersent")]] roster_sent {
         uint64_t    chain_code = 0;
         checksum256 roster_hash; ///< SHA-256 of the encoded OPERATORS attestation last queued

         SYSLIB_SERIALIZE(roster_sent, (chain_code)(roster_hash))
      };

      using rostersent_t = sysio::kv::table<"rostersent"_n, chain_key, roster_sent>;

      // Well-known accounts
      static constexpr name CHALG_ACCOUNT  = "sysio.chalg"_n;
      static constexpr name MSGCH_ACCOUNT  = "sysio.msgch"_n;
      static constexpr name EPOCH_ACCOUNT  = "sysio.epoch"_n;
      static constexpr name OPREG_ACCOUNT  = "sysio.opreg"_n;
      static constexpr name AUTHEX_ACCOUNT = "sysio.authex"_n;
      static constexpr name CHAINS_ACCOUNT = "sysio.chains"_n;

      /// True once the network genesis has happened: the first genesis advance of any outpost chain.
      /// Never throws.
      static bool network_genesis_done() {
         return netgenesis_t(EPOCH_ACCOUNT).exists();
      }

      /// True while the depot is in its bootstrap window: before the network genesis. Never throws.
      /// The one window test `sysio.chains`, `sysio.tokens`, `sysio.liq` and `sysio.synd` gate their
      /// launch ingestion on.
      static bool in_bootstrap_window() {
         return !network_genesis_done();
      }

      /// Outpost `chain_code`'s consensus epoch state, or nullopt before it has any. Never throws.
      static std::optional<epoch_state> epoch_state_of(uint64_t chain_code) {
         return epochstate_t(EPOCH_ACCOUNT).try_get(chain_key{chain_code});
      }

      /// Outpost `chain_code`'s current consensus epoch index, or 0 before its genesis advance. Never throws.
      static uint32_t current_epoch_index(uint64_t chain_code) {
         const std::optional<epoch_state> state = epoch_state_of(chain_code);
         return state ? state->current_epoch_index : 0;
      }

      /// One outpost chain's current consensus epoch index.
      struct chain_epoch_index {
         uint64_t chain_code  = 0;
         uint32_t epoch_index = 0;
      };

      /// Every outpost chain whose consensus epoch has started (index > 0) with its current index, in
      /// chain-code order. Never throws. `sysio.opreg`'s withdrawal consensus guard reads it.
      static std::vector<chain_epoch_index> started_chain_epochs() {
         std::vector<chain_epoch_index> started;
         epochstate_t states(EPOCH_ACCOUNT);
         for (auto it = states.begin(); it != states.end(); ++it) {
            if (it->current_epoch_index == 0) continue;
            started.push_back(chain_epoch_index{.chain_code  = it->chain_code,
                                                .epoch_index = it->current_epoch_index});
         }
         return started;
      }

      /// The current economic epoch index, or 0 before the economic clock's first tick. Never throws.
      static uint32_t current_economic_epoch_index() {
         economystate_t es(EPOCH_ACCOUNT);
         return es.exists() ? es.get().current_epoch_index : 0;
      }

      /// The economic epoch the grid is in at `now`: the last one whose boundary has passed, counting
      /// boundaries `tickeconomy` has not yet processed. Equals `current_economic_epoch_index()` while
      /// the crank keeps up. 0 before the first boundary or when the clock is not configured. Never throws.
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

      /// Bounds on each chain's consensus `epoch_duration_sec`, checked by `setconfig(chain, ...)`. The
      /// floor is the hard minimum for any chain; a chain whose relay round trip needs longer is
      /// configured longer. The ceiling stops a governance typo from setting a multi-year epoch.
      static constexpr uint32_t MIN_EPOCH_DURATION_SEC = 10;
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

   /// Asserts the caller signed the transaction and is a member of outpost `chain_code`'s current
   /// resident batch-operator group. This is a group-membership check only: the
   /// group is a snapshot from the last schedule, so current operator eligibility
   /// (sysio.opreg ACTIVE status) must be enforced separately at the delivery
   /// call site (see msgch::deliver).
   inline void is_batch_operator_active(const name& batch_op_name, uint64_t chain_code) {
      require_auth(batch_op_name);
      const std::optional<epoch::epoch_state> state = epoch::epoch_state_of(chain_code);
      check(state.has_value(), "epoch state not initialized");

      const uint8_t cur_group = state->current_batch_op_group;
      check(cur_group < state->batch_op_groups.size(), "active group index out of range");
      const std::vector<name>& active_members = state->batch_op_groups[cur_group];
      check(
         std::find(active_members.begin(), active_members.end(), batch_op_name) != active_members.end(),
         "caller is not in the active batch operator group"
      );
   }

} // namespace sysio

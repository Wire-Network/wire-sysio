#pragma once

#include <sysio/sysio.hpp>
#include <sysio/kv_global.hpp>
#include <sysio/kv_table.hpp>
#include <sysio/asset.hpp>
#include <sysio/crypto.hpp>
#include <sysio/system.hpp>
#include <sysio/opp/types/types.pb.hpp>
#include <sysio.opp.common/opp_table_types.hpp>

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

      /// Emissions readiness gate block log. One row per epoch_index that
      /// the gate has blocked from advancing. Inserted on the first gate
      /// failure for a given epoch; same-reason retries update last_retry_at
      /// and retry_count without re-broadcast. Pruned when the gate
      /// eventually passes for that epoch (advance proceeds normally).
      struct blocklog_key {
         uint64_t epoch_index;
         uint64_t primary_key() const { return epoch_index; }
         SYSLIB_SERIALIZE(blocklog_key, (epoch_index))
      };

      struct [[sysio::table("blocklog")]] blocklog_entry {
         uint32_t                              epoch_index        = 0;
         sysio::opp::types::EmissionsBlockReason reason           =
            sysio::opp::types::EMISSIONS_BLOCK_REASON_UNSPECIFIED;
         int64_t                               attempted_emission = 0;
         int64_t                               treasury_remaining = 0;
         int64_t                               sysio_balance      = 0;
         uint32_t                              first_blocked_at   = 0; // unix seconds
         uint32_t                              last_retry_at      = 0; // unix seconds
         uint32_t                              retry_count        = 0;

         SYSLIB_SERIALIZE(blocklog_entry,
            (epoch_index)(reason)(attempted_emission)(treasury_remaining)
            (sysio_balance)(first_blocked_at)(last_retry_at)(retry_count))
      };

      using blocklog_t = sysio::kv::table<"blocklog"_n, blocklog_key, blocklog_entry>;

      // Well-known accounts
      static constexpr name CHALG_ACCOUNT  = "sysio.chalg"_n;
      static constexpr name MSGCH_ACCOUNT  = "sysio.msgch"_n;
      static constexpr name EPOCH_ACCOUNT  = "sysio.epoch"_n;
      static constexpr name OPREG_ACCOUNT  = "sysio.opreg"_n;
      static constexpr name AUTHEX_ACCOUNT = "sysio.authex"_n;
      static constexpr name CHAINS_ACCOUNT = "sysio.chains"_n;
      static constexpr name UWRIT_ACCOUNT  = "sysio.uwrit"_n;

      /// Bounds on `epoch_duration_sec`. Floor is a typo-guard: well below this
      /// value, `expected_rounds` in sysio.system::payepoch falls back to 1
      /// for any non-trivial epoch, masking misconfig. Ceiling bounds the
      /// `(epoch_duration_sec * 2) / TOTAL_BLOCKS_PER_ROUND` arithmetic and
      /// prevents governance typo from setting a multi-year epoch.
      static constexpr uint32_t MIN_EPOCH_DURATION_SEC = 60;
      static constexpr uint32_t MAX_EPOCH_DURATION_SEC = 30u * 24u * 60u * 60u;

      /// Upper bound on the number of batch-operator groups. The active group is
      /// carried on-chain and in the batch_operator_plugin as a `uint8_t`, which
      /// reserves 255 as its "not in any group" sentinel. Groups are indexed
      /// 0..batch_op_groups-1, so capping at 255 keeps the largest index (254)
      /// below the sentinel and inside the byte, preventing index truncation or a
      /// collision that would miselect batch operators.
      static constexpr uint32_t MAX_BATCH_OP_GROUPS = 255;

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

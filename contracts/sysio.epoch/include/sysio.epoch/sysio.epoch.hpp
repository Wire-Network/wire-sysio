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

      /// Register an outpost chain.
      [[sysio::action]]
      void regoutpost(opp::types::ChainKind chain_kind, uint32_t chain_id);

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

      /// Outpost registry table primary key.
      struct outpost_key {
         uint64_t id;
         uint64_t primary_key() const { return id; }
         SYSLIB_SERIALIZE(outpost_key, (id))
      };

      /// Outpost registry table.
      struct [[sysio::table("outposts")]] outpost_info {
         uint64_t    id;
         sysio::opp::types::ChainKind chain_kind;
         uint32_t    chain_id;
         checksum256 last_inbound_msg_id;
         checksum256 last_outbound_msg_id;
         uint32_t    last_inbound_epoch = 0;
         uint32_t    last_outbound_epoch = 0;

         uint64_t by_chain() const {
            return (static_cast<uint64_t>(chain_kind) << 32) | chain_id;
         }

         SYSLIB_SERIALIZE(outpost_info,
            (id)(chain_kind)(chain_id)(last_inbound_msg_id)(last_outbound_msg_id)
            (last_inbound_epoch)(last_outbound_epoch))
      };

      using outposts_t = sysio::kv::table<"outposts"_n, outpost_key, outpost_info,
         sysio::kv::index<"bychain"_n,
            sysio::const_mem_fun<outpost_info, uint64_t, &outpost_info::by_chain>>
      >;

      // Well-known accounts
      static constexpr name CHALG_ACCOUNT  = "sysio.chalg"_n;
      static constexpr name MSGCH_ACCOUNT  = "sysio.msgch"_n;
      static constexpr name EPOCH_ACCOUNT  = "sysio.epoch"_n;
      static constexpr name OPREG_ACCOUNT  = "sysio.opreg"_n;
      static constexpr name AUTHEX_ACCOUNT = "sysio.authex"_n;

   private:

      // Namespace alias for OPP protobuf enum types
      using OperatorType   = sysio::opp::types::OperatorType;
      using OperatorStatus = sysio::opp::types::OperatorStatus;
   };

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

#pragma once

#include <sysio/sysio.hpp>
#include <sysio/singleton.hpp>
#include <sysio/asset.hpp>
#include <sysio/crypto.hpp>
#include <sysio/system.hpp>
#include <sysio/opp/types/types.pb.hpp>

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
                     uint32_t attestation_retention_epoch_count);

      /// Advance epoch if duration elapsed (permissionless crank).
      [[sysio::action]]
      void advance();

      /// Group assignment — reads AVAILABLE batch ops from sysio.opreg.
      [[sysio::action]]
      void initgroups();

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
         uint32_t    attestation_retention_epoch_count = 1000;

         SYSLIB_SERIALIZE(epoch_config,
            (epoch_duration_sec)(operators_per_epoch)
            (batch_operator_minimum_active)(batch_op_groups)
            (attestation_retention_epoch_count))
      };

      using epochcfg_t = sysio::singleton<"epochcfg"_n, epoch_config>;

      /// Current epoch state singleton.
      struct [[sysio::table("epochstate")]] epoch_state {
         uint32_t                          current_epoch_index = 0;
         time_point                        current_epoch_start;
         time_point                        next_epoch_start;
         uint8_t                           current_batch_op_group = 0; // 0, 1, or 2
         std::vector<std::vector<name>>    batch_op_groups;           // 3 groups of 7
         checksum256                       last_consensus_hash;
         bool                              is_paused = false;

         SYSLIB_SERIALIZE(epoch_state,
            (current_epoch_index)(current_epoch_start)(next_epoch_start)
            (current_batch_op_group)(batch_op_groups)(last_consensus_hash)(is_paused))
      };

      using epochstate_t = sysio::singleton<"epochstate"_n, epoch_state>;

      /// Outpost registry table.
      struct [[sysio::table, sysio::contract("sysio.epoch")]] outpost_info {
         uint64_t    id;
         sysio::opp::types::ChainKind chain_kind;
         uint32_t    chain_id;
         checksum256 last_inbound_msg_id;
         checksum256 last_outbound_msg_id;
         uint32_t    last_inbound_epoch = 0;
         uint32_t    last_outbound_epoch = 0;

         uint64_t primary_key() const { return id; }
         uint64_t by_chain() const {
            return (static_cast<uint64_t>(chain_kind) << 32) | chain_id;
         }
      };

      using outposts_t = multi_index<"outposts"_n, outpost_info,
         indexed_by<"bychain"_n, const_mem_fun<outpost_info, uint64_t, &outpost_info::by_chain>>
      >;

      // Well-known accounts
      static constexpr name CHALG_ACCOUNT = "sysio.chalg"_n;
      static constexpr name MSGCH_ACCOUNT = "sysio.msgch"_n;
      static constexpr name EPOCH_ACCOUNT = "sysio.epoch"_n;
      static constexpr name OPREG_ACCOUNT = "sysio.opreg"_n;

   private:

      // Namespace alias for OPP protobuf enum types
      using OperatorType   = sysio::opp::types::OperatorType;
      using OperatorStatus = sysio::opp::types::OperatorStatus;
   };

   inline void is_batch_operator_active(const name& batch_op_name) {
      require_auth(batch_op_name);
      epoch::epochstate_t epoch_tbl(epoch::EPOCH_ACCOUNT, epoch::EPOCH_ACCOUNT.value);
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

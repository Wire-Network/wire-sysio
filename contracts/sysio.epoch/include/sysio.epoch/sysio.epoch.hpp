#pragma once

#include <sysio/sysio.hpp>
#include <sysio/singleton.hpp>
#include <sysio/asset.hpp>
#include <sysio/crypto.hpp>
#include <sysio/system.hpp>
#include <fc-lite/crypto/chain_types.hpp>
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
                     uint32_t warmup_epochs,
                     uint32_t cooldown_epochs);

      /// Register operator (processes OperatorAction attestation).
      [[sysio::action]]
      void regoperator(name account, opp::types::OperatorType type);

      /// Begin operator deregistration (cooldown).
      [[sysio::action]]
      void unregoper(name account);

      /// Advance epoch if duration elapsed (permissionless crank).
      [[sysio::action]]
      void advance();

      /// Force-activate an operator (privileged, for bootstrap).
      [[sysio::action]]
      void activateop(name account);

      /// One-time group assignment when all batch operators are active.
      [[sysio::action]]
      void initgroups();

      /// Replace operator in-place within their group.
      [[sysio::action]]
      void replaceop(name old_op, name new_op);

      /// Register an outpost chain.
      [[sysio::action]]
      void regoutpost(fc::crypto::chain_kind_t chain_kind, uint32_t chain_id);

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
         uint32_t    warmup_epochs = 1;
         uint32_t    cooldown_epochs = 1;

         SYSLIB_SERIALIZE(epoch_config,
            (epoch_duration_sec)(operators_per_epoch)
            (batch_operator_minimum_active)(batch_op_groups)(warmup_epochs)(cooldown_epochs))
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

      /// Operator roster table.
      struct [[sysio::table, sysio::contract("sysio.epoch")]] operator_info {
         name                              account;
         sysio::opp::types::OperatorType   type;
         sysio::opp::types::OperatorStatus status;
         uint32_t    registered_epoch;
         std::vector<std::pair<fc::crypto::chain_kind_t, checksum256>> chain_addresses;
         std::vector<std::pair<fc::crypto::chain_kind_t, int64_t>>   collateral;
         uint8_t     assigned_batch_op_group; // 0, 1, or 2
         uint32_t    last_elected_epoch;
         uint32_t    slash_count = 0;
         bool        is_blacklisted = false;

         uint64_t primary_key() const { return account.value; }
         uint64_t by_type() const { return static_cast<uint64_t>(type); }
         uint64_t by_status() const { return static_cast<uint64_t>(status); }
      };

      using operators_t = multi_index<"operators"_n, operator_info,
         indexed_by<"bytype"_n, const_mem_fun<operator_info, uint64_t, &operator_info::by_type>>,
         indexed_by<"bystatus"_n, const_mem_fun<operator_info, uint64_t, &operator_info::by_status>>
      >;

      /// Outpost registry table.
      struct [[sysio::table, sysio::contract("sysio.epoch")]] outpost_info {
         uint64_t    id;
         fc::crypto::chain_kind_t chain_kind;
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

   private:
      // Well-known accounts
      static constexpr name CHALG_ACCOUNT = "sysio.chalg"_n;
      static constexpr name MSGCH_ACCOUNT = "sysio.msgch"_n;

      // Namespace alias for OPP protobuf enum types
      using OperatorType   = sysio::opp::types::OperatorType;
      using OperatorStatus = sysio::opp::types::OperatorStatus;
   };

} // namespace sysio

#include <sysio/chain/deep_mind.hpp>
#include <sysio/chain/block_state.hpp>
#include <sysio/chain/contract_table_objects.hpp>
#include <sysio/chain/resource_limits_private.hpp>
#include <sysio/chain/permission_object.hpp>
#include <sysio/chain/global_property_object.hpp>
#include <sysio/chain/account_object.hpp>
#include <sysio/chain/protocol_feature_manager.hpp>
#include <fc/crypto/hex.hpp>

namespace {

   void set_trace_elapsed_to_zero(sysio::chain::action_trace& trace) {
      trace.elapsed = fc::microseconds{};
   }

   void set_trace_elapsed_to_zero(sysio::chain::transaction_trace& trace) {
      trace.elapsed = fc::microseconds{};
      for (auto& act_trace : trace.action_traces) {
         set_trace_elapsed_to_zero(act_trace);
      }
   }

}

namespace sysio::chain {

   void deep_mind_handler::update_config(deep_mind_config config)
   {
      _config = std::move(config);
   }

   void deep_mind_handler::update_logger(const std::string& logger_name)
   {
      fc::logger::update( logger_name, _logger );
   }

   void deep_mind_handler::on_startup(chainbase::database& db, uint32_t head_block_num)
   {
      // FIXME: We should probably feed that from CMake directly somehow ...
      fc_dlog(_logger, "DEEP_MIND_VERSION wire_sysio 1 0");

      fc_dlog(_logger, "ABIDUMP START {} {}", head_block_num, db.get<dynamic_global_property_object>().global_action_sequence);
      const auto& idx = db.get_index<account_metadata_index>();
      for (auto& row : idx.indices()) {
         if (row.abi.size() != 0) {
            std::string abi_str = fc::format_string("${data}", fc::mutable_variant_object("data", fc::variant(row.abi)));
            fc_dlog(_logger, "ABIDUMP ABI {} {}", row.name, abi_str);
         }
      }
      fc_dlog(_logger, "ABIDUMP END");
   }

   void deep_mind_handler::on_start_block(uint32_t block_num)
   {
      fc_dlog(_logger, "START_BLOCK {}", block_num);
   }

   void deep_mind_handler::on_accepted_block_v2(const block_id_type& id, block_num_type lib,
                                                const signed_block_ptr& b,
                                                const finality_data_t& fd,
                                                const proposer_policy_ptr& active_proposer_policy,
                                                const finalizer_policy_with_string_key& active_finalizer_policy)
   {
      assert(b);
      assert(active_proposer_policy);
      auto finality_data = fc::raw::pack(fd);
      auto packed_proposer_policy = fc::raw::pack(*active_proposer_policy);
      auto packed_finalizer_policy = fc::raw::pack(active_finalizer_policy);

      fc_dlog(_logger, "ACCEPTED_BLOCK_V2 {} {} {} {} {} {} {}",
         id,
         b->block_num(),
         lib,
         fc::to_hex(b->packed_signed_block()),
         fc::to_hex(finality_data),
         fc::to_hex(packed_proposer_policy),
         fc::to_hex(packed_finalizer_policy)
      );
   }

   void deep_mind_handler::on_switch_forks(const block_id_type& old_head, const block_id_type& new_head)
   {
      fc_dlog(_logger, "SWITCH_FORK {} {}", old_head, new_head);
   }

   void deep_mind_handler::on_onblock(const signed_transaction& trx)
   {
      auto packed_trx = fc::raw::pack(trx);

      fc_dlog(_logger, "TRX_OP CREATE onblock {} {}", trx.id(), fc::to_hex(packed_trx));
   }

   void deep_mind_handler::on_start_transaction()
   {
      _action_id = 0;
   }

   void deep_mind_handler::on_end_transaction()
   {
      _action_id = 0;
   }

   void deep_mind_handler::on_applied_transaction(uint32_t block_num, const transaction_trace_ptr& trace)
   {
      std::vector<char> packed_trace;
      
      if (_config.zero_elapsed) {
         transaction_trace trace_copy = *trace;
         set_trace_elapsed_to_zero(trace_copy);
         packed_trace = fc::raw::pack(trace_copy);

      } else {
         packed_trace = fc::raw::pack(*trace);
      }

      fc_dlog(_logger, "APPLIED_TRANSACTION {} {}", block_num, fc::to_hex(packed_trace) );
   }

   void deep_mind_handler::on_preactivate_feature(const protocol_feature& feature)
   {
      std::string f_str = fc::format_string("${data}", fc::mutable_variant_object("data", feature.to_variant()));
      fc_dlog(_logger, "FEATURE_OP PRE_ACTIVATE {} {} {}",
              _action_id, feature.feature_digest, std::move(f_str) );
   }

   void deep_mind_handler::on_activate_feature(const protocol_feature& feature)
   {
      std::string f_str = fc::format_string("${data}", fc::mutable_variant_object("data", feature.to_variant()));
      fc_dlog(_logger, "FEATURE_OP ACTIVATE {} {}",
              feature.feature_digest, std::move(f_str) );
   }

   void deep_mind_handler::on_input_action()
   {
      fc_dlog(_logger, "CREATION_OP ROOT {}", _action_id);
   }
   void deep_mind_handler::on_end_action()
   {
      ++_action_id;
   }
   void deep_mind_handler::on_require_recipient()
   {
      fc_dlog(_logger, "CREATION_OP NOTIFY {}", _action_id);
   }
   void deep_mind_handler::on_send_inline()
   {
      fc_dlog(_logger, "CREATION_OP INLINE {}", _action_id);
   }
   void deep_mind_handler::on_send_context_free_inline()
   {
      fc_dlog(_logger, "CREATION_OP CFA_INLINE {}", _action_id);
   }
   void deep_mind_handler::on_create_table(const table_id_object& tid)
   {
      fc_dlog(_logger, "TBL_OP INS {} {} {} {} {}",
         _action_id,
         tid.code,
         tid.scope,
         tid.table,
         tid.payer
      );
   }
   void deep_mind_handler::on_remove_table(const table_id_object& tid)
   {
      fc_dlog(_logger, "TBL_OP REM {} {} {} {} {}",
         _action_id,
         tid.code,
         tid.scope,
         tid.table,
         tid.payer
      );
   }
   void deep_mind_handler::on_db_store_i64(const table_id_object& tid, const key_value_object& kvo)
   {
      fc_dlog(_logger, "DB_OP INS {} {} {} {} {} {} {}",
         _action_id,
         kvo.payer,
         tid.code,
         tid.scope,
         tid.table,
         name(kvo.primary_key),
         fc::to_hex(kvo.value.data(), kvo.value.size())
      );
   }
   void deep_mind_handler::on_db_update_i64(const table_id_object& tid, const key_value_object& kvo, account_name payer, const char* buffer, std::size_t buffer_size)
   {
      fc_dlog(_logger, "DB_OP UPD {} {}:{} {} {} {} {} {}:{}",
         _action_id,
         kvo.payer,
         payer,
         tid.code,
         tid.scope,
         tid.table,
         name(kvo.primary_key),
         fc::to_hex(kvo.value.data(),kvo.value.size()),
         fc::to_hex(buffer, buffer_size)
      );
   }
   void deep_mind_handler::on_db_remove_i64(const table_id_object& tid, const key_value_object& kvo)
   {
      fc_dlog(_logger, "DB_OP REM {} {} {} {} {} {} {}",
         _action_id,
         kvo.payer,
         tid.code,
         tid.scope,
         tid.table,
         name(kvo.primary_key),
         fc::to_hex(kvo.value.data(), kvo.value.size())
      );
   }
   void deep_mind_handler::on_init_resource_limits(const resource_limits::resource_limits_config_object& config, const resource_limits::resource_limits_state_object& state)
   {
      std::string config_str = fc::format_string("${data}", fc::mutable_variant_object("data", config));
      std::string state_str = fc::format_string("${data}", fc::mutable_variant_object("data", state));
      fc_dlog(_logger, "RLIMIT_OP CONFIG INS {}",
         std::move(config_str)
      );
      fc_dlog(_logger, "RLIMIT_OP STATE INS {}",
         std::move(state_str)
      );
   }
   void deep_mind_handler::on_update_resource_limits_config(const resource_limits::resource_limits_config_object& config)
   {
      std::string config_str = fc::format_string("${data}", fc::mutable_variant_object("data", config));
      fc_dlog(_logger, "RLIMIT_OP CONFIG UPD {}",
         std::move(config_str)
      );
   }
   void deep_mind_handler::on_update_resource_limits_state(const resource_limits::resource_limits_state_object& state)
   {
      std::string state_str = fc::format_string("${data}", fc::mutable_variant_object("data", state));
      fc_dlog(_logger, "RLIMIT_OP STATE UPD {}",
         std::move(state_str)
      );
   }

   // maintain expected format
   struct resource_limits_object {
      account_name owner; //< owner should not be changed within a chainbase modifier lambda

      int64_t net_weight = -1;
      int64_t cpu_weight = -1;
      int64_t ram_bytes = -1;
   };
   struct resource_usage_object {
      account_name owner; //< owner should not be changed within a chainbase modifier lambda

      resource_limits::usage_accumulator net_usage;
      resource_limits::usage_accumulator cpu_usage;
      uint64_t                           ram_usage = 0;
   };

   void deep_mind_handler::on_newaccount_resource_limits(const resource_limits::resource_object& obj)
   {
      resource_limits_object limits{
         .owner = obj.owner,
         .net_weight = obj.net_weight,
         .cpu_weight = obj.cpu_weight,
         .ram_bytes = obj.ram_bytes,
      };
      std::string limits_str = fc::format_string("${data}", fc::mutable_variant_object("data", limits));
      fc_dlog(_logger, "RLIMIT_OP ACCOUNT_LIMITS INS {}",
         std::move(limits_str)
      );
      resource_usage_object usage{
         .owner = obj.owner,
         .net_usage = obj.net_usage,
         .cpu_usage = obj.cpu_usage,
         .ram_usage = obj.ram_usage,
      };
      std::string usage_str = fc::format_string("${data}", fc::mutable_variant_object("data", usage));
      fc_dlog(_logger, "RLIMIT_OP ACCOUNT_USAGE INS {}",
         std::move(usage_str)
      );
   }
   void deep_mind_handler::on_update_account_usage(const resource_limits::resource_object& obj)
   {
      resource_usage_object usage{
         .owner = obj.owner,
         .net_usage = obj.net_usage,
         .cpu_usage = obj.cpu_usage,
         .ram_usage = obj.ram_usage,
      };
      std::string usage_str = fc::format_string("${data}", fc::mutable_variant_object("data", usage));
      fc_dlog(_logger, "RLIMIT_OP ACCOUNT_USAGE UPD {}",
         std::move(usage_str)
      );
   }
   void deep_mind_handler::on_set_account_limits(const resource_limits::resource_pending_object& limits)
   {
      std::string limits_str = fc::format_string("${data}", fc::mutable_variant_object("data", limits));
      fc_dlog(_logger, "RLIMIT_OP ACCOUNT_LIMITS UPD {}",
         std::move(limits_str)
      );
   }
   void deep_mind_handler::on_ram_trace(std::string&& event_id, const char* family, const char* operation, const char* legacy_tag)
   {
      _ram_trace = ram_trace(std::move(event_id), family, operation, legacy_tag);
   }
   void deep_mind_handler::on_ram_event(account_name account, uint64_t new_usage, int64_t delta)
   {
      fc_dlog(_logger, "RAM_OP {} {} {} {} {} {} {} {}",
         _action_id,
         _ram_trace.event_id,
         _ram_trace.family,
         _ram_trace.operation,
         _ram_trace.legacy_tag,
         account,
         new_usage,
         delta
      );
      _ram_trace = ram_trace();
   }

   void deep_mind_handler::on_create_permission(const permission_object& p)
   {
      std::string p_str = fc::format_string("${data}", fc::mutable_variant_object("data", p));
      fc_dlog(_logger, "PERM_OP INS {} {} {}",
         _action_id,
         p.id,
         p_str
      );
   }
   void deep_mind_handler::on_modify_permission(const permission_object& old_permission, const permission_object& new_permission)
   {
      std::string p_str = fc::format_string("${data}", fc::mutable_variant_object()
                                                          ("old", old_permission)
                                                          ("new", new_permission));
      fc_dlog(_logger, "PERM_OP UPD {} {} {}",
         _action_id,
         new_permission.id,
         p_str
      );
   }
   void deep_mind_handler::on_remove_permission(const permission_object& permission)
   {
      std::string p_str = fc::format_string("${data}", fc::mutable_variant_object("data", permission));
      fc_dlog(_logger, "PERM_OP REM {} {} {}",
        _action_id,
        permission.id,
        p_str
      );
   }

}

FC_REFLECT(sysio::chain::resource_limits_object, (owner)(net_weight)(cpu_weight)(ram_bytes));
FC_REFLECT(sysio::chain::resource_usage_object, (owner)(net_usage)(cpu_usage)(ram_usage));

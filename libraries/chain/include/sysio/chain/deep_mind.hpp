#pragma once

#include <fc/log/logger.hpp>

#include <sysio/chain/types.hpp>
#include <sysio/chain/block.hpp>

namespace sysio::chain {

struct kv_row;
struct kv_index_entry;
class permission_object;
struct protocol_feature;
struct signed_transaction;
struct packed_transaction;
struct transaction_trace;
struct ram_trace;
struct finality_data_t;
namespace resource_limits {
   class resource_limits_config_object;
   class resource_limits_state_object;
   struct resource_object;
   struct resource_pending_object;
}

#define RAM_EVENT_ID( FORMAT, ... ) \
   fc::format_string( FORMAT, fc::mutable_variant_object()__VA_ARGS__ )

struct ram_trace {
public:
   ram_trace(std::string&& event_id, const char* family, const char* operation, const char* legacy_tag)
      :event_id(std::move(event_id)),family(family),operation(operation),legacy_tag(legacy_tag)
   {}

   std::string  event_id   = "generic";
   const char*  family     = "generic";
   const char*  operation  = "generic";
   const char*  legacy_tag = "generic";

   ram_trace()
   {}
};

/// Names one kind of KV RAM charge on its RAM_OP line.
struct kv_ram_op {
   const char* family;
   const char* operation;
   const char* legacy_tag;
};

/// The kinds of RAM charge a KV write can make for one type of object.
struct kv_ram_ops {
   kv_ram_op add;
   kv_ram_op update;
   kv_ram_op remove;
   kv_ram_op update_remove_old_payer;
   kv_ram_op update_add_new_payer;
};

/// KV rows keep the family and tag names that contract table rows have always had on RAM_OP lines.
inline constexpr kv_ram_ops kv_row_ram_ops = {
   .add                     = {"table_row", "add", "primary_index_add"},
   .update                  = {"table_row", "update", "primary_index_update"},
   .remove                  = {"table_row", "remove", "primary_index_remove"},
   .update_remove_old_payer = {"table_row", "remove", "primary_index_update_remove_old_payer"},
   .update_add_new_payer    = {"table_row", "add", "primary_index_update_add_new_payer"},
};

/// KV index entries keep the secondary_index family. `update` is new, since legacy secondary keys had a fixed size, and
/// the old payer's refund gets the same tag shape as a row's.
inline constexpr kv_ram_ops kv_index_ram_ops = {
   .add                     = {"secondary_index", "add", "secondary_index_add"},
   .update                  = {"secondary_index", "update", "secondary_index_update"},
   .remove                  = {"secondary_index", "remove", "secondary_index_remove"},
   .update_remove_old_payer = {"secondary_index", "remove", "secondary_index_update_remove_old_payer"},
   .update_add_new_payer    = {"secondary_index", "add", "secondary_index_update_add_new_payer"},
};

/// What a KV RAM charge is for, as its RAM_OP event id gives it. `key` is the primary key. `sec_key` is empty for a
/// row.
struct kv_ram_subject {
   uint16_t         table_id = 0;
   std::string_view key;
   std::string_view sec_key = {};
};

class deep_mind_handler
{
public:
   struct deep_mind_config {
      bool zero_elapsed = false; // if true, the elapsed field of transaction and action traces is always set to 0 (for reproducibility)
   };

   void update_config(deep_mind_config config);

   void update_logger(const std::string& logger_name);
   enum class operation_qualifier { none, modify, push };

   void on_startup(chainbase::database& db, uint32_t head_block_num);
   void on_start_block(uint32_t block_num);
   void on_accepted_block_v2(const block_id_type& id, block_num_type lib,
                             const signed_block_ptr& b,
                             const finality_data_t& fd,
                             const proposer_policy_ptr& active_proposer_policy,
                             const finalizer_policy_with_string_key& active_finalizer_policy);
   void on_switch_forks(const block_id_type& old_head, const block_id_type& new_head);
   void on_onblock(const signed_transaction& trx);
   void on_start_transaction();
   void on_end_transaction();
   void on_applied_transaction(uint32_t block_num, const std::shared_ptr<transaction_trace>& trace);
   void on_preactivate_feature(const protocol_feature& feature);
   void on_activate_feature(const protocol_feature& feature);
   void on_input_action();
   void on_end_action();
   void on_require_recipient();
   void on_send_inline();
   void on_kv_set(const kv_row& row, bool is_new, account_name old_payer = account_name(),
                  const char* old_value = nullptr, std::size_t old_value_size = 0);
   void on_kv_erase(const kv_row& row);
   /// Logs a KV_IDX_OP INS line for an entry just stored.
   void on_kv_idx_store(const kv_index_entry& entry);
   /// Logs a KV_IDX_OP UPD line. `entry` is the entry after the update, and the other two are what it held before.
   void on_kv_idx_update(const kv_index_entry& entry, account_name old_payer, std::string_view old_sec_key);
   /// Logs a KV_IDX_OP REM line for an entry about to be erased.
   void on_kv_idx_erase(const kv_index_entry& entry);
   void on_init_resource_limits(const resource_limits::resource_limits_config_object& config, const resource_limits::resource_limits_state_object& state);
   void on_update_resource_limits_config(const resource_limits::resource_limits_config_object& config);
   void on_update_resource_limits_state(const resource_limits::resource_limits_state_object& state);
   void on_newaccount_resource_limits(const resource_limits::resource_object& limits);
   void on_update_account_usage(const resource_limits::resource_object& usage);
   void on_set_account_limits(const resource_limits::resource_pending_object& limits);
   // The trace is consumed by the next ram_event or ram_correction
   void on_ram_trace(std::string&& event_id, const char* family, const char* operation, const char* legacy_tag);
   // A KV write's trace, consumed the same way. A write can make no charge or two, so set it right before each charge.
   void on_kv_ram_trace(const kv_ram_op& op, account_name code, const kv_ram_subject& subject);
   void on_ram_event(account_name account, uint64_t new_usage, int64_t delta);
   void on_create_permission(const permission_object& p);
   void on_modify_permission(const permission_object& old_permission, const permission_object& new_permission);
   void on_remove_permission(const permission_object& permission);
private:
   uint32_t         _action_id = 0;
   ram_trace        _ram_trace;
   deep_mind_config _config;
   fc::logger       _logger;
};

}

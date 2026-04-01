#pragma once
#include <sysio/chain/controller.hpp>
#include <sysio/chain/transaction.hpp>
#include <sysio/chain/transaction_context.hpp>
#include <sysio/chain/kv_table_objects.hpp>
#include <sysio/chain/kv_context.hpp>
#include <sysio/chain/deep_mind.hpp>
#include <boost/unordered/unordered_flat_map.hpp>
#include <fc/utility.hpp>
#include <sstream>
#include <algorithm>
#include <set>

namespace chainbase { class database; }

namespace sysio { namespace chain {

class controller;
class account_object;

class apply_context {

   /// Constructor
   public:
      apply_context(controller& con, transaction_context& trx_ctx, uint32_t action_ordinal, uint32_t depth=0);

   /// Execution methods:
   public:

      void exec_one();
      void exec();
      void execute_inline( action&& a );
      void execute_context_free_inline( action&& a );

   protected:
      uint32_t schedule_action( uint32_t ordinal_of_action_to_schedule, account_name receiver, bool context_free );
      uint32_t schedule_action( action&& act_to_schedule, account_name receiver, bool context_free );


   /// Authorization methods:
   public:

      /**
       * @brief Require @ref account to have approved of this message
       * @param account The account whose approval is required
       *
       * This method will check that @ref account is listed in the message's declared authorizations, and marks the
       * authorization as used. Note that all authorizations on a message must be used, or the message is invalid.
       *
       * @throws missing_auth_exception If no sufficient permission was found
       */
      void require_authorization(const account_name& account);
      bool has_authorization(const account_name& account) const;
      void require_authorization(const account_name& account, const permission_name& permission);

      /**
       * @return true if account exists, false if it does not
       */
      bool is_account(const account_name& account)const;

      void get_code_hash(
         account_name account, uint64_t& code_sequence, fc::sha256& code_hash, uint8_t& vm_type, uint8_t& vm_version) const;

      /**
       * Requires that the current action be delivered to account
       */
      void require_recipient(account_name account);

      /**
       * Return true if the current action has already been scheduled to be
       * delivered to the specified account.
       */
      bool has_recipient(account_name account)const;

   /// Console methods:
   public:

      void console_append( std::string_view val ) {
         _pending_console_output += val;
      }

   /// KV Database methods:
   public:

      // Primary KV operations
      int64_t  kv_set(uint8_t key_format, uint64_t payer, const char* key, uint32_t key_size, const char* value, uint32_t value_size);
      int32_t  kv_get(uint8_t key_format, name code, const char* key, uint32_t key_size, char* value, uint32_t value_size);
      int64_t  kv_erase(uint8_t key_format, const char* key, uint32_t key_size);
      int32_t  kv_contains(uint8_t key_format, name code, const char* key, uint32_t key_size);

      // Primary KV iterators
      uint32_t kv_it_create(uint8_t key_format, name code, const char* prefix, uint32_t prefix_size);
      void     kv_it_destroy(uint32_t handle);
      int32_t  kv_it_status(uint32_t handle);
      int32_t  kv_it_next(uint32_t handle);
      int32_t  kv_it_prev(uint32_t handle);
      int32_t  kv_it_lower_bound(uint32_t handle, const char* key, uint32_t key_size);
      int32_t  kv_it_key(uint32_t handle, uint32_t offset, char* dest, uint32_t dest_size, uint32_t& actual_size);
      int32_t  kv_it_value(uint32_t handle, uint32_t offset, char* dest, uint32_t dest_size, uint32_t& actual_size);

      // Secondary KV index operations
      void     kv_idx_store(uint64_t payer, name table, uint8_t index_id,
                            const char* pri_key, uint32_t pri_key_size,
                            const char* sec_key, uint32_t sec_key_size);
      void     kv_idx_remove(name table, uint8_t index_id,
                             const char* pri_key, uint32_t pri_key_size,
                             const char* sec_key, uint32_t sec_key_size);
      void     kv_idx_update(uint64_t payer, name table, uint8_t index_id,
                             const char* pri_key, uint32_t pri_key_size,
                             const char* old_sec_key, uint32_t old_sec_key_size,
                             const char* new_sec_key, uint32_t new_sec_key_size);
      int32_t  kv_idx_find_secondary(name code, name table, uint8_t index_id,
                                     const char* sec_key, uint32_t sec_key_size);
      int32_t  kv_idx_lower_bound(name code, name table, uint8_t index_id,
                                  const char* sec_key, uint32_t sec_key_size);
      int32_t  kv_idx_next(uint32_t handle);
      int32_t  kv_idx_prev(uint32_t handle);
      int32_t  kv_idx_key(uint32_t handle, uint32_t offset, char* dest, uint32_t dest_size, uint32_t& actual_size);
      int32_t  kv_idx_primary_key(uint32_t handle, uint32_t offset, char* dest, uint32_t dest_size, uint32_t& actual_size);
      void     kv_idx_destroy(uint32_t handle);

   /// Database methods:
   public:

      void update_db_usage( const account_name& payer, int64_t delta );

   private:

      void validate_account_ram_deltas();

   /// Misc methods:
   public:


      int get_action( uint32_t type, uint32_t index, char* buffer, size_t buffer_size )const;
      int get_context_free_data( uint32_t index, char* buffer, size_t buffer_size )const;
      vector<account_name> get_active_producers() const;

      uint64_t next_global_sequence();
      uint64_t next_recv_sequence( const account_object& receiver_account );
      uint64_t next_auth_sequence( account_name actor );

      void add_ram_usage( account_name account, int64_t ram_delta );
      void finalize_trace( action_trace& trace, const fc::time_point& start );

      bool is_context_free()const { return context_free; }
      bool is_privileged()const { return privileged; }
      action_name get_receiver()const { return receiver; }
      const action& get_action()const { return *act; }

      action_name get_sender() const;

      bool is_applying_block() const { return trx_context.explicit_billed_cpu_time; }
      bool is_sys_vm_oc_whitelisted() const;
      bool should_use_sys_vm_oc()const;

   /// Fields:
   public:

      controller&                   control;
      chainbase::database&          db;  ///< database where state is stored
      transaction_context&          trx_context; ///< transaction context in which the action is running

   private:
      const action*                 act = nullptr; ///< action being applied
      // act pointer may be invalidated on call to trx_context.schedule_action
      account_name                  receiver; ///< the code that is currently running
      uint32_t                      recurse_depth; ///< how deep inline actions can recurse
      uint32_t                      first_receiver_action_ordinal = 0;
      uint32_t                      action_ordinal = 0;
      bool                          privileged   = false;
      bool                          context_free = false;

   public:
      std::vector<char>             action_return_value;

   private:

      kv_iterator_pool                    kv_iterators;
      std::optional<const account_metadata_object*> _first_receiver_metadata; ///< cached across notification dispatch
      vector< std::pair<account_name, uint32_t> > _notified; ///< keeps track of new accounts to be notifed of current message
      vector<uint32_t>                    _inline_actions; ///< action_ordinals of queued inline actions
      vector<uint32_t>                    _cfa_inline_actions; ///< action_ordinals of queued inline context-free actions
      std::string                         _pending_console_output;
      flat_set<account_delta>             _account_ram_deltas; ///< flat_set of account_delta so json is an array of objects

      //bytes                               _cached_trx;
};

using apply_handler = std::function<void(apply_context&)>;

} } // namespace sysio::chain

//FC_REFLECT(sysio::chain::apply_context::apply_results, (applied_actions)(deferred_transaction_requests)(deferred_transactions_count))

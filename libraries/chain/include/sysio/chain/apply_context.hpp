#pragma once
#include <sysio/chain/controller.hpp>
#include <sysio/chain/transaction.hpp>
#include <sysio/chain/transaction_context.hpp>
#include <sysio/chain/kv_table_objects.hpp>
#include <sysio/chain/kv_context.hpp>
#include <sysio/chain/deep_mind.hpp>
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

   protected:
      uint32_t schedule_action( uint32_t ordinal_of_action_to_schedule, account_name receiver );
      uint32_t schedule_action( action&& act_to_schedule, account_name receiver );


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

      // These implement the KV intrinsics, so webassembly/interface.hpp gives their full contract. All of them act on
      // the receiver's tables except the reads, which take a `code`. Handles last for one receiver's execution.

      // Primary KV operations

      /// Writes a row in the receiver's table. Payer 0 keeps an existing row's payer and is invalid for a new row.
      kv_status kv_set(uint16_t table_id, uint64_t payer, std::string_view key, std::string_view value, kv_set_mode mode);
      /// Erases a row and refunds its current payer.
      kv_status kv_erase(uint16_t table_id, std::string_view key);
      /// Copies a row's value from `offset`, and returns its full size, or -1 if the key is free.
      int32_t   kv_get(uint16_t table_id, name code, std::string_view key, uint32_t offset, char* value, uint32_t value_size);

      // Secondary KV index operations

      /// Stores an entry. Payer 0 is invalid unless the position is taken, which returns kv_status::exists.
      kv_status kv_idx_store(uint16_t table_id, uint64_t payer, std::string_view pri_key, std::string_view sec_key);
      /// Moves an entry to `new_sec_key`. Payer 0 keeps its payer. Returns kv_status::exists if that position is taken.
      kv_status kv_idx_update(uint16_t table_id, uint64_t payer, std::string_view pri_key,
                              std::string_view old_sec_key, std::string_view new_sec_key);
      /// Erases an entry and refunds its current payer.
      kv_status kv_idx_erase(uint16_t table_id, std::string_view pri_key, std::string_view sec_key);

      // Primary KV iterators

      /// Opens an iterator over the rows that start with `prefix`, at the first one at or after `seek`.
      int32_t   kv_it_create(uint16_t table_id, name code, std::string_view prefix, std::string_view seek);
      /// Steps forward and reads the row it lands on.
      kv_status kv_it_next(int32_t handle, kv_row_buffers& row);
      /// Steps back and reads the row it lands on. From end, lands on the last row in the prefix.
      kv_status kv_it_prev(int32_t handle, kv_row_buffers& row);
      /// Moves to the first row at or after `seek` and reads it.
      kv_status kv_it_lower_bound(int32_t handle, std::string_view seek, kv_row_buffers& row);
      /// Reads the row at the iterator's key, with the value from `offset`, without moving.
      kv_status kv_it_read(int32_t handle, uint32_t offset, kv_row_buffers& row);
      /// Returns a handle to the pool.
      void      kv_it_destroy(int32_t handle);

      // Secondary KV iterators

      /// Opens an iterator over the entries whose secondary key starts with `prefix`, at the first at or after `seek`.
      int32_t   kv_idx_create(uint16_t table_id, name code, std::string_view prefix, std::string_view seek);
      /// Steps forward and reads the entry it lands on.
      kv_status kv_idx_next(int32_t handle, kv_row_buffers& entry);
      /// Steps back and reads the entry it lands on. From end, lands on the last entry in the prefix.
      kv_status kv_idx_prev(int32_t handle, kv_row_buffers& entry);
      /// Moves to the first entry whose secondary key is at or after `seek` and reads it.
      kv_status kv_idx_lower_bound(int32_t handle, std::string_view seek, kv_row_buffers& entry);
      /// Reads the entry at the iterator's position without moving.
      kv_status kv_idx_read(int32_t handle, kv_row_buffers& entry);
      /// Returns a handle to the pool.
      void      kv_idx_destroy(int32_t handle);
      /// Opens an iterator on the first entry with exactly this secondary key, or returns -1 and takes no slot.
      int32_t   kv_idx_find(uint16_t table_id, name code, std::string_view prefix, std::string_view sec_key);

   /// Database methods:
   public:

      void update_db_usage( const account_name& payer, int64_t delta );

   private:

      void validate_account_ram_deltas();

   /// Misc methods:
   public:


      int get_action( uint32_t type, uint32_t index, char* buffer, size_t buffer_size )const;
      vector<account_name> get_active_producers() const;

      uint64_t next_global_sequence();
      uint64_t next_recv_sequence( const account_object& receiver_account );
      uint64_t next_auth_sequence( account_name actor );

      void add_ram_usage( account_name account, int64_t ram_delta );
      void finalize_trace( action_trace& trace, const fc::time_point& start );

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
      kv_store&                     kv;  ///< engine holding KV rows and index entries
      transaction_context&          trx_context; ///< transaction context in which the action is running

   private:
      const action*                 act = nullptr; ///< action being applied
      // act pointer may be invalidated on call to trx_context.schedule_action
      account_name                  receiver; ///< the code that is currently running
      uint32_t                      recurse_depth; ///< how deep inline actions can recurse
      uint32_t                      first_receiver_action_ordinal = 0;
      uint32_t                      action_ordinal = 0;
      bool                          privileged   = false;

   public:
      std::vector<char>             action_return_value;

   private:

      kv_primary_iterator_pool            kv_primary_iterators;
      kv_secondary_iterator_pool          kv_secondary_iterators;
      /// Searches for kv_idx_find. A hit hands it to the new slot.
      std::unique_ptr<kv_index_cursor>    kv_idx_find_cursor;

      /// Bills one KV RAM charge, and names it for deep mind.
      void kv_update_usage( const kv_ram_op& op, const kv_ram_subject& subject, account_name payer, int64_t delta );
      /// Bills a row's RAM change, which on a payer change moves the whole charge from the old payer to the new one.
      void kv_update_row_usage( const kv_ram_ops& ops, const kv_ram_subject& subject, account_name old_payer,
                                int64_t old_billable, account_name payer, int64_t new_billable );
      /// Fails a payer with no account before it can fail deep in billing with no useful message.
      void kv_check_payer( account_name payer ) const;

      std::optional<const account_metadata_object*> _first_receiver_metadata; ///< cached across notification dispatch
      vector< std::pair<account_name, uint32_t> > _notified; ///< keeps track of new accounts to be notifed of current message
      vector<uint32_t>                    _inline_actions; ///< action_ordinals of queued inline actions
      std::string                         _pending_console_output;
      flat_set<account_delta>             _account_ram_deltas; ///< flat_set of account_delta so json is an array of objects

      //bytes                               _cached_trx;
};

using apply_handler = std::function<void(apply_context&)>;

} } // namespace sysio::chain

//FC_REFLECT(sysio::chain::apply_context::apply_results, (applied_actions)(deferred_transaction_requests)(deferred_transactions_count))

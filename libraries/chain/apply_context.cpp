#include <algorithm>
#include <optional>
#include <sysio/chain/apply_context.hpp>
#include <sysio/chain/kv_iterator_steps.hpp>
#include <sysio/chain/controller.hpp>
#include <sysio/chain/transaction_context.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/authorization_manager.hpp>
#include <sysio/chain/resource_limits.hpp>
#include <sysio/chain/account_object.hpp>
#include <sysio/chain/code_object.hpp>
#include <sysio/chain/global_property_object.hpp>
#include <sysio/chain/deep_mind.hpp>
#include <boost/container/flat_set.hpp>

using boost::container::flat_set;

namespace sysio::chain {

// RAM billing formulas for kv primary rows and secondary index rows. Variable
// key/value bytes plus a fixed overhead, which is a protocol constant in config.hpp.
static inline int64_t kv_object_ram(uint64_t key_size, uint64_t value_size) {
   return static_cast<int64_t>(key_size + value_size + config::kv_row_overhead_ram_bytes);
}
static inline int64_t kv_object_ram(const kv_row& row) {
   return kv_object_ram(row.key.size(), row.value.size());
}
static inline int64_t kv_index_object_ram(uint64_t sec_key_size, uint64_t pri_key_size) {
   return static_cast<int64_t>(sec_key_size + pri_key_size + config::kv_index_entry_overhead_ram_bytes);
}
static inline int64_t kv_index_object_ram(const kv_index_entry& entry) {
   return kv_index_object_ram(entry.sec_key.size(), entry.pri_key.size());
}

static inline void print_debug(account_name receiver, const action_trace& ar) {
   if (!ar.console.empty()) {
      if (fc::logger::default_logger().is_enabled( fc::log_level::debug )) {
         std::string prefix;
         prefix.reserve(3 + 13 + 1 + 13 + 3 + 13 + 1);
         prefix += "\n[(";
         prefix += ar.act.account.to_string();
         prefix += ",";
         prefix += ar.act.name.to_string();
         prefix += ")->";
         prefix += receiver.to_string();
         prefix += "]";

         std::string output;
         output.reserve(512);
         output += prefix;
         output += ": CONSOLE OUTPUT BEGIN =====================\n";
         output += ar.console;
         output += prefix;
         output += ": CONSOLE OUTPUT END   =====================";
         dlog( "{}", std::move(output) );
      }
   }
}

apply_context::apply_context(controller& con, transaction_context& trx_ctx, uint32_t action_ordinal, uint32_t depth)
:control(con)
,db(con.mutable_db())
,kv(con.mutable_kv())
,trx_context(trx_ctx)
,recurse_depth(depth)
,first_receiver_action_ordinal(action_ordinal)
,action_ordinal(action_ordinal)
{
   action_trace& trace = trx_ctx.get_action_trace(action_ordinal);
   act = &trace.act;
   receiver = trace.receiver;
   context_free = trace.context_free;
}

void apply_context::exec_one()
{
   auto start = fc::time_point::now();

   // Notified contracts share this context, but a contract's handles must not depend on what ran before it.
   kv_primary_iterators.release_all();
   kv_secondary_iterators.release_all();

   digest_type act_digest;

   const account_object* receiver_account = nullptr;
   const account_metadata_object* receiver_account_metadata = nullptr;

   auto handle_exception = [&](const auto& e)
   {
      action_trace& trace = trx_context.get_action_trace( action_ordinal );
      trace.error_code = controller::convert_exception_to_error_code( e );
      trace.except = e;
      finalize_trace( trace, start );
      throw;
   };

   try {
      try {
         action_return_value.clear();
         receiver_account = &db.get<account_object,by_name>( receiver );
         receiver_account_metadata = db.find<account_metadata_object,by_name>( receiver );
         if( !(context_free && control.skip_trx_checks()) ) {
            privileged = receiver_account_metadata != nullptr && receiver_account_metadata->is_privileged();
            auto native = control.find_apply_handler( receiver, act->account, act->name );
            if( native ) {
               if( trx_context.enforce_whiteblacklist && control.is_speculative_block() ) {
                  control.check_contract_list( receiver );
                  control.check_action_list( act->account, act->name );
               }
               (*native)( *this );
            }

            if( receiver_account_metadata != nullptr && receiver_account_metadata->code_hash != digest_type() ) {
               if( trx_context.enforce_whiteblacklist && control.is_speculative_block() ) {
                  control.check_contract_list( receiver );
                  control.check_action_list( act->account, act->name );
               }
               try {
                  control.get_wasm_interface().apply( receiver_account_metadata->code_hash,
                                                      receiver_account_metadata->vm_type,
                                                      receiver_account_metadata->vm_version,
                                                      *this );
               } catch( const wasm_exit& ) {}
            } else {
               // allow inline and notify to non-existing contracts
               // allow onblock and native actions when no sysio system contract by allowing no contract on sysio
               if (receiver != sysio::chain::config::system_account_name && act->account == receiver && get_sender().empty()) {
                  SYS_ASSERT(false, action_validate_exception,
                             "No contract for action {} on account {}", act->name, receiver);
               }
            }
            validate_account_ram_deltas();
         }
      } FC_RETHROW_EXCEPTIONS( warn, "{} <= {}::{} pending console output: {}",
                               receiver, act->account, act->name, _pending_console_output )

      act_digest = generate_action_digest(*act, action_return_value);
   } catch ( const std::bad_alloc& ) {
      throw;
   } catch ( const boost::interprocess::bad_alloc& ) {
      throw;
   } catch( const fc::exception& e ) {
      handle_exception(e);
   } catch ( const std::exception& e ) {
      auto wrapper = fc::std_exception_wrapper::from_current_exception(e);
      handle_exception(wrapper);
   }

   // Note: It should not be possible for receiver_account to be invalidated because:
   //    * a pointer to an object in a chainbase index is not invalidated if other objects in that index are modified, removed, or added;
   //    * a pointer to an object in a chainbase index is not invalidated if the fields of that object are modified;
   //    * and, the *receiver_account object itself cannot be removed because accounts cannot be deleted in SYSIO.

   action_trace& trace = trx_context.get_action_trace( action_ordinal );
   trace.return_value  = std::move(action_return_value);
   trace.receipt.emplace();

   action_receipt& r  = *trace.receipt;
   r.receiver         = receiver;
   r.act_digest       = act_digest;
   r.global_sequence  = next_global_sequence();
   r.recv_sequence    = next_recv_sequence( *receiver_account );

   // Use cached first-receiver metadata for notifications to avoid redundant DB lookups.
   // act->account is invariant across the notification loop in exec().
   const account_metadata_object* first_receiver_account_metadata = nullptr;
   if( act->account == receiver ) {
      first_receiver_account_metadata = receiver_account_metadata;
      _first_receiver_metadata = receiver_account_metadata; // cache for subsequent notifications
   } else {
      if( !_first_receiver_metadata.has_value() )
         _first_receiver_metadata = db.find<account_metadata_object, by_name>(act->account);
      first_receiver_account_metadata = _first_receiver_metadata.value();
   }

   r.code_sequence    = first_receiver_account_metadata != nullptr ? first_receiver_account_metadata->code_sequence : 0; // could be modified by action execution above
   r.abi_sequence     = first_receiver_account_metadata != nullptr ? first_receiver_account_metadata->abi_sequence: 0;  // could be modified by action execution above

   for( const auto& auth : act->authorization ) {
      r.auth_sequence[auth.actor] = next_auth_sequence( auth.actor );
   }

   trx_context.executed_action_receipts.compute_and_append_digests_from(trace);

   finalize_trace( trace, start );

   if( control.contracts_console() ) {
      print_debug(receiver, trace);
   }

   if( auto dm_logger = control.get_deep_mind_logger(trx_context.is_transient()))
   {
      dm_logger->on_end_action();
   }
}

void apply_context::validate_account_ram_deltas() {
   const size_t checktime_interval = 10;
   size_t counter = 0;
   bool not_in_notify_context = (receiver == act->account);
   const auto end = _account_ram_deltas.end();
   for( auto itr = _account_ram_deltas.begin(); itr != end; ++itr, ++counter ) {
      // wlog("pending RAM delta: ${account} ${delta}", ("account", itr->account)("delta", itr->delta) );
      if( counter == checktime_interval ) {
         trx_context.checktime();
         counter = 0;
      }
      if( !privileged && itr->delta > 0 && itr->account != receiver ) {
         SYS_ASSERT( not_in_notify_context, unauthorized_ram_usage_increase,
                     "unprivileged contract cannot increase RAM usage of another account within a notify context: {}",
                     itr->account
         );
         SYS_ASSERT( has_authorization( itr->account ), unauthorized_ram_usage_increase,
                     "unprivileged contract cannot increase RAM usage of another account that has not authorized the action: {}",
                     itr->account
         );
      }
      auto& payer = itr->account;
      auto& ram_delta = itr->delta;
      if (payer != receiver && ram_delta > 0) {
         if (receiver.prefix() == sysio::chain::config::system_account_name && is_privileged()) {
            // sysio.* contracts allowed
         } else if (payer == sysio::chain::config::system_account_name && is_privileged()) {
            // explicit sysio payer allowed when privileged
         } else {
            auto payer_found = false;
            // search act->authorization for a payer permission role
            for( const auto& auth : act->authorization ) {
               if( auth.actor == payer && auth.permission == config::sysio_payer_name ) {
                  payer_found = true;
                  break;
               }
               if ( auth.actor == sysio::chain::config::system_account_name ) {
                  // If the payer is the system account, we don't enforce explicit payer authorization.
                  // This avoids a lot of changes for tests and sysio should not be calling untrusted contracts
                  // or spending user's RAM unexpectedly.
                  payer_found = true;
                  break;
               }
            }
            SYS_ASSERT(payer_found, unsatisfied_authorization,
                       "Requested payer {} did not authorize payment. Missing {}.", payer, config::sysio_payer_name);
         }
      }
   }
}

void apply_context::finalize_trace( action_trace& trace, const fc::time_point& start )
{
   trace.account_ram_deltas = std::move( _account_ram_deltas );
   _account_ram_deltas.clear();

   trace.console = std::move( _pending_console_output );
   _pending_console_output.clear();

   trace.elapsed = std::max(fc::time_point::now() - start, fc::microseconds{1});
}

void apply_context::exec()
{
   _notified.emplace_back( receiver, action_ordinal );
   exec_one();
   for( uint32_t i = 1; i < _notified.size(); ++i ) {
      std::tie( receiver, action_ordinal ) = _notified[i];
      exec_one();
   }

   if( !_cfa_inline_actions.empty() || !_inline_actions.empty() ) {
      SYS_ASSERT( recurse_depth < control.get_global_properties().configuration.max_inline_action_depth,
                  transaction_exception, "max inline action depth per transaction reached" );

      for( uint32_t ordinal : _cfa_inline_actions ) {
         trx_context.execute_action( ordinal, recurse_depth + 1 );
      }

      for( uint32_t ordinal : _inline_actions ) {
         trx_context.execute_action( ordinal, recurse_depth + 1 );
      }
   }

} /// exec()

bool apply_context::is_account( const account_name& account )const {
   return nullptr != db.find<account_object,by_name>( account );
}

void apply_context::get_code_hash(
   account_name account, uint64_t& code_sequence, fc::sha256& code_hash, uint8_t& vm_type, uint8_t& vm_version) const {

   auto obj = db.find<account_metadata_object,by_name>(account);
   if(!obj || obj->code_hash == fc::sha256{}) {
      if(obj)
         code_sequence = obj->code_sequence;
      else
         code_sequence = 0;
      code_hash = {};
      vm_type = 0;
      vm_version = 0;
   } else {
      code_sequence = obj->code_sequence;
      code_hash = obj->code_hash;
      vm_type = obj->vm_type;
      vm_version = obj->vm_version;
   }
}

void apply_context::require_authorization( const account_name& account ) {
   for( uint32_t i=0; i < act->authorization.size(); i++ ) {
     if( act->authorization[i].actor == account ) {
        return;
     }
   }
   SYS_ASSERT( false, missing_auth_exception, "missing authority of {}", account );
}

bool apply_context::has_authorization( const account_name& account )const {
   for( const auto& auth : act->authorization )
     if( auth.actor == account )
        return true;
  return false;
}

void apply_context::require_authorization(const account_name& account,
                                          const permission_name& permission) {
  for( uint32_t i=0; i < act->authorization.size(); i++ )
     if( act->authorization[i].actor == account ) {
        if( act->authorization[i].permission == permission ) {
           return;
        }
     }
  SYS_ASSERT( false, missing_auth_exception, "missing authority of {}/{}",
              account, permission );
}

bool apply_context::has_recipient( account_name code )const {
   for( const auto& p : _notified )
      if( p.first == code )
         return true;
   return false;
}

void apply_context::require_recipient( account_name recipient ) {
   if( !has_recipient(recipient) ) {
      _notified.emplace_back(
         recipient,
         schedule_action( action_ordinal, recipient, false )
      );

      if( auto dm_logger = control.get_deep_mind_logger(trx_context.is_transient()) ) {
         dm_logger->on_require_recipient();
      }
   }
}


/**
 *  This will execute an action after checking the authorization. Inline transactions are
 *  implicitly authorized by the current receiver (running code). This method has significant
 *  security considerations and several options have been considered:
 *
 *  1. privileged accounts (those marked as such by block producers) can authorize any action
 *  2. all other actions are only authorized by 'receiver' which means the following:
 *         a. the user must set permissions on their account to allow the 'receiver' to act on their behalf
 *
 *  Discarded Implementation: at one point we allowed any account that authorized the current transaction
 *   to implicitly authorize an inline transaction. This approach would allow privilege escalation and
 *   make it unsafe for users to interact with certain contracts.  We opted instead to have applications
 *   ask the user for permission to take certain actions rather than making it implicit. This way users
 *   can better understand the security risk.
 */
void apply_context::execute_inline( action&& a ) {
   auto* code = control.db().find<account_object, by_name>(a.account);
   SYS_ASSERT( code != nullptr, action_validate_exception,
               "inline action's code account {} does not exist", a.account );

   bool enforce_actor_whitelist_blacklist = trx_context.enforce_whiteblacklist && control.is_speculative_block();
   flat_set<account_name> actors;

   for (const auto &auth: a.authorization) {
      auto *actor = control.db().find<account_object, by_name>(auth.actor);
      SYS_ASSERT(actor != nullptr, action_validate_exception,
                 "inline action's authorizing actor {} does not exist", auth.actor);
      if (auth.permission != config::sysio_payer_name) {
         SYS_ASSERT(control.get_authorization_manager().find_permission(auth) != nullptr, action_validate_exception,
                    "inline action's authorizations include a non-existent permission: {}", auth);
      }
      if (enforce_actor_whitelist_blacklist)
         actors.insert(auth.actor);
   }

   if( enforce_actor_whitelist_blacklist ) {
      control.check_actor_list( actors );
   }

   // No need to check authorization if replaying irreversible blocks or contract is privileged
   if( !control.skip_auth_check() && !privileged && !trx_context.is_read_only() ) {
      control.get_authorization_manager()
             .check_authorization( {a},
                                   {},
                                   {{receiver, config::sysio_code_name}},
                                   std::bind(&transaction_context::checktime, &this->trx_context),
                                   false,
                                   trx_context.is_dry_run(), // check_but_dont_fail
                                   {}
                                 );
   }

   auto inline_receiver = a.account;
   _inline_actions.emplace_back(
      schedule_action( std::move(a), inline_receiver, false )
   );

   if (auto dm_logger = control.get_deep_mind_logger(trx_context.is_transient())) {
      dm_logger->on_send_inline();
   }
}

void apply_context::execute_context_free_inline( action&& a ) {
   auto* code = control.db().find<account_object, by_name>(a.account);
   SYS_ASSERT( code != nullptr, action_validate_exception,
               "inline action's code account {} does not exist", a.account );

   SYS_ASSERT( a.authorization.size() == 0, action_validate_exception,
               "inline context-free actions cannot have authorizations" );

   auto inline_receiver = a.account;
   _cfa_inline_actions.emplace_back(
      schedule_action( std::move(a), inline_receiver, true )
   );

   if( auto dm_logger = control.get_deep_mind_logger(trx_context.is_transient())) {
      dm_logger->on_send_context_free_inline();
   }
}

uint32_t apply_context::schedule_action( uint32_t ordinal_of_action_to_schedule, account_name receiver, bool context_free )
{
   uint32_t scheduled_action_ordinal = trx_context.schedule_action( ordinal_of_action_to_schedule,
                                                                    receiver, context_free,
                                                                    action_ordinal, first_receiver_action_ordinal );

   act = &trx_context.get_action_trace( action_ordinal ).act;
   return scheduled_action_ordinal;
}

uint32_t apply_context::schedule_action( action&& act_to_schedule, account_name receiver, bool context_free )
{
   uint32_t scheduled_action_ordinal = trx_context.schedule_action( std::move(act_to_schedule),
                                                                    receiver, context_free,
                                                                    action_ordinal, first_receiver_action_ordinal );

   act = &trx_context.get_action_trace( action_ordinal ).act;
   return scheduled_action_ordinal;
}

vector<account_name> apply_context::get_active_producers() const {
   const auto& ap = control.active_producers();
   vector<account_name> accounts; accounts.reserve( ap.producers.size() );

   for(const auto& producer : ap.producers )
      accounts.push_back(producer.producer_name);

   return accounts;
}

void apply_context::update_db_usage( const account_name& payer, int64_t delta ) {
   // wlog("ApplyContext update_db_usage for payer ${payer} of ${delta} bytes,"
   //      " privileged ${privileged}, receiver ${receiver}, action ${action} of account ${act_account}",
   //      ("payer", payer)("delta", delta)("privileged", privileged)("receiver", receiver)("action", act->name)("act_account", act->account));
   add_ram_usage(payer, delta);
}


int apply_context::get_action( uint32_t type, uint32_t index, char* buffer, size_t buffer_size )const
{
   const auto& trx = trx_context.packed_trx.get_transaction();
   const action* act_ptr = nullptr;

   if( type == 0 ) {
      if( index >= trx.context_free_actions.size() )
         return -1;
      act_ptr = &trx.context_free_actions[index];
   }
   else if( type == 1 ) {
      if( index >= trx.actions.size() )
         return -1;
      act_ptr = &trx.actions[index];
   }

   SYS_ASSERT(act_ptr, action_not_found_exception, "action is not found" );

   auto ps = fc::raw::pack_size( *act_ptr );
   if( ps <= buffer_size ) {
      fc::datastream<char*> ds(buffer, buffer_size);
      fc::raw::pack( ds, *act_ptr );
   }
   return ps;
}

int apply_context::get_context_free_data( uint32_t index, char* buffer, size_t buffer_size )const
{
   const auto& trx = trx_context.packed_trx.get_signed_transaction();

   if( index >= trx.context_free_data.size() ) return -1;

   auto s = trx.context_free_data[index].size();
   if( buffer_size == 0 ) return s;

   auto copy_size = std::min( buffer_size, s );
   memcpy( buffer, trx.context_free_data[index].data(), copy_size );

   return copy_size;
}

uint64_t apply_context::next_global_sequence() {
   const auto& p = control.get_dynamic_global_properties();
   if ( trx_context.is_read_only() ) {
      // To avoid confusion of duplicated global sequence number, hard code to be 0.
      return 0;
   } else {
      db.modify( p, [&]( auto& dgp ) {
         ++dgp.global_action_sequence;
      });
      return p.global_action_sequence;
   }
}

uint64_t apply_context::next_recv_sequence( const account_object& receiver_account ) {
   if ( trx_context.is_read_only() ) {
      // To avoid confusion of duplicated receive sequence number, hard code to be 0.
      return 0;
   } else {
      db.modify( receiver_account, [&]( auto& ra ) {
         ++ra.recv_sequence;
      });
      return receiver_account.recv_sequence;
   }
}
uint64_t apply_context::next_auth_sequence( account_name actor ) {
   if ( trx_context.is_read_only() ) {
      // To avoid confusion of duplicated auth sequence number, hard code to be 0.
      // Mirrors next_global_sequence/next_recv_sequence. Currently unreachable:
      // validate_referenced_accounts rejects any authorization on a read-only
      // action before execution. Kept as defense-in-depth — read-only execution
      // has no undo session and runs on parallel read-only-window threads, so if
      // that guard ever changed, the modify below would persist state from a
      // read-only transaction or race other read threads.
      return 0;
   }
   const auto& amo = db.get<account_object,by_name>( actor );
   db.modify( amo, [&](auto& am ){
      ++am.auth_sequence;
   });
   return amo.auth_sequence;
}


void apply_context::add_ram_usage( account_name payer, int64_t ram_delta ) {
   trx_context.add_ram_usage( payer, ram_delta );

   auto p = _account_ram_deltas.emplace( payer, ram_delta );
   if( !p.second ) {
      p.first->delta += ram_delta;
   }
}

action_name apply_context::get_sender() const {
   const action_trace& trace = trx_context.get_action_trace( action_ordinal );
   if (trace.creator_action_ordinal > 0) {
      const action_trace& creator_trace = trx_context.get_action_trace( trace.creator_action_ordinal );
      return creator_trace.receiver;
   }
   return action_name();
}

bool apply_context::is_sys_vm_oc_whitelisted() const {
   return receiver.prefix() == sysio::chain::config::system_account_name || // "sysio"_n
          control.is_sys_vm_oc_whitelisted(receiver);
}

// Context             |    OC?
//-------------------------------------------------------------------------------
// Building block      | baseline, OC for whitelisted
// Applying block      | OC unless a producer, OC for whitelisted including producers
// Speculative API trx | baseline, OC for whitelisted
// Speculative P2P trx | baseline, OC for whitelisted
// Compute trx         | baseline, OC for whitelisted
// Read only trx       | OC
bool apply_context::should_use_sys_vm_oc()const {
   return is_sys_vm_oc_whitelisted() // all whitelisted accounts use OC always
          || (is_applying_block() && !control.is_producer_node()) // validating/applying block
          || trx_context.is_read_only();
}


// ---------------------------------------------------------------------------
// KV Database Implementation
// ---------------------------------------------------------------------------

void apply_context::kv_check_payer(account_name payer) const {
   SYS_ASSERT( payer == receiver || is_account(payer), invalid_table_payer, "KV payer {} is not an account", payer );
}

void apply_context::kv_update_usage(const kv_ram_op& op, const kv_ram_subject& subject, account_name payer,
                                    int64_t delta) {
   if (auto dm_logger = control.get_deep_mind_logger(trx_context.is_transient())) {
      dm_logger->on_kv_ram_trace(op, receiver, subject);
   }
   update_db_usage(payer, delta);
}

void apply_context::kv_update_row_usage(const kv_ram_ops& ops, const kv_ram_subject& subject, account_name old_payer,
                                        int64_t old_billable, account_name payer, int64_t new_billable) {
   if (payer != old_payer) {
      kv_update_usage(ops.update_remove_old_payer, subject, old_payer, -old_billable);
      kv_update_usage(ops.update_add_new_payer, subject, payer, new_billable);
   } else if (new_billable != old_billable) {
      kv_update_usage(ops.update, subject, payer, new_billable - old_billable);
   }
}

// --- Primary KV operations ---

kv_status apply_context::kv_set(uint16_t table_id, uint64_t payer_val, std::string_view key, std::string_view value, kv_set_mode mode) {
   SYS_ASSERT( !trx_context.is_read_only(), table_operation_not_permitted,
               "cannot store a KV record when executing a readonly transaction" );
   const auto& limits = control.get_global_properties().configuration;
   SYS_ASSERT( !key.empty(), kv_empty_key, "KV key must not be empty" );
   SYS_ASSERT( key.size() <= limits.max_kv_key_size, kv_key_too_large,
               "KV key size {} exceeds maximum {}", key.size(), limits.max_kv_key_size );
   SYS_ASSERT( value.size() <= limits.max_kv_value_size, kv_value_too_large,
               "KV value size {} exceeds maximum {}", value.size(), limits.max_kv_value_size );

   auto create_row = [&]() { return kv.insert_row(receiver, table_id, account_name(payer_val), key, value); };
   auto finish_create = [&](const kv_row& row) {
      kv_check_payer(row.payer);
      if (auto dm_logger = control.get_deep_mind_logger(trx_context.is_transient())) {
         dm_logger->on_kv_set(row, true);
      }
      kv_update_usage(kv_row_ram_ops.add, {table_id, key}, row.payer, kv_object_ram(key.size(), value.size()));
   };

   // Insert searches once: the descent that places the row is the one that finds a duplicate.
   if (mode == kv_set_mode::insert && payer_val != 0) {
      const auto row = create_row();
      if (!row) return kv_status::exists;
      finish_create(*row);
      return kv_status::ok;
   }

   const auto existing = kv.find_row(receiver, table_id, key);

   if (!existing) {
      if (mode == kv_set_mode::update) return kv_status::no_row;
      // A new row has no existing payer to keep, so payer 0 (`same_payer`, the CDT name{}) is invalid here. This
      // matches the classic db_store_i64 / generic_index::store `invalid_table_payer` assert.
      SYS_ASSERT( payer_val != 0, invalid_table_payer, "must specify a valid account to pay for new record" );
      const auto row = create_row();
      if (!row) return kv_status::exists;
      finish_create(*row);
      return kv_status::ok;
   }

   if (mode == kv_set_mode::insert) return kv_status::exists;

   // Payer 0 must KEEP the existing payer, not move the row's RAM onto the receiver. Otherwise modify(same_payer, ...)
   // silently re-bills any row whose payer differs from the contract account (e.g. a row billed to the `sysio` pool).
   const account_name old_payer = existing->payer;
   const account_name payer = (payer_val == 0) ? old_payer : account_name(payer_val);
   if (payer != old_payer) kv_check_payer(payer);
   const int64_t old_billable = kv_object_ram(*existing);
   const int64_t new_billable = kv_object_ram(key.size(), value.size());

   // Capture old value for deep_mind before the write
   auto dm_logger = control.get_deep_mind_logger(trx_context.is_transient());
   std::string old_value_copy;
   if (dm_logger) {
      old_value_copy.assign(existing->value);
   }

   const kv_row updated = kv.update_row(*existing, payer, value);

   if (dm_logger) {
      dm_logger->on_kv_set(updated, false, old_payer, old_value_copy.data(), old_value_copy.size());
   }

   kv_update_row_usage(kv_row_ram_ops, {table_id, key}, old_payer, old_billable, payer, new_billable);
   return kv_status::ok;
}

int32_t apply_context::kv_get(uint16_t table_id, name code, std::string_view key, uint32_t offset, char* value, uint32_t value_size) {
   SYS_ASSERT( !key.empty(), kv_empty_key, "KV key must not be empty" );
   const auto row = kv.find_row(code, table_id, key);
   if (!row) return kv_miss;

   kv_copy_field(row->value, offset, value, value_size);
   return static_cast<int32_t>(row->value.size());
}

kv_status apply_context::kv_erase(uint16_t table_id, std::string_view key) {
   SYS_ASSERT( !trx_context.is_read_only(), table_operation_not_permitted,
               "cannot erase a KV record when executing a readonly transaction" );
   SYS_ASSERT( !key.empty(), kv_empty_key, "KV key must not be empty" );

   const auto row = kv.find_row(receiver, table_id, key);
   if (!row) return kv_status::no_row;

   const account_name payer = row->payer;
   const int64_t billable = kv_object_ram(*row);

   if (auto dm_logger = control.get_deep_mind_logger(trx_context.is_transient())) {
      dm_logger->on_kv_erase(*row);
   }

   kv_primary_iterators.forget_row(*row);
   kv.erase_row(*row);
   kv_update_usage(kv_row_ram_ops.remove, {table_id, key}, payer, -billable);
   return kv_status::ok;
}

// --- Primary KV iterators ---

int32_t apply_context::kv_it_create(uint16_t table_id, name code, std::string_view prefix, std::string_view seek) {
   kv_check_prefix_size(prefix.size());
   const uint32_t slot_index = kv_primary_iterators.allocate(table_id, code, prefix);
   auto& slot = kv_primary_iterators.get(slot_index);
   kv_prepare_cursor<kv_primary_family>(kv, slot);
   kv_row row;
   kv_seek<kv_primary_family>(slot, seek, row);
   return static_cast<int32_t>(slot_index);
}

void apply_context::kv_it_destroy(int32_t handle) {
   kv_primary_iterators.release(validate_primary_handle(handle, "kv_it_destroy"));
}

kv_status apply_context::kv_it_next(int32_t handle, kv_row_buffers& row) {
   auto& slot = kv_primary_iterators.get(validate_primary_handle(handle, "kv_it_next"));
   kv_row landed;
   kv_step_next<kv_primary_family>(slot, landed);
   return kv_report_landing<kv_primary_family>(slot, landed, row);
}

kv_status apply_context::kv_it_prev(int32_t handle, kv_row_buffers& row) {
   auto& slot = kv_primary_iterators.get(validate_primary_handle(handle, "kv_it_prev"));
   kv_row landed;
   kv_step_prev<kv_primary_family>(slot, landed);
   return kv_report_landing<kv_primary_family>(slot, landed, row);
}

kv_status apply_context::kv_it_lower_bound(int32_t handle, std::string_view seek, kv_row_buffers& row) {
   auto& slot = kv_primary_iterators.get(validate_primary_handle(handle, "kv_it_lower_bound"));
   kv_row landed;
   kv_seek<kv_primary_family>(slot, seek, landed);
   return kv_report_landing<kv_primary_family>(slot, landed, row);
}

kv_status apply_context::kv_it_read(int32_t handle, uint32_t offset, kv_row_buffers& row) {
   auto& slot = kv_primary_iterators.get(validate_primary_handle(handle, "kv_it_read"));
   return kv_report_position<kv_primary_family>(slot, offset, row);
}

// --- Secondary KV index operations ---

kv_status apply_context::kv_idx_store(uint16_t table_id, uint64_t payer_val, std::string_view pri_key, std::string_view sec_key) {
   SYS_ASSERT( !trx_context.is_read_only(), table_operation_not_permitted,
               "cannot store a KV index when executing a readonly transaction" );
   // Empty keys cannot be written to a snapshot.
   const auto& limits = control.get_global_properties().configuration;
   SYS_ASSERT( !sec_key.empty(), kv_empty_key, "KV secondary key must not be empty" );
   SYS_ASSERT( sec_key.size() <= limits.max_kv_secondary_key_size, kv_secondary_key_too_large,
               "KV secondary key size {} exceeds maximum {}", sec_key.size(), limits.max_kv_secondary_key_size );
   SYS_ASSERT( !pri_key.empty(), kv_empty_key, "KV primary key must not be empty" );
   SYS_ASSERT( pri_key.size() <= limits.max_kv_key_size, kv_key_too_large,
               "KV primary key size {} exceeds maximum {}", pri_key.size(), limits.max_kv_key_size );

   // Payer 0 (`same_payer`) has no existing payer to keep on a new entry, so it fails only if this would create one,
   // matching kv_set and classic generic_index::store.
   if (payer_val == 0) {
      if (kv.find_index_entry(receiver, table_id, sec_key, pri_key)) return kv_status::exists;
      SYS_THROW( invalid_table_payer, "must specify a valid account to pay for new record" );
   }
   const account_name payer(payer_val);

   // A taken position is the only way this insert fails, and finding that out costs no extra search.
   if (!kv.insert_index_entry(receiver, table_id, payer, sec_key, pri_key)) return kv_status::exists;
   kv_check_payer(payer);

   if (auto dm_logger = control.get_deep_mind_logger(trx_context.is_transient())) {
      dm_logger->on_kv_idx_store({receiver, table_id, payer, sec_key, pri_key});
   }
   kv_update_usage(kv_index_ram_ops.add, {table_id, pri_key, sec_key}, payer,
                   kv_index_object_ram(sec_key.size(), pri_key.size()));
   return kv_status::ok;
}

kv_status apply_context::kv_idx_erase(uint16_t table_id, std::string_view pri_key, std::string_view sec_key) {
   SYS_ASSERT( !trx_context.is_read_only(), table_operation_not_permitted,
               "cannot remove a KV index when executing a readonly transaction" );
   SYS_ASSERT( !pri_key.empty(), kv_empty_key, "KV primary key must not be empty" );
   SYS_ASSERT( !sec_key.empty(), kv_empty_key, "KV secondary key must not be empty" );

   const auto entry = kv.find_index_entry(receiver, table_id, sec_key, pri_key);
   if (!entry) return kv_status::no_row;

   const account_name payer = entry->payer;
   const int64_t billable = kv_index_object_ram(*entry);

   if (auto dm_logger = control.get_deep_mind_logger(trx_context.is_transient())) {
      dm_logger->on_kv_idx_erase(*entry);
   }

   kv_secondary_iterators.forget_row(*entry);
   kv.erase_index_entry(*entry);
   kv_update_usage(kv_index_ram_ops.remove, {table_id, pri_key, sec_key}, payer, -billable);
   return kv_status::ok;
}

kv_status apply_context::kv_idx_update(uint16_t table_id, uint64_t payer_val, std::string_view pri_key,
                                       std::string_view old_sec_key, std::string_view new_sec_key) {
   SYS_ASSERT( !trx_context.is_read_only(), table_operation_not_permitted,
               "cannot update a KV index when executing a readonly transaction" );
   // No row has an empty key, and one could not be written to a snapshot.
   const auto& limits = control.get_global_properties().configuration;
   SYS_ASSERT( !pri_key.empty(), kv_empty_key, "KV primary key must not be empty" );
   SYS_ASSERT( !old_sec_key.empty(), kv_empty_key, "KV secondary key must not be empty" );
   SYS_ASSERT( !new_sec_key.empty(), kv_empty_key, "KV secondary key must not be empty" );
   SYS_ASSERT( new_sec_key.size() <= limits.max_kv_secondary_key_size, kv_secondary_key_too_large,
               "KV secondary key size {} exceeds maximum {}", new_sec_key.size(), limits.max_kv_secondary_key_size );

   const auto entry = kv.find_index_entry(receiver, table_id, old_sec_key, pri_key);
   if (!entry) return kv_status::no_row;

   // Unlike an insert, a move that breaks the uniqueness constraint cannot be caught and carried on from: chainbase
   // removes a row that the current undo session already wrote, as it holds no backup to restore. This call keeps
   // running after a collision, so it has to rule one out first.
   if (new_sec_key != old_sec_key && kv.find_index_entry(receiver, table_id, new_sec_key, pri_key)) {
      return kv_status::exists;
   }

   // `same_payer` (payer_val == 0) keeps the existing index entry's payer rather than re-billing it
   // to the receiver, matching the primary-row semantics in kv_set.
   const account_name old_payer = entry->payer;
   const account_name payer = (payer_val == 0) ? old_payer : account_name(payer_val);
   if (payer != old_payer) kv_check_payer(payer);

   const int64_t old_billable = kv_index_object_ram(*entry);
   const int64_t new_billable = kv_index_object_ram(new_sec_key.size(), pri_key.size());

   // The update moves the entry to a new sort position when sec_key changes. Live secondary iterators on it would
   // otherwise advance from the new position and skip entries. Stop them from using their cursors, so they stay
   // positioned by their saved old key bytes. A payer-only update keeps the position, so the cursors stay valid.
   if (new_sec_key != old_sec_key) kv_secondary_iterators.forget_row(*entry);

   kv.update_index_entry(*entry, payer, new_sec_key);

   if (auto dm_logger = control.get_deep_mind_logger(trx_context.is_transient())) {
      dm_logger->on_kv_idx_update({receiver, table_id, payer, new_sec_key, pri_key}, old_payer, old_sec_key);
   }

   kv_update_row_usage(kv_index_ram_ops, {table_id, pri_key, new_sec_key}, old_payer, old_billable, payer,
                       new_billable);
   return kv_status::ok;
}

// --- Secondary KV iterators ---

int32_t apply_context::kv_idx_create(uint16_t table_id, name code, std::string_view prefix, std::string_view seek) {
   kv_check_prefix_size(prefix.size());
   const uint32_t slot_index = kv_secondary_iterators.allocate(table_id, code, prefix);
   auto& slot = kv_secondary_iterators.get(slot_index);
   kv_prepare_cursor<kv_secondary_family>(kv, slot);
   kv_index_entry entry;
   kv_seek<kv_secondary_family>(slot, seek, entry);
   return static_cast<int32_t>(kv_make_secondary_handle(slot_index));
}

int32_t apply_context::kv_idx_find(uint16_t table_id, name code, std::string_view prefix, std::string_view sec_key) {
   kv_check_prefix_size(prefix.size());
   SYS_ASSERT( !sec_key.empty(), kv_empty_key, "KV secondary key must not be empty" );
   SYS_ASSERT( sec_key.starts_with(prefix), kv_key_outside_prefix, "KV secondary key does not start with the prefix" );

   if (!kv_idx_find_cursor) kv_idx_find_cursor = kv.open_index_cursor();
   kv_index_entry entry;
   const bool found = kv_idx_find_cursor->seek_lower({code, table_id, sec_key}, entry);

   if (!found || entry.code != code || entry.table_id != table_id || entry.sec_key != sec_key) {
      return kv_miss; // no iterator slot allocated
   }

   const uint32_t slot_index = kv_secondary_iterators.allocate(table_id, code, prefix);
   auto& slot = kv_secondary_iterators.get(slot_index);
   slot.cursor.swap(kv_idx_find_cursor);
   kv_land<kv_secondary_family>(slot, found, entry);
   return static_cast<int32_t>(kv_make_secondary_handle(slot_index));
}

kv_status apply_context::kv_idx_next(int32_t handle, kv_row_buffers& entry) {
   auto& slot = kv_secondary_iterators.get(validate_secondary_handle(handle, "kv_idx_next"));
   kv_index_entry landed;
   kv_step_next<kv_secondary_family>(slot, landed);
   return kv_report_landing<kv_secondary_family>(slot, landed, entry);
}

kv_status apply_context::kv_idx_prev(int32_t handle, kv_row_buffers& entry) {
   auto& slot = kv_secondary_iterators.get(validate_secondary_handle(handle, "kv_idx_prev"));
   kv_index_entry landed;
   kv_step_prev<kv_secondary_family>(slot, landed);
   return kv_report_landing<kv_secondary_family>(slot, landed, entry);
}

kv_status apply_context::kv_idx_lower_bound(int32_t handle, std::string_view seek, kv_row_buffers& entry) {
   auto& slot = kv_secondary_iterators.get(validate_secondary_handle(handle, "kv_idx_lower_bound"));
   kv_index_entry landed;
   kv_seek<kv_secondary_family>(slot, seek, landed);
   return kv_report_landing<kv_secondary_family>(slot, landed, entry);
}

kv_status apply_context::kv_idx_read(int32_t handle, kv_row_buffers& entry) {
   auto& slot = kv_secondary_iterators.get(validate_secondary_handle(handle, "kv_idx_read"));
   return kv_report_position<kv_secondary_family>(slot, 0, entry);
}

void apply_context::kv_idx_destroy(int32_t handle) {
   kv_secondary_iterators.release(validate_secondary_handle(handle, "kv_idx_destroy"));
}

} /// sysio::chain

#include <algorithm>
#include <sysio/chain/apply_context.hpp>
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

namespace sysio { namespace chain {

static inline void print_debug(account_name receiver, const action_trace& ar) {
   if (!ar.console.empty()) {
      if (fc::logger::get(DEFAULT_LOGGER).is_enabled( fc::log_level::debug )) {
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
         dlog( std::move(output) );
      }
   }
}

apply_context::apply_context(controller& con, transaction_context& trx_ctx, uint32_t action_ordinal, uint32_t depth)
:control(con)
,db(con.mutable_db())
,trx_context(trx_ctx)
,recurse_depth(depth)
,first_receiver_action_ordinal(action_ordinal)
,action_ordinal(action_ordinal)
,idx64(*this)
,idx128(*this)
,idx256(*this)
,idx_double(*this)
,idx_long_double(*this)
{
   action_trace& trace = trx_ctx.get_action_trace(action_ordinal);
   act = &trace.act;
   receiver = trace.receiver;
   context_free = trace.context_free;
}

void apply_context::exec_one()
{
   auto start = fc::time_point::now();

   digest_type act_digest;

   const account_metadata_object* receiver_account = nullptr;

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
         receiver_account = &db.get<account_metadata_object,by_name>( receiver );
         if( !(context_free && control.skip_trx_checks()) ) {
            privileged = receiver_account->is_privileged();
            auto native = control.find_apply_handler( receiver, act->account, act->name );
            if( native ) {
               if( trx_context.enforce_whiteblacklist && control.is_speculative_block() ) {
                  control.check_contract_list( receiver );
                  control.check_action_list( act->account, act->name );
               }
               (*native)( *this );
            }

            if( receiver_account->code_hash != digest_type() ) {
               if( trx_context.enforce_whiteblacklist && control.is_speculative_block() ) {
                  control.check_contract_list( receiver );
                  control.check_action_list( act->account, act->name );
               }
               try {
                  control.get_wasm_interface().apply( receiver_account->code_hash, receiver_account->vm_type, receiver_account->vm_version, *this );
               } catch( const wasm_exit& ) {}
            } else {
               // allow inline and notify to non-existing contracts
               // allow onblock and native actions when no sysio system contract by allowing no contract on sysio
               if (receiver != config::system_account_name && act->account == receiver && get_sender().empty()) {
                  SYS_ASSERT(false, action_validate_exception,
                             "No contract for action ${a} on account ${r}", ("a", act->name)("r", receiver));
               }
            }
            validate_account_ram_deltas();
         }
      } FC_RETHROW_EXCEPTIONS( warn, "${receiver} <= ${account}::${action} pending console output: ${console}", ("console", _pending_console_output)("account", act->account)("action", act->name)("receiver", receiver) )

      act_digest =   generate_action_digest(
                        [this](const char* data, uint32_t datalen) {
                           return trx_context.hash_with_checktime<digest_type>(data, datalen);
                        },
                        *act,
                        action_return_value
                     );
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

   const account_metadata_object* first_receiver_account = nullptr;
   if( act->account == receiver ) {
      first_receiver_account = receiver_account;
   } else {
      first_receiver_account = &db.get<account_metadata_object, by_name>(act->account);
   }

   r.code_sequence    = first_receiver_account->code_sequence; // could be modified by action execution above
   r.abi_sequence     = first_receiver_account->abi_sequence;  // could be modified by action execution above

   for( const auto& auth : act->authorization ) {
      r.auth_sequence[auth.actor] = next_auth_sequence( auth.actor );
   }

   trx_context.executed_action_receipt_digests.emplace_back( r.digest() );

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
                     "unprivileged contract cannot increase RAM usage of another account within a notify context: ${account}",
                     ("account", itr->account)
         );
         SYS_ASSERT( has_authorization( itr->account ), unauthorized_ram_usage_increase,
                     "unprivileged contract cannot increase RAM usage of another account that has not authorized the action: ${account}",
                     ("account", itr->account)
         );
      }
      auto& payer = itr->account;
      auto& ram_delta = itr->delta;
      if (payer != receiver && ram_delta > 0) {
         if (receiver == config::system_account_name) {
            // sysio allowed
         } else if (payer == config::system_account_name && is_privileged()) {
            // explicit sysio payer allowed when privileged
         } else {
            auto payer_found = false;
            // search act->authorization for a payer permission role
            for( const auto& auth : act->authorization ) {
               if( auth.actor == payer && auth.permission == config::sysio_payer_name ) {
                  payer_found = true;
                  break;
               }
               if ( auth.actor == config::system_account_name ) {
                  // If the payer is the system account, we don't enforce explicit payer authorization.
                  // This avoids a lot of changes for tests and sysio should not be calling untrusted contracts
                  // or spending user's RAM unexpectedly.
                  payer_found = true;
                  break;
               }
            }
            SYS_ASSERT(payer_found, unsatisfied_authorization,
                       "Requested payer ${p} did not authorize payment. Missing ${m}.", ("p", payer)("m", config::sysio_payer_name));
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

   trace.elapsed = fc::time_point::now() - start;
}

void apply_context::exec()
{
   _notified.emplace_back( receiver, action_ordinal );
   exec_one();
   for( uint32_t i = 1; i < _notified.size(); ++i ) {
      std::tie( receiver, action_ordinal ) = _notified[i];
      exec_one();
   }

   if( _cfa_inline_actions.size() > 0 || _inline_actions.size() > 0 ) {
      SYS_ASSERT( recurse_depth < control.get_global_properties().configuration.max_inline_action_depth,
                  transaction_exception, "max inline action depth per transaction reached" );
   }

   for( uint32_t ordinal : _cfa_inline_actions ) {
      trx_context.execute_action( ordinal, recurse_depth + 1 );
   }

   for( uint32_t ordinal : _inline_actions ) {
      trx_context.execute_action( ordinal, recurse_depth + 1 );
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
   SYS_ASSERT( false, missing_auth_exception, "missing authority of ${account}", ("account",account));
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
  SYS_ASSERT( false, missing_auth_exception, "missing authority of ${account}/${permission}",
              ("account",account)("permission",permission) );
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
               "inline action's code account ${account} does not exist", ("account", a.account) );

   bool enforce_actor_whitelist_blacklist = trx_context.enforce_whiteblacklist && control.is_speculative_block();
   flat_set<account_name> actors;

   for (const auto &auth: a.authorization) {
      auto *actor = control.db().find<account_object, by_name>(auth.actor);
      SYS_ASSERT(actor != nullptr, action_validate_exception,
                 "inline action's authorizing actor ${account} does not exist", ("account", auth.actor));
      if (auth.permission != config::sysio_payer_name) {
         SYS_ASSERT(control.get_authorization_manager().find_permission(auth) != nullptr, action_validate_exception,
                    "inline action's authorizations include a non-existent permission: ${permission}",
                    ("permission", auth));
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
               "inline action's code account ${account} does not exist", ("account", a.account) );

   SYS_ASSERT( a.authorization.size() == 0, action_validate_exception,
               "context-free actions cannot have authorizations" );

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

const table_id_object* apply_context::find_table( name code, name scope, name table ) {
   return db.find<table_id_object, by_code_scope_table>(boost::make_tuple(code, scope, table));
}

const table_id_object& apply_context::find_or_create_table( name code, name scope, name table, const account_name &payer ) {
   const auto* existing_tid =  db.find<table_id_object, by_code_scope_table>(boost::make_tuple(code, scope, table));
   if (existing_tid != nullptr) {
      return *existing_tid;
   }

   if (auto dm_logger = control.get_deep_mind_logger(trx_context.is_transient())) {
      std::string event_id = RAM_EVENT_ID("${code}:${scope}:${table}",
         ("code", code)
         ("scope", scope)
         ("table", table)
      );
      dm_logger->on_ram_trace(std::move(event_id), "table", "add", "create_table");
   }

   update_db_usage(payer, config::billable_size_v<table_id_object>);

   return db.create<table_id_object>([&](table_id_object &t_id){
      t_id.code = code;
      t_id.scope = scope;
      t_id.table = table;
      t_id.payer = payer;

      if (auto dm_logger = control.get_deep_mind_logger(trx_context.is_transient())) {
         dm_logger->on_create_table(t_id);
      }
   });
}

void apply_context::remove_table( const table_id_object& tid ) {
   if (auto dm_logger = control.get_deep_mind_logger(trx_context.is_transient())) {
      std::string event_id = RAM_EVENT_ID("${code}:${scope}:${table}",
         ("code", tid.code)
         ("scope", tid.scope)
         ("table", tid.table)
      );
      dm_logger->on_ram_trace(std::move(event_id), "table", "remove", "remove_table");
   }

   update_db_usage(tid.payer, - config::billable_size_v<table_id_object>);

   if (auto dm_logger = control.get_deep_mind_logger(trx_context.is_transient())) {
      dm_logger->on_remove_table(tid);
   }

   db.remove(tid);
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

int apply_context::db_store_i64( name scope, name table, const account_name& payer, uint64_t id, const char* buffer, size_t buffer_size ) {
   SYS_ASSERT( !trx_context.is_read_only(), table_operation_not_permitted, "cannot store a db record when executing a readonly transaction" );
   return db_store_i64( receiver, scope, table, payer, id, buffer, buffer_size);
}

int apply_context::db_store_i64( name code, name scope, name table, const account_name& payer, uint64_t id, const char* buffer, size_t buffer_size ) {
//   require_write_lock( scope );
   SYS_ASSERT( !trx_context.is_read_only(), table_operation_not_permitted, "cannot store a db record when executing a readonly transaction" );
   const auto& tab = find_or_create_table( code, scope, table, payer );
   auto tableid = tab.id;

   SYS_ASSERT( payer != account_name(), invalid_table_payer, "must specify a valid account to pay for new record" );

   const auto& obj = db.create<key_value_object>( [&]( auto& o ) {
      o.t_id        = tableid;
      o.primary_key = id;
      o.value.assign( buffer, buffer_size );
      o.payer       = payer;
   });

   db.modify( tab, [&]( auto& t ) {
     ++t.count;
   });

   int64_t billable_size = (int64_t)(buffer_size + config::billable_size_v<key_value_object>);

   if (auto dm_logger = control.get_deep_mind_logger(trx_context.is_transient())) {
      std::string event_id = RAM_EVENT_ID("${table_code}:${scope}:${table_name}:${primkey}",
         ("table_code", tab.code)
         ("scope", tab.scope)
         ("table_name", tab.table)
         ("primkey", name(obj.primary_key))
      );
      dm_logger->on_ram_trace(std::move(event_id), "table_row", "add", "primary_index_add");
   }

   update_db_usage( payer, billable_size);

   if (auto dm_logger = control.get_deep_mind_logger(trx_context.is_transient())) {
      dm_logger->on_db_store_i64(tab, obj);
   }

   keyval_cache.cache_table( tab );
   return keyval_cache.add( obj );
}

void apply_context::db_update_i64( int iterator, account_name payer, const char* buffer, size_t buffer_size ) {
   SYS_ASSERT( !trx_context.is_read_only(), table_operation_not_permitted, "cannot update a db record when executing a readonly transaction" );
   const key_value_object& obj = keyval_cache.get( iterator );

   const auto& table_obj = keyval_cache.get_table( obj.t_id );
   SYS_ASSERT( table_obj.code == receiver, table_access_violation, "db access violation" );

//   require_write_lock( table_obj.scope );

   const int64_t overhead = config::billable_size_v<key_value_object>;
   int64_t old_size = (int64_t)(obj.value.size() + overhead);
   int64_t new_size = (int64_t)(buffer_size + overhead);

   if( payer == account_name() ) payer = obj.payer;

   std::string event_id;
   if (control.get_deep_mind_logger(trx_context.is_transient()) != nullptr) {
      event_id = RAM_EVENT_ID("${table_code}:${scope}:${table_name}:${primkey}",
         ("table_code", table_obj.code)
         ("scope", table_obj.scope)
         ("table_name", table_obj.table)
         ("primkey", name(obj.primary_key))
      );
   }

   if( account_name(obj.payer) != payer ) {
      // refund the existing payer
      if (auto dm_logger = control.get_deep_mind_logger(trx_context.is_transient()))
      {
         dm_logger->on_ram_trace(std::string(event_id), "table_row", "remove", "primary_index_update_remove_old_payer");
      }
      update_db_usage( obj.payer,  -(old_size) );
      // charge the new payer
      if (auto dm_logger = control.get_deep_mind_logger(trx_context.is_transient()))
      {
         dm_logger->on_ram_trace(std::move(event_id), "table_row", "add", "primary_index_update_add_new_payer");
      }
      update_db_usage( payer,  (new_size));
   } else if(old_size != new_size) {
      // charge/refund the existing payer the difference
      if (auto dm_logger = control.get_deep_mind_logger(trx_context.is_transient()))
      {
         dm_logger->on_ram_trace(std::move(event_id) , "table_row", "update", "primary_index_update");
      }
      update_db_usage( obj.payer, new_size - old_size);
   }

   if (auto dm_logger = control.get_deep_mind_logger(trx_context.is_transient())) {
      dm_logger->on_db_update_i64(table_obj, obj, payer, buffer, buffer_size);
   }

   db.modify( obj, [&]( auto& o ) {
     o.value.assign( buffer, buffer_size );
     o.payer = payer;
   });
}

void apply_context::db_remove_i64( int iterator ) {
   SYS_ASSERT( !trx_context.is_read_only(), table_operation_not_permitted, "cannot remove a db record when executing a readonly transaction" );
   const key_value_object& obj = keyval_cache.get( iterator );

   const auto& table_obj = keyval_cache.get_table( obj.t_id );
   SYS_ASSERT( table_obj.code == receiver, table_access_violation, "db access violation" );

//   require_write_lock( table_obj.scope );

   if (auto dm_logger = control.get_deep_mind_logger(trx_context.is_transient())) {
      std::string event_id = RAM_EVENT_ID("${table_code}:${scope}:${table_name}:${primkey}",
         ("table_code", table_obj.code)
         ("scope", table_obj.scope)
         ("table_name", table_obj.table)
         ("primkey", name(obj.primary_key))
      );
      dm_logger->on_ram_trace(std::move(event_id), "table_row", "remove", "primary_index_remove");
   }

   update_db_usage( obj.payer,  -(obj.value.size() + config::billable_size_v<key_value_object>) );

   if (auto dm_logger = control.get_deep_mind_logger(trx_context.is_transient())) {
      dm_logger->on_db_remove_i64(table_obj, obj);
   }

   db.modify( table_obj, [&]( auto& t ) {
      --t.count;
   });
   db.remove( obj );

   if (table_obj.count == 0) {
      remove_table(table_obj);
   }

   keyval_cache.remove( iterator );
}

int apply_context::db_get_i64( int iterator, char* buffer, size_t buffer_size ) {
   const key_value_object& obj = keyval_cache.get( iterator );

   auto s = obj.value.size();
   if( buffer_size == 0 ) return s;

   auto copy_size = std::min( buffer_size, s );
   memcpy( buffer, obj.value.data(), copy_size );

   return copy_size;
}

int apply_context::db_next_i64( int iterator, uint64_t& primary ) {
   if( iterator < -1 ) return -1; // cannot increment past end iterator of table

   const auto& obj = keyval_cache.get( iterator ); // Check for iterator != -1 happens in this call
   const auto& idx = db.get_index<key_value_index, by_scope_primary>();

   auto itr = idx.iterator_to( obj );
   ++itr;

   if( itr == idx.end() || itr->t_id != obj.t_id ) return keyval_cache.get_end_iterator_by_table_id(obj.t_id);

   primary = itr->primary_key;
   return keyval_cache.add( *itr );
}

int apply_context::db_previous_i64( int iterator, uint64_t& primary ) {
   const auto& idx = db.get_index<key_value_index, by_scope_primary>();

   if( iterator < -1 ) // is end iterator
   {
      auto tab = keyval_cache.find_table_by_end_iterator(iterator);
      SYS_ASSERT( tab, invalid_table_iterator, "not a valid end iterator" );

      auto itr = idx.upper_bound(tab->id);
      if( idx.begin() == idx.end() || itr == idx.begin() ) return -1; // Empty table

      --itr;

      if( itr->t_id != tab->id ) return -1; // Empty table

      primary = itr->primary_key;
      return keyval_cache.add(*itr);
   }

   const auto& obj = keyval_cache.get(iterator); // Check for iterator != -1 happens in this call

   auto itr = idx.iterator_to(obj);
   if( itr == idx.begin() ) return -1; // cannot decrement past beginning iterator of table

   --itr;

   if( itr->t_id != obj.t_id ) return -1; // cannot decrement past beginning iterator of table

   primary = itr->primary_key;
   return keyval_cache.add(*itr);
}

int apply_context::db_find_i64( name code, name scope, name table, uint64_t id ) {
   //require_read_lock( code, scope ); // redundant?

   const auto* tab = find_table( code, scope, table );
   if( !tab ) return -1;

   auto table_end_itr = keyval_cache.cache_table( *tab );

   const key_value_object* obj = db.find<key_value_object, by_scope_primary>( boost::make_tuple( tab->id, id ) );
   if( !obj ) return table_end_itr;

   return keyval_cache.add( *obj );
}

int apply_context::db_lowerbound_i64( name code, name scope, name table, uint64_t id ) {
   //require_read_lock( code, scope ); // redundant?

   const auto* tab = find_table( code, scope, table );
   if( !tab ) return -1;

   auto table_end_itr = keyval_cache.cache_table( *tab );

   const auto& idx = db.get_index<key_value_index, by_scope_primary>();
   auto itr = idx.lower_bound( boost::make_tuple( tab->id, id ) );
   if( itr == idx.end() ) return table_end_itr;
   if( itr->t_id != tab->id ) return table_end_itr;

   return keyval_cache.add( *itr );
}

int apply_context::db_upperbound_i64( name code, name scope, name table, uint64_t id ) {
   //require_read_lock( code, scope ); // redundant?

   const auto* tab = find_table( code, scope, table );
   if( !tab ) return -1;

   auto table_end_itr = keyval_cache.cache_table( *tab );

   const auto& idx = db.get_index<key_value_index, by_scope_primary>();
   auto itr = idx.upper_bound( boost::make_tuple( tab->id, id ) );
   if( itr == idx.end() ) return table_end_itr;
   if( itr->t_id != tab->id ) return table_end_itr;

   return keyval_cache.add( *itr );
}

int apply_context::db_end_i64( name code, name scope, name table ) {
   //require_read_lock( code, scope ); // redundant?

   const auto* tab = find_table( code, scope, table );
   if( !tab ) return -1;

   return keyval_cache.cache_table( *tab );
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

uint64_t apply_context::next_recv_sequence( const account_metadata_object& receiver_account ) {
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
   const auto& amo = db.get<account_metadata_object,by_name>( actor );
   db.modify( amo, [&](auto& am ){
      ++am.auth_sequence;
   });
   return amo.auth_sequence;
}

bool is_system_account(const account_name& name) {
   return (name == config::system_account_name) ||
          (name.to_string().size() > 5 && name.to_string().find("sysio.") == 0);
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

// Context             |    OC?
//-------------------------------------------------------------------------------
// Building block      | baseline, OC for sysio.*
// Applying block      | OC unless a producer, OC for sysio.* including producers
// Speculative API trx | baseline, OC for sysio.*
// Speculative P2P trx | baseline, OC for sysio.*
// Compute trx         | baseline, OC for sysio.*
// Read only trx       | OC
bool apply_context::should_use_sys_vm_oc()const {
   return receiver.prefix() == config::system_account_name // "sysio"_n, all cases use OC
          || (is_applying_block() && !control.is_producer_node()) // validating/applying block
          || trx_context.is_read_only();
}


} } /// sysio::chain

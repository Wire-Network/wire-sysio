#include <algorithm>
#include <optional>
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

namespace sysio::chain {

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

static bool key_has_prefix(const kv_object& obj, const std::vector<char>& prefix) {
   if (prefix.empty()) return true;
   if (obj.key.size() < prefix.size()) return false;
   return memcmp(obj.key.data(), prefix.data(), prefix.size()) == 0;
}

static std::string_view to_sv(const char* data, uint32_t size) {
   return std::string_view(data, size);
}

// Compute the lexicographic successor of a byte sequence.
// Returns nullopt if all bytes are 0xFF (no successor).
static std::optional<std::string> byte_successor(std::string_view bytes) {
   for (int64_t i = static_cast<int64_t>(bytes.size()) - 1; i >= 0; --i) {
      if (static_cast<unsigned char>(bytes[i]) < 0xFF) {
         auto result = std::optional<std::string>(std::in_place, bytes.data(), i + 1);
         (*result)[i]++;
         return result;
      }
   }
   return std::nullopt;
}

// --- Primary KV operations ---

int64_t apply_context::kv_set(uint8_t key_format, uint64_t payer_val, const char* key, uint32_t key_size, const char* value, uint32_t value_size) {
   SYS_ASSERT( !trx_context.is_read_only(), table_operation_not_permitted,
               "cannot store a KV record when executing a readonly transaction" );
   SYS_ASSERT( key_format <= 1, kv_key_too_large, "KV key_format must be 0 (raw) or 1 (standard)" );
   SYS_ASSERT( key_size > 0, kv_key_too_large, "KV key must not be empty" );
   SYS_ASSERT( key_size <= control.get_global_properties().configuration.max_kv_key_size, kv_key_too_large,
               "KV key size {} exceeds maximum {}", key_size, control.get_global_properties().configuration.max_kv_key_size );
   SYS_ASSERT( value_size <= control.get_global_properties().configuration.max_kv_value_size, kv_value_too_large,
               "KV value size {} exceeds maximum {}", value_size, control.get_global_properties().configuration.max_kv_value_size );

   // Resolve payer: 0 = receiver (default), non-zero = explicit.
   // Authorization is enforced at the transaction level via unauthorized_ram_usage_increase.
   account_name payer = (payer_val == 0) ? receiver : account_name(payer_val);

   auto sv_key = to_sv(key, key_size);
   const auto& idx = db.get_index<kv_index, by_code_key>();
   auto itr = idx.find(boost::make_tuple(receiver, key_format, sv_key));

   if (itr != idx.end()) {
      // Update existing
      int64_t old_billable = static_cast<int64_t>(itr->key.size() + itr->value.size() + config::billable_size_v<kv_object>);
      int64_t new_billable = static_cast<int64_t>(key_size + value_size + config::billable_size_v<kv_object>);

      // Handle payer change
      account_name old_payer = itr->payer;
      if (payer != old_payer) {
         update_db_usage(old_payer, -old_billable);
         update_db_usage(payer, new_billable);
      } else {
         int64_t delta = new_billable - old_billable;
         if (delta != 0) {
            update_db_usage(payer, delta);
         }
      }

      // Capture old value for deep_mind before modify (old_payer already captured above)
      std::string old_value_copy;
      if (auto dm_logger = control.get_deep_mind_logger(trx_context.is_transient())) {
         old_value_copy.assign(itr->value.data(), itr->value.size());
      }

      db.modify(*itr, [&](auto& o) {
         o.payer = payer;
         o.value.assign(value, value_size);
      });

      if (auto dm_logger = control.get_deep_mind_logger(trx_context.is_transient())) {
         dm_logger->on_kv_set(*itr, false, old_payer, old_value_copy.data(), old_value_copy.size());
      }

      return new_billable - old_billable;
   } else {
      // Create new
      const auto& obj = db.create<kv_object>([&](auto& o) {
         o.code = receiver;
         o.payer = payer;
         o.key_format = key_format;
         o.key.assign(key, key_size);
         o.value.assign(value, value_size);
      });

      if (auto dm_logger = control.get_deep_mind_logger(trx_context.is_transient())) {
         dm_logger->on_kv_set(obj, true);
      }

      int64_t billable = static_cast<int64_t>(key_size + value_size + config::billable_size_v<kv_object>);
      update_db_usage(payer, billable);
      return billable;
   }
}

int32_t apply_context::kv_get(uint8_t key_format, name code, const char* key, uint32_t key_size, char* value, uint32_t value_size) {
   auto sv_key = to_sv(key, key_size);
   const auto& idx = db.get_index<kv_index, by_code_key>();
   auto itr = idx.find(boost::make_tuple(code, key_format, sv_key));

   if (itr == idx.end()) return -1;

   auto s = static_cast<uint32_t>(itr->value.size());
   if (value_size == 0) return static_cast<int32_t>(s);

   auto copy_size = std::min(value_size, s);
   if (copy_size > 0)
      memcpy(value, itr->value.data(), copy_size);
   return static_cast<int32_t>(s);
}

int64_t apply_context::kv_erase(uint8_t key_format, const char* key, uint32_t key_size) {
   SYS_ASSERT( !trx_context.is_read_only(), table_operation_not_permitted,
               "cannot erase a KV record when executing a readonly transaction" );
   SYS_ASSERT( key_size > 0, kv_key_too_large, "KV key must not be empty" );

   auto sv_key = to_sv(key, key_size);
   const auto& idx = db.get_index<kv_index, by_code_key>();
   auto itr = idx.find(boost::make_tuple(receiver, key_format, sv_key));

   SYS_ASSERT( itr != idx.end(), kv_key_not_found, "KV key not found for erase" );

   int64_t delta = -static_cast<int64_t>(itr->key.size() + itr->value.size() + config::billable_size_v<kv_object>);

   if (auto dm_logger = control.get_deep_mind_logger(trx_context.is_transient())) {
      dm_logger->on_kv_erase(*itr);
   }

   update_db_usage(itr->payer, delta);
   db.remove(*itr);
   return delta;
}

int32_t apply_context::kv_contains(uint8_t key_format, name code, const char* key, uint32_t key_size) {
   auto sv_key = to_sv(key, key_size);
   const auto& idx = db.get_index<kv_index, by_code_key>();
   auto itr = idx.find(boost::make_tuple(code, key_format, sv_key));
   return (itr != idx.end()) ? 1 : 0;
}

// --- Primary KV iterators ---

uint32_t apply_context::kv_it_create(uint8_t key_format, name code, const char* prefix, uint32_t prefix_size) {
   SYS_ASSERT( key_format <= 1, kv_key_too_large, "KV key_format must be 0 (raw) or 1 (standard)" );
   uint32_t handle = kv_iterators.allocate_primary(key_format, code, prefix, prefix_size);
   auto& slot = kv_iterators.get(handle);

   // Seek to first entry matching prefix
   const auto& idx = db.get_index<kv_index, by_code_key>();
   auto itr = idx.lower_bound(boost::make_tuple(code, key_format, to_sv(prefix, prefix_size)));

   if (itr != idx.end() && itr->code == code && itr->key_format == key_format && key_has_prefix(*itr, slot.prefix)) {
      slot.status = kv_it_stat::iterator_ok;
      slot.current_key.assign(itr->key.data(), itr->key.data() + itr->key.size());
      slot.cached_id = itr->id._id;
   } else {
      slot.status = kv_it_stat::iterator_end;
   }

   return handle;
}

void apply_context::kv_it_destroy(uint32_t handle) {
   kv_iterators.release(handle);
}

int32_t apply_context::kv_it_status(uint32_t handle) {
   return static_cast<int32_t>(kv_iterators.get(handle).status);
}

int32_t apply_context::kv_it_next(uint32_t handle) {
   auto& slot = kv_iterators.get(handle);
   SYS_ASSERT(slot.is_primary, kv_invalid_iterator, "kv_it_next called on secondary iterator");

   if (slot.status == kv_it_stat::iterator_end) return static_cast<int32_t>(kv_it_stat::iterator_end);

   const auto& idx = db.get_index<kv_index, by_code_key>();
   decltype(idx.end()) itr;

   // Fast path: find current row by cached chainbase ID, then advance via iterator_to
   bool advanced = false;
   if (slot.cached_id >= 0) {
      const auto* obj = db.find<kv_object>(kv_object::id_type(slot.cached_id));
      if (obj && obj->code == slot.code && obj->key_format == slot.key_format) {
         itr = idx.iterator_to(*obj);
         ++itr;
         advanced = true;
      }
   }
   // Slow path: re-seek by key bytes (current row was erased)
   if (!advanced) {
      auto sv_key = to_sv(slot.current_key.data(), slot.current_key.size());
      itr = idx.lower_bound(boost::make_tuple(slot.code, slot.key_format, sv_key));
   }

   if (itr != idx.end() && itr->code == slot.code && itr->key_format == slot.key_format && key_has_prefix(*itr, slot.prefix)) {
      slot.status = kv_it_stat::iterator_ok;
      slot.current_key.assign(itr->key.data(), itr->key.data() + itr->key.size());
      slot.cached_id = itr->id._id;
   } else {
      slot.status = kv_it_stat::iterator_end;
      slot.current_key.clear();
      slot.cached_id = -1;
   }

   return static_cast<int32_t>(slot.status);
}

int32_t apply_context::kv_it_prev(uint32_t handle) {
   auto& slot = kv_iterators.get(handle);
   SYS_ASSERT(slot.is_primary, kv_invalid_iterator, "kv_it_prev called on secondary iterator");

   const auto& idx = db.get_index<kv_index, by_code_key>();

   if (slot.status == kv_it_stat::iterator_end) {
      // Move to last element within prefix range
      decltype(idx.end()) itr;
      auto succ = byte_successor(std::string_view(slot.prefix.data(), slot.prefix.size()));
      if (succ) {
         itr = idx.lower_bound(boost::make_tuple(slot.code, slot.key_format, to_sv(succ->data(), succ->size())));
      } else {
         itr = idx.upper_bound(boost::make_tuple(slot.code, slot.key_format));
      }

      if (itr == idx.begin()) {
         slot.status = kv_it_stat::iterator_end;
         return static_cast<int32_t>(slot.status);
      }
      --itr;

      if (itr->code == slot.code && itr->key_format == slot.key_format && key_has_prefix(*itr, slot.prefix)) {
         slot.status = kv_it_stat::iterator_ok;
         slot.current_key.assign(itr->key.data(), itr->key.data() + itr->key.size());
         slot.cached_id = itr->id._id;
      } else {
         slot.status = kv_it_stat::iterator_end;
         slot.cached_id = -1;
      }
   } else {
      // Fast path: find current row by cached ID, then decrement
      decltype(idx.end()) itr;
      bool found_current = false;
      if (slot.cached_id >= 0) {
         const auto* obj = db.find<kv_object>(kv_object::id_type(slot.cached_id));
         if (obj && obj->code == slot.code && obj->key_format == slot.key_format) {
            itr = idx.iterator_to(*obj);
            found_current = true;
         }
      }
      if (!found_current) {
         auto sv_key = to_sv(slot.current_key.data(), slot.current_key.size());
         itr = idx.lower_bound(boost::make_tuple(slot.code, slot.key_format, sv_key));
      }

      if (itr == idx.begin()) {
         slot.status = kv_it_stat::iterator_end;
         slot.current_key.clear();
         slot.cached_id = -1;
         return static_cast<int32_t>(slot.status);
      }
      --itr;

      if (itr->code == slot.code && itr->key_format == slot.key_format && key_has_prefix(*itr, slot.prefix)) {
         slot.status = kv_it_stat::iterator_ok;
         slot.current_key.assign(itr->key.data(), itr->key.data() + itr->key.size());
         slot.cached_id = itr->id._id;
      } else {
         slot.status = kv_it_stat::iterator_end;
         slot.current_key.clear();
         slot.cached_id = -1;
      }
   }

   return static_cast<int32_t>(slot.status);
}

int32_t apply_context::kv_it_lower_bound(uint32_t handle, const char* key, uint32_t key_size) {
   auto& slot = kv_iterators.get(handle);
   SYS_ASSERT(slot.is_primary, kv_invalid_iterator, "kv_it_lower_bound called on secondary iterator");

   const auto& idx = db.get_index<kv_index, by_code_key>();

   // Seek key must be >= prefix for bounded iteration
   auto sv_key = to_sv(key, key_size);
   auto sv_prefix = to_sv(slot.prefix.data(), slot.prefix.size());
   auto seek_key = (sv_key >= sv_prefix) ? sv_key : sv_prefix;

   auto itr = idx.lower_bound(boost::make_tuple(slot.code, slot.key_format, seek_key));

   if (itr != idx.end() && itr->code == slot.code && itr->key_format == slot.key_format && key_has_prefix(*itr, slot.prefix)) {
      slot.status = kv_it_stat::iterator_ok;
      slot.current_key.assign(itr->key.data(), itr->key.data() + itr->key.size());
      slot.cached_id = itr->id._id;
   } else {
      slot.status = kv_it_stat::iterator_end;
      slot.current_key.clear();
      slot.cached_id = -1;
   }

   return static_cast<int32_t>(slot.status);
}

// Helper: find the current primary row by cached ID (fast) or key bytes (slow).
// Updates cached_id on success. Returns nullptr if the row was erased.
static const kv_object* find_current_primary(const chainbase::database& db, kv_iterator_slot& slot) {
   // Fast path: by cached chainbase ID
   if (slot.cached_id >= 0) {
      const auto* obj = db.find<kv_object>(kv_object::id_type(slot.cached_id));
      if (obj && obj->code == slot.code && obj->key_format == slot.key_format)
         return obj;
   }
   // Slow path: by composite key (row may have been erased and reinserted with new ID)
   const auto& idx = db.get_index<kv_index, by_code_key>();
   auto sv_key = to_sv(slot.current_key.data(), slot.current_key.size());
   auto itr = idx.find(boost::make_tuple(slot.code, slot.key_format, sv_key));
   if (itr != idx.end()) {
      slot.cached_id = itr->id._id;
      return &*itr;
   }
   slot.cached_id = -1;
   return nullptr;
}

int32_t apply_context::kv_it_key(uint32_t handle, uint32_t offset, char* dest, uint32_t dest_size, uint32_t& actual_size) {
   auto& slot = kv_iterators.get(handle);
   SYS_ASSERT(slot.is_primary, kv_invalid_iterator, "kv_it_key called on secondary iterator");

   if (slot.status != kv_it_stat::iterator_ok) {
      actual_size = 0;
      return static_cast<int32_t>(slot.status);
   }

   const kv_object* obj = find_current_primary(db, slot);
   if (!obj) {
      slot.status = kv_it_stat::iterator_erased;
      actual_size = 0;
      return static_cast<int32_t>(slot.status);
   }

   actual_size = static_cast<uint32_t>(obj->key.size());
   if (dest_size > 0 && offset < obj->key.size()) {
      auto copy_size = std::min(static_cast<size_t>(dest_size), obj->key.size() - offset);
      memcpy(dest, obj->key.data() + offset, copy_size);
   }

   return static_cast<int32_t>(kv_it_stat::iterator_ok);
}

int32_t apply_context::kv_it_value(uint32_t handle, uint32_t offset, char* dest, uint32_t dest_size, uint32_t& actual_size) {
   auto& slot = kv_iterators.get(handle);
   SYS_ASSERT(slot.is_primary, kv_invalid_iterator, "kv_it_value called on secondary iterator");

   if (slot.status != kv_it_stat::iterator_ok) {
      actual_size = 0;
      return static_cast<int32_t>(slot.status);
   }

   const kv_object* obj = find_current_primary(db, slot);
   if (!obj) {
      slot.status = kv_it_stat::iterator_erased;
      actual_size = 0;
      return static_cast<int32_t>(slot.status);
   }

   actual_size = static_cast<uint32_t>(obj->value.size());
   if (dest_size > 0 && offset < obj->value.size()) {
      auto copy_size = std::min(static_cast<size_t>(dest_size), obj->value.size() - offset);
      memcpy(dest, obj->value.data() + offset, copy_size);
   }

   return static_cast<int32_t>(kv_it_stat::iterator_ok);
}

// --- Secondary KV index operations ---

void apply_context::kv_idx_store(uint64_t payer_val, name table, uint8_t index_id,
                                 const char* pri_key, uint32_t pri_key_size,
                                 const char* sec_key, uint32_t sec_key_size) {
   SYS_ASSERT( !trx_context.is_read_only(), table_operation_not_permitted,
               "cannot store a KV index when executing a readonly transaction" );
   SYS_ASSERT( sec_key_size <= control.get_global_properties().configuration.max_kv_secondary_key_size, kv_secondary_key_too_large,
               "KV secondary key size {} exceeds maximum {}", sec_key_size, control.get_global_properties().configuration.max_kv_secondary_key_size );
   SYS_ASSERT( pri_key_size <= control.get_global_properties().configuration.max_kv_key_size, kv_key_too_large,
               "KV primary key size {} exceeds maximum {}", pri_key_size, control.get_global_properties().configuration.max_kv_key_size );

   account_name payer = (payer_val == 0) ? receiver : account_name(payer_val);

   db.create<kv_index_object>([&](auto& o) {
      o.code = receiver;
      o.payer = payer;
      o.table = table;
      o.index_id = index_id;
      o.sec_key.assign(sec_key, sec_key_size);
      o.pri_key.assign(pri_key, pri_key_size);
   });

   int64_t billable = static_cast<int64_t>(sec_key_size + pri_key_size + config::billable_size_v<kv_index_object>);
   update_db_usage(payer, billable);
}

void apply_context::kv_idx_remove(name table, uint8_t index_id,
                                  const char* pri_key, uint32_t pri_key_size,
                                  const char* sec_key, uint32_t sec_key_size) {
   SYS_ASSERT( !trx_context.is_read_only(), table_operation_not_permitted,
               "cannot remove a KV index when executing a readonly transaction" );

   auto sv_sec = to_sv(sec_key, sec_key_size);
   auto sv_pri = to_sv(pri_key, pri_key_size);
   const auto& idx = db.get_index<kv_index_index, by_code_table_idx_seckey>();
   auto itr = idx.find(boost::make_tuple(receiver, table, index_id, sv_sec, sv_pri));

   SYS_ASSERT( itr != idx.end(), kv_key_not_found, "KV secondary index entry not found for remove" );

   int64_t delta = -static_cast<int64_t>(itr->sec_key.size() + itr->pri_key.size() +
                                          config::billable_size_v<kv_index_object>);
   update_db_usage(itr->payer, delta);
   db.remove(*itr);
}

void apply_context::kv_idx_update(uint64_t payer_val, name table, uint8_t index_id,
                                  const char* pri_key, uint32_t pri_key_size,
                                  const char* old_sec_key, uint32_t old_sec_key_size,
                                  const char* new_sec_key, uint32_t new_sec_key_size) {
   SYS_ASSERT( !trx_context.is_read_only(), table_operation_not_permitted,
               "cannot update a KV index when executing a readonly transaction" );
   SYS_ASSERT( new_sec_key_size <= control.get_global_properties().configuration.max_kv_secondary_key_size, kv_secondary_key_too_large,
               "KV secondary key size {} exceeds maximum {}", new_sec_key_size, control.get_global_properties().configuration.max_kv_secondary_key_size );

   auto sv_old_sec = to_sv(old_sec_key, old_sec_key_size);
   auto sv_pri = to_sv(pri_key, pri_key_size);
   const auto& idx = db.get_index<kv_index_index, by_code_table_idx_seckey>();
   auto itr = idx.find(boost::make_tuple(receiver, table, index_id, sv_old_sec, sv_pri));

   SYS_ASSERT( itr != idx.end(), kv_key_not_found, "KV secondary index entry not found for update" );

   account_name payer = (payer_val == 0) ? receiver : account_name(payer_val);
   account_name old_payer = itr->payer;

   int64_t old_billable = static_cast<int64_t>(itr->sec_key.size() + itr->pri_key.size() +
                                                config::billable_size_v<kv_index_object>);
   int64_t new_billable = static_cast<int64_t>(new_sec_key_size + pri_key_size +
                                                config::billable_size_v<kv_index_object>);

   if (payer != old_payer) {
      update_db_usage(old_payer, -old_billable);
      update_db_usage(payer, new_billable);
   } else {
      int64_t delta = new_billable - old_billable;
      if (delta != 0) {
         update_db_usage(payer, delta);
      }
   }

   // Remove old and create new (secondary_key is part of index key, can't modify in-place)
   db.remove(*itr);
   db.create<kv_index_object>([&](auto& o) {
      o.code = receiver;
      o.payer = payer;
      o.table = table;
      o.index_id = index_id;
      o.sec_key.assign(new_sec_key, new_sec_key_size);
      o.pri_key.assign(pri_key, pri_key_size);
   });
}

int32_t apply_context::kv_idx_find_secondary(name code, name table, uint8_t index_id,
                                              const char* sec_key, uint32_t sec_key_size) {
   auto sv_sec = to_sv(sec_key, sec_key_size);
   const auto& idx = db.get_index<kv_index_index, by_code_table_idx_seckey>();
   auto itr = idx.lower_bound(boost::make_tuple(code, table, index_id, sv_sec));

   if (itr == idx.end() || itr->code != code || itr->table != table || itr->index_id != index_id ||
       itr->sec_key_view() != to_sv(sec_key, sec_key_size)) {
      return -1; // not found — no iterator slot allocated
   }

   uint32_t handle = kv_iterators.allocate_secondary(code, table, index_id);
   auto& slot = kv_iterators.get(handle);
   slot.status = kv_it_stat::iterator_ok;
   slot.current_sec_key.assign(itr->sec_key.data(), itr->sec_key.data() + itr->sec_key.size());
   slot.current_pri_key.assign(itr->pri_key.data(), itr->pri_key.data() + itr->pri_key.size());
   slot.cached_id = itr->id._id;
   return static_cast<int32_t>(handle);
}

int32_t apply_context::kv_idx_lower_bound(name code, name table, uint8_t index_id,
                                           const char* sec_key, uint32_t sec_key_size) {
   auto sv_sec = to_sv(sec_key, sec_key_size);
   const auto& idx = db.get_index<kv_index_index, by_code_table_idx_seckey>();
   auto itr = idx.lower_bound(boost::make_tuple(code, table, index_id, sv_sec));

   if (itr == idx.end() || itr->code != code || itr->table != table || itr->index_id != index_id) {
      auto first = idx.lower_bound(boost::make_tuple(code, table, index_id));
      if (first != idx.end() && first->code == code && first->table == table && first->index_id == index_id) {
         uint32_t handle = kv_iterators.allocate_secondary(code, table, index_id);
         auto& slot = kv_iterators.get(handle);
         slot.status = kv_it_stat::iterator_end;
         return static_cast<int32_t>(handle);
      }
      return -1;
   }

   uint32_t handle = kv_iterators.allocate_secondary(code, table, index_id);
   auto& slot = kv_iterators.get(handle);
   slot.status = kv_it_stat::iterator_ok;
   slot.current_sec_key.assign(itr->sec_key.data(), itr->sec_key.data() + itr->sec_key.size());
   slot.current_pri_key.assign(itr->pri_key.data(), itr->pri_key.data() + itr->pri_key.size());
   slot.cached_id = itr->id._id;
   return static_cast<int32_t>(handle);
}

int32_t apply_context::kv_idx_next(uint32_t handle) {
   auto& slot = kv_iterators.get(handle);
   SYS_ASSERT(!slot.is_primary, kv_invalid_iterator, "kv_idx_next called on primary iterator");

   if (slot.status == kv_it_stat::iterator_end) return static_cast<int32_t>(kv_it_stat::iterator_end);

   const auto& idx = db.get_index<kv_index_index, by_code_table_idx_seckey>();
   decltype(idx.end()) itr;

   // Fast path: find current entry by cached ID, then advance
   bool advanced = false;
   if (slot.cached_id >= 0) {
      const auto* obj = db.find<kv_index_object>(kv_index_object::id_type(slot.cached_id));
      if (obj && obj->code == slot.code && obj->table == slot.table && obj->index_id == slot.index_id) {
         itr = idx.iterator_to(*obj);
         ++itr;
         advanced = true;
      }
   }
   // Slow path: re-seek by key bytes
   if (!advanced) {
      auto sv_sec = to_sv(slot.current_sec_key.data(), slot.current_sec_key.size());
      auto sv_pri = to_sv(slot.current_pri_key.data(), slot.current_pri_key.size());
      itr = idx.lower_bound(boost::make_tuple(slot.code, slot.table, slot.index_id, sv_sec, sv_pri));
   }

   if (itr != idx.end() && itr->code == slot.code && itr->table == slot.table && itr->index_id == slot.index_id) {
      slot.status = kv_it_stat::iterator_ok;
      slot.current_sec_key.assign(itr->sec_key.data(), itr->sec_key.data() + itr->sec_key.size());
      slot.current_pri_key.assign(itr->pri_key.data(), itr->pri_key.data() + itr->pri_key.size());
      slot.cached_id = itr->id._id;
   } else {
      slot.status = kv_it_stat::iterator_end;
      slot.current_sec_key.clear();
      slot.current_pri_key.clear();
      slot.cached_id = -1;
   }

   return static_cast<int32_t>(slot.status);
}

int32_t apply_context::kv_idx_prev(uint32_t handle) {
   auto& slot = kv_iterators.get(handle);
   SYS_ASSERT(!slot.is_primary, kv_invalid_iterator, "kv_idx_prev called on primary iterator");

   const auto& idx = db.get_index<kv_index_index, by_code_table_idx_seckey>();

   if (slot.status == kv_it_stat::iterator_end) {
      auto itr = idx.upper_bound(boost::make_tuple(slot.code, slot.table, slot.index_id));

      if (itr == idx.begin()) {
         return static_cast<int32_t>(kv_it_stat::iterator_end);
      }
      --itr;

      if (itr->code == slot.code && itr->table == slot.table && itr->index_id == slot.index_id) {
         slot.status = kv_it_stat::iterator_ok;
         slot.current_sec_key.assign(itr->sec_key.data(), itr->sec_key.data() + itr->sec_key.size());
         slot.current_pri_key.assign(itr->pri_key.data(), itr->pri_key.data() + itr->pri_key.size());
         slot.cached_id = itr->id._id;
      } else {
         slot.cached_id = -1;
      }
   } else {
      // Fast path: find current entry by cached ID, then decrement
      decltype(idx.end()) itr;
      bool found_current = false;
      if (slot.cached_id >= 0) {
         const auto* obj = db.find<kv_index_object>(kv_index_object::id_type(slot.cached_id));
         if (obj && obj->code == slot.code && obj->table == slot.table && obj->index_id == slot.index_id) {
            itr = idx.iterator_to(*obj);
            found_current = true;
         }
      }
      if (!found_current) {
         auto sv_sec = to_sv(slot.current_sec_key.data(), slot.current_sec_key.size());
         auto sv_pri = to_sv(slot.current_pri_key.data(), slot.current_pri_key.size());
         itr = idx.lower_bound(boost::make_tuple(slot.code, slot.table, slot.index_id, sv_sec, sv_pri));
      }

      if (itr == idx.begin()) {
         slot.status = kv_it_stat::iterator_end;
         slot.current_sec_key.clear();
         slot.current_pri_key.clear();
         slot.cached_id = -1;
         return static_cast<int32_t>(slot.status);
      }
      --itr;

      if (itr->code == slot.code && itr->table == slot.table && itr->index_id == slot.index_id) {
         slot.status = kv_it_stat::iterator_ok;
         slot.current_sec_key.assign(itr->sec_key.data(), itr->sec_key.data() + itr->sec_key.size());
         slot.current_pri_key.assign(itr->pri_key.data(), itr->pri_key.data() + itr->pri_key.size());
         slot.cached_id = itr->id._id;
      } else {
         slot.status = kv_it_stat::iterator_end;
         slot.current_sec_key.clear();
         slot.current_pri_key.clear();
         slot.cached_id = -1;
      }
   }

   return static_cast<int32_t>(slot.status);
}

// Helper: find the current secondary entry by cached ID (fast) or key bytes (slow).
static const kv_index_object* find_current_secondary(const chainbase::database& db, kv_iterator_slot& slot) {
   if (slot.cached_id >= 0) {
      const auto* obj = db.find<kv_index_object>(kv_index_object::id_type(slot.cached_id));
      if (obj && obj->code == slot.code && obj->table == slot.table && obj->index_id == slot.index_id)
         return obj;
   }
   const auto& idx = db.get_index<kv_index_index, by_code_table_idx_seckey>();
   auto sv_sec = to_sv(slot.current_sec_key.data(), slot.current_sec_key.size());
   auto sv_pri = to_sv(slot.current_pri_key.data(), slot.current_pri_key.size());
   auto itr = idx.find(boost::make_tuple(slot.code, slot.table, slot.index_id, sv_sec, sv_pri));
   if (itr != idx.end()) {
      slot.cached_id = itr->id._id;
      return &*itr;
   }
   slot.cached_id = -1;
   return nullptr;
}

int32_t apply_context::kv_idx_key(uint32_t handle, uint32_t offset, char* dest, uint32_t dest_size, uint32_t& actual_size) {
   auto& slot = kv_iterators.get(handle);
   SYS_ASSERT(!slot.is_primary, kv_invalid_iterator, "kv_idx_key called on primary iterator");

   if (slot.status != kv_it_stat::iterator_ok) {
      actual_size = 0;
      return static_cast<int32_t>(slot.status);
   }

   const kv_index_object* obj = find_current_secondary(db, slot);
   if (!obj) {
      slot.status = kv_it_stat::iterator_erased;
      actual_size = 0;
      return static_cast<int32_t>(slot.status);
   }

   actual_size = static_cast<uint32_t>(obj->sec_key.size());
   if (dest_size > 0 && offset < obj->sec_key.size()) {
      auto copy_size = std::min(static_cast<size_t>(dest_size), obj->sec_key.size() - offset);
      memcpy(dest, obj->sec_key.data() + offset, copy_size);
   }

   return static_cast<int32_t>(kv_it_stat::iterator_ok);
}

int32_t apply_context::kv_idx_primary_key(uint32_t handle, uint32_t offset, char* dest, uint32_t dest_size, uint32_t& actual_size) {
   auto& slot = kv_iterators.get(handle);
   SYS_ASSERT(!slot.is_primary, kv_invalid_iterator, "kv_idx_primary_key called on primary iterator");

   if (slot.status != kv_it_stat::iterator_ok) {
      actual_size = 0;
      return static_cast<int32_t>(slot.status);
   }

   const kv_index_object* obj = find_current_secondary(db, slot);
   if (!obj) {
      slot.status = kv_it_stat::iterator_erased;
      actual_size = 0;
      return static_cast<int32_t>(slot.status);
   }

   actual_size = static_cast<uint32_t>(obj->pri_key.size());
   if (dest_size > 0 && offset < obj->pri_key.size()) {
      auto copy_size = std::min(static_cast<size_t>(dest_size), obj->pri_key.size() - offset);
      memcpy(dest, obj->pri_key.data() + offset, copy_size);
   }

   return static_cast<int32_t>(kv_it_stat::iterator_ok);
}

void apply_context::kv_idx_destroy(uint32_t handle) {
   kv_iterators.release(handle);
}

} /// sysio::chain

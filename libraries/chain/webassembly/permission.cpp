#include <sysio/chain/webassembly/interface.hpp>
#include <sysio/chain/account_object.hpp>
#include <sysio/chain/authorization_manager.hpp>
#include <sysio/chain/transaction_context.hpp>
#include <sysio/chain/apply_context.hpp>
#include <sysio/chain/permission_object.hpp>

namespace sysio { namespace chain { namespace webassembly {
   void unpack_provided_keys( flat_set<public_key_type>& keys, const char* pubkeys_data, uint32_t pubkeys_size ) {
      keys.clear();
      if( pubkeys_size == 0 ) return;

      keys = fc::raw::unpack<flat_set<public_key_type>>( pubkeys_data, pubkeys_size );
   }

   void unpack_provided_permissions( flat_set<permission_level>& permissions, const char* perms_data, uint32_t perms_size ) {
      permissions.clear();
      if( perms_size == 0 ) return;

      permissions = fc::raw::unpack<flat_set<permission_level>>( perms_data, perms_size );
   }

   bool interface::check_transaction_authorization( legacy_span<const char> trx_data,
                                                    legacy_span<const char> pubkeys_data,
                                                    legacy_span<const char> perms_data ) const {
      transaction trx = fc::raw::unpack<transaction>( trx_data.data(), trx_data.size() );

      flat_set<public_key_type> provided_keys;
      unpack_provided_keys( provided_keys, pubkeys_data.data(), pubkeys_data.size() );

      flat_set<permission_level> provided_permissions;
      unpack_provided_permissions( provided_permissions, perms_data.data(), perms_data.size() );

      try {
         context.control
                .get_authorization_manager()
                .check_authorization( trx.actions,
                                      provided_keys,
                                      provided_permissions,
                                      std::bind(&transaction_context::checktime, &context.trx_context),
                                      false
                                    );
         return true;
      } catch( const authorization_exception& e ) {}

      return false;
   }

   bool interface::check_permission_authorization( account_name account, permission_name permission,
                                                   legacy_span<const char> pubkeys_data,
                                                   legacy_span<const char> perms_data,
                                                   uint64_t delay_us ) const {
      SYS_ASSERT( delay_us == 0, action_validate_exception, "delayed transactions not supported" );

      flat_set<public_key_type> provided_keys;
      unpack_provided_keys( provided_keys, pubkeys_data.data(), pubkeys_data.size() );

      flat_set<permission_level> provided_permissions;
      unpack_provided_permissions( provided_permissions, perms_data.data(), perms_data.size() );

      try {
         context.control
                .get_authorization_manager()
                .check_authorization( account,
                                      permission,
                                      provided_keys,
                                      provided_permissions,
                                      std::bind(&transaction_context::checktime, &context.trx_context),
                                      false
                                    );
         return true;
      } catch( const authorization_exception& e ) {}

      return false;
   }

   int64_t interface::get_account_creation_time( account_name account ) const {
      const auto* acct = context.db.find<account_object, by_name>(account);
      SYS_ASSERT( acct != nullptr, action_validate_exception,
                  "account '{}' does not exist", account );
      return time_point(acct->creation_date).time_since_epoch().count();
   }

   int32_t interface::get_permission_lower_bound( account_name account, permission_name permission, span<char> buffer ) {
      const auto& idx = context.db.get_index<permission_index>().indices().get<by_owner>();
      auto itr = idx.lower_bound( boost::make_tuple( account, permission ) );
      if( itr == idx.end() || itr->owner != account )
         return -1;

      // Resolve parent permission name (parent is stored as OID, root has id 0)
      permission_name parent_name;
      if( itr->parent._id != 0 ) {
         const auto* parent_obj = context.db.find<permission_object>( itr->parent );
         if( parent_obj )
            parent_name = parent_obj->name;
      }

      // Serialize: perm_name, parent_name, last_updated, authority
      authority auth = itr->auth.to_authority();
      size_t data_size = fc::raw::pack_size( itr->name ) + fc::raw::pack_size( parent_name )
                       + fc::raw::pack_size( itr->last_updated ) + fc::raw::pack_size( auth );

      std::vector<char> data( data_size );
      fc::datastream<char*> ds( data.data(), data.size() );
      fc::raw::pack( ds, itr->name );
      fc::raw::pack( ds, parent_name );
      fc::raw::pack( ds, itr->last_updated );
      fc::raw::pack( ds, auth );

      auto copy_size = std::min( static_cast<size_t>(buffer.size()), data_size );
      if( copy_size > 0 )
         std::memcpy( buffer.data(), data.data(), copy_size );

      return static_cast<int32_t>( data_size );
   }
}}} // ns sysio::chain::webassembly

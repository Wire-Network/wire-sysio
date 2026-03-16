#include <sysio.system/sysio.system.hpp>

#include <algorithm>
#include <cstring>

namespace sysio { namespace internal_use_do_not_use {
   extern "C" {
      __attribute__((sysio_wasm_import))
      int32_t get_permission_lower_bound(uint64_t account, uint64_t permission, char* buffer, uint32_t buffer_size);
   }
}}

namespace sysiosystem {

void system_contract::expandauth( const name& account, const name& permission,
                                   const std::vector<key_weight>& new_keys,
                                   const std::vector<permission_level_weight>& new_accounts ) {
   require_auth( get_self() );

   check( sysio::is_account(account), "account does not exist" );
   check( !new_keys.empty() || !new_accounts.empty(),
          "must provide at least one key or account to add" );

   // Read current permission via raw intrinsic
   char name_buf[8];
   int32_t sz = sysio::internal_use_do_not_use::get_permission_lower_bound(
      account.value, permission.value, name_buf, sizeof(name_buf) );
   check( sz > 0, "permission does not exist" );

   // Verify exact match (first 8 bytes of serialized data is the permission name)
   sysio::name returned_name;
   std::memcpy( &returned_name, name_buf, sizeof(returned_name) );
   check( returned_name == permission, "permission does not exist" );

   // Read full permission record
   std::vector<char> buf( static_cast<size_t>(sz) );
   sysio::internal_use_do_not_use::get_permission_lower_bound(
      account.value, permission.value, buf.data(), buf.size() );

   // Deserialize permission record fields:
   //   name perm_name, name parent, time_point last_updated, authority auth
   sysio::datastream<const char*> ds( buf.data(), buf.size() );
   sysio::name perm_name, parent;
   sysio::time_point last_updated;
   authority auth;
   ds >> perm_name >> parent >> last_updated >> auth;

   // Add new keys, skipping duplicates
   for( const auto& kw : new_keys ) {
      bool found = false;
      for( const auto& existing : auth.keys ) {
         if( existing.key == kw.key ) {
            found = true;
            break;
         }
      }
      if( !found ) {
         auth.keys.push_back( kw );
      }
   }

   // Add new accounts, skipping duplicates
   for( const auto& aw : new_accounts ) {
      bool found = false;
      for( const auto& existing : auth.accounts ) {
         if( existing.permission == aw.permission ) {
            found = true;
            break;
         }
      }
      if( !found ) {
         auth.accounts.push_back( aw );
      }
   }

   // Sort keys and accounts (required by authority validation)
   std::sort( auth.keys.begin(), auth.keys.end(),
      []( const key_weight& a, const key_weight& b ) {
         return a.key < b.key;
      });
   std::sort( auth.accounts.begin(), auth.accounts.end(),
      []( const permission_level_weight& a, const permission_level_weight& b ) {
         return a.permission < b.permission;
      });

   // Send inline updateauth action
   // Use the account's own permission as authorization; the system contract
   // is privileged so it can provide this auth on inline actions.
   sysio::action(
      { sysio::permission_level{ account, permission } },
      get_self(),
      "updateauth"_n,
      std::make_tuple( account, permission, parent, auth )
   ).send();
}

} // namespace sysiosystem

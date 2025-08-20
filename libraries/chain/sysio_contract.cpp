#include <sysio/chain/sysio_contract.hpp>
#include <sysio/chain/contract_table_objects.hpp>

#include <sysio/chain/controller.hpp>
#include <sysio/chain/transaction_context.hpp>
#include <sysio/chain/apply_context.hpp>
#include <sysio/chain/transaction.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/deep_mind.hpp>

#include <sysio/chain/account_object.hpp>
#include <sysio/chain/code_object.hpp>
#include <sysio/chain/permission_object.hpp>
#include <sysio/chain/permission_link_object.hpp>
#include <sysio/chain/global_property_object.hpp>
#include <sysio/chain/contract_types.hpp>

#include <sysio/chain/wasm_interface.hpp>
#include <sysio/chain/abi_serializer.hpp>

#include <sysio/chain/authorization_manager.hpp>
#include <sysio/chain/resource_limits.hpp>
#include <sysio/chain/sysio_roa_objects.hpp>
#include <fc/io/raw.hpp>

namespace sysio { namespace chain {



uint128_t transaction_id_to_sender_id( const transaction_id_type& tid ) {
   fc::uint128 _id(tid._hash[3], tid._hash[2]);
   return (unsigned __int128)_id;
}

void validate_authority_precondition( const apply_context& context, const authority& auth ) {
   for(const auto& a : auth.accounts) {
      auto* acct = context.db.find<account_object, by_name>(a.permission.actor);
      SYS_ASSERT( acct != nullptr, action_validate_exception,
                  "account '${account}' does not exist",
                  ("account", a.permission.actor)
                );

      if( a.permission.permission == config::owner_name || a.permission.permission == config::active_name )
         continue; // account was already checked to exist, so its owner and active permissions should exist

      if( a.permission.permission == config::sysio_code_name || a.permission.permission == config::sysio_payer_name )
         continue; // virtual sysio.code permission does not really exist but is allowed

      try {
         context.control.get_authorization_manager().get_permission({a.permission.actor, a.permission.permission});
      } catch( const permission_query_exception& ) {
         SYS_THROW( action_validate_exception,
                    "permission '${perm}' does not exist",
                    ("perm", a.permission)
                  );
      }
   }

   if( context.trx_context.enforce_whiteblacklist && context.control.is_speculative_block() ) {
      for( const auto& p : auth.keys ) {
         context.control.check_key_list( p.key );
      }
   }
}

/**
 *  This method is called assuming precondition_system_newaccount succeeds a
 */
void apply_sysio_newaccount(apply_context& context) {
   SYS_ASSERT( !context.trx_context.is_read_only(), action_validate_exception, "newaccount not allowed in read-only transaction" );
   auto create = context.get_action().data_as<newaccount>();
   try {
      context.require_authorization(create.creator);

   //   context.require_write_lock( config::sysio_auth_scope );
   auto& authorization = context.control.get_mutable_authorization_manager();

      SYS_ASSERT(validate(create.owner), action_validate_exception, "Invalid owner authority");
      SYS_ASSERT(validate(create.active), action_validate_exception, "Invalid active authority");

      auto& db = context.db;

      auto name_str = name(create.name).to_string();

      SYS_ASSERT(!create.name.empty(), action_validate_exception, "account name cannot be empty");
      SYS_ASSERT(name_str.size() <= 12, action_validate_exception, "account names can only be 12 chars long");

      // Ensure only privileged accounts can create new accounts
      const auto &creator = db.get<account_metadata_object, by_name>(create.creator);
      SYS_ASSERT(creator.is_privileged(),
                 action_validate_exception,
                 "Only privileged accounts can create new accounts");

      auto existing_account = db.find<account_object, by_name>(create.name);
      SYS_ASSERT(existing_account == nullptr, account_name_exists_exception,
                 "Cannot create account named ${name}, as that name is already taken",
                 ("name", create.name));

      db.create<account_object>([&](auto& a) {
         a.name = create.name;
         a.creation_date = context.control.pending_block_time();
      });

      db.create<account_metadata_object>([&](auto& a) {
         a.name = create.name;
      });

      for (const auto& auth : { create.owner, create.active }) {
         validate_authority_precondition(context, auth);
      }

   const auto& owner_permission  = authorization.create_permission( create.name, config::owner_name, 0,
                                                                    std::move(create.owner), context.trx_context.is_transient() );
   const auto& active_permission = authorization.create_permission( create.name, config::active_name, owner_permission.id,
                                                                    std::move(create.active), context.trx_context.is_transient() );

      context.control.get_mutable_resource_limits_manager().initialize_account(create.name, context.trx_context.is_transient());

      // Determine if this is a system account
      bool is_system_account = (create.name == config::system_account_name) ||
                               (name_str.size() > 5 && name_str.find("sysio.") == 0);

      // If it's not a system account, set CPU, NET, RAM to 0
      if (!is_system_account) {
         // Non-system accounts start with zero resources
         context.control.get_mutable_resource_limits_manager().set_account_limits(create.name, 0, 0, 0, context.trx_context.is_transient());
      }

      int64_t ram_delta = config::overhead_per_account_ram_bytes;
      ram_delta += 2 * config::billable_size_v<permission_object>;
      ram_delta += owner_permission.auth.get_billable_size();
      ram_delta += active_permission.auth.get_billable_size();

   if (auto dm_logger = context.control.get_deep_mind_logger(context.trx_context.is_transient())) {
      dm_logger->on_ram_trace(RAM_EVENT_ID("${name}", ("name", create.name)), "account", "add", "newaccount");
   }

      // Charge the RAM usage to sysio (system payer)
      context.add_ram_usage(config::system_account_name, ram_delta);

} FC_CAPTURE_AND_RETHROW( (create) ) }

void apply_sysio_setcode(apply_context& context) {
   SYS_ASSERT( !context.trx_context.is_read_only(), action_validate_exception, "setcode not allowed in read-only transaction" );
   auto& db = context.db;
   auto  act = context.get_action().data_as<setcode>();
   context.require_authorization(act.account);

   SYS_ASSERT( act.vmtype == 0, invalid_contract_vm_type, "code should be 0" );
   SYS_ASSERT( act.vmversion == 0, invalid_contract_vm_version, "version should be 0" );

   fc::sha256 code_hash; /// default is the all zeros hash

   int64_t code_size = (int64_t)act.code.size();

   if( code_size > 0 ) {
     code_hash = fc::sha256::hash( act.code.data(), (uint32_t)act.code.size() );
     wasm_interface::validate(context.control, act.code);
   }

   const auto& account = db.get<account_metadata_object,by_name>(act.account);
   bool existing_code = (account.code_hash != digest_type());

   SYS_ASSERT( code_size > 0 || existing_code, set_exact_code, "contract is already cleared" );

   int64_t old_size  = 0;
   int64_t new_size  = code_size * config::setcode_ram_bytes_multiplier;

   if( existing_code ) {
      const code_object& old_code_entry = db.get<code_object, by_code_hash>(boost::make_tuple(account.code_hash, account.vm_type, account.vm_version));
      SYS_ASSERT( old_code_entry.code_hash != code_hash, set_exact_code,
                  "contract is already running this version of code" );
      old_size  = (int64_t)old_code_entry.code.size() * config::setcode_ram_bytes_multiplier;
      if( old_code_entry.code_ref_count == 1 ) {
         db.remove(old_code_entry);
         context.control.code_block_num_last_used(account.code_hash, account.vm_type, account.vm_version, context.control.head_block_num() + 1);
      } else {
         db.modify(old_code_entry, [](code_object& o) {
            --o.code_ref_count;
         });
      }
   }

   if( code_size > 0 ) {
      const code_object* new_code_entry = db.find<code_object, by_code_hash>(
                                             boost::make_tuple(code_hash, act.vmtype, act.vmversion) );
      if( new_code_entry ) {
         db.modify(*new_code_entry, [&](code_object& o) {
            ++o.code_ref_count;
         });
      } else {
         db.create<code_object>([&](code_object& o) {
            o.code_hash = code_hash;
            o.code.assign(act.code.data(), code_size);
            o.code_ref_count = 1;
            o.first_block_used = context.control.head_block_num() + 1;
            o.vm_type = act.vmtype;
            o.vm_version = act.vmversion;
         });
      }
   }

   db.modify( account, [&]( auto& a ) {
      a.code_sequence += 1;
      a.code_hash = code_hash;
      a.vm_type = act.vmtype;
      a.vm_version = act.vmversion;
      a.last_code_update = context.control.pending_block_time();
   });

   if (new_size != old_size) {
      if (auto dm_logger = context.control.get_deep_mind_logger(context.trx_context.is_transient())) {
         const char* operation = "update";
         if (old_size <= 0) {
            operation = "add";
         } else if (new_size <= 0) {
            operation = "remove";
         }

         dm_logger->on_ram_trace(RAM_EVENT_ID("${account}", ("account", act.account)), "code", operation, "setcode");
      }

      context.add_ram_usage( act.account, new_size - old_size );
   }
}

void apply_sysio_setabi(apply_context& context) {
   SYS_ASSERT( !context.trx_context.is_read_only(), action_validate_exception, "setabi ot allowed in read-only transaction" );
   auto& db  = context.db;
   auto  act = context.get_action().data_as<setabi>();

   context.require_authorization(act.account);

   const auto& account = db.get<account_object,by_name>(act.account);

   int64_t abi_size = act.abi.size();

   int64_t old_size = (int64_t)account.abi.size();
   int64_t new_size = abi_size;

   db.modify( account, [&]( auto& a ) {
      a.abi.assign(act.abi.data(), abi_size);
   });

   const auto& account_metadata = db.get<account_metadata_object, by_name>(act.account);
   db.modify( account_metadata, [&]( auto& a ) {
      a.abi_sequence += 1;
   });

   if (new_size != old_size) {
      if (auto dm_logger = context.control.get_deep_mind_logger(context.trx_context.is_transient())) {
         const char* operation = "update";
         if (old_size <= 0) {
            operation = "add";
         } else if (new_size <= 0) {
            operation = "remove";
         }

         dm_logger->on_ram_trace(RAM_EVENT_ID("${account}", ("account", act.account)), "abi", operation, "setabi");
      }

      context.add_ram_usage( act.account, new_size - old_size );
   }
}

void apply_sysio_updateauth(apply_context& context) {
   SYS_ASSERT( !context.trx_context.is_read_only(), action_validate_exception, "updateauth not allowed in read-only transaction" );

   auto update = context.get_action().data_as<updateauth>();

   // ** NEW ADDED IF STATEMENT **
   if( update.permission.suffix() != name("ext")) {
      context.require_authorization(update.account); // only here to mark the single authority on this action as used
   }

   auto& authorization = context.control.get_mutable_authorization_manager();
   auto& db = context.db;

   SYS_ASSERT(!update.permission.empty(), action_validate_exception, "Cannot create authority with empty name");
   SYS_ASSERT( update.permission.to_string().find( "sysio." ) != 0, action_validate_exception,
               "Permission names that start with 'sysio.' are reserved" );
   SYS_ASSERT(update.permission != update.parent, action_validate_exception, "Cannot set an authority as its own parent");
   db.get<account_object, by_name>(update.account);
   SYS_ASSERT(validate(update.auth), action_validate_exception,
              "Invalid authority: ${auth}", ("auth", update.auth));
   if( update.permission == config::active_name )
      SYS_ASSERT(update.parent == config::owner_name, action_validate_exception, "Cannot change active authority's parent from owner", ("update.parent", update.parent) );
   if (update.permission == config::owner_name)
      SYS_ASSERT(update.parent.empty(), action_validate_exception, "Cannot change owner authority's parent");
   else
      SYS_ASSERT(!update.parent.empty(), action_validate_exception, "Only owner permission can have empty parent" );

   if( update.auth.waits.size() > 0 ) {
      auto max_delay = context.control.get_global_properties().configuration.max_transaction_delay;
      SYS_ASSERT( update.auth.waits.back().wait_sec <= max_delay, action_validate_exception,
                  "Cannot set delay longer than max_transacton_delay, which is ${max_delay} seconds",
                  ("max_delay", max_delay) );
   }

   validate_authority_precondition(context, update.auth);



   auto permission = authorization.find_permission({update.account, update.permission});

   // If a parent_id of 0 is going to be used to indicate the absence of a parent, then we need to make sure that the chain
   // initializes permission_index with a dummy object that reserves the id of 0.
   authorization_manager::permission_id_type parent_id = 0;
   if( update.permission != config::owner_name ) {
      auto& parent = authorization.get_permission({update.account, update.parent});
      parent_id = parent.id;
   }

   if( permission ) {
      SYS_ASSERT(parent_id == permission->parent, action_validate_exception,
                 "Changing parent authority is not currently supported");


      int64_t old_size = (int64_t)(config::billable_size_v<permission_object> + permission->auth.get_billable_size());

      authorization.modify_permission( *permission, update.auth, context.trx_context.is_transient() );

      int64_t new_size = (int64_t)(config::billable_size_v<permission_object> + permission->auth.get_billable_size());

      if (auto dm_logger = context.control.get_deep_mind_logger(context.trx_context.is_transient())) {
         dm_logger->on_ram_trace(RAM_EVENT_ID("${id}", ("id", permission->id)), "auth", "update", "updateauth_update");
      }

      context.add_ram_usage( permission->owner, new_size - old_size );
   } else {
      const auto& p = authorization.create_permission( update.account, update.permission, parent_id, update.auth, context.trx_context.is_transient() );

      int64_t new_size = (int64_t)(config::billable_size_v<permission_object> + p.auth.get_billable_size());

      if (auto dm_logger = context.control.get_deep_mind_logger(context.trx_context.is_transient())) {
         dm_logger->on_ram_trace(RAM_EVENT_ID("${id}", ("id", p.id)), "auth", "add", "updateauth_create");
      }

      context.add_ram_usage( update.account, new_size );
   }
}

void apply_sysio_deleteauth(apply_context& context) {
//   context.require_write_lock( config::sysio_auth_scope );

   SYS_ASSERT( !context.trx_context.is_read_only(), action_validate_exception, "deleteauth not allowed in read-only transaction" );

   auto remove = context.get_action().data_as<deleteauth>();
   context.require_authorization(remove.account); // only here to mark the single authority on this action as used

   SYS_ASSERT(remove.permission != config::active_name, action_validate_exception, "Cannot delete active authority");
   SYS_ASSERT(remove.permission != config::owner_name, action_validate_exception, "Cannot delete owner authority");

   auto& authorization = context.control.get_mutable_authorization_manager();
   auto& db = context.db;

   { // Check for links to this permission
      const auto& index = db.get_index<permission_link_index, by_permission_name>();
      auto range = index.equal_range(boost::make_tuple(remove.account, remove.permission));
      SYS_ASSERT(range.first == range.second, action_validate_exception,
                 "Cannot delete a linked authority. Unlink the authority first. This authority is linked to ${code}::${type}.",
                 ("code", range.first->code)("type", range.first->message_type));
   }

   const auto& permission = authorization.get_permission({remove.account, remove.permission});
   int64_t old_size = config::billable_size_v<permission_object> + permission.auth.get_billable_size();

   if (auto dm_logger = context.control.get_deep_mind_logger(context.trx_context.is_transient())) {
      dm_logger->on_ram_trace(RAM_EVENT_ID("${id}", ("id", permission.id)), "auth", "remove", "deleteauth");
   }

   authorization.remove_permission( permission, context.trx_context.is_transient() );

   context.add_ram_usage( remove.account, -old_size );

}

void apply_sysio_linkauth(apply_context& context) {
//   context.require_write_lock( config::sysio_auth_scope );

   SYS_ASSERT( !context.trx_context.is_read_only(), action_validate_exception, "linkauth not allowed in read-only transaction" );

   auto requirement = context.get_action().data_as<linkauth>();
   try {
      SYS_ASSERT(!requirement.requirement.empty(), action_validate_exception, "Required permission cannot be empty");

      context.require_authorization(requirement.account); // only here to mark the single authority on this action as used

      auto& db = context.db;
      const auto *account = db.find<account_object, by_name>(requirement.account);
      SYS_ASSERT(account != nullptr, account_query_exception,
                 "Failed to retrieve account: ${account}", ("account", requirement.account)); // Redundant?
      const auto *code = db.find<account_object, by_name>(requirement.code);
      SYS_ASSERT(code != nullptr, account_query_exception,
                 "Failed to retrieve code for account: ${account}", ("account", requirement.code));
      if( requirement.requirement != config::sysio_any_name ) {
         const permission_object* permission = nullptr;
         permission = db.find<permission_object, by_owner>(
                        boost::make_tuple( requirement.account, requirement.requirement )
                      );

         SYS_ASSERT(permission != nullptr, permission_query_exception,
                    "Failed to retrieve permission: ${permission}", ("permission", requirement.requirement));
      }

      auto link_key = boost::make_tuple(requirement.account, requirement.code, requirement.type);
      auto link = db.find<permission_link_object, by_action_name>(link_key);

      if( link ) {
         SYS_ASSERT(link->required_permission != requirement.requirement, action_validate_exception,
                    "Attempting to update required authority, but new requirement is same as old");
         db.modify(*link, [requirement = requirement.requirement](permission_link_object& link) {
             link.required_permission = requirement;
         });
      } else {
         const auto& l =  db.create<permission_link_object>([&requirement](permission_link_object& link) {
            link.account = requirement.account;
            link.code = requirement.code;
            link.message_type = requirement.type;
            link.required_permission = requirement.requirement;
         });

         if (auto dm_logger = context.control.get_deep_mind_logger(context.trx_context.is_transient())) {
            dm_logger->on_ram_trace(RAM_EVENT_ID("${id}", ("id", l.id)), "auth_link", "add", "linkauth");
         }

         context.add_ram_usage(
            l.account,
            (int64_t)(config::billable_size_v<permission_link_object>)
         );
      }

  } FC_CAPTURE_AND_RETHROW((requirement))
}

void apply_sysio_unlinkauth(apply_context& context) {
//   context.require_write_lock( config::sysio_auth_scope );

   SYS_ASSERT( !context.trx_context.is_read_only(), action_validate_exception, "unlinkauth not allowed in read-only transaction" );

   auto& db = context.db;
   auto unlink = context.get_action().data_as<unlinkauth>();

   context.require_authorization(unlink.account); // only here to mark the single authority on this action as used

   auto link_key = boost::make_tuple(unlink.account, unlink.code, unlink.type);
   auto link = db.find<permission_link_object, by_action_name>(link_key);
   SYS_ASSERT(link != nullptr, action_validate_exception, "Attempting to unlink authority, but no link found");

   if (auto dm_logger = context.control.get_deep_mind_logger(context.trx_context.is_transient())) {
      dm_logger->on_ram_trace(RAM_EVENT_ID("${id}", ("id", link->id)), "auth_link", "remove", "unlinkauth");
   }

   context.add_ram_usage(
      link->account,
      -(int64_t)(config::billable_size_v<permission_link_object>)
   );

   db.remove(*link);
}

void apply_sysio_canceldelay(apply_context& context) {
   SYS_ASSERT( !context.trx_context.is_read_only(), action_validate_exception, "canceldelay not allowed in read-only transaction" );
   auto cancel = context.get_action().data_as<canceldelay>();
   context.require_authorization(cancel.canceling_auth.actor); // only here to mark the single authority on this action as used

   const auto& trx_id = cancel.trx_id;

   context.cancel_deferred_transaction(transaction_id_to_sender_id(trx_id), account_name());
}

void apply_roa_reducepolicy(apply_context& context) {
   // 1. Parse action arguments
   auto args = context.get_action().data_as<reducepolicy>();
   const sysio::chain::name& owner       = args.owner;
   const sysio::chain::name& issuer      = args.issuer;
   const sysio::chain::asset& net_weight = args.net_weight;
   const sysio::chain::asset& cpu_weight = args.cpu_weight;
   const sysio::chain::asset& ram_weight = args.ram_weight;
   name network_gen(static_cast<uint64_t>(args.network_gen));

   // 2. Authorization & info
   context.require_authorization(issuer);

   // 3. Locate the "nodeowners" row
   {
      int itr = context.db_find_i64(context.get_receiver(), network_gen, name("nodeowners"), issuer.to_uint64_t());
      SYS_ASSERT(itr != -1, action_validate_exception, "Issuer is not a registered Node Owner");

      // Read and deserialize
      int size = context.db_get_i64(itr, nullptr, 0);
      SYS_ASSERT(size >= 0, action_validate_exception, "Error reading nodeowners row");

      std::vector<char> buffer(size);
      context.db_get_i64(itr, buffer.data(), size);

      fc::datastream<const char*> ds(buffer.data(), buffer.size());
      sysio::roa::nodeowners node_row;
      fc::raw::unpack(ds, node_row);

      // Ensure the row's owner matches issuer
      SYS_ASSERT(node_row.owner == issuer, action_validate_exception, "Mismatched nodeowner row: stored owner != issuer");

      // 4. Locate "policies" row
      {
         int pol_itr = context.db_find_i64(context.get_receiver(), issuer, name("policies"), owner.to_uint64_t());
         SYS_ASSERT(pol_itr != -1, action_validate_exception,
                    "Policy does not exist under this issuer for the owner");

         int pol_size = context.db_get_i64(pol_itr, nullptr, 0);
         SYS_ASSERT(pol_size >= 0, action_validate_exception, "Error reading policies row");

         std::vector<char> pol_buffer(pol_size);
         context.db_get_i64(pol_itr, pol_buffer.data(), pol_size);

         fc::datastream<const char*> pds(pol_buffer.data(), pol_buffer.size());
         sysio::roa::policies pol_row;
         fc::raw::unpack(pds, pol_row);

         // 5. Validate time block
         uint32_t current_block = context.control.head_block_num();
         SYS_ASSERT(current_block >= pol_row.time_block, action_validate_exception,
                    "Cannot reduce policy before time_block");

         // Special sysio check
         {
            std::string acc_str = owner.to_string();
            bool sysio_acct = (acc_str == "sysio") ||
                              (acc_str.size() > 5 && acc_str.rfind("sysio.", 0) == 0);
            if (sysio_acct && pol_row.time_block == 1 && pol_row.issuer == issuer) {
               SYS_ASSERT(false, action_validate_exception,
                          "Cannot reduce the sysio policies created at node registration");
            }
         }

         // 6. Locate "reslimit" row
         {
            int rl_itr = context.db_find_i64(context.get_receiver(), owner, name("reslimit"), owner.to_uint64_t());
            SYS_ASSERT(rl_itr != -1, action_validate_exception,
                       "reslimit row does not exist for this owner");

            int rl_size = context.db_get_i64(rl_itr, nullptr, 0);
            SYS_ASSERT(rl_size >= 0, action_validate_exception, "Error reading reslimit row");

            std::vector<char> rl_buffer(rl_size);
            context.db_get_i64(rl_itr, rl_buffer.data(), rl_size);

            fc::datastream<const char*> rlds(rl_buffer.data(), rl_buffer.size());
            sysio::roa::reslimit rl_row;
            fc::raw::unpack(rlds, rl_row);

            // 7. Validate the new weights
            SYS_ASSERT(net_weight.get_amount() >= 0, action_validate_exception, "NET weight cannot be negative");
            SYS_ASSERT(cpu_weight.get_amount() >= 0, action_validate_exception, "CPU weight cannot be negative");
            SYS_ASSERT(ram_weight.get_amount() >= 0, action_validate_exception, "RAM weight cannot be negative");
            SYS_ASSERT(net_weight.get_amount()  > 0 ||
                       cpu_weight.get_amount()  > 0 ||
                       ram_weight.get_amount() > 0,
                       action_validate_exception, "All weights are 0, nothing to reduce.");

            // Ensure we don't reduce below zero
            SYS_ASSERT(net_weight.get_amount() <= pol_row.net_weight.get_amount(), action_validate_exception,
                       "Cannot reduce NET below zero");
            SYS_ASSERT(cpu_weight.get_amount() <= pol_row.cpu_weight.get_amount(), action_validate_exception,
                       "Cannot reduce CPU below zero");
            SYS_ASSERT(ram_weight.get_amount() <= pol_row.ram_weight.get_amount(), action_validate_exception,
                       "Cannot reduce RAM below zero");

            // 8. Adjust resource limits
            auto& resource_manager = context.control.get_mutable_resource_limits_manager();

            int64_t ram_bytes, net_limit, cpu_limit;
            resource_manager.get_account_limits(owner, ram_bytes, net_limit, cpu_limit);

            uint64_t bytes_per_unit = pol_row.bytes_per_unit;
            int64_t divisible_ram_to_reclaim = 0;
            sysio::chain::asset reclaimed_ram_weight(0, ram_weight.get_symbol());

            if (ram_weight.get_amount() > 0) {
               int64_t ram_usage = resource_manager.get_account_ram_usage(owner);
               int64_t ram_unused = ram_bytes - ram_usage;
               int64_t requested_ram_bytes = ram_weight.get_amount() * (int64_t)bytes_per_unit;
               int64_t ram_to_reclaim = std::min(ram_unused, requested_ram_bytes);

               SYS_ASSERT(ram_to_reclaim >= 0, action_validate_exception, "Invalid RAM reclaim calculation");

               int64_t remainder = ram_to_reclaim % (int64_t)bytes_per_unit;
               divisible_ram_to_reclaim = ram_to_reclaim - remainder;

               int64_t reclaimed_units = divisible_ram_to_reclaim / (int64_t)bytes_per_unit;
               reclaimed_ram_weight = sysio::chain::asset(reclaimed_units, ram_weight.get_symbol());

               SYS_ASSERT(reclaimed_ram_weight.get_amount() <= ram_weight.get_amount(),
                          action_validate_exception, "Cannot reclaim more RAM than requested");
            }

            resource_manager.set_account_limits(
               owner,
               ram_bytes - divisible_ram_to_reclaim,
               net_limit - net_weight.get_amount(),
               cpu_limit - cpu_weight.get_amount(),
               context.trx_context.is_transient()
            );

            // 9. Update reslimit row
            {
               rl_row.net_weight -= net_weight;
               rl_row.cpu_weight -= cpu_weight;
               rl_row.ram_bytes  = static_cast<uint64_t>(rl_row.ram_bytes - divisible_ram_to_reclaim);

               fc::datastream<std::vector<char>> ds;
               fc::raw::pack(ds, rl_row);
               auto out = ds.storage();

               // TODO: If we want to remove the reslimit row if all values are 0 do it here.
               context.db_update_i64(rl_itr, context.get_receiver(), out.data(), out.size());
            }

            // 10. Update / remove the policies row
            {
               pol_row.net_weight -= net_weight;
               pol_row.cpu_weight -= cpu_weight;
               pol_row.ram_weight -= reclaimed_ram_weight;

               fc::datastream<std::vector<char>> ds;
               fc::raw::pack(ds, pol_row);
               auto pol_out = ds.storage();

               bool all_zero = (pol_row.net_weight.get_amount() == 0 &&
                                pol_row.cpu_weight.get_amount() == 0 &&
                                pol_row.ram_weight.get_amount() == 0);
               if (all_zero) {
                  context.db_remove_i64(pol_itr);
               } else {
                  context.db_update_i64(pol_itr, context.get_receiver(), pol_out.data(), pol_out.size());
               }
            }

            // 11. Update nodeowners row
            {
               sysio::chain::asset total_reclaimed_sys = net_weight + cpu_weight + reclaimed_ram_weight;
               node_row.allocated_sys -= total_reclaimed_sys;
               node_row.allocated_bw  -= (net_weight + cpu_weight);
               node_row.allocated_ram -= reclaimed_ram_weight;

               fc::datastream<std::vector<char>> ds;
               fc::raw::pack(ds, node_row);
               auto node_out = ds.storage();

               context.db_update_i64(itr, context.get_receiver(), node_out.data(), node_out.size());
            }

         } // end reslimit block
      } // end policies block
   }
}

} } // namespace sysio::chain

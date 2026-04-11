#include <sysio/action.hpp>
#include <sysio/crypto.hpp>
#include <sysio/permission.hpp>

#include "sysio.msig.hpp"

namespace sysio {

transaction_header get_trx_header(const char* ptr, size_t sz);
bool trx_is_authorized(const std::vector<permission_level>& approvals, const std::vector<char>& packed_trx);

template<typename Function>
std::vector<permission_level> get_approvals_and_adjust_table(name self, name proposer, name proposal_name, Function&& table_op) {
   multisig::approvals approval_table( self, proposer.value );
   auto ak = multisig::approval_key{proposal_name.value};
   std::vector<permission_level> approvals_vector;
   multisig::invalidations invalidations_table( self );

   if ( approval_table.contains(ak) ) {
      auto app = approval_table.get(ak);
      approvals_vector.reserve( app.provided_approvals.size() );
      for ( const auto& permission : app.provided_approvals ) {
         auto ik = multisig::inval_key{permission.level.actor.value};
         if ( !invalidations_table.contains(ik) ) {
            approvals_vector.push_back(permission.level);
         } else {
            auto inv = invalidations_table.get(ik);
            if ( inv.last_invalidation_time < permission.time ) {
               approvals_vector.push_back(permission.level);
            }
         }
      }
      table_op( approval_table, ak );
   } else {
      multisig::old_approvals old_approval_table( self, proposer.value );
      auto oak = multisig::old_approval_key{proposal_name.value};
      const auto old_approvals_obj = old_approval_table.get( oak, "proposal not found" );
      for ( const auto& permission : old_approvals_obj.provided_approvals ) {
         auto ik = multisig::inval_key{permission.actor.value};
         if ( !invalidations_table.contains(ik) ) {
            approvals_vector.push_back( permission );
         }
      }
      table_op( old_approval_table, oak );
   }
   return approvals_vector;
}

void multisig::propose( name proposer,
                        name proposal_name,
                        std::vector<permission_level> requested,
                        ignore<transaction> trx )
{
   require_auth( proposer );
   auto& ds = get_datastream();

   const char* trx_pos = ds.pos();
   size_t size = ds.remaining();

   transaction_header trx_header;
   std::vector<action> context_free_actions;
   ds >> trx_header;
   check( trx_header.expiration >= sysio::time_point_sec(current_time_point()), "transaction expired" );
   ds >> context_free_actions;
   check( context_free_actions.empty(), "not allowed to `propose` a transaction with context-free actions" );

   proposals proptable( get_self(), proposer.value );
   auto pk = proposal_key{proposal_name.value};
   check( !proptable.contains(pk), "proposal with the same name exists" );

   auto packed_requested = pack(requested);
   auto res =  check_transaction_authorization(
         trx_pos, size,
         (const char*)0, 0,
         packed_requested.data(), packed_requested.size()
   );

   check( res > 0, "transaction authorization failed" );

   std::vector<char> pkd_trans;
   pkd_trans.resize(size);
   memcpy((char*)pkd_trans.data(), trx_pos, size);

   proptable.emplace( proposer, pk, proposal{
      .proposal_name      = proposal_name,
      .packed_transaction = pkd_trans,
      .earliest_exec_time = binary_extension< std::optional<time_point> >{},
   });

   approvals apptable( get_self(), proposer.value );
   std::vector<multisig::approval> req_approvals;
   req_approvals.reserve( requested.size() );
   for ( auto& level : requested ) {
      req_approvals.push_back( multisig::approval{ level, time_point{ microseconds{0} } } );
   }
   apptable.emplace( proposer, approval_key{proposal_name.value}, approvals_info{
      .version = 1,
      .proposal_name = proposal_name,
      .requested_approvals = std::move(req_approvals),
      .provided_approvals = {},
   });
}

void multisig::approve( name proposer, name proposal_name, permission_level level,
                        const sysio::binary_extension<sysio::checksum256>& proposal_hash )
{
   require_auth( level );

   proposals proptable( get_self(), proposer.value );
   auto pk = proposal_key{proposal_name.value};
   const auto prop = proptable.get( pk, "proposal not found" );

   if( proposal_hash ) {
      assert_sha256( prop.packed_transaction.data(), prop.packed_transaction.size(), *proposal_hash );
   }

   approvals apptable( get_self(), proposer.value );
   auto ak = approval_key{proposal_name.value};
   if ( apptable.contains(ak) ) {
      auto apps = apptable.get(ak);
      auto itr = std::find_if( apps.requested_approvals.begin(), apps.requested_approvals.end(), [&](const multisig::approval& a) { return a.level == level; } );
      check( itr != apps.requested_approvals.end(), "approval is not on the list of requested approvals" );

      apptable.modify( proposer, ak, [&]( auto& a ) {
         a.provided_approvals.push_back( multisig::approval{ level, current_time_point() } );
         a.requested_approvals.erase( std::find_if( a.requested_approvals.begin(), a.requested_approvals.end(), [&](const multisig::approval& ap) { return ap.level == level; } ) );
      });
   } else {
      old_approvals old_apptable( get_self(), proposer.value );
      auto oak = old_approval_key{proposal_name.value};
      auto apps = old_apptable.get( oak, "proposal not found" );

      auto itr = std::find( apps.requested_approvals.begin(), apps.requested_approvals.end(), level );
      check( itr != apps.requested_approvals.end(), "approval is not on the list of requested approvals" );

      old_apptable.modify( proposer, oak, [&]( auto& a ) {
         a.provided_approvals.push_back( level );
         a.requested_approvals.erase( std::find( a.requested_approvals.begin(), a.requested_approvals.end(), level ) );
      });
   }

   transaction_header trx_header = get_trx_header(prop.packed_transaction.data(), prop.packed_transaction.size());

   if( prop.earliest_exec_time.has_value() ) {
      if( !prop.earliest_exec_time->has_value() ) {
         auto table_op = [](auto&&, auto&&){};
         if( trx_is_authorized(get_approvals_and_adjust_table(get_self(), proposer, proposal_name, table_op), prop.packed_transaction) ) {
            proptable.modify( proposer, pk, [&]( auto& p ) {
               p.earliest_exec_time.emplace(time_point{ current_time_point() + sysio::seconds(trx_header.delay_sec.value)});
            });
         }
      }
   } else {
      check( trx_header.delay_sec.value == 0, "old proposals are not allowed to have non-zero `delay_sec`; cancel and retry" );
   }
}

void multisig::unapprove( name proposer, name proposal_name, permission_level level ) {
   require_auth( level );

   approvals apptable( get_self(), proposer.value );
   auto ak = approval_key{proposal_name.value};
   if ( apptable.contains(ak) ) {
      auto apps = apptable.get(ak);
      auto itr = std::find_if( apps.provided_approvals.begin(), apps.provided_approvals.end(), [&](const multisig::approval& a) { return a.level == level; } );
      check( itr != apps.provided_approvals.end(), "no approval previously granted" );
      apptable.modify( proposer, ak, [&]( auto& a ) {
         a.requested_approvals.push_back( multisig::approval{ level, current_time_point() } );
         a.provided_approvals.erase( std::find_if( a.provided_approvals.begin(), a.provided_approvals.end(), [&](const multisig::approval& ap) { return ap.level == level; } ) );
      });
   } else {
      old_approvals old_apptable( get_self(), proposer.value );
      auto oak = old_approval_key{proposal_name.value};
      auto apps = old_apptable.get( oak, "proposal not found" );
      auto itr = std::find( apps.provided_approvals.begin(), apps.provided_approvals.end(), level );
      check( itr != apps.provided_approvals.end(), "no approval previously granted" );
      old_apptable.modify( proposer, oak, [&]( auto& a ) {
         a.requested_approvals.push_back( level );
         a.provided_approvals.erase( std::find( a.provided_approvals.begin(), a.provided_approvals.end(), level ) );
      });
   }

   proposals proptable( get_self(), proposer.value );
   auto pk = proposal_key{proposal_name.value};
   const auto prop = proptable.get( pk, "proposal not found" );

   if( prop.earliest_exec_time.has_value() ) {
      if( prop.earliest_exec_time->has_value() ) {
         auto table_op = [](auto&&, auto&&){};
         if( !trx_is_authorized(get_approvals_and_adjust_table(get_self(), proposer, proposal_name, table_op), prop.packed_transaction) ) {
            proptable.modify( proposer, pk, [&]( auto& p ) {
               p.earliest_exec_time.emplace();
            });
         }
      }
   } else {
      transaction_header trx_header = get_trx_header(prop.packed_transaction.data(), prop.packed_transaction.size());
      check( trx_header.delay_sec.value == 0, "old proposals are not allowed to have non-zero `delay_sec`; cancel and retry" );
   }
}

void multisig::cancel( name proposer, name proposal_name, name canceler ) {
   require_auth( canceler );

   proposals proptable( get_self(), proposer.value );
   auto pk = proposal_key{proposal_name.value};
   const auto prop = proptable.get( pk, "proposal not found" );

   if( canceler != proposer ) {
      check( unpack<transaction_header>( prop.packed_transaction ).expiration < sysio::time_point_sec(current_time_point()), "cannot cancel until expiration" );
   }
   proptable.erase(pk);

   //remove from new table
   approvals apptable( get_self(), proposer.value );
   auto ak = approval_key{proposal_name.value};
   if ( apptable.contains(ak) ) {
      apptable.erase(ak);
   } else {
      old_approvals old_apptable( get_self(), proposer.value );
      auto oak = old_approval_key{proposal_name.value};
      check( old_apptable.contains(oak), "proposal not found" );
      old_apptable.erase(oak);
   }
}

void multisig::exec( name proposer, name proposal_name, name executer ) {
   require_auth( executer );

   proposals proptable( get_self(), proposer.value );
   auto pk = proposal_key{proposal_name.value};
   const auto prop = proptable.get( pk, "proposal not found" );
   transaction_header trx_header;
   std::vector<action> context_free_actions;
   std::vector<action> actions;
   datastream<const char*> ds( prop.packed_transaction.data(), prop.packed_transaction.size() );
   ds >> trx_header;
   check( trx_header.expiration >= sysio::time_point_sec(current_time_point()), "transaction expired" );
   ds >> context_free_actions;
   check( context_free_actions.empty(), "not allowed to `exec` a transaction with context-free actions" );
   ds >> actions;

   auto table_op = [](auto&& table, auto&& key) { table.erase(key); };
   bool ok = trx_is_authorized(get_approvals_and_adjust_table(get_self(), proposer, proposal_name, table_op), prop.packed_transaction);
   check( ok, "transaction authorization failed" );

   if ( prop.earliest_exec_time.has_value() && prop.earliest_exec_time->has_value() ) {
      check( **prop.earliest_exec_time <= current_time_point(), "too early to execute" );
   } else {
      check( trx_header.delay_sec.value == 0, "old proposals are not allowed to have non-zero `delay_sec`; cancel and retry" );
   }

   for (const auto& act : actions) {
      act.send();
   }

   proptable.erase(pk);
}

void multisig::invalidate( name account ) {
   require_auth( account );
   invalidations inv_table( get_self() );
   auto ik = inval_key{account.value};
   inv_table.upsert( account, ik,
      invalidation{ .account = account, .last_invalidation_time = current_time_point() },
      [&](auto& i) { i.last_invalidation_time = current_time_point(); } );
}

transaction_header get_trx_header(const char* ptr, size_t sz) {
   datastream<const char*> ds = {ptr, sz};
   transaction_header trx_header;
   ds >> trx_header;
   return trx_header;
}

bool trx_is_authorized(const std::vector<permission_level>& approvals, const std::vector<char>& packed_trx) {
   auto packed_approvals = pack(approvals);
   return check_transaction_authorization(
         packed_trx.data(), packed_trx.size(),
         (const char*)0, 0,
         packed_approvals.data(), packed_approvals.size()
   );
}

} /// namespace sysio

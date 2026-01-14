#include <sysio/producer_plugin/trx_priority_db.hpp>
#include <sysio/chain/abi_serializer.hpp>
#include <sysio/chain/contract_table_objects.hpp>
#include <sysio/chain/wasm_interface_private.hpp>
#include <sysio/chain/account_object.hpp>

#include <appbase/application_base.hpp>

namespace sysio {

using namespace sysio::chain;

int trx_priority_db::get_trx_priority(const transaction& trx) const {
   const std::shared_ptr<const trx_priority_map_t> map_ptr = _trx_priority_map.load();
   if (map_ptr == nullptr || map_ptr->empty())
      return appbase::priority::low;

   bool first_action = true;
   const size_t num_actions = trx.actions.size();
   for (const auto& a : trx.actions) {
      auto [b, e] = map_ptr->equal_range(a.account);
      for (auto i = b; i != e; ++i) { // see if exact match provided
         const trx_prio& trx_p = i->second;
         if (a.name == trx_p.action_name) {
            if (trx_p.match_type == only && num_actions == 1) {
               return appbase::priority::low + trx_p.priority;
            }
            if ((first_action && trx_p.match_type == first) || trx_p.match_type == any) {
               return appbase::priority::low + trx_p.priority;
            }
         }
      }
      for (auto i = b; i != e; ++i) { // check for wildcard empty action_name
         const trx_prio& trx_p = i->second;
         if (trx_p.action_name.empty()) {
            if (trx_p.match_type == only && num_actions == 1) {
               return appbase::priority::low + trx_p.priority;
            }
            if ((first_action && trx_p.match_type == first) || trx_p.match_type == any) {
               return appbase::priority::low + trx_p.priority;
            }
         }
      }
      first_action = false;
   }
   return appbase::priority::low;
}

// -----------------------------------------------------------------------------------------------------------------

std::vector<char> get_row_by_id(const controller& control, name code, name scope, name table, uint64_t id ) {
   vector<char> data;
   const auto& db = control.db();
   const auto* t_id = db.find<chain::table_id_object, chain::by_code_scope_table>( boost::make_tuple( code, scope, table ) );
   if ( !t_id ) {
      return data;
   }

   const auto& idx = db.get_index<chain::key_value_index, chain::by_scope_primary>();

   auto itr = idx.lower_bound( boost::make_tuple( t_id->id, id ) );
   if ( itr == idx.end() || itr->t_id != t_id->id || id != itr->primary_key ) {
      return data;
   }

   data.resize( itr->value.size() );
   memcpy( data.data(), itr->value.data(), data.size() );
   return data;
}

vector<char> get_row_by_account(const controller& control, name code, name scope, name table, account_name act ) {
   return get_row_by_id( control, code, scope, table, act.to_uint64_t() );
}

block_timestamp_type get_last_trx_priority_update(const controller& control) {
   try {
      vector<char> data = get_row_by_account( control, config::system_account_name, config::system_account_name, "trxpglobal"_n, "trxpglobal"_n );
      if (data.empty())
         return {};
      return fc::raw::unpack<block_timestamp_type>(data);
   } FC_LOG_AND_DROP()
   return {};
}

// -----------------------------------------------------------------------------------------------------------------

void copy_inline_row(const chain::key_value_object& obj, vector<char>& data) {
   data.resize( obj.value.size() );
   memcpy( data.data(), obj.value.data(), obj.value.size() );
}

void trx_priority_db::load_trx_priority_map(const controller& control, trx_priority_map_t& m) {
   try {
      const fc::time_point deadline = fc::time_point::now() + serializer_max_time;

      const auto& db = control.db();
      const auto table_lookup_tuple = boost::make_tuple( config::system_account_name, config::system_account_name, "trxpriority"_n );
      const auto* t_id = db.find<chain::table_id_object, chain::by_code_scope_table>( table_lookup_tuple );
      if ( !t_id ) {
         return;
      }

      const auto lower_bound_lookup_tuple = std::make_tuple( t_id->id, std::numeric_limits<uint64_t>::lowest() );
      const auto upper_bound_lookup_tuple = std::make_tuple( t_id->id, std::numeric_limits<uint64_t>::max() );

      auto unpack_trx_prio = [&](const chain::key_value_object& obj) {
         trx_prio tmp;
         datastream<const char*>  ds( obj.value.data(), obj.value.size() );
         fc::raw::unpack(ds, tmp);
         return tmp;
      };

      auto walk_table_row_range = [&]( auto itr, auto end_itr ) {
         for( ; itr != end_itr; ++itr ) {
            trx_prio p = unpack_trx_prio( *itr );
            m.insert({p.receiver, p});
            if (fc::time_point::now() > deadline) {
               dlog("Unable to deserialize trx priority table before deadline");
               _last_trx_priority_update = block_timestamp_type{}; // try again on next interval
               break;
            }
         }
      };

      const auto& idx = db.get_index<chain::key_value_index, chain::by_scope_primary>();
      auto lower = idx.lower_bound( lower_bound_lookup_tuple );
      auto upper = idx.upper_bound( upper_bound_lookup_tuple );
      size_t s = std::distance( lower, upper );
      m.reserve( s );
      // walk by descending priority
      walk_table_row_range( std::make_reverse_iterator(upper), std::make_reverse_iterator(lower) );

   } FC_LOG_AND_DROP()
}

// -----------------------------------------------------------------------------------------------------------------


void trx_priority_db::on_irreversible_block(const signed_block_ptr& lib, const block_id_type& block_id, const controller& chain) {
   if (lib->block_num() % trx_priority_refresh_interval != 0)
      return;

   try {
      if (_last_trx_priority_update.load() == get_last_trx_priority_update(chain))
         return;

      std::shared_ptr<trx_priority_map_t> new_map = std::make_shared<trx_priority_map_t>();

      load_trx_priority_map(chain, *new_map);

      _trx_priority_map = new_map; // atomic swap
   } FC_LOG_AND_DROP();
}


} // namespace sysio
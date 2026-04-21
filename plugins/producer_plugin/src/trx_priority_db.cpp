#include <sysio/producer_plugin/trx_priority_db.hpp>
#include <sysio/chain/abi_serializer.hpp>
#include <sysio/chain/kv_table_objects.hpp>
#include <sysio/chain/wasm_interface_private.hpp>
#include <sysio/chain/account_object.hpp>

#include <appbase/application_base.hpp>

#include <vector>
#include <cstring>

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
namespace {

// Read a kv::global value — key is [Name:8B BE]
std::vector<char> get_global_row(const controller& control, name code, name table_name) {
   const auto& db = control.db();
   auto tid = compute_table_id(table_name.to_uint64_t());
   char key_buf[kv_pri_key_size];
   kv_encode_be64(key_buf, table_name.to_uint64_t());
   std::string_view key_sv(key_buf, kv_pri_key_size);

   const auto& kv_idx = db.get_index<chain::kv_index, chain::by_code_key>();
   auto itr = kv_idx.find(boost::make_tuple(code, tid, key_sv));
   if (itr == kv_idx.end())
      return {};
   return {itr->value.data(), itr->value.data() + itr->value.size()};
}

block_timestamp_type get_last_trx_priority_update(const controller& control) {
   try {
      // trxpglobal is kv::global<"trxpglobal"_n, ...>
      auto data = get_global_row(control, config::system_account_name, "trxpglobal"_n);
      if (data.empty())
         return {};
      return fc::raw::unpack<block_timestamp_type>(data);
   } FC_LOG_AND_DROP()
   return {};
}

} // anonymous namespace
// -----------------------------------------------------------------------------------------------------------------

void trx_priority_db::load_trx_priority_map(const controller& control, trx_priority_map_t& m) {
   try {
      const fc::time_point deadline = fc::time_point::now() + serializer_max_time;

      const auto& db = control.db();

      // trxpriority is kv::table (unscoped) — key is [priority:8B BE]
      auto trx_tid = compute_table_id("trxpriority"_n.to_uint64_t());

      const auto& kv_idx = db.get_index<chain::kv_index, chain::by_code_key>();
      // Start from beginning of this table_id — all-zero is the minimum BE uint64
      char min_key[kv_pri_key_size] = {};
      auto itr = kv_idx.lower_bound(boost::make_tuple(config::system_account_name, trx_tid,
                                                        std::string_view(min_key, kv_pri_key_size)));

      while (itr != kv_idx.end() && itr->code == config::system_account_name && itr->table_id == trx_tid) {
         auto kv = itr->key_view();
         if (kv.size() != kv_pri_key_size) break;

         trx_prio tmp;
         datastream<const char*> ds(itr->value.data(), itr->value.size());
         fc::raw::unpack(ds, tmp);
         m.insert({tmp.receiver, tmp});

         if (fc::time_point::now() > deadline) {
            dlog("Unable to deserialize trx priority table before deadline");
            _last_trx_priority_update = block_timestamp_type{}; // try again on next interval
            break;
         }
         ++itr;
      }

   } FC_LOG_AND_DROP()
}

// -----------------------------------------------------------------------------------------------------------------

void trx_priority_db::on_irreversible_block(const signed_block_ptr& lib, const block_id_type&, const controller& chain) {
   if (lib->block_num() % trx_priority_refresh_interval != 0)
      return;

   try {
      auto last_chain_update = get_last_trx_priority_update(chain);
      if (last_chain_update == block_timestamp_type{})
         return; // not available

      if (_last_trx_priority_update == last_chain_update)
         return; // not changed

      std::shared_ptr<trx_priority_map_t> new_map = std::make_shared<trx_priority_map_t>();

      _last_trx_priority_update = last_chain_update; // reset in load_trx_priority_map on failure
      load_trx_priority_map(chain, *new_map);

      if (_last_trx_priority_update == last_chain_update)
         _trx_priority_map = new_map; // atomic swap
   } FC_LOG_AND_DROP();
}


} // namespace sysio

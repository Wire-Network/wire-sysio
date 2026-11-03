#include <sysio/state_history/create_deltas.hpp>
#include <sysio/state_history/serialization.hpp>

#include <algorithm>

namespace sysio {
namespace state_history {

template <typename T>
bool include_delta(const T& old, const T& curr) {
   return true;
}

bool include_delta(const chain::resource_limits::resource_limits_state_object& old,
                   const chain::resource_limits::resource_limits_state_object& curr) {
   return                                                                                       //
       old.average_block_net_usage.last_ordinal != curr.average_block_net_usage.last_ordinal || //
       old.average_block_net_usage.value_ex != curr.average_block_net_usage.value_ex ||         //
       old.average_block_net_usage.consumed != curr.average_block_net_usage.consumed ||         //
       old.average_block_cpu_usage.last_ordinal != curr.average_block_cpu_usage.last_ordinal || //
       old.average_block_cpu_usage.value_ex != curr.average_block_cpu_usage.value_ex ||         //
       old.average_block_cpu_usage.consumed != curr.average_block_cpu_usage.consumed ||         //
       old.total_net_weight != curr.total_net_weight ||                                         //
       old.total_cpu_weight != curr.total_cpu_weight ||                                         //
       old.total_ram_bytes != curr.total_ram_bytes ||                                           //
       old.virtual_net_limit != curr.virtual_net_limit ||                                       //
       old.virtual_cpu_limit != curr.virtual_cpu_limit;
}

bool include_delta(const chain::account_metadata_object& old, const chain::account_metadata_object& curr) {
   return                                               //
       old.name != curr.name ||                         //
       old.is_privileged() != curr.is_privileged() ||   //
       old.last_code_update != curr.last_code_update || //
       old.vm_type != curr.vm_type ||                   //
       old.vm_version != curr.vm_version ||             //
       old.code_hash != curr.code_hash;
}

bool include_delta(const chain::code_object& old, const chain::code_object& curr) { //
   // code_object data that is exported by SHiP is never modified they are only deleted or created,
   // see serialization of history_serial_wrapper<sysio::chain::code_object>
   return false;
}

bool include_delta(const chain::protocol_state_object& old, const chain::protocol_state_object& curr) {
   return old.activated_protocol_features != curr.activated_protocol_features;
}

void pack_deltas(boost::iostreams::filtering_ostreambuf& obuf, const chainbase::database& db, const chain::kv_store& kv,
                 bool full_snapshot) {

   fc::datastream<boost::iostreams::filtering_ostreambuf&> ds{obuf};

   auto pack_row          = [&](auto& ds, auto& row) { fc::raw::pack(ds, make_history_serial_wrapper(db, row)); };

   auto pack_row_v0 = [](auto& ds, bool present, auto& row, auto& pack_row) {
      fc::raw::pack(ds, present);
      fc::datastream<size_t> ps;
      pack_row(ps, row);
      fc::raw::pack(ds, fc::unsigned_int(ps.tellp()));
      pack_row(ds, row);
   };

   auto process_table = [&](auto& ds, auto* name, auto& index, auto& pack_row) {

      if (full_snapshot) {
         if (index.indices().empty())
            return;

         fc::raw::pack(ds, fc::unsigned_int(0)); // table_delta = std::variant<table_delta_v0> and fc::unsigned_int struct_version
         fc::raw::pack(ds, name);
         fc::raw::pack(ds, fc::unsigned_int(index.indices().size()));
         for (auto& row : index.indices()) {
            pack_row_v0(ds, true, row, pack_row);
         }
      } else {
         auto undo = index.last_undo_session();

         size_t num_entries =
             std::count_if(undo.old_values.begin(), undo.old_values.end(),
                           [&index](const auto& old) { return include_delta(old, index.get(old.id)); }) +
             std::distance(undo.removed_values.begin(), undo.removed_values.end()) +
             std::distance(undo.new_values.begin(), undo.new_values.end());

         if (num_entries) {
            fc::raw::pack(ds, fc::unsigned_int(0)); // table_delta = std::variant<table_delta_v0> and fc::unsigned_int struct_version
            fc::raw::pack(ds, name);
            fc::raw::pack(ds, fc::unsigned_int((uint32_t)num_entries));

            for (auto& old : undo.old_values) {
               auto& row = index.get(old.id);
               if (include_delta(old, row)) {
                  pack_row_v0(ds, true, row, pack_row);
               }
            }

            for (auto& old : undo.removed_values) {
               pack_row_v0(ds, false, old, pack_row);
            }

            for (auto& row : undo.new_values) {
               pack_row_v0(ds, true, row, pack_row);
            }
         }
      }
   };

   // KV tables come from the engine, in key order, so their deltas read the same whichever engine holds them.
   auto process_kv_table = [&](auto& ds, auto* name, uint64_t row_count, auto open_cursor, const auto& deltas) {
      const uint64_t num_entries = full_snapshot ? row_count : deltas.size();
      if (num_entries == 0)
         return;

      // table_delta = std::variant<table_delta_v0> and fc::unsigned_int struct_version
      fc::raw::pack(ds, fc::unsigned_int(0));
      fc::raw::pack(ds, name);
      fc::raw::pack(ds, fc::unsigned_int((uint32_t)num_entries));

      if (full_snapshot) {
         auto cursor = open_cursor();
         typename std::remove_cvref_t<decltype(deltas)>::value_type::row_type row;
         for (bool more = cursor->seek_lower({}, row); more; more = cursor->next(row))
            pack_row_v0(ds, true, row, pack_row);
      } else {
         for (const auto& delta : deltas)
            pack_row_v0(ds, delta.present, delta.row, pack_row);
      }
   };

   // Removals go first, so a client mirroring an index never holds an entry at its old and new positions at once.
   auto removals_first = [](auto deltas) {
      std::stable_partition(deltas.begin(), deltas.end(), [](const auto& delta) { return !delta.present; });
      return deltas;
   };
   const auto kv_row_deltas   = full_snapshot ? decltype(kv.row_deltas()){} : removals_first(kv.row_deltas());
   const auto kv_entry_deltas =
      full_snapshot ? decltype(kv.index_entry_deltas()){} : removals_first(kv.index_entry_deltas());
   const int  num_kv_tables   = full_snapshot ? (kv.row_count() != 0) + (kv.index_entry_count() != 0)
                                              : !kv_row_deltas.empty() + !kv_entry_deltas.empty();

   auto has_table = [&](auto x) -> int {
      auto& index = db.get_index<std::remove_pointer_t<decltype(x)>>();
      if (full_snapshot) {
         return !index.indices().empty();
      } else {
         auto undo = index.last_undo_session();
         return std::find_if(undo.old_values.begin(), undo.old_values.end(),
                           [&index](const auto& old) { return include_delta(old, index.get(old.id)); }) != undo.old_values.end() ||
             !undo.removed_values.empty() || !undo.new_values.empty();
      }
   };

   int num_tables = std::apply(
       [&has_table](auto... args) { return (has_table(args) + ... ); },
       std::tuple<chain::account_index*, chain::account_metadata_index*, chain::code_index*,
                  chain::global_property_multi_index*,
                  chain::protocol_state_multi_index*, chain::permission_index*, chain::permission_link_index*,
                  chain::resource_limits::resource_index*,
                  chain::resource_limits::resource_limits_state_index*,
                  chain::resource_limits::resource_limits_config_index*>());

   fc::raw::pack(ds, fc::unsigned_int(num_tables + num_kv_tables));

   process_table(ds, "account", db.get_index<chain::account_index>(), pack_row);
   process_table(ds, "account_metadata", db.get_index<chain::account_metadata_index>(), pack_row);
   process_table(ds, "code", db.get_index<chain::code_index>(), pack_row);

   // All KV rows are emitted as "contract_row_kv" with table_id.
   // Clients resolve table_id → table name via contract ABI.
   process_kv_table(ds, "contract_row_kv", kv.row_count(), [&] { return kv.open_row_cursor(); }, kv_row_deltas);
   process_kv_table(ds, "contract_index_kv", kv.index_entry_count(), [&] { return kv.open_index_cursor(); },
                    kv_entry_deltas);

   process_table(ds, "global_property", db.get_index<chain::global_property_multi_index>(), pack_row);
   process_table(ds, "protocol_state", db.get_index<chain::protocol_state_multi_index>(), pack_row);

   process_table(ds, "permission", db.get_index<chain::permission_index>(), pack_row);
   process_table(ds, "permission_link", db.get_index<chain::permission_link_index>(), pack_row);

   auto pack_resource_limit_row = [&](auto& ds, auto& row) { fc::raw::pack(ds, make_history_serial_wrapper(db, row, history_serial_wrapper_enum_t::resource_limits)); };
   process_table(ds, "resource_limits", db.get_index<chain::resource_limits::resource_index>(), pack_resource_limit_row);
   auto pack_resource_usage_row = [&](auto& ds, auto& row) { fc::raw::pack(ds, make_history_serial_wrapper(db, row, history_serial_wrapper_enum_t::resource_usage)); };
   process_table(ds, "resource_usage", db.get_index<chain::resource_limits::resource_index>(), pack_resource_usage_row);
   process_table(ds, "resource_limits_state", db.get_index<chain::resource_limits::resource_limits_state_index>(),
                 pack_row);
   process_table(ds, "resource_limits_config", db.get_index<chain::resource_limits::resource_limits_config_index>(),
                 pack_row);

   obuf.pubsync();

}


} // namespace state_history
} // namespace sysio

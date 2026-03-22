#include <sysio/state_history/create_deltas.hpp>
#include <sysio/state_history/serialization.hpp>

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

void pack_deltas(boost::iostreams::filtering_ostreambuf& obuf, const chainbase::database& db, bool full_snapshot) {

   fc::datastream<boost::iostreams::filtering_ostreambuf&> ds{obuf};

   auto pack_row          = [&](auto& ds, auto& row) { fc::raw::pack(ds, make_history_serial_wrapper(db, row)); };

   auto process_table = [&](auto& ds, auto* name, auto& index, auto& pack_row) {

      auto pack_row_v0 = [&](auto& ds, bool present, auto& row) {
         fc::raw::pack(ds, present);
         fc::datastream<size_t> ps;
         pack_row(ps, row);
         fc::raw::pack(ds, fc::unsigned_int(ps.tellp()));
         pack_row(ds, row);
      };

      if (full_snapshot) {
         if (index.indices().empty())
            return;

         fc::raw::pack(ds, fc::unsigned_int(0)); // table_delta = std::variant<table_delta_v0> and fc::unsigned_int struct_version
         fc::raw::pack(ds, name);
         fc::raw::pack(ds, fc::unsigned_int(index.indices().size()));
         for (auto& row : index.indices()) {
            pack_row_v0(ds, true, row);
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
                  pack_row_v0(ds, true, row);
               }
            }

            for (auto& old : undo.removed_values) {
               pack_row_v0(ds, false, old);
            }

            for (auto& row : undo.new_values) {
               pack_row_v0(ds, true, row);
            }
         }
      }
   };

   // Like process_table but only includes rows matching a filter predicate.
   auto process_table_filtered = [&](auto& ds, auto* name, auto& index, auto& pack_row, auto filter) {
      auto pack_row_v0 = [&](auto& ds, bool present, auto& row) {
         fc::raw::pack(ds, present);
         fc::datastream<size_t> ps;
         pack_row(ps, row);
         fc::raw::pack(ds, fc::unsigned_int(ps.tellp()));
         pack_row(ds, row);
      };

      if (full_snapshot) {
         size_t count = 0;
         for (auto& row : index.indices())
            if (filter(row)) ++count;
         if (count == 0) return;

         fc::raw::pack(ds, fc::unsigned_int(0));
         fc::raw::pack(ds, name);
         fc::raw::pack(ds, fc::unsigned_int(count));
         for (auto& row : index.indices())
            if (filter(row)) pack_row_v0(ds, true, row);
      } else {
         auto undo = index.last_undo_session();
         size_t num_entries = 0;
         for (auto& old : undo.old_values)
            if (filter(old) && include_delta(old, index.get(old.id))) ++num_entries;
         for (auto& old : undo.removed_values)
            if (filter(old)) ++num_entries;
         for (auto& row : undo.new_values)
            if (filter(row)) ++num_entries;

         if (num_entries) {
            fc::raw::pack(ds, fc::unsigned_int(0));
            fc::raw::pack(ds, name);
            fc::raw::pack(ds, fc::unsigned_int((uint32_t)num_entries));

            for (auto& old : undo.old_values) {
               auto& row = index.get(old.id);
               if (filter(old) && include_delta(old, row))
                  pack_row_v0(ds, true, row);
            }
            for (auto& old : undo.removed_values)
               if (filter(old)) pack_row_v0(ds, false, old);
            for (auto& row : undo.new_values)
               if (filter(row)) pack_row_v0(ds, true, row);
         }
      }
   };

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

   // Count KV tables separately since kv_index may produce 2 tables
   // (contract_row for format=1, contract_row_kv for format=0)
   auto is_standard_kv = [](const chain::kv_object& o) { return o.key_format == 1 && o.key_size == 24; };
   auto is_raw_kv      = [](const chain::kv_object& o) { return o.key_format != 1 || o.key_size != 24; };

   auto count_kv_tables = [&]() -> int {
      auto& index = db.get_index<chain::kv_index>();
      if (full_snapshot) {
         bool has_std = false, has_raw = false;
         for (auto& row : index.indices()) {
            if (is_standard_kv(row)) has_std = true;
            else has_raw = true;
            if (has_std && has_raw) break;
         }
         return (has_std ? 1 : 0) + (has_raw ? 1 : 0);
      } else {
         auto undo = index.last_undo_session();
         bool has_std = false, has_raw = false;
         auto check = [&](const auto& o) {
            if (is_standard_kv(o)) has_std = true;
            else has_raw = true;
         };
         for (auto& old : undo.old_values) check(old);
         for (auto& old : undo.removed_values) check(old);
         for (auto& row : undo.new_values) check(row);
         return (has_std ? 1 : 0) + (has_raw ? 1 : 0);
      }
   };

   int num_tables = std::apply(
       [&has_table](auto... args) { return (has_table(args) + ... ); },
       std::tuple<chain::account_index*, chain::account_metadata_index*, chain::code_index*,
                  chain::global_property_multi_index*,
                  chain::protocol_state_multi_index*, chain::permission_index*, chain::permission_link_index*,
                  chain::resource_limits::resource_index*,
                  chain::resource_limits::resource_limits_state_index*,
                  chain::resource_limits::resource_limits_config_index*,
                  chain::kv_index_index*>())
       + count_kv_tables();

   fc::raw::pack(ds, fc::unsigned_int(num_tables));

   process_table(ds, "account", db.get_index<chain::account_index>(), pack_row);
   process_table(ds, "account_metadata", db.get_index<chain::account_metadata_index>(), pack_row);
   process_table(ds, "code", db.get_index<chain::code_index>(), pack_row);

   // KV rows with key_format=1 (standard 24-byte keys) are emitted as "contract_row"
   // for backward compatibility with Hyperion and other SHiP clients.
   // KV rows with key_format=0 (raw) are emitted as "contract_row_kv" — a new delta type
   // that clients can opt into. This avoids confusing clients with zeroed-out fields.
   {
      auto& kv_idx = db.get_index<chain::kv_index>();
      process_table_filtered(ds, "contract_row", kv_idx, pack_row, is_standard_kv);
      process_table_filtered(ds, "contract_row_kv", kv_idx, pack_row, is_raw_kv);
   }
   process_table(ds, "contract_index_kv", db.get_index<chain::kv_index_index>(), pack_row);

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

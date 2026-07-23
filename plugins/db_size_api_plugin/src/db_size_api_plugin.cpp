#include <fc/variant.hpp>
#include <fc/io/json.hpp>
#include <sysio/db_size_api_plugin/db_size_api_plugin.hpp>
#include <sysio/http_plugin/bind_stream.hpp>

namespace sysio {

using namespace sysio;

void db_size_api_plugin::plugin_startup() {
   auto& _http_plugin = app().get_plugin<http_plugin>();
   _http_plugin.add_api_stream({
      bind_stream<&db_size_api_plugin::get, dispatch::sync>(
         _http_plugin, this, "/v1/db_size/get",
         api_category::db_size, http_params_types::no_params, 200),
   }, appbase::exec_queue::read_only);
}

db_size_stats db_size_api_plugin::get() {
   const chainbase::database& db = app().get_plugin<chain_plugin>().chain().db();
   db_size_stats ret;

   ret.free_bytes = db.get_segment_manager()->get_free_memory();
   ret.size = db.get_segment_manager()->get_size();
   ret.used_bytes = ret.size - ret.free_bytes;
   ret.reclaimable_bytes = db.get_reclaimable_memory();

   chainbase::database::database_index_row_count_multiset indices = db.row_count_per_index();
   for(const auto& i : indices)
      ret.indices.emplace_back(db_size_index_count{i.second, i.first});

   return ret;
}

}

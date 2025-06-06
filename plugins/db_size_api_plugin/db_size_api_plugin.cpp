#include <fc/variant.hpp>
#include <fc/io/json.hpp>
#include <sysio/db_size_api_plugin/db_size_api_plugin.hpp>
#include <sysio/http_plugin/http_plugin.hpp>

namespace sysio {

   static auto _db_size_api_plugin = application::register_plugin<db_size_api_plugin>();

using namespace sysio;

#define CALL_WITH_400(api_name, api_handle, call_name, INVOKE, http_response_code) \
{std::string("/v1/" #api_name "/" #call_name), \
   api_category::db_size, \
   [api_handle](string&&, string&& body, url_response_callback&& cb) mutable { \
          try { \
             body = parse_params<std::string, http_params_types::no_params>(body); \
             INVOKE \
             cb(http_response_code, fc::variant(result)); \
          } catch (...) { \
             http_plugin::handle_exception(#api_name, #call_name, body, cb); \
          } \
       }}

#define INVOKE_R_V(api_handle, call_name) \
     auto result = api_handle->call_name();


void db_size_api_plugin::plugin_startup() {
   app().get_plugin<http_plugin>().add_api({
       CALL_WITH_400(db_size, this, get,  INVOKE_R_V(this, get), 200),
   }, appbase::exec_queue::read_only);
}

db_size_stats db_size_api_plugin::get() {
   const chainbase::database& db = app().get_plugin<chain_plugin>().chain().db();
   db_size_stats ret;

   ret.free_bytes = db.get_segment_manager()->get_free_memory();
   ret.size = db.get_segment_manager()->get_size();
   ret.used_bytes = ret.size - ret.free_bytes;

   chainbase::database::database_index_row_count_multiset indices = db.row_count_per_index();
   for(const auto& i : indices)
      ret.indices.emplace_back(db_size_index_count{i.second, i.first});

   return ret;
}

#undef INVOKE_R_V
#undef CALL

}

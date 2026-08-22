#include <sysio/net_api_plugin/net_api_plugin.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/transaction.hpp>
#include <sysio/http_plugin/bind_stream.hpp>

#include <fc/variant.hpp>
#include <fc/io/json.hpp>

#include <chrono>

namespace sysio {

using namespace sysio;

void net_api_plugin::plugin_startup() {
   dlog("starting net_api_plugin");
   // lifetime of plugin is lifetime of application
   auto& net_mgr = app().get_plugin<net_plugin>();
   auto& _http_plugin = app().get_plugin<http_plugin>();

   using cat = api_category;
   using pt = http_params_types;

   _http_plugin.add_async_api_stream({
      bind_stream<&net_plugin::connect, dispatch::sync>(
         _http_plugin, std::ref(net_mgr), "/v1/net/connect", cat::net_rw, pt::params_required, 201),
      bind_stream<&net_plugin::disconnect, dispatch::sync>(
         _http_plugin, std::ref(net_mgr), "/v1/net/disconnect", cat::net_rw, pt::params_required, 201),
      bind_stream<&net_plugin::status, dispatch::sync>(
         _http_plugin, std::ref(net_mgr), "/v1/net/status", cat::net_ro, pt::params_required, 201),
      bind_stream<&net_plugin::connections, dispatch::sync>(
         _http_plugin, std::ref(net_mgr), "/v1/net/connections", cat::net_ro, pt::no_params, 201),
      bind_stream<&net_plugin::bp_gossip_peers, dispatch::sync>(
         _http_plugin, std::ref(net_mgr), "/v1/net/bp_gossip_peers", cat::net_ro, pt::no_params, 201),
   });
}

void net_api_plugin::plugin_initialize(const variables_map& options) {
   try {
      const auto& _http_plugin = app().get_plugin<http_plugin>();
      if( !_http_plugin.is_on_loopback(api_category::net_rw)) {
         wlog( "\n"
               "**********SECURITY WARNING**********\n"
               "*                                  *\n"
               "* --        Net RW API          -- *\n"
               "* - EXPOSED to the LOCAL NETWORK - *\n"
               "* - USE ONLY ON SECURE NETWORKS! - *\n"
               "*                                  *\n"
               "************************************\n" );
      }
   } FC_LOG_AND_RETHROW()
}

}

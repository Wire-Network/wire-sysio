#include <sysio/producer_api_plugin/producer_api_plugin.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/http_plugin/bind_stream.hpp>

#include <fc/time.hpp>
#include <fc/variant.hpp>

#include <chrono>

namespace sysio {

using namespace sysio;

void producer_api_plugin::plugin_startup() {
   dlog("starting producer_api_plugin");
   // lifetime of plugin is lifetime of application
   auto& producer = app().get_plugin<producer_plugin>();
   auto& _http_plugin = app().get_plugin<http_plugin>();

   using pp = producer_plugin;
   using cat = api_category;
   using pt = http_params_types;

   _http_plugin.add_api_stream({
      bind_stream<&pp::paused, dispatch::sync>(
         _http_plugin, std::ref(producer), "/v1/producer/paused", cat::producer_ro, pt::no_params, 201),
      bind_stream<&pp::get_runtime_options, dispatch::sync>(
         _http_plugin, std::ref(producer), "/v1/producer/get_runtime_options", cat::producer_ro, pt::no_params, 201),
      bind_stream<&pp::get_greylist, dispatch::sync>(
         _http_plugin, std::ref(producer), "/v1/producer/get_greylist", cat::producer_ro, pt::no_params, 201),
      bind_stream<&pp::get_whitelist_blacklist, dispatch::sync>(
         _http_plugin, std::ref(producer), "/v1/producer/get_whitelist_blacklist", cat::producer_ro, pt::no_params, 201),
      bind_stream<&pp::get_scheduled_protocol_feature_activations, dispatch::sync>(
         _http_plugin, std::ref(producer), "/v1/producer/get_scheduled_protocol_feature_activations",
         cat::producer_ro, pt::no_params, 201),
      bind_stream<&pp::get_supported_protocol_features, dispatch::sync>(
         _http_plugin, std::ref(producer), "/v1/producer/get_supported_protocol_features",
         cat::producer_ro, pt::possible_no_params, 201),
      bind_stream<&pp::get_unapplied_transactions, dispatch::sync>(
         _http_plugin, std::ref(producer), "/v1/producer/get_unapplied_transactions",
         cat::producer_ro, pt::possible_no_params, 200),
      bind_stream<&pp::get_snapshot_requests, dispatch::sync>(
         _http_plugin, std::ref(producer), "/v1/producer/get_snapshot_requests",
         cat::producer_ro, pt::no_params, 201),
   }, appbase::exec_queue::read_only, appbase::priority::medium_high);

   // Not safe to run in parallel
   _http_plugin.add_api_stream({
      bind_stream<&pp::pause, dispatch::sync_void>(
         _http_plugin, std::ref(producer), "/v1/producer/pause", cat::producer_rw, pt::no_params, 201),
      bind_stream<&pp::pause_at_block, dispatch::sync_void>(
         _http_plugin, std::ref(producer), "/v1/producer/pause_at_block", cat::producer_rw, pt::params_required, 201),
      bind_stream<&pp::resume, dispatch::sync_void>(
         _http_plugin, std::ref(producer), "/v1/producer/resume", cat::producer_rw, pt::no_params, 201),
      bind_stream<&pp::update_runtime_options, dispatch::sync_void>(
         _http_plugin, std::ref(producer), "/v1/producer/update_runtime_options",
         cat::producer_rw, pt::params_required, 201),
      bind_stream<&pp::add_greylist_accounts, dispatch::sync_void>(
         _http_plugin, std::ref(producer), "/v1/producer/add_greylist_accounts",
         cat::producer_rw, pt::params_required, 201),
      bind_stream<&pp::remove_greylist_accounts, dispatch::sync_void>(
         _http_plugin, std::ref(producer), "/v1/producer/remove_greylist_accounts",
         cat::producer_rw, pt::params_required, 201),
      bind_stream<&pp::set_whitelist_blacklist, dispatch::sync_void>(
         _http_plugin, std::ref(producer), "/v1/producer/set_whitelist_blacklist",
         cat::producer_rw, pt::params_required, 201),
      bind_stream<&pp::create_snapshot, dispatch::async>(
         _http_plugin, std::ref(producer), "/v1/producer/create_snapshot", cat::snapshot, pt::no_params, 201),
      bind_stream<&pp::schedule_snapshot, dispatch::sync>(
         _http_plugin, std::ref(producer), "/v1/producer/schedule_snapshot",
         cat::snapshot, pt::possible_no_params, 201),
      bind_stream<&pp::unschedule_snapshot, dispatch::sync>(
         _http_plugin, std::ref(producer), "/v1/producer/unschedule_snapshot",
         cat::snapshot, pt::params_required, 201),
      bind_stream<&pp::get_integrity_hash, dispatch::sync>(
         _http_plugin, std::ref(producer), "/v1/producer/get_integrity_hash",
         cat::producer_rw, pt::no_params, 201),
      bind_stream<&pp::schedule_protocol_feature_activations, dispatch::sync_void>(
         _http_plugin, std::ref(producer), "/v1/producer/schedule_protocol_feature_activations",
         cat::producer_rw, pt::params_required, 201),
   }, appbase::exec_queue::read_write, appbase::priority::medium_high);
}

void producer_api_plugin::plugin_initialize(const variables_map& options) {
   try {
      const auto& _http_plugin = app().get_plugin<http_plugin>();
      if( !_http_plugin.is_on_loopback(api_category::producer_rw)) {
         wlog( "\n"
               "**********SECURITY WARNING**********\n"
               "*                                  *\n"
               "* --       Producer RW API      -- *\n"
               "* - EXPOSED to the LOCAL NETWORK - *\n"
               "* - USE ONLY ON SECURE NETWORKS! - *\n"
               "*                                  *\n"
               "************************************\n" );

      }
      if( !_http_plugin.is_on_loopback(api_category::snapshot)) {
         wlog( "\n"
               "**********SECURITY WARNING**********\n"
               "*                                  *\n"
               "* --         Snapshot API       -- *\n"
               "* - EXPOSED to the LOCAL NETWORK - *\n"
               "* - USE ONLY ON SECURE NETWORKS! - *\n"
               "*                                  *\n"
               "************************************\n" );

      }
   } FC_LOG_AND_RETHROW()
}

}

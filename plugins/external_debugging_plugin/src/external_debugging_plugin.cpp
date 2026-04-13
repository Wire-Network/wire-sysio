#include <boost/signals2/connection.hpp>
#include <fc/log/logger.hpp>
#include <fc/network/json_rpc/json_rpc_client.hpp>
#include <fc/parallel/worker_task_queue.hpp>
#include <sysio/batch_operator_plugin/batch_operator_plugin.hpp>
#include <sysio/external_debugging_plugin/external_debugging_plugin.hpp>
#include <sysio/external_debugging_plugin/external_debugging_rpc_client.hpp>
#include <sysio/opp/debugging/debugging.pb.h>

namespace sysio {

using boost::signals2::scoped_connection;
namespace rpc = fc::network::json_rpc;

namespace {
constexpr auto option_name_ext_debugging_server = "ext-debugging-server";
}

struct external_debugging_plugin::impl {
   std::string server_url;
   bool enabled = false;

   // Signal connection to batch_operator_plugin
   std::optional<scoped_connection> bo_signal_connection;

   // RPC client — created on startup after server validation
   std::unique_ptr<rpc::json_rpc_client> rpc_client;

   // Async event delivery queue
   std::shared_ptr<fc::parallel::worker_task_queue<opp::debugging::DebugEnvelopeEvent>> event_queue;

   // ------------------------------------------------------------------
   //  Serialize DebugEnvelopeEvent to PutEnvelopeRequest and send
   //  via typed protobuf RPC. No manual JSON construction.
   // ------------------------------------------------------------------
   void send_envelope(opp::debugging::DebugEnvelopeEvent& event) {
      auto& [epoch_index, endpoints_type, batch_op_name, envelope_data] = event;

      opp::debugging::PutEnvelopeRequest req;
      req.set_batch_op_name(batch_op_name.to_string());
      req.set_endpoints_type(endpoints_type);
      req.set_envelope_data(envelope_data.data(), envelope_data.size());

      auto res =
         sysio::debugging::rpc_client::execute<opp::debugging::PutEnvelopeResponse>(*rpc_client, debugging::rpc_client::api_paths::opp_envelope, req);

      ilog("opp_tracking: delivered epoch={} endpoints={} batch_op={} "
           "({} bytes) -> key={} existed={}",
           epoch_index, opp::debugging::DebugOutpostEndpointsType_Name(endpoints_type), batch_op_name.to_string(),
           envelope_data.size(), res.key(), res.data_existed());
   }

   // ------------------------------------------------------------------
   //  Validate server connectivity at startup.
   // ------------------------------------------------------------------
   bool validate_server() {
      try {
         rpc_client->send_http(rpc::http_verb::GET, debugging::rpc_client::api_paths::ping);
         return true;
      } catch (const fc::exception& e) {
         elog("opp_tracking: server validation failed at {}: {}", server_url, e.to_string());
         return false;
      }
   }
};

// -----------------------------------------------------------------------
//  Plugin lifecycle
// -----------------------------------------------------------------------

external_debugging_plugin::external_debugging_plugin()
   : _impl(std::make_unique<impl>()) {}

external_debugging_plugin::~external_debugging_plugin() = default;

void external_debugging_plugin::set_program_options(options_description& cli, options_description& cfg) {
   auto opts = cfg.add_options();
   opts(option_name_ext_debugging_server, bpo::value<std::string>(),
        "URL of the external debugging server (e.g. http://localhost:9876). "
        "If not provided, OPP tracking is disabled.");
}

void external_debugging_plugin::plugin_initialize(const variables_map& options) {
   if (options.contains(option_name_ext_debugging_server)) {
      _impl->server_url = options[option_name_ext_debugging_server].as<std::string>();
      _impl->enabled = true;
   }
}

void external_debugging_plugin::plugin_startup() {
   if (!_impl->enabled) {
      ilog("external_debugging_plugin: no --{} provided, disabled", option_name_ext_debugging_server);
      return;
   }

   // Create RPC client pointed at the OPP base path —
   // json_rpc_client::call() POSTs JSON-RPC 2.0 to this URL
   auto rpc_url = _impl->server_url + debugging::rpc_client::api_paths::opp_base;
   _impl->rpc_client = std::make_unique<rpc::json_rpc_client>(fc::url(rpc_url));

   // Validate server connectivity
   FC_ASSERT(_impl->validate_server(), "External debugging server not reachable at {}", _impl->server_url);

   // Start async event delivery queue
   _impl->event_queue = fc::parallel::worker_task_queue<opp::debugging::DebugEnvelopeEvent>::create(
      {.max_threads = 1},
      [impl = _impl.get()](opp::debugging::DebugEnvelopeEvent& event) {
         try {
            impl->send_envelope(event);
         } FC_LOG_AND_DROP("external_debugging_plugin: error sending envelope to {}", impl->server_url)
      });

   // Connect to batch_operator_plugin signal
   auto& bo_plug = app().get_plugin<batch_operator_plugin>();
   _impl->bo_signal_connection.emplace(bo_plug.debugging_opp_envelope().connect(
      [impl = _impl.get()](const opp::debugging::DebugEnvelopeEvent& event) { impl->event_queue->push(event); }));

   ilog("external_debugging_plugin: connected to batch_operator_plugin, "
        "forwarding to {}",
        _impl->server_url);
}

void external_debugging_plugin::plugin_shutdown() {
   // Disconnect signal first
   _impl->bo_signal_connection.reset();

   // Stop the event delivery queue (drains workers and joins)
   if (_impl->event_queue) {
      _impl->event_queue->stop();
      _impl->event_queue.reset();
   }

   ilog("external_debugging_plugin: shutdown complete");
}

} // namespace sysio

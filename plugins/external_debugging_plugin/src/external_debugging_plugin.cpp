#include <boost/signals2/connection.hpp>
#include <fc/log/logger.hpp>
#include <fc/network/json_rpc/json_rpc_client.hpp>
#include <fc/task/deadline.hpp>
#include <sysio/batch_operator_plugin/batch_operator_plugin.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/external_debugging_plugin/debug_envelope_event_sink.hpp>
#include <sysio/external_debugging_plugin/external_debugging_plugin.hpp>
#include <sysio/external_debugging_plugin/external_debugging_rpc_client.hpp>
#include <sysio/opp/debugging/debugging.pb.h>
#include <sysio/opp/opp.pb.h>

namespace sysio {

using boost::signals2::scoped_connection;
namespace rpc = fc::network::json_rpc;

namespace {
constexpr auto option_name_ext_debugging_server = "ext-debugging-server";
constexpr auto option_name_ext_debugging_max_pending_envelopes = "ext-debugging-max-pending-envelopes";
constexpr auto option_name_ext_debugging_request_timeout_ms = "ext-debugging-request-timeout-ms";
constexpr uint32_t default_max_pending_envelopes = 16;
constexpr uint32_t default_request_timeout_ms = 5'000;
}

struct external_debugging_plugin::impl {
   std::string server_url;
   bool enabled = false;
   uint32_t max_pending_envelopes = default_max_pending_envelopes;
   uint32_t request_timeout_ms = default_request_timeout_ms;

   // Signal connection to batch_operator_plugin
   std::optional<scoped_connection> bo_signal_connection;

   // RPC client — resolves once at startup and then uses request deadlines
   std::unique_ptr<rpc::json_rpc_client> rpc_client;

   // Async event delivery sink
   std::shared_ptr<external_debugging::debug_envelope_event_sink> event_sink;

   /** Return the absolute deadline for one debugging-server request. */
   fc::time_point next_request_deadline() const { return fc::time_point::now() + fc::milliseconds(request_timeout_ms); }

   // ------------------------------------------------------------------
   //  Serialize DebugEnvelopeEvent to PutEnvelopeRequest and send
   //  via typed protobuf RPC. No manual JSON construction.
   // ------------------------------------------------------------------
   void send_envelope(opp::debugging::DebugEnvelopeEvent& event) {
      fc::task::deadline_scope request_deadline(next_request_deadline());
      auto& [epoch_index, endpoints_type, batch_op_name, envelope_data] = event;

      opp::debugging::PutEnvelopeRequest req;
      req.set_batch_op_name(batch_op_name.to_string());
      req.set_endpoints_type(endpoints_type);
      req.set_envelope_data(envelope_data.data(), envelope_data.size());

      auto res =
         sysio::debugging::rpc_client::execute<opp::debugging::PutEnvelopeResponse>(*rpc_client, debugging::rpc_client::api_paths::opp_envelope, req);

      ilog("external_debugging_plugin: send_envelope: delivered epoch={} endpoints={} batch_op={} "
           "({} bytes) -> key={} existed={}",
           epoch_index, opp::debugging::DebugOutpostEndpointsType_Name(endpoints_type), batch_op_name.to_string(),
           envelope_data.size(), res.key(), res.data_existed());
   }

   // ------------------------------------------------------------------
   //  Validate server connectivity at startup.
   // ------------------------------------------------------------------
   bool validate_server() {
      try {
         fc::task::deadline_scope request_deadline(next_request_deadline());
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
   opts(option_name_ext_debugging_max_pending_envelopes,
        bpo::value<uint32_t>()->default_value(default_max_pending_envelopes),
        "Maximum debugging envelopes waiting behind the active server request.");
   opts(option_name_ext_debugging_request_timeout_ms, bpo::value<uint32_t>()->default_value(default_request_timeout_ms),
        "Maximum time in milliseconds for each debugging-server request.");
}

void external_debugging_plugin::plugin_initialize(const variables_map& options) {
   if (options.contains(option_name_ext_debugging_max_pending_envelopes)) {
      _impl->max_pending_envelopes = options[option_name_ext_debugging_max_pending_envelopes].as<uint32_t>();
   }
   if (options.contains(option_name_ext_debugging_request_timeout_ms)) {
      _impl->request_timeout_ms = options[option_name_ext_debugging_request_timeout_ms].as<uint32_t>();
   }

   SYS_ASSERT(_impl->max_pending_envelopes > 0, chain::plugin_config_exception, "--{} must be greater than 0",
              option_name_ext_debugging_max_pending_envelopes);
   SYS_ASSERT(_impl->request_timeout_ms > 0, chain::plugin_config_exception, "--{} must be greater than 0",
              option_name_ext_debugging_request_timeout_ms);

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
   _impl->rpc_client =
      std::make_unique<rpc::json_rpc_client>(fc::url(rpc_url), std::nullopt, rpc::endpoint_refresh_policy::never);

   // Validate server connectivity
   FC_ASSERT(_impl->validate_server(), "External debugging server not reachable at {}", _impl->server_url);

   // Start the bounded asynchronous event sink
   _impl->event_sink = std::make_shared<external_debugging::debug_envelope_event_sink>(
      _impl->max_pending_envelopes, [impl = _impl.get()](opp::debugging::DebugEnvelopeEvent& event) {
         std::vector<char>& event_data = std::get<3>(event);
         opp::Envelope envelope;
         if (!envelope.ParseFromArray(event_data.data(), event_data.size())) {
            elog("external_debugging_plugin: failed to parse envelope data for event from batch_op={}, skipping",
                 std::get<2>(event).to_string());
            return;
         };

         std::string event_json;
         if (auto res = google::protobuf::json::MessageToJsonString(envelope, &event_json); !res.ok()) {
            elog("external_debugging_plugin: failed to unpack DebugEnvelope for event from batch_op={}: code={}, message={}",
                 std::get<2>(event).to_string(), res.raw_code(), res.message());
            return;
         }

         try {
            impl->send_envelope(event);
         }
         FC_LOG_AND_DROP("external_debugging_plugin: error sending envelope to {}: {}", impl->server_url, event_json);
      });

   // Connect to batch_operator_plugin signal
   auto& bo_plug = app().get_plugin<batch_operator_plugin>();
   _impl->bo_signal_connection.emplace(
      bo_plug.debugging_opp_envelope().connect(external_debugging::make_debug_envelope_slot(_impl->event_sink)));

   ilog("external_debugging_plugin: connected to batch_operator_plugin, "
        "forwarding to {} with pending_capacity={} request_timeout_ms={}",
        _impl->server_url, _impl->max_pending_envelopes, _impl->request_timeout_ms);
}

void external_debugging_plugin::plugin_shutdown() {
   // Disconnect signal first
   _impl->bo_signal_connection.reset();

   // Stop the event delivery sink (joins the active worker)
   if (_impl->event_sink) {
      _impl->event_sink->stop();
      _impl->event_sink.reset();
   }

   ilog("external_debugging_plugin: shutdown complete");
}

} // namespace sysio

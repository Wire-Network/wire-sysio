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
constexpr auto option_name_ext_debugging_additional_ca_file = "ext-debugging-additional-ca-file";
constexpr auto option_name_ext_debugging_additional_ca_path = "ext-debugging-additional-ca-path";
constexpr auto option_name_ext_debugging_proxy = "ext-debugging-proxy";
constexpr uint32_t default_max_pending_envelopes = 16;
constexpr uint32_t default_request_timeout_ms = 5'000;
constexpr uint64_t max_debugging_request_bytes = 1ULL * 1024ULL * 1024ULL;
constexpr uint64_t max_debugging_response_bytes = 1ULL * 1024ULL * 1024ULL;
}

struct external_debugging_plugin::impl {
   std::string server_url;
   bool enabled = false;
   uint32_t max_pending_envelopes = default_max_pending_envelopes;
   uint32_t request_timeout_ms = default_request_timeout_ms;
   fc::http::transport_options transport_options;

   // Signal connection to batch_operator_plugin
   std::optional<scoped_connection> bo_signal_connection;

   // RPC client — resolves once at startup and then uses request deadlines
   std::unique_ptr<rpc::json_rpc_client> rpc_client;

   // Async event delivery sink
   std::shared_ptr<external_debugging::debug_envelope_event_sink> event_sink;

   /** Return the absolute deadline for one debugging-server request. */
   fc::time_point next_request_deadline() const { return fc::time_point::now() + fc::milliseconds(request_timeout_ms); }

   /** Return a credential- and path-free endpoint label for diagnostics. */
   std::string log_endpoint() const {
      try {
         return fc::http::sanitized_endpoint(fc::url(server_url));
      } catch (...) {
         return "<invalid-endpoint>";
      }
   }

   /** Return the named bounded request policy for external-debugging traffic. */
   rpc::client_options client_options() const {
      const auto request_timeout = fc::milliseconds(request_timeout_ms);
      return rpc::client_options{
         .transport = transport_options,
         .request =
            fc::http::request_options{
               .max_request_body_bytes = max_debugging_request_bytes,
               .max_response_body_bytes = max_debugging_response_bytes,
               .timeouts =
                  fc::http::timeout_options{
                     .connect = request_timeout,
                     .header = request_timeout,
                     .read = request_timeout,
                     .idle = request_timeout,
                     .total = request_timeout,
                  },
            },
      };
   }

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
         elog("opp_tracking: server validation failed at {}: {}", log_endpoint(), e.to_string());
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
   opts(option_name_ext_debugging_additional_ca_file,
        bpo::value<std::filesystem::path>(),
        "PEM CA bundle added to system trust for external-debugging HTTPS requests.");
   opts(option_name_ext_debugging_additional_ca_path,
        bpo::value<std::filesystem::path>(),
        "Hashed CA directory added to system trust for external-debugging HTTPS requests.");
   opts(option_name_ext_debugging_proxy,
        bpo::value<std::string>(),
        "Explicit proxy URL for external-debugging HTTP requests.");
}

void external_debugging_plugin::plugin_initialize(const variables_map& options) {
   if (options.contains(option_name_ext_debugging_max_pending_envelopes)) {
      _impl->max_pending_envelopes = options[option_name_ext_debugging_max_pending_envelopes].as<uint32_t>();
   }
   if (options.contains(option_name_ext_debugging_request_timeout_ms)) {
      _impl->request_timeout_ms = options[option_name_ext_debugging_request_timeout_ms].as<uint32_t>();
   }
   if (options.contains(option_name_ext_debugging_additional_ca_file))
      _impl->transport_options.additional_ca_file =
         options.at(option_name_ext_debugging_additional_ca_file).as<std::filesystem::path>();
   if (options.contains(option_name_ext_debugging_additional_ca_path))
      _impl->transport_options.additional_ca_path =
         options.at(option_name_ext_debugging_additional_ca_path).as<std::filesystem::path>();
   if (options.contains(option_name_ext_debugging_proxy))
      _impl->transport_options.proxy =
         options.at(option_name_ext_debugging_proxy).as<std::string>();

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

   // Create and validate the RPC client under one startup deadline. The nested validation deadline
   // clamps to this outer scope, so DNS resolution and the ping share the configured request budget.
   {
      fc::task::deadline_scope startup_deadline(_impl->next_request_deadline());
      auto rpc_url = _impl->server_url + debugging::rpc_client::api_paths::opp_base;
      _impl->rpc_client = std::make_unique<rpc::json_rpc_client>(
         fc::url(rpc_url),
         std::nullopt,
         rpc::endpoint_refresh_policy::never,
         _impl->client_options());

      FC_ASSERT(_impl->validate_server(),
                "External debugging server not reachable at {}",
                _impl->log_endpoint());
   }

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

         try {
            impl->send_envelope(event);
         }
         FC_LOG_AND_DROP(
            "external_debugging_plugin: error sending envelope to {} "
            "(epoch={}, batch_op={})",
            impl->log_endpoint(),
            std::get<0>(event),
            std::get<2>(event).to_string());
      });

   // Connect to batch_operator_plugin signal
   auto& bo_plug = app().get_plugin<batch_operator_plugin>();
   _impl->bo_signal_connection.emplace(
      bo_plug.debugging_opp_envelope().connect(external_debugging::make_debug_envelope_slot(_impl->event_sink)));

   ilog("external_debugging_plugin: connected to batch_operator_plugin, "
        "forwarding to {} with pending_capacity={} request_timeout_ms={}",
        _impl->log_endpoint(), _impl->max_pending_envelopes, _impl->request_timeout_ms);
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

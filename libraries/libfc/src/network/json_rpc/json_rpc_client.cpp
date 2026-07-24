// Implementation file for JSON-RPC client
#include <fc/network/json_rpc/json_rpc_client.hpp>

#include <boost/beast/version.hpp>

#include <algorithm>
#include <cctype>
#include <string_view>

#include <magic_enum/magic_enum.hpp>

namespace fc::network::json_rpc {

namespace {

constexpr std::string_view http_scheme = "http";
constexpr std::string_view https_scheme = "https";
constexpr std::string_view default_http_path = "/";
constexpr uint32_t single_attempt = 1;
constexpr uint32_t stale_connection_max_attempts = 2;

/** Prevent implicit replay through caller-supplied base request options. */
fc::http::request_options
non_replaying_request_options(fc::http::request_options options) {
   options.retry.max_attempts = single_attempt;
   options.idempotent = false;
   options.retry_only_reused_connection = false;
   return options;
}

/** Allow one immediate replay only after a cached connection proves stale. */
fc::http::request_options
stale_connection_retry_options(fc::http::request_options options) {
   options.retry.max_attempts = stale_connection_max_attempts;
   options.retry.initial_backoff = fc::microseconds(0);
   options.retry.max_backoff = fc::microseconds(0);
   options.idempotent = true;
   options.retry_only_reused_connection = true;
   return options;
}

/** Return transport options adjusted for the legacy endpoint-refresh contract. */
client_options normalize_options(endpoint_refresh_policy refresh_policy, client_options options) {
   options.transport.refresh_dns_on_connection_failure =
      refresh_policy == endpoint_refresh_policy::on_connection_failure;
   if (refresh_policy == endpoint_refresh_policy::never) {
      options.transport.dns_cache_timeout_seconds = -1;
   }
   return options;
}

/** Convert the public JSON-RPC HTTP verb to the shared transport enum by name. */
fc::http::request_method to_transport_method(http_verb verb) {
   std::string name(magic_enum::enum_name(verb));
   FC_ASSERT(!name.empty(), "Unknown JSON-RPC HTTP verb");
   std::transform(name.begin(), name.end(), name.begin(), [](unsigned char value) {
      return static_cast<char>(std::tolower(value));
   });
   const auto converted = magic_enum::enum_cast<fc::http::request_method>(name);
   FC_ASSERT(converted, "Unsupported JSON-RPC HTTP verb {}", name);
   return *converted;
}

/** Return @p base with its path replaced while preserving endpoint identity and credentials. */
fc::url with_path(const fc::url& base, const std::string& path) {
   const auto query_separator = path.find('?');
   const auto path_part = path.substr(0, query_separator);
   const fc::ostring query =
      query_separator == std::string::npos
         ? fc::ostring{}
         : fc::ostring{path.substr(query_separator + 1)};
   return fc::url(base.proto(),
                  base.host(),
                  base.user(),
                  base.pass(),
                  std::filesystem::path(path_part.empty() ? default_http_path : path_part),
                  query,
                  base.args(),
                  base.port());
}

/** Require HTTP 200 while retaining a sanitized response diagnostic. */
void require_ok(const fc::http::response& response, std::string_view operation) {
   if (response.status != 200) {
      FC_THROW("{} failed with HTTP status {}{}{}",
               std::string(operation),
               response.status,
               response.reason.empty() ? "" : " ",
               response.reason);
   }
}

} // namespace

json_rpc_error::json_rpc_error(const std::string& message)
   : json_rpc_error(0, message, {}) {}

json_rpc_error::json_rpc_error(int code_in,
                               const std::string& message,
                               const variant& data_in)
   : fc::exception(code_in, "json_rpc_error", message)
   , code(code_in)
   , data(data_in) {
   append_log(FC_LOG_MESSAGE(error, "{}", message));
}

json_rpc_client json_rpc_client::create(
   const std::variant<std::string, fc::url>& source,
   client_options options) {
   fc::url target;
   if (std::holds_alternative<std::string>(source))
      target = fc::url(std::get<std::string>(source));
   else
      target = std::get<fc::url>(source);

   FC_ASSERT(target.proto() == http_scheme || target.proto() == https_scheme,
             "Unsupported JSON-RPC URL scheme: {}", target.proto());
   return json_rpc_client(std::move(target),
                          std::nullopt,
                          endpoint_refresh_policy::on_connection_failure,
                          std::move(options));
}

json_rpc_client::json_rpc_client(fc::url url,
                                 const std::optional<std::string>& user_agent,
                                 endpoint_refresh_policy refresh_policy,
                                 client_options options)
   : _url(std::move(url))
   , _user_agent(user_agent.value_or(BOOST_BEAST_VERSION_STRING))
   , _next_id(1)
   , _options(normalize_options(refresh_policy, std::move(options)))
   , _transport(_options.transport) {
   const auto scheme = _url.proto();
   FC_ASSERT(scheme == http_scheme || scheme == https_scheme,
             "Unsupported JSON-RPC URL scheme: {}", scheme);
   FC_ASSERT(_url.host() && !_url.host()->empty(), "JSON-RPC URL is missing host");
   _transport.prime_endpoint(_url, _options.request);
}

variant json_rpc_client::call(const std::string& method, const fc::variant& params) {
   return call_with_policy(
      method,
      params,
      non_replaying_request_options(_options.request));
}

variant json_rpc_client::call_idempotent(
   const std::string& method,
   const fc::variant& params) {
   return call_with_policy(
      method,
      params,
      stale_connection_retry_options(_options.request));
}

variant json_rpc_client::call_with_policy(
   const std::string& method,
   const fc::variant& params,
   fc::http::request_options request_options) {
   const auto id = _next_id++;

   mutable_variant_object obj;
   obj("jsonrpc", "2.0")
      ("method", std::string{method})
      ("params", params)
      ("id", id);

   variant response =
      send_json(
         variant(obj),
         true,
         std::move(request_options));
   validate_basic_response(response);

   const auto& object = response.get_object();
   if (!object.contains("id"))
      FC_THROW("JSON-RPC: missing 'id' in response");

   const auto& response_id = object["id"];
   int64_t got_id = 0;
   if (response_id.is_int64())
      got_id = response_id.as_int64();
   else if (response_id.is_uint64())
      got_id = static_cast<int64_t>(response_id.as_uint64());
   else
      FC_THROW("JSON-RPC: invalid 'id' type in response");

   if (got_id != id)
      FC_THROW("JSON-RPC: response 'id' does not match request 'id'");

   if (object.contains("error")) {
      const auto& error = object["error"];
      int code = 0;
      std::string message = "JSON-RPC error";
      variant data;
      if (error.is_object()) {
         const auto& error_object = error.get_object();
         if (error_object.contains("code")) {
            const auto& value = error_object["code"];
            if (value.is_int64())
               code = static_cast<int>(value.as_int64());
            else if (value.is_uint64())
               code = static_cast<int>(value.as_uint64());
         }
         if (error_object.contains("message"))
            message = error_object["message"].as_string();
         if (error_object.contains("data"))
            data = error_object["data"];
      }
      throw json_rpc_error(code, message, data);
   }

   if (!object.contains("result"))
      FC_THROW("JSON-RPC: response missing 'result'");
   return object["result"];
}

void json_rpc_client::notify(const std::string& method, const fc::variant& params) {
   mutable_variant_object obj;
   obj("jsonrpc", "2.0")("method", method)("params", params);
   send_json(
      fc::variant(obj),
      false,
      non_replaying_request_options(_options.request));
}

fc::variant json_rpc_client::call_batch(const std::vector<fc::variant>& requests) {
   variants payload;
   payload.reserve(requests.size());
   for (const auto& request : requests) {
      if (!request.is_object())
         throw json_rpc_error("JSON-RPC batch: each element must be an object");
      fc::mutable_variant_object object(request.get_object());
      if (!object.contains("jsonrpc"))
         object("jsonrpc", "2.0");
      if (!object.contains("method"))
         throw json_rpc_error("JSON-RPC batch: missing 'method'");
      payload.emplace_back(std::move(object));
   }

   variant response =
      send_json(
         variant(payload),
         true,
         non_replaying_request_options(_options.request));
   if (!response.is_array())
      throw json_rpc_error("JSON-RPC batch: server did not return an array");
   return response;
}

variant json_rpc_client::send_json(
   const variant& payload,
   bool expect_json_body,
   fc::http::request_options request_options) {
   const auto body = fc::json::to_string(payload, fc::json::yield_function_t{});
   fc::http::request request{
      .method = fc::http::request_method::post,
      .target = _url,
      .body = body,
      .content_type = "application/json",
      .user_agent = _user_agent,
   };
   const auto response =
      _transport.perform(
         request,
         std::move(request_options));
   require_ok(response, "JSON-RPC request");

   if (!expect_json_body)
      return variant();
   if (response.body.empty())
      FC_THROW("Empty HTTP body, expected JSON-RPC response");
   return fc::json::from_string(response.body);
}

std::string json_rpc_client::send_http(http_verb verb,
                                       const std::string& path,
                                       const std::string& body,
                                       const std::string& content_type) {
   fc::http::request request{
      .method = to_transport_method(verb),
      .target = with_path(_url, path.empty() ? std::string(default_http_path) : path),
      .body = body,
      .content_type = content_type,
      .user_agent = _user_agent,
   };
   const auto response =
      _transport.perform(
         request,
         non_replaying_request_options(_options.request));
   require_ok(response, "HTTP request");
   return response.body;
}

void json_rpc_client::validate_basic_response(const variant& response) {
   if (!response.is_object())
      FC_THROW("JSON-RPC: response must be an object");

   const auto& object = response.get_object();
   if (!object.contains("jsonrpc") || !object["jsonrpc"].is_string() ||
       object["jsonrpc"].get_string() != "2.0") {
      FC_THROW("JSON-RPC: invalid or missing 'jsonrpc' == \"2.0\"");
   }
}

} // namespace fc::network::json_rpc

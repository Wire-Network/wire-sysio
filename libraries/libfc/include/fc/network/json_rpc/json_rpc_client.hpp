#pragma once

#include <fc/variant.hpp>
#include <fc/variant_object.hpp>
#include <fc/io/json.hpp>
#include <fc/network/http/http_client.hpp>
#include <fc/time.hpp>

#include <optional>
#include <string>
#include <variant>
#include <vector>
#include <fc/network/url.hpp>

namespace fc::network::json_rpc {

// -----------------------------------------------------------------------
//  HTTP verb — used by send_http and typed REST methods.
//  Existing JSON-RPC methods (call, notify, call_batch) are unchanged
//  and always use POST internally.
// -----------------------------------------------------------------------
enum class http_verb { GET, PUT, POST, DELETE_ };

/// Control whether connection failures invalidate the client's startup DNS result.
enum class endpoint_refresh_policy { on_connection_failure, never };

/** Shared transport and per-request policy for one JSON-RPC client. */
struct client_options {
   /// TLS trust, explicit proxy, and DNS-cache configuration.
   fc::http::transport_options transport{
      .dns_cache_timeout_seconds = -1,
   };
   /// Caller-specific deadlines, byte ceilings, cancellation, and retry policy.
   fc::http::request_options request;
};

// JSON-RPC error type
struct json_rpc_error : fc::exception {
   int      code;
   variant  data;
   explicit json_rpc_error(const std::string& message);
   json_rpc_error(int code_in, const std::string& message, const variant& data_in = {});
};

// Simple synchronous JSON-RPC 2.0 client over HTTP/1.1, extended with
// raw HTTP verb support for REST-style endpoints.
class json_rpc_client {
public:
   explicit json_rpc_client(fc::url url, const std::optional<std::string>& user_agent = std::nullopt,
                            endpoint_refresh_policy refresh_policy = endpoint_refresh_policy::on_connection_failure,
                            client_options options = {});

   // -----------------------------------------------------------------------
   //  JSON-RPC 2.0 methods (unchanged, backwards compatible)
   // -----------------------------------------------------------------------

   /**
    * Perform one non-replaying JSON-RPC request and return its `result`.
    *
    * This call is always single-attempt even when the client's base request
    * policy enables retries.
    */
   fc::variant call(const std::string& method, const fc::variant& params = variants{});

   /**
    * Perform a read-only call that may be replayed once when a cached
    * connection proves stale.
    */
   fc::variant call_idempotent(
      const std::string& method,
      const fc::variant& params = variants{});

   /**
    * Send one non-replaying notification and consume its HTTP response.
    *
    * A JSON-RPC notification has no `id`; the response body is ignored.
    */
   void notify(const std::string& method, const fc::variant& params = variants{});

   /** Perform one non-replaying JSON-RPC batch request. */
   variant call_batch(const std::vector<variant>& requests);

   // -----------------------------------------------------------------------
   //  Raw HTTP verb support — for REST-style endpoints.
   //  Does NOT wrap in JSON-RPC envelope.
   // -----------------------------------------------------------------------

   /// Raw HTTP with verb + path + body. Returns the response body string.
   std::string send_http(http_verb verb, const std::string& path,
                         const std::string& body = "",
                         const std::string& content_type = "application/json");

   static json_rpc_client create(const std::variant<std::string, fc::url>& source,
                                 client_options options = {});

private:
   fc::url      _url;
   std::string  _user_agent;
   std::int64_t _next_id;
   client_options _options;
   fc::http::transport _transport;

   /** Build and perform one call with an explicit replay policy. */
   variant call_with_policy(
      const std::string& method,
      const fc::variant& params,
      fc::http::request_options request_options);

   /** Perform HTTP POST with JSON payload and an explicit replay policy. */
   variant send_json(
      const variant& payload,
      bool expect_json_body,
      fc::http::request_options request_options);

   static void validate_basic_response(const variant& response);
};

} // namespace fc::network::json_rpc

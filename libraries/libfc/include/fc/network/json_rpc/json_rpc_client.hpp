#pragma once

#include <fc/variant.hpp>
#include <fc/variant_object.hpp>
#include <fc/io/json.hpp>
#include <boost/asio/io_context.hpp>

#include <string>
#include <variant>
#include <vector>
#include <optional>
#include <fc/network/url.hpp>

namespace fc::network::json_rpc {

namespace asio = boost::asio;

// -----------------------------------------------------------------------
//  HTTP verb — used by send_http and typed REST methods.
//  Existing JSON-RPC methods (call, notify, call_batch) are unchanged
//  and always use POST internally.
// -----------------------------------------------------------------------
enum class http_verb { GET, PUT, POST, DELETE_ };

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
   explicit json_rpc_client(fc::url                           url,
                            const std::optional<std::string>& user_agent = std::nullopt);

   // -----------------------------------------------------------------------
   //  JSON-RPC 2.0 methods (unchanged, backwards compatible)
   // -----------------------------------------------------------------------

   // Perform a JSON-RPC request and return the "result" member.
   // Throws json_rpc_error for JSON-RPC error and std::runtime_error for transport/protocol issues.
   fc::variant call(const std::string& method, const fc::variant& params = variants{});

   // JSON-RPC notification (no "id", no response expected).
   // This still uses HTTP/1.1 and reads the HTTP response; it just ignores JSON body.
   void notify(const std::string& method, const fc::variant& params = variants{});

   // Batch call. 'requests' is an array of JSON-RPC request/notification objects.
   variant call_batch(const std::vector<variant>& requests);

   // -----------------------------------------------------------------------
   //  Raw HTTP verb support — for REST-style endpoints.
   //  Does NOT wrap in JSON-RPC envelope.
   // -----------------------------------------------------------------------

   /// Raw HTTP with verb + path + body. Returns the response body string.
   std::string send_http(http_verb verb, const std::string& path,
                         const std::string& body = "",
                         const std::string& content_type = "application/json");

   static json_rpc_client create(const std::variant<std::string, fc::url>& source);

private:
   asio::io_context _io_ctx{};
   fc::url      _url;
   std::string  _user_agent;
   std::int64_t _next_id;

   // Perform HTTP POST with JSON payload; optionally parse JSON body.
   variant send_json(const variant& payload, bool expect_json_body = true);

   static void validate_basic_response(const variant& response);
};

} // namespace fc::network::json_rpc

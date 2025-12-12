// Implementation file for JSON-RPC client
#include <fc/network/json_rpc/json_rpc_client.hpp>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/version.hpp>

#include <format>
#include <ranges>
#include <gsl-lite/gsl-lite.hpp>

#define HTTP_VERSION 11

namespace fc::network::json_rpc {

using namespace std::literals;
namespace beast = boost::beast;
namespace http = beast::http;
namespace asio = boost::asio;
using tcp = asio::ip::tcp;

// json_rpc_error
json_rpc_error::json_rpc_error(const std::string& message) : json_rpc_error(0, message, {}) {}

json_rpc_error::json_rpc_error(int code_in, const std::string& message, const variant& data_in)
   : fc::exception(code_in, message)
     , code(code_in)
     , data(data_in) {}

json_rpc_client json_rpc_client::create(const std::variant<std::string, fc::url>& source) {
   fc::url url;
   if (std::holds_alternative<std::string>(source)) {
      url = fc::url(std::get<std::string>(source));
   } else {
      url = std::get<fc::url>(source);
   }
   FC_ASSERT(url.proto() == "http" || url.proto() == "https");
   //return json_rpc_client(url.host().value(), std::to_string(url.port().value_or(80)), url.path().value());
   return json_rpc_client(url);
}

// json_rpc_client
json_rpc_client::json_rpc_client(fc::url                           url,
                                 const std::optional<std::string>& user_agent)
   : _url(std::move(url))
     , _user_agent(user_agent.value_or(BOOST_BEAST_VERSION_STRING))
     , _next_id(1) {}

variant json_rpc_client::call(const std::string& method, const fc::variant& params) {
   const auto id = _next_id++;

   mutable_variant_object obj;
   obj("jsonrpc", "2.0")("method", std::string{method})("params", std::move(params))("id", id);

   variant response = send_json(variant(obj));

   validate_basic_response(response);

   // Ensure id matches
   const auto& ro = response.get_object();
   if (!ro.contains("id")) {
      FC_THROW("JSON-RPC: missing 'id' in response");
   }
   const auto& idv    = ro["id"];
   int64_t     got_id = 0;
   if (idv.is_int64())
      got_id = idv.as_int64();
   else if (idv.is_uint64())
      got_id = static_cast<int64_t>(idv.as_uint64());
   else {
      FC_THROW("JSON-RPC: invalid 'id' type in response");
   }

   if (got_id != id) {
      FC_THROW("JSON-RPC: response 'id' does not match request 'id'");
   }

   // Handle error
   if (ro.contains("error")) {
      const auto& e    = ro["error"];
      int         code = 0;
      std::string msg  = "JSON-RPC error";
      variant     data;
      if (e.is_object()) {
         const auto& eo = e.get_object();
         if (eo.contains("code")) {
            const auto& cv = eo["code"];
            if (cv.is_int64())
               code = static_cast<int>(cv.as_int64());
            else if (cv.is_uint64())
               code = static_cast<int>(cv.as_uint64());
         }
         if (eo.contains("message")) {
            msg = eo["message"].as_string();
         }
         if (eo.contains("data")) {
            data = eo["data"];
         }
      }
      throw json_rpc_error(code, msg, std::move(data));
   }

   if (!ro.contains("result")) {
      FC_THROW("JSON-RPC: response missing 'result'");
   }

   return ro["result"];
}

void json_rpc_client::notify(const std::string& method, const fc::variant& params) {
   mutable_variant_object obj;
   obj("jsonrpc", "2.0")("method", method)("params", params);

   // Send but ignore JSON body; only ensu.value()re HTTP transport succeeds.
   send_json(fc::variant(obj), false);
}

fc::variant json_rpc_client::call_batch(const std::vector<fc::variant>& requests) {
   // Insert ids where missing (only for "request", not "notification")
   variants payload_variants = requests | std::views::transform([&](auto& req) {
      if (!req.is_object()) {
         throw json_rpc_error("JSON-RPC batch: each element must be an object");
      }
      fc::mutable_variant_object o(req.get_object());
      if (!o.contains("jsonrpc")) {

         o("jsonrpc", "2.0");
         // req = variant(mvo);
      }

      if (!o.contains("method")) {
         throw json_rpc_error("JSON-RPC batch: missing 'method'");
      }

      return o;
   }) | std::ranges::to<std::vector<fc::variant>>();

   variant response = send_json(variant(payload_variants));

   if (!response.is_array()) {
      throw json_rpc_error("JSON-RPC batch: server did not return an array");
   }

   return response;
}

variant json_rpc_client::send_json(const variant& payload, bool expect_json_body) {
   auto& ioc  = _io_ctx;
   auto  body = fc::json::to_string(payload, fc::json::yield_function_t{});

   auto        scheme = _url.proto();
   auto        host   = _url.host().value();
   auto        port   = std::to_string(_url.port().value_or(scheme == "https" ? 443 : 80));
   std::string path   = _url.path().value_or("/");

   asio::ssl::context ctx{asio::ssl::context::tlsv12_client};
   tcp::resolver      resolver{_io_ctx};

   // Resolve and connect
   auto dest = resolver.resolve(
      host,
      port);


   // Build HTTP request
   http::request<http::string_body> req{http::verb::post, path, HTTP_VERSION};
   req.set(http::field::host, host);
   req.set(http::field::user_agent, _user_agent);
   req.set(http::field::content_type, "application/json");
   req.body() = body;
   req.prepare_payload();
   beast::flat_buffer                buffer;
   http::response<http::string_body> res;
   if (scheme == "https") {
      // ---------------- HTTPS ----------------

      beast::ssl_stream<beast::tcp_stream> stream(ioc, ctx);
      // auto                                 stream_cleaner = gsl_lite::finally([&stream] {
      //    stream.shutdown();
      //    // stream.socket().shutdown(tcp::socket::shutdown_both);
      // });


      // SNI required for most servers
      if (!SSL_set_tlsext_host_name(stream.native_handle(), host.c_str())) {
         throw beast::system_error(
            beast::error_code(static_cast<int>(::ERR_get_error()),
                              asio::error::get_ssl_category()));
      }

      beast::get_lowest_layer(stream).connect(dest);
      stream.handshake(asio::ssl::stream_base::client);

      http::write(stream, req);


      http::read(stream, buffer, res);

      // Shutdown TLS
      beast::error_code ec;
      stream.shutdown(ec);


   } else if (scheme == "http") {
      beast::tcp_stream stream{_io_ctx};

      auto stream_cleaner = gsl_lite::finally([&stream] {
         stream.close();
         // stream.socket().shutdown(tcp::socket::shutdown_both);
      });

      stream.connect(dest);
      // Send request
      http::write(stream, req);

      // Receive response
      http::read(stream, buffer, res);

      // Gracefully close the socket
      beast::error_code ec;
      stream.socket().shutdown(tcp::socket::shutdown_both, ec);


   } else {
      throw std::runtime_error("Unsupported URL scheme: " + scheme);
   }
   // ignore ec on shutdown

   // Check HTTP status
   if (res.result() != http::status::ok) {
      FC_THROW_FMT("HTTP ERROR ({}): {}", static_cast<unsigned>(res.result()), std::string(res.reason()));
   }

   if (!expect_json_body) {
      // For notifications, we ignore the response body.
      return variant();
   }

   if (res.body().empty()) {
      FC_THROW("Empty HTTP body, expected JSON-RPC response");
   }

   variant j = fc::json::from_string(res.body());

   return j;
}

void json_rpc_client::validate_basic_response(const variant& response) {
   if (!response.is_object()) {
      FC_THROW("JSON-RPC: response must be an object");
   }

   const auto& o = response.get_object();
   if (!o.contains("jsonrpc") || !o["jsonrpc"].is_string() ||
       o["jsonrpc"].get_string() != "2.0") {
      FC_THROW("JSON-RPC: invalid or missing 'jsonrpc' == \"2.0\"");
   }
}

} // namespace fc::network::json_rpc
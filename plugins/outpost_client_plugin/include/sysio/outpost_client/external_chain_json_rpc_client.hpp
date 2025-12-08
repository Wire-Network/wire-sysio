#pragma once
//
// Created by jglanz on 12/4/25.
//

#pragma once

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <chrono>
#include <fc/crypto/chain_types_reflect.hpp>
#include <fc/crypto/sha256.hpp>
#include <fc/exception/exception.hpp>
#include <fc/io/json.hpp>
#include <fc/network/url.hpp>
#include <fc/time.hpp>
#include <fc/variant.hpp>
#include <fc/variant_object.hpp>
#include <memory>
#include <string>
#include <sysio/signature_provider_manager_plugin/signature_provider_manager_plugin.hpp>
#include <utility>

namespace sysio::outpost_client {

using envelope_t        = fc::variant_object;
using envelope_params_t = fc::variants;

/**
 * Generic JSON-RPC 2.0 client for external chains. Derivations provide a concrete
 * chain-kind so that the signature provider can be validated up front.
 */
template <fc::crypto::chain_kind_t TargetChain>
class external_chain_json_rpc_client {
protected:
   sysio::signature_provider_ptr _signing_provider;
   std::string                   _endpoint;
   uint64_t                      _request_counter{1};

public:
   constexpr static auto target_chain = TargetChain;

   external_chain_json_rpc_client(std::shared_ptr<sysio::signature_provider> signing_provider, std::string endpoint)
      : _signing_provider(std::move(signing_provider))
      , _endpoint(std::move(endpoint)) {
      FC_ASSERT(_signing_provider, "A signing provider is required for chain=${c}", ("c", TargetChain));
      FC_ASSERT(_signing_provider->target_chain == TargetChain,
                "Signature provider chain (${p}) does not match target (${t})",
                ("p", _signing_provider->target_chain)("t", TargetChain));
   }

   external_chain_json_rpc_client(const signature_provider_id_t& sig_provider_query, std::string endpoint)
      : external_chain_json_rpc_client(sysio::get_signature_provider(sig_provider_query), std::move(endpoint)) {}

   virtual ~external_chain_json_rpc_client() = default;

   /**
    * Issue a read-only JSON-RPC call.
    */
   fc::variant invoke_read(const std::string& method, const envelope_params_t& params,
                           const fc::microseconds& timeout = fc::microseconds(0)) {
      auto env = make_envelope(method, params);
      return dispatch(env, timeout);
   }

   /**
    * Issue a mutating JSON-RPC call. Derived classes must construct a chain-valid
    * request (including signing) before the dispatch.
    */
   fc::variant invoke_write(const std::string& method, const envelope_params_t& params,
                            const fc::microseconds& timeout = fc::microseconds(0)) {
      auto env = make_write_envelope(method, params);
      return dispatch(env, timeout);
   }

   const std::string& endpoint() const { return _endpoint; }
   void               set_endpoint(std::string endpoint) { _endpoint = std::move(endpoint); }

protected:
   envelope_t make_envelope(const std::string& method, const envelope_params_t& params) {
      fc::mutable_variant_object obj;
      obj("jsonrpc", "2.0")("method", method)("params", params)("id", _request_counter++);
      return obj;
   }

   fc::variant dispatch(const envelope_t& envelope, const fc::microseconds& timeout) const {
      namespace beast = boost::beast;
      namespace http  = beast::http;
      namespace net   = boost::asio;
      using tcp       = net::ip::tcp;

      fc::url url(_endpoint);
      auto    host_opt = url.host();
      FC_ASSERT(host_opt, "Endpoint host missing in ${u}", ("u", _endpoint));
      auto host = *host_opt;

      auto        port_opt = url.port();
      std::string port     = port_opt ? std::to_string(*port_opt) : (url.proto() == "https" ? "443" : "80");

      std::string target = "/";
      if (auto p = url.path()) {
         target = p->generic_string().empty() ? "/" : p->generic_string();
      }
      if (auto q = url.query()) {
         target += "?";
         target += *q;
      }

      auto body     = fc::json::to_string(envelope, fc::json::yield_function_t{});
      
      try {
         net::io_context   ioc;
         tcp::resolver     resolver{ioc};
         beast::tcp_stream stream{ioc};

         auto const results = resolver.resolve(host, port);
         stream.connect(results);

         if (timeout.count() > 0) {
            stream.expires_after(std::chrono::microseconds(timeout.count()));
         }

         http::request<http::string_body> req{http::verb::post, target, 11};
         req.set(http::field::host, host);
         req.set(http::field::content_type, "application/json");
         req.set(http::field::user_agent, "sysio-outpost-json-rpc-client");
         req.body() = body;
         req.prepare_payload();

         http::write(stream, req);

         beast::flat_buffer                buffer;
         http::response<http::string_body> res;
         http::read(stream, buffer, res);

         beast::error_code ec;
         stream.socket().shutdown(tcp::socket::shutdown_both, ec);

         FC_ASSERT(res.result() == http::status::ok, "JSON-RPC HTTP status error: ${status}",
                   ("status", res.result_int()));

         return fc::json::from_string(res.body());
      } catch (const std::exception& e) {
         FC_THROW("JSON-RPC request failed for ${url}: ${e}", ("url", _endpoint)("e", e.what()));
      }
   }

   virtual envelope_t make_write_envelope(const std::string& method, const envelope_params_t& params) = 0;
};
} // namespace sysio::outpost_client

#pragma once

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <fc/io/json.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fc::test {

/**
 * One deterministic response consumed by {@link scripted_json_rpc_server}.
 */
struct scripted_json_rpc_response {
   enum class kind {
      result,
      error,
      raw_body,
      close_connection,
      hang
   };

   kind        response_kind = kind::result;
   fc::variant payload;
   std::chrono::microseconds delay{0};

   /** Return a successful JSON-RPC result. */
   static scripted_json_rpc_response result(fc::variant value) {
      return {.response_kind = kind::result, .payload = std::move(value)};
   }

   /** Return a successful result after a deterministic server-side delay. */
   static scripted_json_rpc_response delayed_result(
      fc::variant value,
      std::chrono::microseconds delay) {
      return {
         .response_kind = kind::result,
         .payload = std::move(value),
         .delay = delay,
      };
   }

   /** Return a JSON-RPC error object supplied as `payload`. */
   static scripted_json_rpc_response error(fc::variant value) {
      return {.response_kind = kind::error, .payload = std::move(value)};
   }

   /** Return a complete caller-supplied HTTP response body. */
   static scripted_json_rpc_response raw(std::string body) {
      return {.response_kind = kind::raw_body, .payload = fc::variant(std::move(body))};
   }

   /** Accept and then close the connection without an HTTP response. */
   static scripted_json_rpc_response close() {
      return {.response_kind = kind::close_connection};
   }

   /** Accept the request and withhold a response until server destruction. */
   static scripted_json_rpc_response hanging() {
      return {.response_kind = kind::hang};
   }
};

/**
 * Loopback JSON-RPC server that consumes a fixed response script.
 *
 * Each accepted request consumes one script entry. Successful and error
 * responses copy the request ID into their JSON-RPC envelope, allowing a
 * long-lived client to issue multiple calls. Request methods are retained for
 * assertions. The hanging response cooperates with destruction so deadline
 * tests cannot strand a worker thread.
 */
class scripted_json_rpc_server {
public:
   explicit scripted_json_rpc_server(std::vector<scripted_json_rpc_response> responses)
      : scripted_json_rpc_server(std::move(responses), "127.0.0.1", 0, false) {}

   /**
    * Bind a caller-selected loopback address/port.
    *
    * `stop_after_script` is useful for deterministic DNS-fallback tests: the
    * server closes its listener after the final response so a later connect
    * attempt advances to the next resolved address.
    */
   scripted_json_rpc_server(std::vector<scripted_json_rpc_response> responses,
                            const std::string& bind_address,
                            uint16_t port,
                            bool stop_after_script)
      : _responses(std::move(responses))
      , _acceptor(_io, tcp::endpoint(boost::asio::ip::make_address(bind_address), port))
      , _port(_acceptor.local_endpoint().port())
      , _bind_address(bind_address)
      , _stop_after_script(stop_after_script)
      , _worker([this] { serve(); }) {}

   scripted_json_rpc_server(const scripted_json_rpc_server&) = delete;
   scripted_json_rpc_server& operator=(const scripted_json_rpc_server&) = delete;

   ~scripted_json_rpc_server() {
      {
         std::lock_guard lock(_mutex);
         _stopping = true;
      }
      _stopping_changed.notify_all();

      boost::system::error_code error;
      boost::asio::io_context wake_io;
      tcp::socket wake_socket(wake_io);
      wake_socket.connect(
         tcp::endpoint(boost::asio::ip::make_address(_bind_address), _port),
         error);
      wake_socket.close(error);
      if (_worker.joinable()) {
         _worker.join();
      }
      _acceptor.close(error);
   }

   /** Return the loopback URL selected for this server. */
   std::string url() const {
      return "http://127.0.0.1:" + std::to_string(_port);
   }

   /** Return a URL using a hostname that resolves to this server's port. */
   std::string url_for_host(const std::string& host) const {
      return "http://" + host + ":" + std::to_string(_port);
   }

   /** Return the selected TCP port. */
   uint16_t port() const noexcept { return _port; }

   /** Return the number of fully parsed requests received so far. */
   size_t request_count() const {
      std::lock_guard lock(_mutex);
      return _request_methods.size();
   }

   /** Return a copy of received JSON-RPC method names in request order. */
   std::vector<std::string> request_methods() const {
      std::lock_guard lock(_mutex);
      return _request_methods;
   }

private:
   using tcp = boost::asio::ip::tcp;

   /** Return whether destruction requested the server loop to stop. */
   bool stopping() const {
      std::lock_guard lock(_mutex);
      return _stopping;
   }

   /** Consume scripted responses sequentially on the worker thread. */
   void serve() {
      size_t response_index = 0;
      while (!stopping()) {
         boost::system::error_code error;
         tcp::socket socket(_io);
         _acceptor.accept(socket, error);
         if (error || stopping()) {
            return;
         }

         boost::beast::flat_buffer request_buffer;
         while (!stopping()) {
            boost::beast::http::request<boost::beast::http::string_body> request;
            boost::beast::http::read(socket, request_buffer, request, error);
            if (error) {
               break;
            }

            fc::variant request_variant;
            try {
               request_variant = fc::json::from_string(request.body());
               const auto& request_object = request_variant.get_object();
               const auto method = request_object["method"].as_string();
               {
                  std::lock_guard lock(_mutex);
                  _request_methods.push_back(method);
               }
            } catch (...) {
               break;
            }

            if (response_index >= _responses.size()) {
               socket.close(error);
               break;
            }
            const auto response = _responses[response_index++];
            const bool script_complete = response_index >= _responses.size();
            if (response.response_kind == scripted_json_rpc_response::kind::close_connection) {
               socket.set_option(boost::asio::socket_base::linger(true, 0), error);
               socket.close(error);
               if (_stop_after_script && script_complete) {
                  _acceptor.close(error);
                  return;
               }
               break;
            }
            if (response.response_kind == scripted_json_rpc_response::kind::hang) {
               std::unique_lock lock(_mutex);
               _stopping_changed.wait(lock, [this] { return _stopping; });
               socket.close(error);
               return;
            }
            if (response.delay.count() > 0) {
               std::this_thread::sleep_for(response.delay);
            }

            std::string response_body;
            if (response.response_kind == scripted_json_rpc_response::kind::raw_body) {
               response_body = response.payload.as_string();
            } else {
               const auto& request_object = request_variant.get_object();
               fc::mutable_variant_object envelope;
               envelope("jsonrpc", "2.0")("id", request_object["id"]);
               if (response.response_kind == scripted_json_rpc_response::kind::result) {
                  envelope("result", response.payload);
               } else {
                  envelope("error", response.payload);
               }
               response_body =
                  fc::json::to_string(fc::variant(envelope), fc::json::yield_function_t{});
            }

            boost::beast::http::response<boost::beast::http::string_body> http_response{
               boost::beast::http::status::ok, request.version()};
            http_response.set(boost::beast::http::field::content_type, "application/json");
            http_response.body() = std::move(response_body);
            http_response.keep_alive(request.keep_alive() && !(_stop_after_script && script_complete));
            http_response.prepare_payload();
            boost::beast::http::write(socket, http_response, error);
            if (error || !http_response.keep_alive()) {
               socket.close(error);
            }
            if (_stop_after_script && script_complete) {
               _acceptor.close(error);
               return;
            }
            if (error || !http_response.keep_alive()) {
               break;
            }
         }
      }
   }

   std::vector<scripted_json_rpc_response> _responses;
   boost::asio::io_context                  _io;
   tcp::acceptor                            _acceptor;
   uint16_t                                 _port;
   std::string                              _bind_address;
   mutable std::mutex                       _mutex;
   std::condition_variable                  _stopping_changed;
   bool                                     _stopping = false;
   bool                                     _stop_after_script = false;
   std::vector<std::string>                 _request_methods;
   std::thread                              _worker;
};

} // namespace fc::test

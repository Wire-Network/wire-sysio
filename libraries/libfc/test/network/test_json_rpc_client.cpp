/**
 * @file test_json_rpc_client.cpp
 * @brief Regression tests for deadline-bound JSON-RPC transport calls.
 */

#include <fc/network/json_rpc/json_rpc_client.hpp>
#include <fc/task/deadline.hpp>

#include <boost/asio.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>
#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {

using tcp = boost::asio::ip::tcp;

constexpr size_t OVERSIZED_RESPONSE_BODY_BYTES = 2 * 1024 * 1024;

/**
 * HTTP endpoint that reads one request and deliberately withholds the response.
 */
class hanging_http_server {
public:
   /**
    * Start listening on a loopback port and launch the accept worker.
    */
   hanging_http_server()
      : _acceptor(_io, tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0))
      , _port(_acceptor.local_endpoint().port())
      , _worker([this] { serve(); }) {}

   hanging_http_server(const hanging_http_server&) = delete;
   hanging_http_server& operator=(const hanging_http_server&) = delete;

   /**
    * Stop the worker without waiting for the client's read timeout.
    */
   ~hanging_http_server() {
      _stop = true;
      boost::system::error_code ec;
      _acceptor.close(ec);
      unblock_accept();
      if (_worker.joinable()) {
         _worker.join();
      }
   }

   /**
    * Return the TCP port assigned by the OS.
    */
   uint16_t port() const { return _port; }

private:
   /**
    * Connect once to the listening socket so a blocked accept can observe shutdown.
    */
   void unblock_accept() {
      boost::asio::io_context io;
      tcp::socket socket(io);
      boost::system::error_code ec;
      socket.connect(tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), _port), ec);
   }

   /**
    * Accept a single client, consume the request header, and then hang.
    */
   void serve() {
      boost::system::error_code ec;
      tcp::socket socket(_io);
      _acceptor.accept(socket, ec);
      if (ec) {
         return;
      }

      boost::asio::streambuf request;
      boost::asio::read_until(socket, request, "\r\n\r\n", ec);
      while (!_stop.load()) {
         std::this_thread::sleep_for(10ms);
      }
      socket.close(ec);
   }

   boost::asio::io_context _io;
   tcp::acceptor           _acceptor;
   uint16_t                _port;
   std::atomic_bool        _stop{false};
   std::thread             _worker;
};

/**
 * HTTP endpoint that replies to one request with a caller-supplied response body.
 */
class fixed_response_http_server {
public:
   /**
    * Start listening on a loopback port and launch the response worker.
    */
   explicit fixed_response_http_server(std::string response_body)
      : _acceptor(_io, tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0))
      , _port(_acceptor.local_endpoint().port())
      , _response_body(std::move(response_body))
      , _worker([this] { serve(); }) {}

   fixed_response_http_server(const fixed_response_http_server&) = delete;
   fixed_response_http_server& operator=(const fixed_response_http_server&) = delete;

   /**
    * Stop the worker if the client failed before accepting the response.
    */
   ~fixed_response_http_server() {
      boost::system::error_code ec;
      _acceptor.close(ec);
      unblock_accept();
      if (_worker.joinable()) {
         _worker.join();
      }
   }

   /**
    * Return the TCP port assigned by the OS.
    */
   uint16_t port() const { return _port; }

private:
   /**
    * Connect once to the listening socket so a blocked accept can observe shutdown.
    */
   void unblock_accept() {
      boost::asio::io_context io;
      tcp::socket socket(io);
      boost::system::error_code ec;
      socket.connect(tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), _port), ec);
   }

   /**
    * Accept a single client, consume its request header, and write the response.
    */
   void serve() {
      boost::system::error_code ec;
      tcp::socket socket(_io);
      _acceptor.accept(socket, ec);
      if (ec) {
         return;
      }

      boost::asio::streambuf request;
      boost::asio::read_until(socket, request, "\r\n\r\n", ec);
      if (ec) {
         return;
      }

      std::ostringstream response;
      response << "HTTP/1.1 200 OK\r\n"
               << "Content-Type: application/json\r\n"
               << "Content-Length: " << _response_body.size() << "\r\n"
               << "Connection: close\r\n\r\n"
               << _response_body;
      const auto response_text = response.str();
      boost::asio::write(socket, boost::asio::buffer(response_text), ec);
      socket.close(ec);
   }

   boost::asio::io_context _io;
   tcp::acceptor           _acceptor;
   uint16_t                _port;
   std::string             _response_body;
   std::thread             _worker;
};

/**
 * JSON-RPC endpoint that either serves two calls on one connection or closes
 * the first keep-alive connection before accepting the second call.
 */
class reusable_json_rpc_server {
public:
   /** Connection behavior exercised by one server instance. */
   enum class behavior {
      healthy_keep_alive,
      stale_after_first_response,
   };

   /** Start the scripted endpoint on an ephemeral loopback port. */
   explicit reusable_json_rpc_server(behavior selected_behavior)
      : _acceptor(_io, tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0))
      , _port(_acceptor.local_endpoint().port())
      , _behavior(selected_behavior)
      , _worker([this] { serve(); }) {}

   reusable_json_rpc_server(const reusable_json_rpc_server&) = delete;
   reusable_json_rpc_server& operator=(const reusable_json_rpc_server&) = delete;

   /** Stop a blocked accept and join the endpoint worker. */
   ~reusable_json_rpc_server() {
      _stop = true;
      boost::system::error_code error;
      _acceptor.close(error);
      unblock_accept();
      if (_worker.joinable())
         _worker.join();
   }

   /** Return the TCP port assigned by the OS. */
   uint16_t port() const { return _port; }

   /** Return how many TCP connections the endpoint accepted. */
   size_t connection_count() const { return _connection_count.load(); }

   /** Return how many complete JSON-RPC requests the endpoint received. */
   size_t request_count() const { return _request_count.load(); }

private:
   /** Connect once so a blocked accept can observe shutdown. */
   void unblock_accept() {
      boost::asio::io_context io;
      tcp::socket socket(io);
      boost::system::error_code error;
      socket.connect(
         tcp::endpoint(
            boost::asio::ip::make_address("127.0.0.1"),
            _port),
         error);
   }

   /** Accept one connection unless shutdown has started. */
   std::optional<tcp::socket> accept_connection() {
      tcp::socket socket(_io);
      boost::system::error_code error;
      _acceptor.accept(socket, error);
      if (error || _stop.load())
         return std::nullopt;
      _connection_count.fetch_add(1);
      return socket;
   }

   /** Read one complete request and send the matching JSON-RPC response. */
   bool serve_request(
      tcp::socket& socket,
      int64_t response_id,
      std::string_view result,
      bool keep_alive) {
      boost::beast::flat_buffer request_buffer;
      boost::beast::http::request<boost::beast::http::string_body> request;
      boost::system::error_code error;
      boost::beast::http::read(
         socket,
         request_buffer,
         request,
         error);
      if (error)
         return false;
      _request_count.fetch_add(1);

      boost::beast::http::response<boost::beast::http::string_body> response{
         boost::beast::http::status::ok,
         11};
      response.set(
         boost::beast::http::field::content_type,
         "application/json");
      response.keep_alive(keep_alive);
      response.body() =
         "{\"jsonrpc\":\"2.0\",\"id\":" +
         std::to_string(response_id) +
         ",\"result\":\"" +
         std::string(result) +
         "\"}";
      response.prepare_payload();
      boost::beast::http::write(socket, response, error);
      return !error;
   }

   /** Execute the selected two-call connection script. */
   void serve() {
      auto first = accept_connection();
      if (!first)
         return;
      if (!serve_request(*first, 1, "first", true))
         return;

      if (_behavior == behavior::healthy_keep_alive) {
         (void)serve_request(*first, 2, "second", false);
         return;
      }

      boost::system::error_code error;
      first->shutdown(tcp::socket::shutdown_both, error);
      first->close(error);
      auto second = accept_connection();
      if (!second)
         return;
      (void)serve_request(*second, 2, "second", false);
   }

   boost::asio::io_context _io;
   tcp::acceptor _acceptor;
   uint16_t _port;
   behavior _behavior;
   std::atomic_bool _stop{false};
   std::atomic_size_t _connection_count{0};
   std::atomic_size_t _request_count{0};
   std::thread _worker;
};

/** One deterministic response consumed by continuation_json_rpc_server. */
struct continuation_json_rpc_response {
   /** Response behavior for one received request. */
   enum class kind { result, error, raw_body, close_connection };

   kind response_kind = kind::result;
   fc::variant payload;
   std::optional<int64_t> response_id;
   std::chrono::microseconds delay{0};
   bool keep_alive = true;
   bool reset_after_response = false;

   /** Return a successful JSON-RPC result. */
   static continuation_json_rpc_response result(fc::variant value) {
      return {.response_kind = kind::result, .payload = std::move(value)};
   }

   /** Return a successful result after a deterministic delay. */
   static continuation_json_rpc_response delayed_result(fc::variant value, std::chrono::microseconds delay) {
      return {.response_kind = kind::result, .payload = std::move(value), .delay = delay};
   }

   /** Return a JSON-RPC error object. */
   static continuation_json_rpc_response error(fc::variant value) {
      return {.response_kind = kind::error, .payload = std::move(value)};
   }

   /** Return a complete caller-supplied HTTP response body. */
   static continuation_json_rpc_response raw(std::string body) {
      return {.response_kind = kind::raw_body, .payload = fc::variant(std::move(body))};
   }

   /** Close the accepted connection without sending an HTTP response. */
   static continuation_json_rpc_response close() { return {.response_kind = kind::close_connection}; }
};

/**
 * Scripted JSON-RPC endpoint that records connection affinity per request.
 *
 * Every parsed request consumes one response. Successful and error responses
 * echo the request ID unless the script supplies an override.
 */
class continuation_json_rpc_server {
public:
   /** One request observed by the scripted endpoint. */
   struct request_record {
      std::string method;
      size_t connection;
   };

   /** Start the endpoint with a fixed response script. */
   explicit continuation_json_rpc_server(std::vector<continuation_json_rpc_response> responses)
      : _responses(std::move(responses))
      , _acceptor(_io, tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0))
      , _port(_acceptor.local_endpoint().port())
      , _worker([this] { serve(); }) {}

   continuation_json_rpc_server(const continuation_json_rpc_server&) = delete;
   continuation_json_rpc_server& operator=(const continuation_json_rpc_server&) = delete;

   /** Stop any blocked socket operation and join the worker. */
   ~continuation_json_rpc_server() {
      _stop = true;
      boost::system::error_code error;
      _acceptor.close(error);
      {
         std::scoped_lock lock(_socket_mutex);
         if (_active_socket) {
            _active_socket->cancel(error);
            _active_socket->close(error);
         }
      }
      unblock_accept();
      if (_worker.joinable())
         _worker.join();
   }

   /** Return the loopback URL selected for this endpoint. */
   std::string url() const { return "http://127.0.0.1:" + std::to_string(_port); }

   /** Return how many TCP connections accepted a scripted request. */
   size_t connection_count() const { return _connection_count.load(); }

   /** Return the requests observed so far. */
   std::vector<request_record> requests() const {
      std::scoped_lock lock(_records_mutex);
      return _requests;
   }

private:
   /** Connect once so a synchronous accept can observe teardown. */
   void unblock_accept() {
      boost::asio::io_context io;
      tcp::socket socket(io);
      boost::system::error_code error;
      socket.connect(tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), _port), error);
   }

   /** Return a JSON-RPC envelope for one scripted response. */
   static std::string response_body(const continuation_json_rpc_response& response, const fc::variant_object& request) {
      if (response.response_kind == continuation_json_rpc_response::kind::raw_body)
         return response.payload.as_string();

      fc::mutable_variant_object envelope;
      envelope("jsonrpc", "2.0")("id", response.response_id ? fc::variant(*response.response_id) : request["id"]);
      if (response.response_kind == continuation_json_rpc_response::kind::result)
         envelope("result", response.payload);
      else
         envelope("error", response.payload);
      return fc::json::to_string(fc::variant(envelope), fc::json::yield_function_t{});
   }

   /** Close one socket with a reset so the client retains a stale cached lease. */
   static void reset_socket(tcp::socket& socket) {
      boost::system::error_code error;
      socket.set_option(boost::asio::socket_base::linger(true, 0), error);
      socket.close(error);
   }

   /** Consume the response script across as many connections as required. */
   void serve() {
      size_t response_index = 0;
      while (!_stop.load() && response_index < _responses.size()) {
         auto socket = std::make_shared<tcp::socket>(_io);
         {
            std::scoped_lock lock(_socket_mutex);
            _active_socket = socket;
         }

         boost::system::error_code error;
         _acceptor.accept(*socket, error);
         if (error || _stop.load())
            break;
         const auto connection = _connection_count.fetch_add(1) + 1;
         boost::beast::flat_buffer request_buffer;

         while (!_stop.load() && response_index < _responses.size()) {
            boost::beast::http::request<boost::beast::http::string_body> request;
            boost::beast::http::read(*socket, request_buffer, request, error);
            if (error)
               break;

            fc::variant parsed;
            try {
               parsed = fc::json::from_string(request.body());
               const auto& object = parsed.get_object();
               std::scoped_lock lock(_records_mutex);
               _requests.push_back({.method = object["method"].as_string(), .connection = connection});
            } catch (...) {
               reset_socket(*socket);
               break;
            }

            const auto response = _responses[response_index++];
            if (response.response_kind == continuation_json_rpc_response::kind::close_connection) {
               reset_socket(*socket);
               break;
            }
            if (response.delay.count() > 0)
               std::this_thread::sleep_for(response.delay);
            if (_stop.load())
               break;

            boost::beast::http::response<boost::beast::http::string_body> http_response{boost::beast::http::status::ok,
                                                                                        request.version()};
            http_response.set(boost::beast::http::field::content_type, "application/json");
            http_response.body() = response_body(response, parsed.get_object());
            http_response.keep_alive(response.keep_alive);
            http_response.prepare_payload();
            boost::beast::http::write(*socket, http_response, error);
            if (error)
               break;
            if (response.reset_after_response) {
               reset_socket(*socket);
               break;
            }
            if (!response.keep_alive) {
               socket->close(error);
               break;
            }
         }
      }

      boost::system::error_code error;
      _acceptor.close(error);
      std::scoped_lock lock(_socket_mutex);
      _active_socket.reset();
   }

   std::vector<continuation_json_rpc_response> _responses;
   boost::asio::io_context _io;
   tcp::acceptor _acceptor;
   uint16_t _port;
   std::atomic_bool _stop{false};
   std::atomic_size_t _connection_count{0};
   mutable std::mutex _socket_mutex;
   std::shared_ptr<tcp::socket> _active_socket;
   mutable std::mutex _records_mutex;
   std::vector<request_record> _requests;
   std::thread _worker;
};

/** Return a JSON-RPC error object suitable for a scripted response. */
fc::variant json_rpc_error_object(int64_t code, std::string message) {
   return fc::variant(fc::mutable_variant_object()("code", code)("message", std::move(message)));
}

/**
 * Return true when the exception came from the transport response body limit.
 */
bool is_response_body_limit_error(const fc::exception& e) {
   return e.to_detail_string().find("response_limit") != std::string::npos;
}

} // namespace

BOOST_AUTO_TEST_SUITE(json_rpc_client_tests)

/// Legacy JSON-RPC clients retain startup DNS until a connection failure invalidates it.
BOOST_AUTO_TEST_CASE(default_endpoint_refresh_policy_is_preserved) {
   const fc::network::json_rpc::client_options options;

   BOOST_CHECK(!options.transport.dns_cache_timeout);
   BOOST_CHECK(
      options.transport.refresh_dns_on_connection_failure);
}

/// Explicitly idempotent calls reuse one healthy connection.
BOOST_AUTO_TEST_CASE(idempotent_calls_reuse_a_healthy_connection) {
   reusable_json_rpc_server server(
      reusable_json_rpc_server::behavior::healthy_keep_alive);
   fc::network::json_rpc::json_rpc_client client(
      fc::url(
         "http://127.0.0.1:" +
         std::to_string(server.port())));

   BOOST_CHECK_EQUAL(
      client.call_idempotent("wire_first_probe").as_string(),
      "first");
   BOOST_CHECK_EQUAL(
      client.call_idempotent("wire_second_probe").as_string(),
      "second");
   BOOST_CHECK_EQUAL(server.connection_count(), 1U);
   BOOST_CHECK_EQUAL(server.request_count(), 2U);
}

/// A result-aware JSON-RPC hook selects a follow-up on the exact connection.
BOOST_AUTO_TEST_CASE(continuation_hook_selects_same_connection_followup) {
   continuation_json_rpc_server server({
      continuation_json_rpc_response::result("first"),
      continuation_json_rpc_response::result("second"),
   });
   fc::network::json_rpc::json_rpc_client client(fc::url(server.url()));

   bool hook_called = false;
   const auto result = client.call_then("wire_identity_probe", fc::variants{},
                                        fc::network::json_rpc::call_options{
                                           .replay = fc::network::json_rpc::replay_policy::stale_reused_connection_once,
                                           .total_timeout_cap = fc::seconds(1),
                                        },
                                        [&](const fc::variant& first_result) {
                                           hook_called = true;
                                           BOOST_CHECK_EQUAL(first_result.as_string(), "first");
                                           return fc::network::json_rpc::continuation_call{
                                              .method = "wire_protected_operation",
                                              .options =
                                                 fc::network::json_rpc::follow_up_options{
                                                    .total_timeout_cap = fc::seconds(1),
                                                 },
                                           };
                                        });

   BOOST_CHECK(hook_called);
   BOOST_CHECK_EQUAL(result.as_string(), "second");
   const auto requests = server.requests();
   BOOST_REQUIRE_EQUAL(requests.size(), 2U);
   BOOST_CHECK_EQUAL(requests[0].connection, requests[1].connection);
}

/// An initial JSON-RPC error rejects the continuation before invoking its hook.
BOOST_AUTO_TEST_CASE(continuation_initial_json_rpc_error_prevents_hook) {
   continuation_json_rpc_server server({
      continuation_json_rpc_response::error(json_rpc_error_object(-32000, "probe failed")),
   });
   fc::network::json_rpc::json_rpc_client client(fc::url(server.url()));
   bool hook_called = false;

   BOOST_CHECK_THROW(client.call_then("wire_identity_probe", fc::variants{}, {},
                                      [&](const fc::variant&) {
                                         hook_called = true;
                                         return fc::network::json_rpc::continuation_call{.method =
                                                                                            "wire_protected_operation"};
                                      }),
                     fc::network::json_rpc::json_rpc_error);
   BOOST_CHECK(!hook_called);
   BOOST_CHECK_EQUAL(server.requests().size(), 1U);
}

/// A malformed initial envelope rejects the continuation before invoking its hook.
BOOST_AUTO_TEST_CASE(continuation_malformed_initial_envelope_prevents_hook) {
   continuation_json_rpc_server server({
      continuation_json_rpc_response::raw("{\"jsonrpc\":\"1.0\",\"id\":1,\"result\":\"first\"}"),
   });
   fc::network::json_rpc::json_rpc_client client(fc::url(server.url()));
   bool hook_called = false;

   BOOST_CHECK_THROW(client.call_then("wire_identity_probe", fc::variants{}, {},
                                      [&](const fc::variant&) {
                                         hook_called = true;
                                         return fc::network::json_rpc::continuation_call{.method =
                                                                                            "wire_protected_operation"};
                                      }),
                     fc::exception);
   BOOST_CHECK(!hook_called);
   BOOST_CHECK_EQUAL(server.requests().size(), 1U);
}

/// A mismatched initial response ID rejects the continuation before invoking its hook.
BOOST_AUTO_TEST_CASE(continuation_wrong_initial_id_prevents_hook) {
   auto response = continuation_json_rpc_response::result("first");
   response.response_id = 99;
   continuation_json_rpc_server server({std::move(response)});
   fc::network::json_rpc::json_rpc_client client(fc::url(server.url()));
   bool hook_called = false;

   BOOST_CHECK_THROW(client.call_then("wire_identity_probe", fc::variants{}, {},
                                      [&](const fc::variant&) {
                                         hook_called = true;
                                         return fc::network::json_rpc::continuation_call{.method =
                                                                                            "wire_protected_operation"};
                                      }),
                     fc::exception);
   BOOST_CHECK(!hook_called);
   BOOST_CHECK_EQUAL(server.requests().size(), 1U);
}

/// A rejecting hook closes the retained connection without sending a follow-up.
BOOST_AUTO_TEST_CASE(continuation_hook_rejection_prevents_followup) {
   continuation_json_rpc_server server({
      continuation_json_rpc_response::result("first"),
   });
   fc::network::json_rpc::json_rpc_client client(fc::url(server.url()));

   BOOST_CHECK_THROW(client.call_then("wire_identity_probe", fc::variants{}, {},
                                      [](const fc::variant&) -> fc::network::json_rpc::continuation_call {
                                         FC_THROW("identity rejected");
                                      }),
                     fc::exception);
   BOOST_CHECK_EQUAL(server.requests().size(), 1U);
}

/// A JSON-RPC error from the follow-up is returned to the caller.
BOOST_AUTO_TEST_CASE(continuation_followup_json_rpc_error_is_reported) {
   continuation_json_rpc_server server({
      continuation_json_rpc_response::result("first"),
      continuation_json_rpc_response::error(json_rpc_error_object(-32001, "operation failed")),
   });
   fc::network::json_rpc::json_rpc_client client(fc::url(server.url()));

   BOOST_CHECK_THROW(client.call_then("wire_identity_probe", fc::variants{}, {},
                                      [](const fc::variant&) {
                                         return fc::network::json_rpc::continuation_call{.method =
                                                                                            "wire_protected_operation"};
                                      }),
                     fc::network::json_rpc::json_rpc_error);
   BOOST_CHECK_EQUAL(server.requests().size(), 2U);
}

/// A mismatched follow-up response ID is rejected.
BOOST_AUTO_TEST_CASE(continuation_wrong_followup_id_is_rejected) {
   auto wrong_id = continuation_json_rpc_response::result("second");
   wrong_id.response_id = 99;
   continuation_json_rpc_server server({
      continuation_json_rpc_response::result("first"),
      std::move(wrong_id),
   });
   fc::network::json_rpc::json_rpc_client client(fc::url(server.url()));

   BOOST_CHECK_THROW(client.call_then("wire_identity_probe", fc::variants{}, {},
                                      [](const fc::variant&) {
                                         return fc::network::json_rpc::continuation_call{.method =
                                                                                            "wire_protected_operation"};
                                      }),
                     fc::exception);
   BOOST_CHECK_EQUAL(server.requests().size(), 2U);
}

/// A stale cached first connection replays only the probe and retains the replacement for the follow-up.
BOOST_AUTO_TEST_CASE(continuation_replays_stale_initial_connection_once) {
   auto stale = continuation_json_rpc_response::result("warm");
   stale.reset_after_response = true;
   continuation_json_rpc_server server({
      std::move(stale),
      continuation_json_rpc_response::result("first"),
      continuation_json_rpc_response::result("second"),
   });
   fc::network::json_rpc::json_rpc_client client(fc::url(server.url()));

   BOOST_CHECK_EQUAL(client.call_idempotent("wire_warm_connection").as_string(), "warm");
   const auto result =
      client.call_then("wire_identity_probe", fc::variants{},
                       fc::network::json_rpc::call_options{
                          .replay = fc::network::json_rpc::replay_policy::stale_reused_connection_once,
                       },
                       [](const fc::variant&) {
                          return fc::network::json_rpc::continuation_call{.method = "wire_protected_operation"};
                       });

   BOOST_CHECK_EQUAL(result.as_string(), "second");
   BOOST_CHECK_EQUAL(server.connection_count(), 2U);
   const auto requests = server.requests();
   BOOST_REQUIRE_EQUAL(requests.size(), 3U);
   BOOST_CHECK_NE(requests[0].connection, requests[1].connection);
   BOOST_CHECK_EQUAL(requests[1].connection, requests[2].connection);
}

/// A non-replaying continuation probe fails instead of replacing a stale cached connection.
BOOST_AUTO_TEST_CASE(continuation_never_policy_does_not_replay_stale_initial_connection) {
   auto stale = continuation_json_rpc_response::result("warm");
   stale.reset_after_response = true;
   continuation_json_rpc_server server({std::move(stale)});
   fc::network::json_rpc::json_rpc_client client(fc::url(server.url()));
   bool hook_called = false;

   BOOST_CHECK_EQUAL(client.call_idempotent("wire_warm_connection").as_string(), "warm");
   BOOST_CHECK_THROW(client.call_then("wire_identity_probe", fc::variants{}, {},
                                      [&](const fc::variant&) {
                                         hook_called = true;
                                         return fc::network::json_rpc::continuation_call{.method =
                                                                                            "wire_protected_operation"};
                                      }),
                     fc::exception);
   BOOST_CHECK(!hook_called);
   BOOST_CHECK_EQUAL(server.connection_count(), 1U);
   BOOST_CHECK_EQUAL(server.requests().size(), 1U);
}

/// A total-timeout cap preserves stricter configured phase deadlines.
BOOST_AUTO_TEST_CASE(continuation_timeout_cap_preserves_stricter_phase_timeout) {
   continuation_json_rpc_server server({
      continuation_json_rpc_response::delayed_result("first", 250ms),
   });
   fc::network::json_rpc::client_options options;
   options.request.timeouts.header = fc::milliseconds(40);
   options.request.timeouts.total = fc::seconds(2);
   fc::network::json_rpc::json_rpc_client client(fc::url(server.url()), std::nullopt,
                                                 fc::network::json_rpc::endpoint_refresh_policy::on_connection_failure,
                                                 std::move(options));
   bool hook_called = false;

   const auto start = fc::time_point::now();
   BOOST_CHECK_THROW(client.call_then("wire_identity_probe", fc::variants{},
                                      fc::network::json_rpc::call_options{.total_timeout_cap = fc::milliseconds(500)},
                                      [&](const fc::variant&) {
                                         hook_called = true;
                                         return fc::network::json_rpc::continuation_call{.method =
                                                                                            "wire_protected_operation"};
                                      }),
                     fc::timeout_exception);
   const auto elapsed = fc::time_point::now() - start;

   BOOST_CHECK(!hook_called);
   BOOST_CHECK_LT(elapsed.count(), fc::milliseconds(200).count());
}

/// A per-call cap cannot lengthen a stricter base total timeout.
BOOST_AUTO_TEST_CASE(continuation_timeout_cap_cannot_expand_base_total_timeout) {
   continuation_json_rpc_server server({
      continuation_json_rpc_response::delayed_result("first", 250ms),
   });
   fc::network::json_rpc::client_options options;
   options.request.timeouts.header = fc::seconds(1);
   options.request.timeouts.total = fc::milliseconds(40);
   fc::network::json_rpc::json_rpc_client client(fc::url(server.url()), std::nullopt,
                                                 fc::network::json_rpc::endpoint_refresh_policy::on_connection_failure,
                                                 std::move(options));
   bool hook_called = false;

   const auto start = fc::time_point::now();
   BOOST_CHECK_THROW(client.call_then("wire_identity_probe", fc::variants{},
                                      fc::network::json_rpc::call_options{.total_timeout_cap = fc::milliseconds(500)},
                                      [&](const fc::variant&) {
                                         hook_called = true;
                                         return fc::network::json_rpc::continuation_call{.method =
                                                                                            "wire_protected_operation"};
                                      }),
                     fc::timeout_exception);
   const auto elapsed = fc::time_point::now() - start;

   BOOST_CHECK(!hook_called);
   BOOST_CHECK_LT(elapsed.count(), fc::milliseconds(200).count());
}

/// The follow-up receives a fresh total-timeout budget independent of the probe.
BOOST_AUTO_TEST_CASE(continuation_followup_has_independent_total_timeout) {
   continuation_json_rpc_server server({
      continuation_json_rpc_response::result("first"),
      continuation_json_rpc_response::delayed_result("second", 150ms),
   });
   fc::network::json_rpc::json_rpc_client client(fc::url(server.url()));

   const auto result = client.call_then("wire_identity_probe", fc::variants{},
                                        fc::network::json_rpc::call_options{.total_timeout_cap = fc::milliseconds(50)},
                                        [](const fc::variant&) {
                                           return fc::network::json_rpc::continuation_call{
                                              .method = "wire_protected_operation",
                                              .options =
                                                 fc::network::json_rpc::follow_up_options{
                                                    .total_timeout_cap = fc::milliseconds(500),
                                                 },
                                           };
                                        });

   BOOST_CHECK_EQUAL(result.as_string(), "second");
}

/// An idempotent call retries once when its cached connection has gone stale.
BOOST_AUTO_TEST_CASE(idempotent_call_recovers_from_a_stale_cached_connection) {
   reusable_json_rpc_server server(reusable_json_rpc_server::behavior::stale_after_first_response);
   fc::network::json_rpc::json_rpc_client client(fc::url("http://127.0.0.1:" + std::to_string(server.port())));

   BOOST_CHECK_EQUAL(
      client.call_idempotent("wire_first_probe").as_string(),
      "first");
   BOOST_CHECK_EQUAL(
      client.call_idempotent("wire_second_probe").as_string(),
      "second");
   BOOST_CHECK_EQUAL(server.connection_count(), 2U);
   BOOST_CHECK_EQUAL(server.request_count(), 2U);
}

/// Caller-supplied retry options cannot make a default call replay.
BOOST_AUTO_TEST_CASE(default_call_enforces_single_attempt) {
   auto stale =
      continuation_json_rpc_response::result("warm");
   stale.reset_after_response = true;
   continuation_json_rpc_server server({
      std::move(stale),
      continuation_json_rpc_response::close(),
      continuation_json_rpc_response::result("replayed"),
   });
   fc::network::json_rpc::client_options options;
   options.request.retry.max_attempts = 3;
   options.request.retry.initial_backoff = fc::microseconds(0);
   options.request.retry.max_backoff = fc::microseconds(0);
   options.request.retry.allow_retry =
      [](const fc::http::retry_context& context) {
         return context.reused_connection;
      };
   options.request.idempotent = true;
   fc::network::json_rpc::json_rpc_client client(
      fc::url(server.url()),
      std::nullopt,
      fc::network::json_rpc::endpoint_refresh_policy::on_connection_failure,
      std::move(options));

   BOOST_CHECK_EQUAL(
      client.call_idempotent("wire_first_probe").as_string(),
      "warm");
   BOOST_CHECK_THROW(
      client.call("wire_side_effect_probe"),
      fc::exception);
   BOOST_CHECK_EQUAL(server.connection_count(), 2U);
   BOOST_CHECK_EQUAL(server.requests().size(), 2U);
}

/// URL parsing preserves bracketed IPv6 identity, credentials, path, query, and port.
BOOST_AUTO_TEST_CASE(url_round_trips_ipv6_authority_and_query) {
   const fc::url parsed("https://operator:secret@[2001:db8::1]:8443/rpc?commitment=finalized");

   BOOST_REQUIRE(parsed.host());
   BOOST_CHECK_EQUAL(*parsed.host(), "2001:db8::1");
   BOOST_REQUIRE(parsed.port());
   BOOST_CHECK_EQUAL(*parsed.port(), 8443U);
   BOOST_REQUIRE(parsed.path());
   BOOST_CHECK_EQUAL(parsed.path()->generic_string(), "/rpc");
   BOOST_REQUIRE(parsed.query());
   BOOST_CHECK_EQUAL(*parsed.query(), "commitment=finalized");
   BOOST_CHECK_EQUAL(
      static_cast<std::string>(parsed),
      "https://operator:secret@[2001:db8::1]:8443/rpc?commitment=finalized");
}

/// Diagnostic endpoint labels omit URL credentials, paths, and queries.
BOOST_AUTO_TEST_CASE(endpoint_diagnostics_are_credential_free) {
   const fc::url endpoint(
      "https://operator:secret@[2001:db8::1]:8443/"
      "private/token?authorization=hidden");

   const auto sanitized =
      fc::http::sanitized_endpoint(endpoint);
   BOOST_CHECK_EQUAL(
      sanitized,
      "https://[2001:db8::1]:8443");
   BOOST_CHECK(
      sanitized.find("operator") == std::string::npos);
   BOOST_CHECK(
      sanitized.find("secret") == std::string::npos);
   BOOST_CHECK(
      sanitized.find("authorization") == std::string::npos);
}

/// A peer that accepts the TCP request but withholds the HTTP response must
/// release the caller within the active RPC deadline.
BOOST_AUTO_TEST_CASE(call_times_out_when_http_response_hangs) {
   hanging_http_server server;
   fc::network::json_rpc::json_rpc_client client(
      fc::url("http://127.0.0.1:" + std::to_string(server.port())));

   const auto start = fc::time_point::now();
   BOOST_CHECK_THROW(
      [&] {
         fc::task::deadline_scope deadline(fc::time_point::now() + fc::milliseconds(200));
         client.call("wire_deadline_probe");
      }(),
      fc::timeout_exception);
   const auto elapsed = fc::time_point::now() - start;

   BOOST_CHECK_LT(elapsed.count(), 1500 * 1000);
}

/// An already-expired ambient deadline must fail before network I/O starts.
BOOST_AUTO_TEST_CASE(call_rejects_expired_ambient_deadline) {
   fc::network::json_rpc::json_rpc_client client(fc::url("http://localhost:9876"));

   BOOST_CHECK_THROW(
      [&] {
         fc::task::deadline_scope deadline(fc::time_point::now() - fc::milliseconds(1));
         client.call("wire_expired_deadline_probe");
      }(),
      fc::timeout_exception);
}

/// A peer that completes HTTP 200 with an oversized body must be rejected by
/// the transport parser before JSON parsing or outpost envelope decoding.
BOOST_AUTO_TEST_CASE(call_rejects_oversized_response_body) {
   fixed_response_http_server server(std::string(OVERSIZED_RESPONSE_BODY_BYTES, 'x'));
   fc::network::json_rpc::json_rpc_client client(
      fc::url("http://127.0.0.1:" + std::to_string(server.port())));

   BOOST_CHECK_EXCEPTION(
      client.call("wire_body_limit_probe"),
      fc::exception,
      is_response_body_limit_error);
}

/// The raw HTTP helper shares the same bounded transport path as JSON-RPC calls.
BOOST_AUTO_TEST_CASE(send_http_rejects_oversized_response_body) {
   fixed_response_http_server server(std::string(OVERSIZED_RESPONSE_BODY_BYTES, 'x'));
   fc::network::json_rpc::json_rpc_client client(
      fc::url("http://127.0.0.1:" + std::to_string(server.port())));

   BOOST_CHECK_EXCEPTION(
      client.send_http(fc::network::json_rpc::http_verb::GET, "/"),
      fc::exception,
      is_response_body_limit_error);
}

BOOST_AUTO_TEST_SUITE_END()

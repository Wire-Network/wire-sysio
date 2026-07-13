/**
 * @file test_json_rpc_client.cpp
 * @brief Regression tests for deadline-bound JSON-RPC transport calls.
 */

#include <fc/network/json_rpc/json_rpc_client.hpp>
#include <fc/task/deadline.hpp>

#include <boost/asio.hpp>
#include <boost/test/unit_test.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

using namespace std::chrono_literals;

namespace {

using tcp = boost::asio::ip::tcp;

constexpr size_t OVERSIZED_RESPONSE_BODY_BYTES = 2 * 1024 * 1024;
constexpr int64_t STALLED_RESOLVE_TIMEOUT_MS = 200;
constexpr int64_t STALLED_RESOLVE_MAX_ELAPSED_MS = 1'500;
constexpr int64_t CONSUMED_RESOLVE_TIMEOUT_MS = 20;
constexpr int64_t CONSUMED_RESOLVE_DELAY_MS = 40;
constexpr int64_t LOCAL_RESOLVE_TIMEOUT_MS = 1'000;
constexpr std::string_view STALLED_RESOLVE_HOST = "startup-resolve.invalid";
constexpr uint16_t RESOLVE_TEST_PORT = 9'876;

/** Return the non-routable URL used by injected resolver tests. */
fc::url resolve_test_url() {
   return fc::url("http://" + std::string(STALLED_RESOLVE_HOST) + ":" + std::to_string(RESOLVE_TEST_PORT));
}

/**
 * Return a loopback port that has no listener after this function returns.
 */
uint16_t closed_loopback_port() {
   boost::asio::io_context io;
   tcp::acceptor acceptor(io, tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0));
   const auto port = acceptor.local_endpoint().port();
   boost::system::error_code ec;
   acceptor.close(ec);
   return port;
}

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
 * Return true when the exception came from the transport response body limit.
 */
bool is_response_body_limit_error(const fc::exception& e) {
   return e.to_detail_string().find("response body limit") != std::string::npos;
}

/**
 * Return true when the exception came from deadline-bound DNS resolution.
 */
bool is_resolve_timeout_error(const fc::exception& e) {
   return e.to_detail_string().find("JSON-RPC resolve timed out") != std::string::npos;
}

} // namespace

namespace fc::network::json_rpc {

/** Private-constructor access for DNS transport regression tests. */
struct json_rpc_client_test_access {
   /** Construct a client using a caller-supplied asynchronous resolver starter. */
   template <typename ResolverStartFn>
   static json_rpc_client create(fc::url url, ResolverStartFn&& resolver_start) {
      return json_rpc_client(
         std::move(url), std::nullopt, endpoint_refresh_policy::never,
         json_rpc_client::resolver_start_fn(std::forward<ResolverStartFn>(resolver_start)));
   }
};

} // namespace fc::network::json_rpc

BOOST_AUTO_TEST_SUITE(json_rpc_client_tests)

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

/// Initial endpoint resolution must cancel an incomplete platform lookup at the caller's startup deadline.
BOOST_AUTO_TEST_CASE(initial_endpoint_resolution_cancels_stalled_query) {
   std::atomic_bool resolver_started{false};
   std::atomic_bool cancel_called{false};
   std::function<void(const boost::system::error_code&, tcp::resolver::results_type)> late_completion;
   auto stalled_resolver = [&](const std::string&, const std::string&, fc::time_point, auto complete) {
      resolver_started = true;
      late_completion = std::move(complete);
      return [&] { cancel_called = true; };
   };
   const auto start = fc::time_point::now();

   BOOST_CHECK_EXCEPTION(
      [&] {
         fc::task::deadline_scope deadline(fc::time_point::now() + fc::milliseconds(STALLED_RESOLVE_TIMEOUT_MS));
         auto client =
            fc::network::json_rpc::json_rpc_client_test_access::create(resolve_test_url(), stalled_resolver);
         (void)client;
      }(),
      fc::timeout_exception,
      is_resolve_timeout_error);

   const auto elapsed = fc::time_point::now() - start;
   BOOST_CHECK(resolver_started.load());
   BOOST_CHECK(cancel_called.load());
   BOOST_CHECK_LT(elapsed.count(), STALLED_RESOLVE_MAX_ELAPSED_MS * 1'000);

   BOOST_REQUIRE(late_completion);
   late_completion(boost::asio::error::operation_aborted, {});
}

/// An expired startup budget must fail before starting a platform resolver operation.
BOOST_AUTO_TEST_CASE(initial_endpoint_resolution_rejects_expired_deadline) {
   bool resolver_started = false;
   auto resolver = [&](const std::string&, const std::string&, fc::time_point, auto) {
      resolver_started = true;
      return [] {};
   };

   BOOST_CHECK_EXCEPTION(
      [&] {
         fc::task::deadline_scope deadline(fc::time_point::now() - fc::milliseconds(1));
         auto client = fc::network::json_rpc::json_rpc_client_test_access::create(resolve_test_url(), resolver);
         (void)client;
      }(),
      fc::timeout_exception,
      is_resolve_timeout_error);

   BOOST_CHECK(!resolver_started);
}

/// A deadline-bound lookup must preserve a successful asynchronous resolver result.
BOOST_AUTO_TEST_CASE(initial_endpoint_resolution_accepts_completed_lookup) {
   bool resolver_started = false;
   auto resolver = [&](const std::string& host, const std::string& port, fc::time_point, auto complete) {
      resolver_started = true;
      const std::array endpoints{tcp::endpoint(boost::asio::ip::address_v4::loopback(), RESOLVE_TEST_PORT)};
      complete(boost::system::error_code{},
               tcp::resolver::results_type::create(endpoints.begin(), endpoints.end(), host, port));
      return [] {};
   };

   fc::task::deadline_scope deadline(fc::time_point::now() + fc::milliseconds(STALLED_RESOLVE_TIMEOUT_MS));
   auto client = fc::network::json_rpc::json_rpc_client_test_access::create(resolve_test_url(), resolver);
   (void)client;

   BOOST_CHECK(resolver_started);
}

/// Cancellation must remain armed when resolver startup itself consumes the remaining deadline.
BOOST_AUTO_TEST_CASE(initial_endpoint_resolution_cancels_after_starter_consumes_deadline) {
   bool cancel_called = false;
   auto resolver = [&](const std::string&, const std::string&, fc::time_point, auto) {
      std::this_thread::sleep_for(std::chrono::milliseconds(CONSUMED_RESOLVE_DELAY_MS));
      return [&] { cancel_called = true; };
   };

   BOOST_CHECK_EXCEPTION(
      [&] {
         fc::task::deadline_scope deadline(
            fc::time_point::now() + fc::milliseconds(CONSUMED_RESOLVE_TIMEOUT_MS));
         auto client = fc::network::json_rpc::json_rpc_client_test_access::create(resolve_test_url(), resolver);
         (void)client;
      }(),
      fc::timeout_exception,
      is_resolve_timeout_error);

   BOOST_CHECK(cancel_called);
}

/// The production Boost.Asio resolver path must complete a local hostname lookup under a deadline.
BOOST_AUTO_TEST_CASE(initial_endpoint_resolution_resolves_localhost_with_deadline) {
   fc::task::deadline_scope deadline(fc::time_point::now() + fc::milliseconds(LOCAL_RESOLVE_TIMEOUT_MS));
   fc::network::json_rpc::json_rpc_client client(
      fc::url("http://localhost:" + std::to_string(RESOLVE_TEST_PORT)), std::nullopt,
      fc::network::json_rpc::endpoint_refresh_policy::never);
   (void)client;
}

/// Refreshing stale cached endpoints must observe the active RPC deadline.
BOOST_AUTO_TEST_CASE(stale_endpoint_refresh_respects_expired_deadline) {
   fc::network::json_rpc::json_rpc_client client(
      fc::url("http://127.0.0.1:" + std::to_string(closed_loopback_port())));

   BOOST_CHECK_THROW(client.call("wire_stale_cache_probe"), fc::exception);

   BOOST_CHECK_EXCEPTION(
      [&] {
         fc::task::deadline_scope deadline(fc::time_point::now() - fc::milliseconds(1));
         client.call("wire_stale_cache_probe");
      }(),
      fc::timeout_exception,
      is_resolve_timeout_error);
}

/// Callers that require bounded runtime behavior can retain the startup DNS result
/// instead of re-entering platform DNS after a connection failure.
BOOST_AUTO_TEST_CASE(endpoint_refresh_can_be_disabled) {
   fc::network::json_rpc::json_rpc_client client(fc::url("http://localhost:" + std::to_string(closed_loopback_port())),
                                                 std::nullopt, fc::network::json_rpc::endpoint_refresh_policy::never);

   BOOST_CHECK_THROW(client.call("wire_cached_endpoint_probe"), fc::exception);

   BOOST_CHECK_EXCEPTION(
      [&] {
         fc::task::deadline_scope deadline(fc::time_point::now() - fc::milliseconds(1));
         client.call("wire_cached_endpoint_probe");
      }(),
      fc::timeout_exception,
      [](const fc::exception& e) {
         return e.to_detail_string().find("JSON-RPC connect timed out") != std::string::npos;
      });
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

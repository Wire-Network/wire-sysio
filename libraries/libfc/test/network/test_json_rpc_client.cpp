/**
 * @file test_json_rpc_client.cpp
 * @brief Regression tests for deadline-bound JSON-RPC transport calls.
 */

#include <fc/network/json_rpc/json_rpc_client.hpp>
#include <fc/task/deadline.hpp>

#include <boost/asio.hpp>
#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using namespace std::chrono_literals;

namespace {

using tcp = boost::asio::ip::tcp;

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

} // namespace

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

BOOST_AUTO_TEST_SUITE_END()

#pragma once

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <fc/io/json.hpp>

#include <cstdint>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

namespace fc::test {

/**
 * Minimal one-request loopback HTTP server for synchronous RPC client tests.
 *
 * The caller supplies the complete response body. The server accepts one
 * request, returns HTTP 200 with that body, and closes the connection. Its
 * destructor also unblocks an unused accept so tests that fail before making
 * the expected request do not strand a worker thread.
 */
class one_shot_http_server {
public:
   explicit one_shot_http_server(std::string response_body, std::string expected_rpc_method = {})
      : _response_body(std::move(response_body))
      , _expected_rpc_method(std::move(expected_rpc_method))
      , _acceptor(_io, tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0))
      , _port(_acceptor.local_endpoint().port())
      , _worker([this] { serve(); }) {}

   one_shot_http_server(const one_shot_http_server&) = delete;
   one_shot_http_server& operator=(const one_shot_http_server&) = delete;

   ~one_shot_http_server() {
      // Wake a worker blocked in synchronous accept without concurrently
      // operating on the acceptor from two threads. The temporary socket is
      // closed immediately, so a worker that accepts it exits its HTTP read.
      boost::system::error_code error;
      boost::asio::io_context io;
      tcp::socket socket(io);
      socket.connect(tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), _port), error);
      socket.close(error);
      if (_worker.joinable()) {
         _worker.join();
      }
      _acceptor.close(error);
   }

   /** Return the loopback URL selected for this server. */
   std::string url() const {
      return "http://127.0.0.1:" + std::to_string(_port);
   }

private:
   using tcp = boost::asio::ip::tcp;

   void serve() {
      boost::system::error_code error;
      tcp::socket socket(_io);
      _acceptor.accept(socket, error);
      if (error) {
         return;
      }

      boost::beast::flat_buffer request_buffer;
      boost::beast::http::request<boost::beast::http::string_body> request;
      boost::beast::http::read(socket, request_buffer, request, error);
      if (error) {
         return;
      }

      if (!_expected_rpc_method.empty()) {
         bool request_matches = false;
         try {
            const auto request_object = fc::json::from_string(request.body()).get_object();
            request_matches =
               request_object.contains("method") &&
               request_object["method"].as_string() == _expected_rpc_method &&
               request_object.contains("params") &&
               request_object["params"].is_array() &&
               request_object["params"].get_array().empty();
         } catch (...) {
            return;
         }
         if (!request_matches) {
            return;
         }
      }

      std::ostringstream response;
      response << "HTTP/1.1 200 OK\r\n"
               << "Content-Type: application/json\r\n"
               << "Content-Length: " << _response_body.size() << "\r\n"
               << "Connection: close\r\n\r\n"
               << _response_body;
      const auto response_text = response.str();
      boost::asio::write(socket, boost::asio::buffer(response_text), error);
   }

   std::string             _response_body;
   std::string             _expected_rpc_method;
   boost::asio::io_context _io;
   tcp::acceptor           _acceptor;
   uint16_t                _port;
   std::thread             _worker;
};

/**
 * Loopback server that deterministically accepts one connection and closes it.
 *
 * Unlike selecting and releasing an unused port, this fixture keeps ownership
 * of the port until the client connects, so another process cannot race the
 * test and unexpectedly provide a working endpoint.
 */
class connection_closing_http_server {
public:
   connection_closing_http_server()
      : _acceptor(_io, tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0))
      , _port(_acceptor.local_endpoint().port())
      , _worker([this] { close_connection(); }) {}

   connection_closing_http_server(const connection_closing_http_server&) = delete;
   connection_closing_http_server& operator=(const connection_closing_http_server&) = delete;

   ~connection_closing_http_server() {
      boost::system::error_code error;
      boost::asio::io_context io;
      tcp::socket socket(io);
      socket.connect(tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), _port), error);
      socket.close(error);
      if (_worker.joinable()) {
         _worker.join();
      }
      _acceptor.close(error);
   }

   std::string url() const {
      return "http://127.0.0.1:" + std::to_string(_port);
   }

private:
   using tcp = boost::asio::ip::tcp;

   void close_connection() {
      boost::system::error_code error;
      tcp::socket socket(_io);
      _acceptor.accept(socket, error);
      if (error) {
         return;
      }
      socket.set_option(boost::asio::socket_base::linger(true, 0), error);
      socket.close(error);
   }

   boost::asio::io_context _io;
   tcp::acceptor           _acceptor;
   uint16_t                _port;
   std::thread             _worker;
};

} // namespace fc::test

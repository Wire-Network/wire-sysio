#pragma once

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <atomic>
#include <mutex>
#include <memory>
#include <test_ports.hpp>
#include <thread>
#include <vector>

namespace startup_test {

/** Consume one node-HTTP port from this test process's registry lease. */
inline uint16_t next_port() {
   static std::mutex mutex;
   static uint32_t port_index = 0;
   std::lock_guard lock(mutex);
   return sysio::testing::get_port(sysio::testing::port_category::node_http, port_index++);
}

/** Return an unused, registry-owned endpoint, including for refusal and fake-client fixtures. */
inline std::string unused_url() { return "http://127.0.0.1:" + std::to_string(next_port()); }

/** Controllable RPC endpoint: a hung request keeps its socket alive until teardown. */
class rpc_server {
   using tcp = boost::asio::ip::tcp;
   struct session {
      tcp::socket socket;
      boost::beast::flat_buffer buffer;
      boost::beast::http::request<boost::beast::http::string_body> request;
      boost::beast::http::response<boost::beast::http::string_body> response;
      explicit session(tcp::socket socket) : socket(std::move(socket)) {}
   };
   boost::asio::io_context io;
   tcp::acceptor acceptor;
   std::vector<std::shared_ptr<session>> hanging;
   std::thread worker;
   std::string result;

   /** Accept and serve asynchronously so shutdown also interrupts half-read requests. */
   void accept() {
      acceptor.async_accept([this](auto error, tcp::socket socket) {
         if (error) return;
         accept();
         auto peer = std::make_shared<session>(std::move(socket));
         boost::beast::http::async_read(peer->socket, peer->buffer, peer->request,
            [this, peer](auto read_error, size_t) {
               if (read_error) return;
               ++requests;
               if (hang.load()) { hanging.push_back(peer); return; }
               peer->response.result(boost::beast::http::status::ok);
               peer->response.set(boost::beast::http::field::content_type, "application/json");
               peer->response.body() = "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":" + result + "}";
               peer->response.prepare_payload();
               boost::beast::http::async_write(peer->socket, peer->response,
                  [peer](auto, size_t) {});
            });
      });
   }

public:
   std::atomic<unsigned> requests{0};
   std::atomic<bool> hang{false};

   /** Bind a registry-issued port; supplying the same port supports delayed service startup. */
   explicit rpc_server(std::string result = "\"0x7a69\"", uint16_t port = next_port())
      : acceptor(io, tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port))
      , result(std::move(result)) {
      accept();
      worker = std::thread([this] { io.run(); });
   }
   /** Stop pending accepts, reads, and deliberate hangs without network wakeups. */
   ~rpc_server() { io.stop(); worker.join(); }
   /** Return the endpoint owned by this fixture. */
   std::string url() const { return "http://127.0.0.1:" + std::to_string(acceptor.local_endpoint().port()); }
};
} // namespace startup_test

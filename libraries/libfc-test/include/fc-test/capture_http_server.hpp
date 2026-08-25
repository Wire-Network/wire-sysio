#pragma once

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace fc::test {

/**
 * Multi-request loopback HTTP server that records every request and replays
 * scripted responses.
 *
 * The accept loop runs until destruction; each connection serves one request and
 * closes (`Connection: close`, which the fc::http transport honors, so every
 * client request arrives on a fresh connection). Responses replay the supplied
 * script in order; once the script is exhausted every further request receives
 * the default response (an empty successful bulk body). Requests are recorded --
 * and waiters notified -- BEFORE a scripted delay is applied, so a test can
 * observe a request while its response still stalls the client.
 *
 * Like one_shot_http_server, the destructor unblocks a worker parked in accept
 * via a throwaway self-connect.
 */
class capture_http_server {
public:
   /// One scripted reply: HTTP status, body, and an optional artificial service
   /// delay applied before responding (lets tests stall a client's worker).
   struct scripted_response {
      unsigned                  status = 200;
      std::string               body   = R"({"took":1,"errors":false,"items":[]})";
      std::chrono::milliseconds delay{0};
   };

   /// One recorded request. Header names are lower-cased on capture.
   struct captured_request {
      std::string method;
      std::string target;
      std::string body;
      std::vector<std::pair<std::string, std::string>> headers;

      /// Case-insensitive single-header lookup; empty string when absent.
      std::string header(std::string_view name) const {
         std::string wanted{name};
         std::transform(wanted.begin(), wanted.end(), wanted.begin(),
                        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
         for (const auto& entry : headers) {
            if (entry.first == wanted)
               return entry.second;
         }
         return {};
      }
   };

   explicit capture_http_server(std::vector<scripted_response> script = {})
      : _script(std::move(script))
      , _acceptor(_io, tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0))
      , _port(_acceptor.local_endpoint().port())
      , _worker([this] { serve(); }) {}

   capture_http_server(const capture_http_server&) = delete;
   capture_http_server& operator=(const capture_http_server&) = delete;

   ~capture_http_server() {
      _stopping.store(true);
      // Wake a worker blocked in synchronous accept without operating on the
      // acceptor from two threads; the throwaway socket closes immediately.
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

   /// Return the loopback URL selected for this server.
   std::string url() const { return "http://127.0.0.1:" + std::to_string(_port); }

   /// Number of requests captured so far.
   std::size_t request_count() const {
      std::lock_guard<std::mutex> lk(_mtx);
      return _requests.size();
   }

   /// Copy of the i-th captured request (throws std::out_of_range past the end).
   captured_request request(std::size_t i) const {
      std::lock_guard<std::mutex> lk(_mtx);
      return _requests.at(i);
   }

   /// Block until at least @p n requests are captured or @p timeout elapses.
   bool wait_for_requests(std::size_t n, std::chrono::milliseconds timeout) {
      std::unique_lock<std::mutex> lk(_mtx);
      return _cv.wait_for(lk, timeout, [&] { return _requests.size() >= n; });
   }

private:
   using tcp = boost::asio::ip::tcp;

   void serve() {
      while (!_stopping.load()) {
         boost::system::error_code error;
         tcp::socket socket(_io);
         _acceptor.accept(socket, error);
         if (error || _stopping.load()) {
            return;
         }

         boost::beast::flat_buffer request_buffer;
         boost::beast::http::request<boost::beast::http::string_body> request;
         boost::beast::http::read(socket, request_buffer, request, error);
         if (error) {
            continue;
         }

         captured_request captured;
         captured.method = std::string{request.method_string()};
         captured.target = std::string{request.target()};
         captured.body   = request.body();
         for (const auto& field : request) {
            std::string field_name{field.name_string()};
            std::transform(field_name.begin(), field_name.end(), field_name.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            captured.headers.emplace_back(std::move(field_name), std::string{field.value()});
         }

         scripted_response reply;
         {
            std::lock_guard<std::mutex> lk(_mtx);
            _requests.push_back(std::move(captured));
            if (_next < _script.size()) {
               reply = _script[_next++];
            }
         }
         _cv.notify_all();

         if (reply.delay.count() > 0) {
            std::this_thread::sleep_for(reply.delay);
         }

         std::ostringstream response;
         response << "HTTP/1.1 " << reply.status << " Scripted\r\n"
                  << "Content-Type: application/json\r\n"
                  << "Content-Length: " << reply.body.size() << "\r\n"
                  << "Connection: close\r\n\r\n"
                  << reply.body;
         const auto response_text = response.str();
         boost::asio::write(socket, boost::asio::buffer(response_text), error);
         socket.close(error);
      }
   }

   std::vector<scripted_response> _script;
   std::size_t                    _next = 0;
   mutable std::mutex             _mtx;
   std::condition_variable        _cv;
   std::vector<captured_request>  _requests;
   std::atomic<bool>              _stopping{false};
   boost::asio::io_context        _io;
   tcp::acceptor                  _acceptor;
   uint16_t                       _port;
   std::thread                    _worker;
};

} // namespace fc::test

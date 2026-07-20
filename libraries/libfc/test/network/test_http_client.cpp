/**
 * @file test_http_client.cpp
 * @brief Regression tests for bounded streamed HTTP file downloads.
 */

#include <fc/filesystem.hpp>
#include <fc/network/http/http_client.hpp>

#include <boost/asio.hpp>
#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

using namespace std::chrono_literals;

namespace {

using tcp = boost::asio::ip::tcp;

constexpr int64_t short_timeout_ms = 100;
constexpr int64_t normal_timeout_ms = 2'000;
constexpr int64_t max_test_elapsed_ms = 1'500;
constexpr int64_t framing_delay_ms = 70;
constexpr size_t exact_body_bytes = 8;
constexpr size_t oversized_chunk_extension_bytes = 128 * 1024;
constexpr std::string_view exact_body = "12345678";

/** A one-request loopback HTTP server driven by a caller-supplied response script. */
class scripted_http_server {
public:
   using handler_type = std::function<void(tcp::socket&, const std::atomic_bool&)>;

   /** Start a loopback listener and execute @p handler after receiving one request header. */
   explicit scripted_http_server(handler_type handler)
      : _acceptor(_io, tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0))
      , _socket(_io)
      , _port(_acceptor.local_endpoint().port())
      , _handler(std::move(handler))
      , _worker([this] { serve(); }) {}

   scripted_http_server(const scripted_http_server&) = delete;
   scripted_http_server& operator=(const scripted_http_server&) = delete;

   /** Stop the response script and release its worker. */
   ~scripted_http_server() {
      _stop = true;
      boost::system::error_code ec;
      _acceptor.close(ec);
      _socket.cancel(ec);
      _socket.close(ec);
      unblock_accept();
      if (_worker.joinable()) {
         _worker.join();
      }
   }

   /** Return the ephemeral loopback port assigned to this server. */
   uint16_t port() const { return _port; }

private:
   /** Connect to the listener so a platform that does not cancel synchronous accept can exit. */
   void unblock_accept() {
      boost::asio::io_context io;
      tcp::socket socket(io);
      boost::system::error_code ec;
      socket.connect(tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), _port), ec);
   }

   /** Accept one request, consume its header, and run the configured response script. */
   void serve() {
      boost::system::error_code ec;
      _acceptor.accept(_socket, ec);
      if (ec) {
         return;
      }

      boost::asio::streambuf request;
      boost::asio::read_until(_socket, request, "\r\n\r\n", ec);
      if (!ec) {
         _handler(_socket, _stop);
      }
   }

   boost::asio::io_context _io;
   tcp::acceptor _acceptor;
   tcp::socket _socket;
   uint16_t _port;
   handler_type _handler;
   std::atomic_bool _stop{false};
   std::thread _worker;
};

/** Write all of @p bytes unless the peer has already disconnected. */
bool write_bytes(tcp::socket& socket, std::string_view bytes) {
   boost::system::error_code ec;
   boost::asio::write(socket, boost::asio::buffer(bytes.data(), bytes.size()), ec);
   return !ec;
}

/** Return a complete 200 response header for a fixed-length body. */
std::string fixed_length_header(uint64_t body_bytes) {
   std::ostringstream response;
   response << "HTTP/1.1 200 OK\r\n"
            << "Content-Type: application/octet-stream\r\n"
            << "Content-Length: " << body_bytes << "\r\n"
            << "Connection: close\r\n\r\n";
   return response.str();
}

/** Return a complete 200 response header for a chunked body. */
std::string chunked_header() {
   return "HTTP/1.1 200 OK\r\n"
          "Content-Type: application/octet-stream\r\n"
          "Transfer-Encoding: chunked\r\n"
          "Connection: close\r\n\r\n";
}

/** Return finite test limits with a caller-selected response-body maximum. */
fc::http_file_download_options download_options(uint64_t max_body_bytes) {
   return fc::http_file_download_options{
      .connect_timeout = fc::milliseconds(normal_timeout_ms),
      .response_header_timeout = fc::milliseconds(normal_timeout_ms),
      .idle_read_timeout = fc::milliseconds(normal_timeout_ms),
      .total_timeout = fc::milliseconds(normal_timeout_ms),
      .max_response_body_bytes = max_body_bytes,
      .min_free_disk_space_bytes = 1,
   };
}

/** Return the URL for @p server. */
fc::url server_url(const scripted_http_server& server) {
   return fc::url("http://127.0.0.1:" + std::to_string(server.port()) + "/download");
}

/** Invoke a bounded empty-payload download into @p output. */
void download(const scripted_http_server& server, const std::filesystem::path& output,
              const fc::http_file_download_options& options) {
   fc::http_client client;
   client.post_to_file(server_url(server), fc::variant(fc::mutable_variant_object()), output, options);
}

/** Return the temporary sibling used while @p output is being downloaded. */
std::filesystem::path partial_path(const std::filesystem::path& output) {
   auto partial = output;
   partial += ".downloading";
   return partial;
}

/** Verify that neither the final output nor its temporary sibling exists. */
void check_download_files_removed(const std::filesystem::path& output) {
   BOOST_CHECK(!std::filesystem::exists(output));
   BOOST_CHECK(!std::filesystem::exists(partial_path(output)));
}

/** Return the complete binary contents of @p path. */
std::string read_file(const std::filesystem::path& path) {
   std::ifstream input(path, std::ios::binary);
   return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

} // namespace

BOOST_AUTO_TEST_SUITE(http_client_file_download_tests)

/// A peer that never sends a response header must hit the configured header deadline.
BOOST_AUTO_TEST_CASE(response_header_timeout_removes_partial_file) {
   scripted_http_server server([](tcp::socket&, const std::atomic_bool& stop) {
      while (!stop.load()) {
         std::this_thread::sleep_for(10ms);
      }
   });
   fc::temp_directory temp;
   const auto output = temp.path() / "header-timeout.bin";
   auto options = download_options(exact_body_bytes);
   options.response_header_timeout = fc::milliseconds(short_timeout_ms);

   const auto start = std::chrono::steady_clock::now();
   BOOST_CHECK_THROW(download(server, output, options), fc::exception);
   const auto elapsed = std::chrono::steady_clock::now() - start;

   BOOST_CHECK_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), max_test_elapsed_ms);
   check_download_files_removed(output);
}

/// A body that stops after partial progress must hit the reset-on-progress idle deadline.
BOOST_AUTO_TEST_CASE(idle_body_timeout_removes_partial_file) {
   scripted_http_server server([](tcp::socket& socket, const std::atomic_bool& stop) {
      if (!write_bytes(socket, fixed_length_header(2)) || !write_bytes(socket, "1")) {
         return;
      }
      while (!stop.load()) {
         std::this_thread::sleep_for(10ms);
      }
   });
   fc::temp_directory temp;
   const auto output = temp.path() / "idle-timeout.bin";
   auto options = download_options(exact_body_bytes);
   options.idle_read_timeout = fc::milliseconds(short_timeout_ms);

   BOOST_CHECK_THROW(download(server, output, options), fc::exception);
   check_download_files_removed(output);
}

/// Chunk framing without decoded body bytes must not refresh the response-progress deadline.
BOOST_AUTO_TEST_CASE(chunk_framing_does_not_reset_idle_timeout) {
   scripted_http_server server([](tcp::socket& socket, const std::atomic_bool&) {
      if (!write_bytes(socket, chunked_header())) {
         return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(framing_delay_ms));
      if (!write_bytes(socket, "1\r\n")) {
         return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(framing_delay_ms));
      write_bytes(socket, "1\r\n0\r\n\r\n");
   });
   fc::temp_directory temp;
   const auto output = temp.path() / "framing-idle-timeout.bin";
   auto options = download_options(exact_body_bytes);
   options.idle_read_timeout = fc::milliseconds(short_timeout_ms);

   BOOST_CHECK_THROW(download(server, output, options), fc::exception);
   check_download_files_removed(output);
}

/// Continuous sub-idle progress must still stop at the independent total deadline.
BOOST_AUTO_TEST_CASE(total_timeout_stops_trickled_body_and_removes_partial_file) {
   scripted_http_server server([](tcp::socket& socket, const std::atomic_bool& stop) {
      if (!write_bytes(socket, fixed_length_header(1'000))) {
         return;
      }
      while (!stop.load()) {
         if (!write_bytes(socket, "1")) {
            return;
         }
         std::this_thread::sleep_for(40ms);
      }
   });
   fc::temp_directory temp;
   const auto output = temp.path() / "total-timeout.bin";
   auto options = download_options(1'000);
   options.idle_read_timeout = fc::milliseconds(short_timeout_ms);
   options.total_timeout = fc::milliseconds(250);

   BOOST_CHECK_THROW(download(server, output, options), fc::exception);
   check_download_files_removed(output);
}

/// Oversized Content-Length is rejected while parsing the header, before a temp file is opened.
BOOST_AUTO_TEST_CASE(oversized_fixed_length_response_is_rejected_before_write) {
   scripted_http_server server([](tcp::socket& socket, const std::atomic_bool&) {
      write_bytes(socket, fixed_length_header(exact_body_bytes + 1));
   });
   fc::temp_directory temp;
   const auto output = temp.path() / "oversized-fixed.bin";

   BOOST_CHECK_THROW(download(server, output, download_options(exact_body_bytes)), fc::exception);
   check_download_files_removed(output);
}

/// A chunked response that exceeds the configured maximum is aborted and its temp file is removed.
BOOST_AUTO_TEST_CASE(oversized_chunked_response_is_rejected_and_removed) {
   scripted_http_server server([](tcp::socket& socket, const std::atomic_bool&) {
      write_bytes(socket, chunked_header() + "9\r\n123456789\r\n0\r\n\r\n");
   });
   fc::temp_directory temp;
   const auto output = temp.path() / "oversized-chunked.bin";

   BOOST_CHECK_THROW(download(server, output, download_options(exact_body_bytes)), fc::exception);
   check_download_files_removed(output);
}

/// An unterminated oversized chunk extension must fail within bounded parser memory and time.
BOOST_AUTO_TEST_CASE(oversized_chunk_extension_is_bounded_and_removed) {
   scripted_http_server server([](tcp::socket& socket, const std::atomic_bool&) {
      if (!write_bytes(socket, chunked_header())) {
         return;
      }
      write_bytes(socket, "1;" + std::string(oversized_chunk_extension_bytes, 'a'));
   });
   fc::temp_directory temp;
   const auto output = temp.path() / "oversized-chunk-extension.bin";
   auto options = download_options(exact_body_bytes);
   options.idle_read_timeout = fc::milliseconds(short_timeout_ms);

   const auto start = std::chrono::steady_clock::now();
   BOOST_CHECK_EXCEPTION(download(server, output, options), fc::exception,
                         [](const fc::exception& error) {
                            return error.to_detail_string().find("buffer overflow") != std::string::npos;
                         });
   const auto elapsed = std::chrono::steady_clock::now() - start;

   BOOST_CHECK_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), max_test_elapsed_ms);
   check_download_files_removed(output);
}

/// A response exactly equal to the configured byte maximum must complete and be atomically renamed.
BOOST_AUTO_TEST_CASE(exact_maximum_response_succeeds) {
   scripted_http_server server([](tcp::socket& socket, const std::atomic_bool&) {
      write_bytes(socket, fixed_length_header(exact_body_bytes) + std::string(exact_body));
   });
   fc::temp_directory temp;
   const auto output = temp.path() / "exact-maximum.bin";

   BOOST_CHECK_NO_THROW(download(server, output, download_options(exact_body_bytes)));
   BOOST_CHECK(std::filesystem::exists(output));
   BOOST_CHECK(!std::filesystem::exists(partial_path(output)));
   BOOST_CHECK_EQUAL(read_file(output), exact_body);
}

/// A reserved-headroom requirement larger than the filesystem capacity must fail before writing.
BOOST_AUTO_TEST_CASE(insufficient_disk_headroom_is_rejected_before_write) {
   scripted_http_server server([](tcp::socket& socket, const std::atomic_bool&) {
      write_bytes(socket, fixed_length_header(1) + "1");
   });
   fc::temp_directory temp;
   const auto output = temp.path() / "disk-headroom.bin";
   auto options = download_options(exact_body_bytes);
   options.min_free_disk_space_bytes = std::numeric_limits<uint64_t>::max();

   BOOST_CHECK_THROW(download(server, output, options), fc::exception);
   check_download_files_removed(output);
}

BOOST_AUTO_TEST_SUITE_END()

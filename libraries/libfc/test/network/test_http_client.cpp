/**
 * @file test_http_client.cpp
 * @brief Regression tests for bounded streamed HTTP file downloads.
 */

#include <fc/filesystem.hpp>
#include <fc/network/http/http_client.hpp>

#include <boost/asio.hpp>
#include <boost/test/unit_test.hpp>

#include <algorithm>
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
constexpr size_t blocked_request_body_bytes = 16 * 1024 * 1024;
constexpr size_t disk_space_budget_bytes = 64 * 1024 * 1024;
constexpr size_t large_body_chunk_bytes = 1024 * 1024;
constexpr size_t oversized_chunk_extension_bytes = 128 * 1024;
constexpr std::string_view exact_body = "12345678";

/** A loopback HTTP server driven by a caller-supplied script for each accepted connection. */
class scripted_http_server {
public:
   using handler_type = std::function<void(tcp::socket&, const std::atomic_bool&)>;

   /** Start a listener and execute @p handler for each requested connection. */
   explicit scripted_http_server(handler_type handler, bool consume_request_header = true,
                                 size_t connections_to_accept = 1)
      : _acceptor(_io, tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0))
      , _socket(_io)
      , _port(_acceptor.local_endpoint().port())
      , _handler(std::move(handler))
      , _consume_request_header(consume_request_header)
      , _connections_to_accept(connections_to_accept)
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

   /** Return whether the configured server script has completed. */
   bool finished() const { return _finished.load(); }

private:
   /** Connect to the listener so a platform that does not cancel synchronous accept can exit. */
   void unblock_accept() {
      boost::asio::io_context io;
      tcp::socket socket(io);
      boost::system::error_code ec;
      socket.connect(tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), _port), ec);
   }

   /** Accept configured requests, consume each header, and run the response script. */
   void serve() {
      boost::system::error_code ec;
      for (size_t connection_index = 0; connection_index < _connections_to_accept; ++connection_index) {
         if (connection_index != 0) {
            _socket = tcp::socket(_io);
         }
         _acceptor.accept(_socket, ec);
         if (ec) {
            break;
         }

         if (_consume_request_header) {
            boost::asio::streambuf request;
            boost::asio::read_until(_socket, request, "\r\n\r\n", ec);
            if (ec) {
               break;
            }
         } else {
            _socket.set_option(tcp::socket::receive_buffer_size(1'024), ec);
            if (ec) {
               break;
            }
         }
         _handler(_socket, _stop);
         if (connection_index + 1 < _connections_to_accept) {
            _socket.close(ec);
         }
      }
      _acceptor.close(ec);
      _finished = true;
   }

   boost::asio::io_context _io;
   tcp::acceptor _acceptor;
   tcp::socket _socket;
   uint16_t _port;
   handler_type _handler;
   bool _consume_request_header;
   size_t _connections_to_accept;
   std::atomic_bool _stop{false};
   std::atomic_bool _finished{false};
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

/** Return a JSON metadata response that invites a second request on the same connection. */
std::string keep_alive_metadata_response() {
   return "HTTP/1.1 200 OK\r\n"
          "Content-Type: application/json\r\n"
          "Content-Length: 2\r\n"
          "Connection: keep-alive\r\n\r\n{}";
}

/** Write @p body_bytes bytes in bounded blocks. */
bool write_repeated_body(tcp::socket& socket, uint64_t body_bytes) {
   const std::string block(large_body_chunk_bytes, 'x');
   while (body_bytes != 0) {
      const auto write_bytes_count = std::min<uint64_t>(body_bytes, block.size());
      if (!write_bytes(socket, std::string_view(block.data(), write_bytes_count))) {
         return false;
      }
      body_bytes -= write_bytes_count;
   }
   return true;
}

/** Write a chunked body in one-MiB decoded blocks. */
bool write_chunked_body(tcp::socket& socket, uint64_t body_bytes) {
   const std::string block(large_body_chunk_bytes, 'x');
   while (body_bytes != 0) {
      const auto write_bytes_count = std::min<uint64_t>(body_bytes, block.size());
      std::ostringstream chunk_size;
      chunk_size << std::hex << write_bytes_count << "\r\n";
      if (!write_bytes(socket, chunk_size.str()) ||
          !write_bytes(socket, std::string_view(block.data(), write_bytes_count)) ||
          !write_bytes(socket, "\r\n")) {
         return false;
      }
      body_bytes -= write_bytes_count;
   }
   return write_bytes(socket, "0\r\n\r\n");
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
      .retry_failed_reused_connection = false,
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

/// A peer that accepts but does not read the request must hit the request-write phase deadline.
BOOST_AUTO_TEST_CASE(request_write_timeout_precedes_total_timeout) {
   scripted_http_server server([](tcp::socket&, const std::atomic_bool& stop) {
      while (!stop.load()) {
         std::this_thread::sleep_for(10ms);
      }
   }, false);
   fc::temp_directory temp;
   const auto output = temp.path() / "request-write-timeout.bin";
   auto options = download_options(exact_body_bytes);
   options.response_header_timeout = fc::milliseconds(short_timeout_ms);

   const auto start = std::chrono::steady_clock::now();
   fc::http_client client;
   BOOST_CHECK_EXCEPTION(
      client.post_to_file(server_url(server), fc::variant(std::string(blocked_request_body_bytes, 'a')),
                          output, options),
      fc::exception,
      [](const fc::exception& error) {
         return error.to_detail_string().find("Failed to send POST request") != std::string::npos;
      });
   const auto elapsed = std::chrono::steady_clock::now() - start;

   BOOST_CHECK_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), max_test_elapsed_ms);
   check_download_files_removed(output);
}

/// Metadata and download requests should reuse one healthy keep-alive connection.
BOOST_AUTO_TEST_CASE(healthy_metadata_connection_is_reused_for_download) {
   scripted_http_server server([](tcp::socket& socket, const std::atomic_bool&) {
      if (!write_bytes(socket, keep_alive_metadata_response())) {
         return;
      }
      boost::asio::streambuf request;
      boost::system::error_code ec;
      boost::asio::read_until(socket, request, "\r\n\r\n", ec);
      if (!ec) {
         write_bytes(socket, fixed_length_header(exact_body_bytes) + std::string(exact_body));
      }
   });
   fc::temp_directory temp;
   const auto output = temp.path() / "reused-connection.bin";
   fc::http_client client;
   auto metadata_deadline = fc::time_point::now();
   metadata_deadline.safe_add(fc::milliseconds(normal_timeout_ms));

   BOOST_REQUIRE_NO_THROW(
      client.post_sync(server_url(server), fc::variant(fc::mutable_variant_object()), metadata_deadline));
   auto options = download_options(exact_body_bytes);
   options.retry_failed_reused_connection = true;
   BOOST_REQUIRE_NO_THROW(
      client.post_to_file(server_url(server), fc::variant(fc::mutable_variant_object()), output, options));
   BOOST_CHECK_EQUAL(read_file(output), exact_body);
}

/// A cached connection closed after metadata should retry the idempotent download once.
BOOST_AUTO_TEST_CASE(stale_metadata_connection_retries_download_on_fresh_connection) {
   std::atomic_size_t connection_index{0};
   scripted_http_server server([&](tcp::socket& socket, const std::atomic_bool&) {
      if (connection_index.fetch_add(1) == 0) {
         if (write_bytes(socket, keep_alive_metadata_response())) {
            boost::system::error_code ec;
            socket.shutdown(tcp::socket::shutdown_both, ec);
         }
         return;
      }
      write_bytes(socket, fixed_length_header(exact_body_bytes) + std::string(exact_body));
   }, true, 2);
   fc::temp_directory temp;
   const auto output = temp.path() / "retried-connection.bin";
   fc::http_client client;
   auto metadata_deadline = fc::time_point::now();
   metadata_deadline.safe_add(fc::milliseconds(normal_timeout_ms));

   BOOST_REQUIRE_NO_THROW(
      client.post_sync(server_url(server), fc::variant(fc::mutable_variant_object()), metadata_deadline));
   auto options = download_options(exact_body_bytes);
   options.retry_failed_reused_connection = true;
   BOOST_REQUIRE_NO_THROW(
      client.post_to_file(server_url(server), fc::variant(fc::mutable_variant_object()), output, options));
   BOOST_CHECK_EQUAL(connection_index.load(), 2U);
   BOOST_CHECK_EQUAL(read_file(output), exact_body);
}

/// A failed reconnect after stale reuse must unwind without touching an invalid connection iterator.
BOOST_AUTO_TEST_CASE(stale_metadata_reconnect_failure_cleans_up_safely) {
   scripted_http_server server([](tcp::socket& socket, const std::atomic_bool&) {
      if (write_bytes(socket, keep_alive_metadata_response())) {
         boost::system::error_code ec;
         socket.shutdown(tcp::socket::shutdown_both, ec);
      }
   });
   fc::temp_directory temp;
   const auto output = temp.path() / "failed-reconnect.bin";
   fc::http_client client;
   auto metadata_deadline = fc::time_point::now();
   metadata_deadline.safe_add(fc::milliseconds(normal_timeout_ms));

   BOOST_REQUIRE_NO_THROW(
      client.post_sync(server_url(server), fc::variant(fc::mutable_variant_object()), metadata_deadline));
   for (size_t wait_count = 0; wait_count < 1'000 && !server.finished(); ++wait_count) {
      std::this_thread::sleep_for(1ms);
   }
   BOOST_REQUIRE(server.finished());
   auto options = download_options(exact_body_bytes);
   options.retry_failed_reused_connection = true;
   BOOST_CHECK_EXCEPTION(
      client.post_to_file(server_url(server), fc::variant(fc::mutable_variant_object()), output, options),
      fc::exception,
      [](const fc::exception& error) {
         return error.to_detail_string().find("Failed to connect") != std::string::npos;
      });
   check_download_files_removed(output);
}

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

/// A fixed-length response exactly one disk-check budget wide should not require a refill.
BOOST_AUTO_TEST_CASE(exact_disk_space_budget_response_succeeds) {
   scripted_http_server server([](tcp::socket& socket, const std::atomic_bool&) {
      if (write_bytes(socket, fixed_length_header(disk_space_budget_bytes))) {
         write_repeated_body(socket, disk_space_budget_bytes);
      }
   });
   fc::temp_directory temp;
   const auto output = temp.path() / "exact-disk-budget.bin";
   auto options = download_options(disk_space_budget_bytes);
   options.idle_read_timeout = fc::milliseconds(30'000);
   options.total_timeout = fc::milliseconds(30'000);

   BOOST_REQUIRE_NO_THROW(download(server, output, options));
   BOOST_CHECK_EQUAL(std::filesystem::file_size(output), disk_space_budget_bytes);
}

/// A chunked response beyond one disk-check budget must refill and complete successfully.
BOOST_AUTO_TEST_CASE(chunked_response_refills_disk_space_budget) {
   constexpr auto response_bytes = disk_space_budget_bytes + large_body_chunk_bytes;
   scripted_http_server server([](tcp::socket& socket, const std::atomic_bool&) {
      if (write_bytes(socket, chunked_header())) {
         write_chunked_body(socket, response_bytes);
      }
   });
   fc::temp_directory temp;
   const auto output = temp.path() / "refilled-disk-budget.bin";
   auto options = download_options(response_bytes);
   options.idle_read_timeout = fc::milliseconds(30'000);
   options.total_timeout = fc::milliseconds(30'000);

   BOOST_REQUIRE_NO_THROW(download(server, output, options));
   BOOST_CHECK_EQUAL(std::filesystem::file_size(output), response_bytes);
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

/// A write that only barely fits must still leave room for the concurrent-consumer margin.
BOOST_AUTO_TEST_CASE(disk_space_budget_requires_concurrency_margin) {
   scripted_http_server server([](tcp::socket& socket, const std::atomic_bool&) {
      write_bytes(socket, fixed_length_header(exact_body_bytes) + std::string(exact_body));
   });
   fc::temp_directory temp;
   const auto output = temp.path() / "disk-concurrency-margin.bin";
   std::error_code ec;
   const auto space = std::filesystem::space(temp.path(), ec);
   BOOST_REQUIRE(!ec);
   BOOST_REQUIRE_GT(space.available, exact_body_bytes);
   auto options = download_options(exact_body_bytes);
   options.min_free_disk_space_bytes = space.available - exact_body_bytes;

   BOOST_CHECK_THROW(download(server, output, options), fc::exception);
   check_download_files_removed(output);
}

/// Fixed-length preflight must include the margin before opening a temporary file.
BOOST_AUTO_TEST_CASE(fixed_length_preflight_includes_concurrency_margin) {
   constexpr auto response_bytes = disk_space_budget_bytes + 1;
   constexpr auto headroom_without_full_margin = disk_space_budget_bytes / 2;
   scripted_http_server server([](tcp::socket& socket, const std::atomic_bool&) {
      write_bytes(socket, fixed_length_header(response_bytes));
   });
   fc::temp_directory temp;
   const auto output = temp.path() / "fixed-preflight-margin.bin";
   std::error_code ec;
   const auto space = std::filesystem::space(temp.path(), ec);
   BOOST_REQUIRE(!ec);
   BOOST_REQUIRE_GT(space.available, response_bytes + headroom_without_full_margin);
   auto options = download_options(response_bytes);
   options.min_free_disk_space_bytes =
      space.available - response_bytes - headroom_without_full_margin;

   BOOST_CHECK_EXCEPTION(
      download(server, output, options),
      fc::exception,
      [](const fc::exception& error) {
         return error.to_detail_string().find("Insufficient disk space") != std::string::npos;
      });
   check_download_files_removed(output);
}

BOOST_AUTO_TEST_SUITE_END()

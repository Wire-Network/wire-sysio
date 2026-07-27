/**
 * @file test_http_client.cpp
 * @brief Regression tests for bounded streamed HTTP file downloads.
 */

#include <fc/filesystem.hpp>
#include <fc/network/http/http_client.hpp>
#include <fc/task/deadline.hpp>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/http/status.hpp>
#include <boost/test/unit_test.hpp>

#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iomanip>
#include <iterator>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace fc {

/** Private free-space-query injection for HTTP download tests. */
struct http_client_test_access {
   static void set_available_disk_space(http_client& client, uint64_t available_bytes) {
      client.set_space_available_provider_for_testing(
         [available_bytes](const std::filesystem::path&) { return available_bytes; });
   }
};

namespace http {

/** Private-constructor access for deterministic transport resolver tests. */
struct transport_test_access {
   /** Construct a transport with @p resolver_start replacing the platform resolver. */
   static transport create(
      transport_options options,
      detail::resolver_start_fn resolver_start) {
      return transport(
         std::move(options),
         std::move(resolver_start));
   }
};

} // namespace http

} // namespace fc

namespace {

using tcp = boost::asio::ip::tcp;
using local_protocol = boost::asio::local::stream_protocol;

constexpr int64_t normal_timeout_ms = 2'000;
constexpr int64_t cancellation_delay_ms = 100;
constexpr int64_t max_test_elapsed_ms = 1'500;
constexpr int64_t reuse_metadata_deadline_ms = 500;
constexpr int64_t reuse_after_deadline_wait_ms = 600;
constexpr size_t exact_body_bytes = 8;
constexpr size_t blocked_request_body_bytes = 16 * 1024 * 1024;
constexpr size_t disk_space_budget_bytes = 64 * 1024 * 1024;
constexpr size_t large_body_chunk_bytes = 1024 * 1024;
constexpr size_t oversized_chunk_extension_bytes = 128 * 1024;
constexpr std::string_view exact_body = "12345678";

/** Return finite policy used by deterministic shared-transport tests. */
fc::http::request_options tls_request_options();

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

/** One-request Unix-socket HTTP server that records the exact request head. */
class scripted_unix_http_server {
public:
   /** Bind @p socket_path and start accepting one HTTP request. */
   explicit scripted_unix_http_server(
      const std::filesystem::path& socket_path)
      : _socket_path(socket_path)
      , _acceptor(
           _io,
           local_protocol::endpoint(
              _socket_path.string()))
      , _socket(_io)
      , _worker([this] { serve(); }) {}

   scripted_unix_http_server(
      const scripted_unix_http_server&) = delete;
   scripted_unix_http_server&
   operator=(
      const scripted_unix_http_server&) = delete;

   /** Stop a pending accept and join the server worker. */
   ~scripted_unix_http_server() {
      _stop = true;
      unblock_accept();
      if (_worker.joinable())
         _worker.join();
   }

   /** Join the completed request and return its exact serialized head. */
   const std::string& wait_for_request() {
      if (_worker.joinable())
         _worker.join();
      return _observed_request;
   }

private:
   /** Connect to the listener so synchronous accept exits on every supported platform. */
   void unblock_accept() {
      boost::asio::io_context io;
      local_protocol::socket socket(io);
      boost::system::error_code error;
      socket.connect(
         local_protocol::endpoint(
            _socket_path.string()),
         error);
   }

   /** Accept one request, record it, and return a successful empty object. */
   void serve() {
      boost::system::error_code error;
      _acceptor.accept(_socket, error);
      if (error || _stop)
         return;

      boost::asio::streambuf request;
      boost::asio::read_until(
         _socket,
         request,
         "\r\n\r\n",
         error);
      if (error)
         return;
      _observed_request.assign(
         boost::asio::buffers_begin(request.data()),
         boost::asio::buffers_end(request.data()));

      constexpr std::string_view response =
         "HTTP/1.1 200 OK\r\n"
         "Content-Length: 2\r\n"
         "Connection: close\r\n\r\n{}";
      boost::asio::write(
         _socket,
         boost::asio::buffer(
            response.data(),
            response.size()),
         error);
   }

   std::filesystem::path _socket_path;
   boost::asio::io_context _io;
   local_protocol::acceptor _acceptor;
   local_protocol::socket _socket;
   std::string _observed_request;
   std::atomic_bool _stop{false};
   std::thread _worker;
};

/** Send one POST request to @p endpoint through a Unix socket. */
fc::http::response
perform_unix_request(
   const std::filesystem::path& endpoint) {
   fc::http::transport transport;
   return transport.perform(
      fc::http::request{
         .method =
            fc::http::request_method::post,
         .target =
            fc::url(
               "unix",
               fc::ostring{endpoint.string()},
               {},
               {},
               {},
               {},
               {},
               std::nullopt),
      },
      tls_request_options());
}

/** One-request HTTPS server backed by deterministic private-CA test fixtures. */
class https_response_server {
public:
   /** Start a loopback TLS server with @p certificate and @p private_key. */
   https_response_server(const std::filesystem::path& certificate,
                         const std::filesystem::path& private_key,
                         boost::asio::ip::address listen_address =
                            boost::asio::ip::address_v4::loopback())
      : _context(boost::asio::ssl::context::tls_server)
      , _listen_address(std::move(listen_address))
      , _acceptor(_io, tcp::endpoint(_listen_address, 0))
      , _port(_acceptor.local_endpoint().port()) {
      _context.use_certificate_chain_file(certificate.string());
      _context.use_private_key_file(private_key.string(), boost::asio::ssl::context::pem);
      SSL_CTX_set_tlsext_servername_callback(
         _context.native_handle(),
         +[](SSL* ssl, int*, void* opaque) {
            auto& server = *static_cast<https_response_server*>(opaque);
            const auto* name = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
            std::scoped_lock lock(server._sni_mutex);
            server._sni = name ? name : "";
            return SSL_TLSEXT_ERR_OK;
         });
      SSL_CTX_set_tlsext_servername_arg(_context.native_handle(), this);
      _worker = std::thread([this] { serve(); });
   }

   https_response_server(const https_response_server&) = delete;
   https_response_server& operator=(const https_response_server&) = delete;

   /** Stop the accept worker when the client fails before opening a connection. */
   ~https_response_server() {
      boost::system::error_code ec;
      _acceptor.close(ec);
      unblock_accept();
      if (_worker.joinable())
         _worker.join();
   }

   /** Return the loopback port selected by the OS. */
   uint16_t port() const { return _port; }

   /** Return the DNS SNI value observed by the server, or empty for an IP-literal request. */
   std::string sni() const {
      std::scoped_lock lock(_sni_mutex);
      return _sni;
   }

   /** Return whether TLS verification completed far enough for an HTTP request to arrive. */
   bool request_observed() const {
      return _request_observed.load();
   }

private:
   /** Connect once so a synchronous accept observes teardown on all supported platforms. */
   void unblock_accept() {
      boost::asio::io_context io;
      tcp::socket socket(io);
      boost::system::error_code ec;
      socket.connect(tcp::endpoint(_listen_address, _port), ec);
   }

   /** Complete one TLS request and return a small fixed response. */
   void serve() {
      boost::asio::ssl::stream<tcp::socket> stream(_io, _context);
      boost::system::error_code ec;
      _acceptor.accept(stream.next_layer(), ec);
      if (ec)
         return;
      stream.handshake(boost::asio::ssl::stream_base::server, ec);
      if (ec)
         return;

      boost::asio::streambuf request;
      boost::asio::read_until(stream, request, "\r\n\r\n", ec);
      if (ec)
         return;
      _request_observed = true;
      constexpr std::string_view response =
         "HTTP/1.1 200 OK\r\n"
         "Content-Type: application/json\r\n"
         "Content-Length: 2\r\n"
         "Connection: close\r\n\r\n{}";
      boost::asio::write(stream, boost::asio::buffer(response), ec);
      stream.shutdown(ec);
   }

   boost::asio::io_context _io;
   boost::asio::ssl::context _context;
   boost::asio::ip::address _listen_address;
   tcp::acceptor _acceptor;
   uint16_t _port;
   mutable std::mutex _sni_mutex;
   std::string _sni;
   std::atomic_bool _request_observed{false};
   std::thread _worker;
};

/** Write all of @p bytes unless the peer has already disconnected. */
bool write_bytes(tcp::socket& socket, std::string_view bytes) {
   boost::system::error_code ec;
   boost::asio::write(socket, boost::asio::buffer(bytes.data(), bytes.size()), ec);
   return !ec;
}

/** Read one complete HTTP request header from @p socket. */
std::string read_request_header(tcp::socket& socket) {
   boost::asio::streambuf request;
   boost::system::error_code error;
   boost::asio::read_until(socket, request, "\r\n\r\n", error);
   if (error)
      return {};
   return {
      boost::asio::buffers_begin(request.data()),
      boost::asio::buffers_end(request.data())};
}

/** Return a complete fixed-length response header. */
std::string fixed_length_header(uint64_t body_bytes, std::string_view status = "200 OK") {
   std::ostringstream response;
   response << "HTTP/1.1 " << status << "\r\n"
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

/** Return snapshot-style download limits with a caller-selected response-body maximum. */
fc::http_file_download_options download_options(uint64_t max_body_bytes) {
   return fc::http_file_download_options{
      .max_response_body_bytes = max_body_bytes,
      .retry_failed_reused_connection = false,
   };
}

/** Return the URL for @p server. */
fc::url server_url(const scripted_http_server& server) {
   return fc::url("http://127.0.0.1:" + std::to_string(server.port()) + "/download");
}

/** Invoke a bounded empty-payload download into @p output. */
void download(const scripted_http_server& server, const std::filesystem::path& output,
              const fc::http_file_download_options& options,
              std::function<bool()> cancel_check = {}) {
   fc::http_client client;
   client.set_cancel_check(std::move(cancel_check));
   client.post_to_file(server_url(server), fc::variant(fc::mutable_variant_object()), output, options);
}

/** Invoke a download with a deterministic free-space query result. */
void download_with_available_disk_space(const scripted_http_server& server,
                                        const std::filesystem::path& output,
                                        const fc::http_file_download_options& options,
                                        uint64_t available_bytes) {
   fc::http_client client;
   fc::http_client_test_access::set_available_disk_space(client, available_bytes);
   client.post_to_file(server_url(server), fc::variant(fc::mutable_variant_object()), output, options);
}

/** Request cancellation after a short deterministic delay. */
class delayed_cancellation {
public:
   delayed_cancellation()
      : _worker([this]() {
           std::this_thread::sleep_for(std::chrono::milliseconds(cancellation_delay_ms));
           _requested = true;
        }) {}

   delayed_cancellation(const delayed_cancellation&) = delete;
   delayed_cancellation& operator=(const delayed_cancellation&) = delete;

   /** Join the worker that publishes the cancellation request. */
   ~delayed_cancellation() {
      if (_worker.joinable()) {
         _worker.join();
      }
   }

   /** Return a predicate suitable for http_client::set_cancel_check(). */
   std::function<bool()> check() {
      return [this]() { return _requested.load(); };
   }

private:
   std::atomic_bool _requested{false};
   std::thread _worker;
};

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

/** Run-time TLS material with validity windows relative to the test start. */
class tls_test_material {
public:
   tls_test_material() {
      auto primary_ca_key = make_key();
      auto primary_ca =
         make_certificate(
            *primary_ca_key,
            nullptr,
            nullptr,
            "Wire HTTP Test CA",
            -day,
            30 * day,
            true,
            {});
      write_certificate("ca.pem", *primary_ca);

      auto secondary_ca_key = make_key();
      auto secondary_ca =
         make_certificate(
            *secondary_ca_key,
            nullptr,
            nullptr,
            "Wire HTTP Secondary Test CA",
            -day,
            30 * day,
            true,
            {});
      write_certificate(
         "secondary-ca.pem",
         *secondary_ca);

      write_leaf(
         "dns",
         *primary_ca,
         *primary_ca_key,
         "DNS:localhost",
         -hour,
         7 * day);
      write_leaf(
         "ip",
         *primary_ca,
         *primary_ca_key,
         "IP:127.0.0.1",
         -hour,
         7 * day);
      write_leaf(
         "ipv6",
         *secondary_ca,
         *secondary_ca_key,
         "IP:::1",
         -hour,
         7 * day);
      write_leaf(
         "expired",
         *primary_ca,
         *primary_ca_key,
         "DNS:localhost",
         -7 * day,
         -day);
      write_leaf(
         "future",
         *secondary_ca,
         *secondary_ca_key,
         "DNS:localhost",
         day,
         7 * day);
   }

   /** Return one generated PEM path. */
   std::filesystem::path
   path(std::string_view name) const {
      return _directory.path() / name;
   }

private:
   static constexpr long hour = 60 * 60;
   static constexpr long day = 24 * hour;
   using key_ptr =
      std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
   using certificate_ptr =
      std::unique_ptr<X509, decltype(&X509_free)>;

   /** Generate one RSA key through the non-deprecated EVP interface. */
   static key_ptr make_key() {
      std::unique_ptr<
         EVP_PKEY_CTX,
         decltype(&EVP_PKEY_CTX_free)>
         context(
            EVP_PKEY_CTX_new_id(
               EVP_PKEY_RSA,
               nullptr),
            &EVP_PKEY_CTX_free);
      FC_ASSERT(
         context,
         "Failed to allocate TLS test key context");
      FC_ASSERT(
         EVP_PKEY_keygen_init(context.get()) == 1,
         "Failed to initialize TLS test key generation");
      FC_ASSERT(
         EVP_PKEY_CTX_set_rsa_keygen_bits(
            context.get(),
            2'048) == 1,
         "Failed to configure TLS test key size");
      EVP_PKEY* generated = nullptr;
      FC_ASSERT(
         EVP_PKEY_keygen(
            context.get(),
            &generated) == 1,
         "Failed to generate TLS test key");
      return key_ptr(generated, &EVP_PKEY_free);
   }

   /** Add one X.509v3 extension. */
   static void add_extension(
      X509& certificate,
      int nid,
      std::string value) {
      std::unique_ptr<
         X509_EXTENSION,
         decltype(&X509_EXTENSION_free)>
         extension(
            X509V3_EXT_conf_nid(
               nullptr,
               nullptr,
               nid,
               value.data()),
            &X509_EXTENSION_free);
      FC_ASSERT(
         extension,
         "Failed to create TLS test certificate extension");
      FC_ASSERT(
         X509_add_ext(
            &certificate,
            extension.get(),
            -1) == 1,
         "Failed to add TLS test certificate extension");
   }

   /** Build and sign one CA or leaf certificate. */
   static certificate_ptr make_certificate(
      EVP_PKEY& subject_key,
      X509* issuer,
      EVP_PKEY* issuer_key,
      std::string_view common_name,
      long not_before_offset,
      long not_after_offset,
      bool certificate_authority,
      std::string subject_alt_name) {
      static std::atomic_long serial{1};
      certificate_ptr certificate(
         X509_new(),
         &X509_free);
      FC_ASSERT(
         certificate,
         "Failed to allocate TLS test certificate");
      FC_ASSERT(
         X509_set_version(
            certificate.get(),
            2) == 1,
         "Failed to set TLS test certificate version");
      ASN1_INTEGER_set(
         X509_get_serialNumber(
            certificate.get()),
         serial.fetch_add(1));
      X509_gmtime_adj(
         X509_getm_notBefore(
            certificate.get()),
         not_before_offset);
      X509_gmtime_adj(
         X509_getm_notAfter(
            certificate.get()),
         not_after_offset);
      FC_ASSERT(
         X509_set_pubkey(
            certificate.get(),
            &subject_key) == 1,
         "Failed to set TLS test certificate key");

      auto* subject =
         X509_get_subject_name(
            certificate.get());
      FC_ASSERT(
         X509_NAME_add_entry_by_txt(
            subject,
            "CN",
            MBSTRING_ASC,
            reinterpret_cast<
               const unsigned char*>(
               common_name.data()),
            static_cast<int>(
               common_name.size()),
            -1,
            0) == 1,
         "Failed to set TLS test certificate name");
      FC_ASSERT(
         X509_set_issuer_name(
            certificate.get(),
            issuer
               ? X509_get_subject_name(issuer)
               : subject) == 1,
         "Failed to set TLS test certificate issuer");

      add_extension(
         *certificate,
         NID_basic_constraints,
         certificate_authority
            ? "critical,CA:TRUE"
            : "critical,CA:FALSE");
      add_extension(
         *certificate,
         NID_key_usage,
         certificate_authority
            ? "critical,keyCertSign,cRLSign"
            : "critical,digitalSignature,keyEncipherment");
      if (!subject_alt_name.empty()) {
         add_extension(
            *certificate,
            NID_subject_alt_name,
            std::move(subject_alt_name));
      }
      FC_ASSERT(
         X509_sign(
            certificate.get(),
            issuer_key
               ? issuer_key
               : &subject_key,
            EVP_sha256()) > 0,
         "Failed to sign TLS test certificate");
      return certificate;
   }

   /** Generate and write one leaf key/certificate pair. */
   void write_leaf(
      std::string_view stem,
      X509& issuer,
      EVP_PKEY& issuer_key,
      std::string subject_alt_name,
      long not_before_offset,
      long not_after_offset) {
      auto key = make_key();
      auto certificate =
         make_certificate(
            *key,
            &issuer,
            &issuer_key,
            "Wire HTTP Test Leaf",
            not_before_offset,
            not_after_offset,
            false,
            std::move(subject_alt_name));
      write_certificate(
         std::string(stem) + ".pem",
         *certificate);
      write_key(
         std::string(stem) + ".key",
         *key);
   }

   /** Write one PEM certificate. */
   void write_certificate(
      std::string_view name,
      X509& certificate) {
      std::unique_ptr<
         FILE,
         decltype(&std::fclose)>
         output(
            std::fopen(
               path(name).c_str(),
               "w"),
            &std::fclose);
      FC_ASSERT(
         output &&
            PEM_write_X509(
               output.get(),
               &certificate) == 1,
         "Failed to write TLS test certificate");
   }

   /** Write one unencrypted PEM private key. */
   void write_key(
      std::string_view name,
      EVP_PKEY& key) {
      std::unique_ptr<
         FILE,
         decltype(&std::fclose)>
         output(
            std::fopen(
               path(name).c_str(),
               "w"),
            &std::fclose);
      FC_ASSERT(
         output &&
            PEM_write_PrivateKey(
               output.get(),
               &key,
               nullptr,
               nullptr,
               0,
               nullptr,
               nullptr) == 1,
         "Failed to write TLS test private key");
   }

   fc::temp_directory _directory;
};

/** Return a path to one process-local TLS fixture. */
std::filesystem::path tls_fixture(std::string_view name) {
   static const tls_test_material material;
   return material.path(name);
}

/** Return finite policy used by deterministic shared-transport TLS tests. */
fc::http::request_options tls_request_options() {
   return fc::http::request_options{
      .max_request_body_bytes = 1'024,
      .max_response_body_bytes = 1'024,
      .timeouts =
         fc::http::timeout_options{
            .connect = fc::seconds(1),
            .header = fc::seconds(1),
            .read = fc::seconds(1),
            .idle = fc::seconds(1),
            .total = fc::seconds(2),
         },
      .idempotent = true,
   };
}

/** Perform one HTTPS GET through the shared transport. */
fc::http::response tls_get(https_response_server& server,
                           std::string_view host,
                           fc::http::transport_options transport_options) {
   fc::http::transport transport(std::move(transport_options));
   const auto authority =
      host.find(':') == std::string_view::npos
         ? std::string(host)
         : "[" + std::string(host) + "]";
   return transport.perform(
      fc::http::request{
         .method = fc::http::request_method::get,
         .target =
            fc::url(
               "https://" + authority + ":" +
               std::to_string(server.port()) + "/"),
         .user_agent = "wire-http-tls-test",
      },
      tls_request_options());
}

/** Return whether this host supports binding the IPv6 loopback address. */
bool supports_ipv6_loopback() {
   boost::asio::io_context io;
   boost::system::error_code error;
   tcp::acceptor acceptor(io);
   acceptor.open(tcp::v6(), error);
   if (error)
      return false;
   acceptor.bind(
      tcp::endpoint(
         boost::asio::ip::address_v6::loopback(),
         0),
      error);
   return !error;
}

/** Return the OpenSSL hashed-directory filename for @p certificate. */
std::string certificate_hash_filename(const std::filesystem::path& certificate) {
   std::unique_ptr<FILE, decltype(&std::fclose)> input(std::fopen(certificate.c_str(), "r"),
                                                       &std::fclose);
   BOOST_REQUIRE(input);
   std::unique_ptr<X509, decltype(&X509_free)> parsed(
      PEM_read_X509(input.get(), nullptr, nullptr, nullptr), &X509_free);
   BOOST_REQUIRE(parsed);
   std::ostringstream name;
   name << std::hex << std::setw(8) << std::setfill('0')
        << X509_NAME_hash(X509_get_subject_name(parsed.get())) << ".0";
   return name.str();
}

} // namespace

BOOST_AUTO_TEST_SUITE(http_authenticated_transport_tests)

/// A continuation observes the first response and sends its follow-up on the same socket.
BOOST_AUTO_TEST_CASE(connection_affine_continuation_reuses_exact_connection) {
   std::atomic_uint32_t requests_observed{1};
   scripted_http_server server(
      [&](tcp::socket& socket, const std::atomic_bool&) {
         BOOST_REQUIRE(write_bytes(
            socket,
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: 5\r\n"
            "Connection: keep-alive\r\n\r\n"
            "first"));
         if (!read_request_header(socket).empty()) {
            ++requests_observed;
            (void)write_bytes(
               socket,
               "HTTP/1.1 200 OK\r\n"
               "Content-Length: 6\r\n"
               "Connection: close\r\n\r\n"
               "second");
         }
      });
   fc::http::transport transport;
   const fc::http::request request{
      .method = fc::http::request_method::get,
      .target = server_url(server),
   };
   bool hook_called = false;

   const auto response =
      transport.perform_then(
         request,
         tls_request_options(),
         [&](const fc::http::response& first) {
            hook_called = true;
            BOOST_CHECK_EQUAL(first.body, "first");
            return fc::http::continuation_request{
               .next_request = request,
               .options = tls_request_options(),
            };
         });

   BOOST_CHECK(hook_called);
   BOOST_CHECK_EQUAL(response.body, "second");
   BOOST_CHECK_EQUAL(requests_observed.load(), 2U);
}

/// A continuation that re-enters its synchronous transport fails instead of deadlocking.
BOOST_AUTO_TEST_CASE(connection_affine_continuation_reentry_fails_fast) {
   scripted_http_server server(
      [](tcp::socket& socket,
         const std::atomic_bool&) {
         (void)write_bytes(
            socket,
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: 5\r\n"
            "Connection: keep-alive\r\n\r\n"
            "first");
      });
   fc::http::transport transport;
   const fc::http::request request{
      .method = fc::http::request_method::get,
      .target = server_url(server),
   };
   const auto started =
      std::chrono::steady_clock::now();

   BOOST_CHECK_EXCEPTION(
      transport.perform_then(
         request,
         tls_request_options(),
         [&](const fc::http::response&) {
            (void)transport.perform(
               request,
               tls_request_options());
            return fc::http::continuation_request{
               .next_request = request,
               .options = tls_request_options(),
            };
         }),
      fc::exception,
      [](const fc::exception& error) {
         return error.to_detail_string().find(
                   "cannot be re-entered") !=
                std::string::npos;
      });
   BOOST_CHECK_LT(
      std::chrono::duration_cast<
         std::chrono::milliseconds>(
         std::chrono::steady_clock::now() -
         started)
         .count(),
      max_test_elapsed_ms);
}

/// Waiting for the synchronous facade consumes the inherited task deadline.
BOOST_AUTO_TEST_CASE(synchronous_transport_lock_wait_is_deadline_bounded) {
   std::atomic_bool first_started{false};
   scripted_http_server server(
      [&](tcp::socket& socket,
          const std::atomic_bool&) {
         first_started = true;
         std::this_thread::sleep_for(250ms);
         (void)write_bytes(
            socket,
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: 2\r\n"
            "Connection: close\r\n\r\n{}");
      });
   fc::http::transport transport;
   const fc::http::request request{
      .method = fc::http::request_method::get,
      .target = server_url(server),
   };
   std::exception_ptr first_failure;
   std::jthread first([&] {
      try {
         (void)transport.perform(
            request,
            tls_request_options());
      } catch (...) {
         first_failure =
            std::current_exception();
      }
   });
   while (!first_started.load())
      std::this_thread::yield();

   const auto started =
      std::chrono::steady_clock::now();
   {
      fc::task::deadline_scope deadline(
         fc::time_point::now() +
         fc::milliseconds(50));
      BOOST_CHECK_EXCEPTION(
         transport.perform(
            request,
            tls_request_options()),
         fc::timeout_exception,
         [](const fc::exception& error) {
            return error.to_detail_string().find(
                      "waiting for the synchronous transport") !=
                   std::string::npos;
         });
   }
   BOOST_CHECK_LT(
      std::chrono::duration_cast<
         std::chrono::milliseconds>(
         std::chrono::steady_clock::now() -
         started)
         .count(),
      max_test_elapsed_ms);
   first.join();
   if (first_failure)
      std::rethrow_exception(first_failure);
}

/// A zero idle-pool cap closes completed connections instead of retaining them.
BOOST_AUTO_TEST_CASE(idle_connection_pool_cap_can_disable_reuse) {
   std::atomic_uint32_t connections{0};
   scripted_http_server server(
      [&](tcp::socket& socket,
          const std::atomic_bool&) {
         ++connections;
         (void)write_bytes(
            socket,
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: 2\r\n"
            "Connection: keep-alive\r\n\r\n{}");
      },
      true,
      2);
   fc::http::transport transport(
      fc::http::transport_options{
         .max_idle_connections = 0,
      });
   const fc::http::request request{
      .method = fc::http::request_method::get,
      .target = server_url(server),
   };

   (void)transport.perform(
      request,
      tls_request_options());
   (void)transport.perform(
      request,
      tls_request_options());

   BOOST_CHECK_EQUAL(connections.load(), 2U);
}

/// Connections older than the configured idle age are closed before reuse.
BOOST_AUTO_TEST_CASE(expired_idle_connection_is_not_reused) {
   std::atomic_uint32_t connections{0};
   scripted_http_server server(
      [&](tcp::socket& socket,
          const std::atomic_bool&) {
         ++connections;
         (void)write_bytes(
            socket,
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: 2\r\n"
            "Connection: keep-alive\r\n\r\n{}");
      },
      true,
      2);
   fc::http::transport transport(
      fc::http::transport_options{
         .max_idle_connection_age =
            fc::milliseconds(1),
      });
   const fc::http::request request{
      .method = fc::http::request_method::get,
      .target = server_url(server),
   };

   (void)transport.perform(
      request,
      tls_request_options());
   std::this_thread::sleep_for(5ms);
   (void)transport.perform(
      request,
      tls_request_options());

   BOOST_CHECK_EQUAL(connections.load(), 2U);
}

/// A continuation cannot redirect the retained connection to another endpoint.
BOOST_AUTO_TEST_CASE(connection_affine_continuation_rejects_different_endpoint) {
   scripted_http_server server([](tcp::socket& socket, const std::atomic_bool&) {
      (void)write_bytes(socket, "HTTP/1.1 200 OK\r\n"
                                "Content-Length: 5\r\n"
                                "Connection: keep-alive\r\n\r\n"
                                "first");
   });
   fc::http::transport transport;
   const fc::http::request request{
      .method = fc::http::request_method::get,
      .target = server_url(server),
   };

   BOOST_CHECK_EXCEPTION(transport.perform_then(request, tls_request_options(),
                                                [&](const fc::http::response&) {
                                                   auto different_endpoint = request;
                                                   different_endpoint.target = fc::url("http://127.0.0.1:1/download");
                                                   return fc::http::continuation_request{
                                                      .next_request = std::move(different_endpoint),
                                                      .options = tls_request_options(),
                                                   };
                                                }),
                         fc::exception, [](const fc::exception& error) {
                            return error.to_detail_string().find("does not match the retained endpoint") !=
                                   std::string::npos;
                         });
}

/// A rejecting continuation closes the retained connection before a follow-up is written.
BOOST_AUTO_TEST_CASE(connection_affine_continuation_rejection_fails_closed) {
   std::atomic_uint32_t requests_observed{1};
   scripted_http_server server(
      [&](tcp::socket& socket, const std::atomic_bool&) {
         BOOST_REQUIRE(write_bytes(
            socket,
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: 5\r\n"
            "Connection: keep-alive\r\n\r\n"
            "first"));
         if (!read_request_header(socket).empty())
            ++requests_observed;
      });
   fc::http::transport transport;
   const fc::http::request request{
      .method = fc::http::request_method::get,
      .target = server_url(server),
   };

   BOOST_CHECK_THROW(
      transport.perform_then(
         request,
         tls_request_options(),
         [](const fc::http::response&)
            -> fc::http::continuation_request {
            FC_THROW("continuation rejected");
         }),
      fc::exception);
   BOOST_CHECK_EQUAL(requests_observed.load(), 1U);
}

/// Cancellation emitted by the hook closes the retained socket before request two is written.
BOOST_AUTO_TEST_CASE(connection_affine_continuation_honors_slot_cancellation_before_write) {
   std::atomic_uint32_t requests_observed{1};
   scripted_http_server server(
      [&](tcp::socket& socket, const std::atomic_bool&) {
         BOOST_REQUIRE(write_bytes(
            socket,
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: 5\r\n"
            "Connection: keep-alive\r\n\r\n"
            "first"));
         if (!read_request_header(socket).empty())
            ++requests_observed;
      });
   const fc::http::request request{
      .method = fc::http::request_method::get,
      .target = server_url(server),
   };
   boost::asio::io_context io;
   fc::http::client client(io.get_executor());
   boost::asio::cancellation_signal cancellation;
   std::exception_ptr failure;

   boost::asio::co_spawn(
      io,
      [&]() -> boost::asio::awaitable<void> {
         (void)co_await client.async_request_then(
            request,
            tls_request_options(),
            [&](const fc::http::response&) {
               cancellation.emit(
                  boost::asio::cancellation_type::terminal);
               return fc::http::continuation_request{
                  .next_request = request,
                  .options = tls_request_options(),
               };
            },
            cancellation.slot());
      },
      [&](std::exception_ptr operation_failure) {
         failure = std::move(operation_failure);
      });
   io.run();

   BOOST_REQUIRE(failure);
   BOOST_CHECK_EXCEPTION(
      std::rethrow_exception(failure),
      fc::canceled_exception,
      [](const fc::exception& error) {
         return error.to_detail_string().find("cancelled") !=
                std::string::npos;
      });
   BOOST_CHECK_EQUAL(requests_observed.load(), 1U);
}

/// The blocking adapter checks its predicate at the hook boundary, not only on its timer.
BOOST_AUTO_TEST_CASE(connection_affine_continuation_honors_predicate_cancellation_before_write) {
   std::atomic_uint32_t requests_observed{1};
   scripted_http_server server(
      [&](tcp::socket& socket, const std::atomic_bool&) {
         BOOST_REQUIRE(write_bytes(
            socket,
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: 5\r\n"
            "Connection: keep-alive\r\n\r\n"
            "first"));
         if (!read_request_header(socket).empty())
            ++requests_observed;
      });
   fc::http::transport transport;
   const fc::http::request request{
      .method = fc::http::request_method::get,
      .target = server_url(server),
   };
   std::atomic_bool cancelled{false};
   auto options = tls_request_options();
   options.cancel_check =
      [&cancelled] { return cancelled.load(); };

   BOOST_CHECK_EXCEPTION(
      transport.perform_then(
         request,
         options,
         [&](const fc::http::response&) {
            cancelled = true;
            auto next_options = tls_request_options();
            next_options.cancel_check =
               [] { return false; };
            return fc::http::continuation_request{
               .next_request = request,
               .options = std::move(next_options),
            };
         }),
      fc::canceled_exception,
      [](const fc::exception& error) {
         return error.to_detail_string().find("cancelled") !=
                std::string::npos;
      });
   BOOST_CHECK_EQUAL(requests_observed.load(), 1U);
}

/// A peer-closed first connection never causes the follow-up to reconnect.
BOOST_AUTO_TEST_CASE(connection_affine_continuation_does_not_reconnect) {
   scripted_http_server server(
      [](tcp::socket& socket, const std::atomic_bool&) {
         (void)write_bytes(
            socket,
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: 5\r\n"
            "Connection: close\r\n\r\n"
            "first");
      });
   fc::http::transport transport;
   const fc::http::request request{
      .method = fc::http::request_method::get,
      .target = server_url(server),
   };

   BOOST_CHECK_EXCEPTION(
      transport.perform_then(
         request,
         tls_request_options(),
         [&](const fc::http::response&) {
            return fc::http::continuation_request{
               .next_request = request,
               .options = tls_request_options(),
            };
         }),
      fc::exception,
      [](const fc::exception& error) {
         return error.to_detail_string().find(
                   "connection-affine") !=
                std::string::npos;
      });
}

/// Each request in a continuation receives an independent total deadline.
BOOST_AUTO_TEST_CASE(connection_affine_continuation_has_per_request_deadlines) {
   scripted_http_server server(
      [](tcp::socket& socket, const std::atomic_bool& stop) {
         BOOST_REQUIRE(write_bytes(
            socket,
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: 5\r\n"
            "Connection: keep-alive\r\n\r\n"
            "first"));
         if (read_request_header(socket).empty())
            return;
         const auto respond_at =
            std::chrono::steady_clock::now() + 150ms;
         while (!stop.load() &&
                std::chrono::steady_clock::now() < respond_at) {
            std::this_thread::sleep_for(5ms);
         }
         if (!stop.load()) {
            (void)write_bytes(
               socket,
               "HTTP/1.1 200 OK\r\n"
               "Content-Length: 6\r\n"
               "Connection: close\r\n\r\n"
               "second");
         }
      });
   fc::http::transport transport;
   const fc::http::request request{
      .method = fc::http::request_method::get,
      .target = server_url(server),
   };
   auto first_options = tls_request_options();
   first_options.timeouts.total = fc::milliseconds(100);

   const auto response =
      transport.perform_then(
         request,
         first_options,
         [&](const fc::http::response&) {
            auto next_options = tls_request_options();
            next_options.timeouts.total =
               fc::milliseconds(500);
            return fc::http::continuation_request{
               .next_request = request,
               .options = std::move(next_options),
            };
         });

   BOOST_CHECK_EQUAL(response.body, "second");
}

/// A private CA file augments trust and accepts the matching DNS identity with SNI.
BOOST_AUTO_TEST_CASE(private_ca_file_accepts_matching_dns_and_sends_sni) {
   https_response_server server(tls_fixture("dns.pem"), tls_fixture("dns.key"));
   const auto response = tls_get(
      server, "localhost", fc::http::transport_options{.additional_ca_file = tls_fixture("ca.pem")});

   BOOST_CHECK_EQUAL(response.status, 200U);
   BOOST_CHECK_EQUAL(response.body, "{}");
   BOOST_CHECK_EQUAL(server.sni(), "localhost");
}

/// A hashed private CA directory augments trust for a matching DNS identity.
BOOST_AUTO_TEST_CASE(private_ca_path_accepts_matching_dns) {
   fc::temp_directory temp;
   const auto hashed_ca =
      temp.path() / certificate_hash_filename(tls_fixture("ca.pem"));
   std::filesystem::copy_file(tls_fixture("ca.pem"), hashed_ca);
   https_response_server server(tls_fixture("dns.pem"), tls_fixture("dns.key"));

   const auto response = tls_get(
      server, "localhost", fc::http::transport_options{.additional_ca_path = temp.path()});
   BOOST_CHECK_EQUAL(response.status, 200U);
}

/// IP-literal verification uses the certificate IP SAN and omits DNS SNI.
BOOST_AUTO_TEST_CASE(private_ca_accepts_matching_ip_without_sni) {
   https_response_server server(tls_fixture("ip.pem"), tls_fixture("ip.key"));
   const auto response = tls_get(
      server, "127.0.0.1", fc::http::transport_options{.additional_ca_file = tls_fixture("ca.pem")});

   BOOST_CHECK_EQUAL(response.status, 200U);
   BOOST_CHECK(server.sni().empty());
}

/// IPv6 literals use IP-SAN verification and omit SNI when loopback IPv6 is available.
BOOST_AUTO_TEST_CASE(private_ca_accepts_matching_ipv6_without_sni) {
   if (!supports_ipv6_loopback()) {
      BOOST_TEST_MESSAGE("IPv6 loopback unavailable; skipping IPv6 TLS identity test");
      return;
   }

   https_response_server server(
      tls_fixture("ipv6.pem"),
      tls_fixture("ipv6.key"),
      boost::asio::ip::address_v6::loopback());
   const auto response = tls_get(
      server,
      "::1",
      fc::http::transport_options{
         .additional_ca_file =
            tls_fixture("secondary-ca.pem"),
      });

   BOOST_CHECK_EQUAL(response.status, 200U);
   BOOST_CHECK(server.sni().empty());
}

/// A private certificate is rejected when no matching trust anchor is configured.
BOOST_AUTO_TEST_CASE(untrusted_certificate_is_rejected) {
   https_response_server server(tls_fixture("dns.pem"), tls_fixture("dns.key"));
   BOOST_CHECK_EXCEPTION(
      tls_get(server, "localhost", {}),
      fc::exception,
      [](const fc::exception& error) {
         return error.to_detail_string().find("tls_verification") !=
                std::string::npos;
      });
   BOOST_CHECK(!server.request_observed());
}

/// DNS certificate identity is checked against the original URL host, not its resolved address.
BOOST_AUTO_TEST_CASE(dns_identity_mismatch_is_rejected) {
   https_response_server server(
      tls_fixture("dns.pem"),
      tls_fixture("dns.key"));
   auto transport =
      fc::http::transport_test_access::create(
         fc::http::transport_options{
            .additional_ca_file =
               tls_fixture("ca.pem"),
         },
         [&](const std::string&,
             const std::string&,
             fc::time_point,
             fc::http::detail::resolver_complete_fn complete) {
            complete(
               std::nullopt,
               {{
                  .address = "127.0.0.1",
                  .port = server.port(),
               }});
            return [] {};
         });

   BOOST_CHECK_EXCEPTION(
      transport.perform(
         fc::http::request{
            .method = fc::http::request_method::get,
            .target =
               fc::url(
                  "https://wrong.example:" +
                  std::to_string(server.port()) + "/"),
         },
         tls_request_options()),
      fc::exception,
      [](const fc::exception& error) {
         return error.to_detail_string().find("tls_hostname") !=
                std::string::npos;
      });
   BOOST_CHECK(!server.request_observed());
   BOOST_CHECK_EQUAL(server.sni(), "wrong.example");
}

/// A trusted DNS-only certificate cannot authenticate an IP-literal endpoint.
BOOST_AUTO_TEST_CASE(ip_identity_mismatch_is_rejected) {
   https_response_server server(tls_fixture("dns.pem"), tls_fixture("dns.key"));
   BOOST_CHECK_EXCEPTION(
      tls_get(
         server, "127.0.0.1", fc::http::transport_options{.additional_ca_file = tls_fixture("ca.pem")}),
      fc::exception,
      [](const fc::exception& error) {
         return error.to_detail_string().find("tls_ip") !=
                std::string::npos;
      });
   BOOST_CHECK(!server.request_observed());
}

/// An expired leaf is rejected even when its issuing private CA is trusted.
BOOST_AUTO_TEST_CASE(expired_certificate_is_rejected) {
   https_response_server server(tls_fixture("expired.pem"), tls_fixture("expired.key"));
   BOOST_CHECK_EXCEPTION(
      tls_get(
         server, "localhost", fc::http::transport_options{.additional_ca_file = tls_fixture("ca.pem")}),
      fc::exception,
      [](const fc::exception& error) {
         return error.to_detail_string().find("tls_verification") != std::string::npos;
      });
   BOOST_CHECK(!server.request_observed());
}

/// A not-yet-valid leaf is rejected before the server observes HTTP bytes.
BOOST_AUTO_TEST_CASE(not_yet_valid_certificate_is_rejected) {
   https_response_server server(
      tls_fixture("future.pem"),
      tls_fixture("future.key"));
   BOOST_CHECK_EXCEPTION(
      tls_get(
         server,
         "localhost",
         fc::http::transport_options{
            .additional_ca_file =
               tls_fixture("secondary-ca.pem"),
         }),
      fc::exception,
      [](const fc::exception& error) {
         return error.to_detail_string().find("tls_verification") !=
                std::string::npos;
      });
   BOOST_CHECK(!server.request_observed());
}

/// Per-client trust contexts remain isolated when requests execute concurrently.
BOOST_AUTO_TEST_CASE(concurrent_clients_do_not_share_private_ca_state) {
   https_response_server trusted_server(
      tls_fixture("dns.pem"),
      tls_fixture("dns.key"));
   https_response_server untrusted_server(
      tls_fixture("dns.pem"),
      tls_fixture("dns.key"));
   std::atomic_bool trusted_succeeded{false};
   std::atomic_bool untrusted_rejected{false};

   std::thread trusted([&] {
      try {
         trusted_succeeded =
            tls_get(
               trusted_server,
               "localhost",
               fc::http::transport_options{
                  .additional_ca_file =
                     tls_fixture("ca.pem"),
               })
               .status == 200;
      } catch (...) {
      }
   });
   std::thread untrusted([&] {
      try {
         (void)tls_get(
            untrusted_server,
            "localhost",
            {});
      } catch (const fc::exception&) {
         untrusted_rejected = true;
      }
   });
   trusted.join();
   untrusted.join();

   BOOST_CHECK(trusted_succeeded.load());
   BOOST_CHECK(untrusted_rejected.load());
   BOOST_CHECK(!untrusted_server.request_observed());
}

/// Missing and malformed custom trust configuration fails during client construction.
BOOST_AUTO_TEST_CASE(invalid_custom_ca_configuration_is_rejected) {
   fc::temp_directory temp;
   const auto missing = temp.path() / "missing.pem";
   const auto empty = temp.path() / "empty.pem";
   const auto malformed = temp.path() / "malformed.pem";
   {
      std::ofstream empty_output(empty);
      std::ofstream output(malformed);
      output << "not a certificate";
   }

   BOOST_CHECK_THROW(
      fc::http::transport(fc::http::transport_options{.additional_ca_file = missing}),
      fc::exception);
   BOOST_CHECK_THROW(
      fc::http::transport(fc::http::transport_options{.additional_ca_file = empty}),
      fc::exception);
   BOOST_CHECK_THROW(
      fc::http::transport(fc::http::transport_options{.additional_ca_file = malformed}),
      fc::exception);
   BOOST_CHECK_THROW(
      fc::http::transport(
         fc::http::transport_options{
            .additional_ca_path = temp.path(),
         }),
      fc::exception);
}

/// An explicit HTTP proxy receives absolute-form request targets without resolving the origin locally.
BOOST_AUTO_TEST_CASE(explicit_http_proxy_uses_absolute_request_target) {
   std::mutex observed_mutex;
   std::string observed_request;
   scripted_http_server proxy(
      [&](tcp::socket& socket, const std::atomic_bool&) {
         {
            std::scoped_lock lock(observed_mutex);
            observed_request = read_request_header(socket);
         }
         write_bytes(socket, fixed_length_header(2) + "{}");
      },
      false);
   fc::http::transport transport(
      fc::http::transport_options{
         .proxy =
            "http://127.0.0.1:" + std::to_string(proxy.port()),
      });

   const auto response = transport.perform(
      fc::http::request{
         .method = fc::http::request_method::get,
         .target =
            fc::url("http://origin.invalid:8123/rpc?commitment=finalized"),
      },
      tls_request_options());

   BOOST_REQUIRE_EQUAL(response.status, 200U);
   std::scoped_lock lock(observed_mutex);
   BOOST_CHECK(
      observed_request.starts_with(
         "GET http://origin.invalid:8123/rpc?commitment=finalized HTTP/1.1\r\n"));
}

/// HTTPS proxy tunnels use authority-form targets with an explicit port, including IPv6 literals.
BOOST_AUTO_TEST_CASE(explicit_proxy_connect_uses_ipv6_authority_and_port) {
   std::mutex observed_mutex;
   std::string observed_request;
   scripted_http_server proxy(
      [&](tcp::socket& socket, const std::atomic_bool&) {
         {
            std::scoped_lock lock(observed_mutex);
            observed_request = read_request_header(socket);
         }
         write_bytes(
            socket,
            "HTTP/1.1 407 Proxy Authentication Required\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n\r\n");
      },
      false);
   fc::http::transport transport(
      fc::http::transport_options{
         .proxy =
            "http://127.0.0.1:" + std::to_string(proxy.port()),
      });

   BOOST_CHECK_EXCEPTION(
      transport.perform(
         fc::http::request{
            .method = fc::http::request_method::get,
            .target = fc::url("https://[2001:db8::1]/"),
         },
         tls_request_options()),
      fc::exception,
      [](const fc::exception& error) {
         return error.to_detail_string().find("connect") !=
                std::string::npos;
      });
   std::scoped_lock lock(observed_mutex);
   BOOST_CHECK(
      observed_request.starts_with(
         "CONNECT [2001:db8::1]:443 HTTP/1.1\r\n"));
}

/// Proxy credentials are rejected because this transport has no implicit authentication policy.
BOOST_AUTO_TEST_CASE(explicit_proxy_credentials_are_rejected) {
   BOOST_CHECK_THROW(
      fc::http::transport(
         fc::http::transport_options{
            .proxy = "http://operator:secret@127.0.0.1:8080",
         }),
      fc::exception);
}

/// Parsed URL authorities reject control characters before they can reach transport framing.
BOOST_AUTO_TEST_CASE(parsed_url_authority_control_characters_are_rejected) {
   BOOST_CHECK_THROW(
      fc::url("https://origin.invalid\r\nX-Injected: yes/"),
      fc::exception);
}

/// Programmatically constructed hosts cannot inject fields into an HTTPS proxy CONNECT request.
BOOST_AUTO_TEST_CASE(proxy_connect_rejects_programmatic_host_injection) {
   fc::http::transport transport(
      fc::http::transport_options{
         .proxy = "http://127.0.0.1:9",
      });
   const fc::url target(
      "https",
      fc::ostring{"origin.invalid\r\nX-Injected: yes"},
      {},
      {},
      fc::opath{std::filesystem::path("/")},
      {},
      {},
      std::nullopt);

   BOOST_CHECK_EXCEPTION(
      transport.perform(
         fc::http::request{
            .method = fc::http::request_method::get,
            .target = target,
         },
         tls_request_options()),
      fc::exception,
      [](const fc::exception& error) {
         const auto detail = error.to_detail_string();
         return detail.find("request_limit") != std::string::npos &&
                detail.find("X-Injected") == std::string::npos;
      });
}

/// A missing Unix socket fails promptly instead of looping at the filesystem root.
BOOST_AUTO_TEST_CASE(nonexistent_unix_socket_path_is_bounded) {
   fc::temp_directory temp;
   const auto missing =
      temp.path() / "missing.sock" / "v1" / "sign_digest";
   const fc::url target(
      "unix",
      fc::ostring{missing.string()},
      {},
      {},
      {},
      {},
      {},
      std::nullopt);
   fc::http::transport transport;
   const auto started = std::chrono::steady_clock::now();

   BOOST_CHECK_EXCEPTION(
      transport.perform(
         fc::http::request{
            .method = fc::http::request_method::post,
            .target = target,
         },
         tls_request_options()),
      fc::exception,
      [](const fc::exception& error) {
         return error.to_detail_string().find("connect") !=
                std::string::npos;
      });
   BOOST_CHECK_LT(
      std::chrono::duration_cast<std::chrono::milliseconds>(
         std::chrono::steady_clock::now() - started)
         .count(),
      max_test_elapsed_ms);
}

/// A KIOD-style exact Unix route is preserved without an added trailing slash.
BOOST_AUTO_TEST_CASE(unix_socket_exact_request_target_is_preserved) {
   fc::temp_directory temp;
   const auto socket_path =
      temp.path() / "kiod.sock";
   scripted_unix_http_server server(socket_path);
   const auto endpoint =
      socket_path /
      "v1/wallet/sign_transaction";
   const auto response =
      perform_unix_request(endpoint);
   const auto& observed_request =
      server.wait_for_request();

   BOOST_CHECK_EQUAL(
      boost::beast::http::int_to_status(
         response.status),
      boost::beast::http::status::ok);
   BOOST_CHECK(
      observed_request.starts_with(
         "POST /v1/wallet/sign_transaction HTTP/1.1\r\n"));
}

/// A Unix URL naming only its socket addresses the HTTP root.
BOOST_AUTO_TEST_CASE(unix_socket_root_request_target_is_preserved) {
   fc::temp_directory temp;
   const auto socket_path =
      temp.path() / "root.sock";
   scripted_unix_http_server server(socket_path);

   const auto response =
      perform_unix_request(socket_path);
   const auto& observed_request =
      server.wait_for_request();

   BOOST_CHECK_EQUAL(
      boost::beast::http::int_to_status(
         response.status),
      boost::beast::http::status::ok);
   BOOST_CHECK(
      observed_request.starts_with(
         "POST / HTTP/1.1\r\n"));
}

/// TLS failures do not disclose URL credentials in their diagnostic text.
BOOST_AUTO_TEST_CASE(tls_failure_diagnostic_omits_url_credentials) {
   https_response_server server(tls_fixture("dns.pem"), tls_fixture("dns.key"));
   fc::http::transport transport;
   auto options = tls_request_options();

   BOOST_CHECK_EXCEPTION(
      transport.perform(
         fc::http::request{
            .method = fc::http::request_method::get,
            .target = fc::url(
               "https://operator:super-secret@localhost:" + std::to_string(server.port()) + "/"),
         },
         options),
      fc::exception,
      [](const fc::exception& error) {
         const auto detail = error.to_detail_string();
         return detail.find("tls_verification") != std::string::npos &&
                detail.find("super-secret") == std::string::npos;
      });
}

/// The legacy facade rejects every attempt to disable HTTPS verification.
BOOST_AUTO_TEST_CASE(peer_verification_cannot_be_disabled) {
   fc::http_client client;
   BOOST_CHECK_THROW(client.set_verify_peers(false), fc::exception);
   BOOST_CHECK_NO_THROW(client.set_verify_peers(true));
}

/// A peer that never completes TLS negotiation is bounded by the connect-phase deadline.
BOOST_AUTO_TEST_CASE(tls_handshake_is_bounded) {
   scripted_http_server server(
      [](tcp::socket&, const std::atomic_bool& stop) {
         while (!stop.load())
            std::this_thread::sleep_for(10ms);
      },
      false);
   fc::http::transport transport;
   auto options = tls_request_options();
   options.timeouts.connect = fc::milliseconds(200);
   const auto start = std::chrono::steady_clock::now();

   BOOST_CHECK_EXCEPTION(
      transport.perform(
         fc::http::request{
            .method = fc::http::request_method::get,
            .target =
               fc::url("https://127.0.0.1:" + std::to_string(server.port()) + "/"),
         },
         options),
      fc::timeout_exception,
      [](const fc::exception& error) {
         return error.to_detail_string().find("timeout_connect") != std::string::npos;
      });
   const auto elapsed = std::chrono::steady_clock::now() - start;
   BOOST_CHECK_LT(
      std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
      max_test_elapsed_ms);
}

/// Cancellation interrupts a TLS peer that never completes negotiation.
BOOST_AUTO_TEST_CASE(tls_handshake_is_cancellable) {
   scripted_http_server server(
      [](tcp::socket&, const std::atomic_bool& stop) {
         while (!stop.load())
            std::this_thread::sleep_for(10ms);
      },
      false);
   delayed_cancellation cancellation;
   fc::http::transport transport;
   auto options = tls_request_options();
   options.cancel_check = cancellation.check();
   const auto start = std::chrono::steady_clock::now();

   BOOST_CHECK_EXCEPTION(
      transport.perform(
         fc::http::request{
            .method = fc::http::request_method::get,
            .target =
               fc::url(
                  "https://127.0.0.1:" +
                  std::to_string(server.port()) + "/"),
         },
         options),
      fc::canceled_exception,
      [](const fc::exception& error) {
         return error.to_detail_string().find("cancelled") !=
                std::string::npos;
      });
   const auto elapsed = std::chrono::steady_clock::now() - start;
   BOOST_CHECK_LT(
      std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
      max_test_elapsed_ms);
}

/// A peer that closes without presenting a certificate fails before any HTTP write.
BOOST_AUTO_TEST_CASE(missing_peer_certificate_is_rejected) {
   scripted_http_server server(
      [](tcp::socket& socket, const std::atomic_bool&) {
         boost::system::error_code error;
         socket.close(error);
      },
      false);
   fc::http::transport transport;

   BOOST_CHECK_EXCEPTION(
      transport.perform(
         fc::http::request{
            .method = fc::http::request_method::get,
            .target =
               fc::url(
                  "https://127.0.0.1:" +
                  std::to_string(server.port()) + "/"),
         },
         tls_request_options()),
      fc::exception,
      [](const fc::exception& error) {
         return error.to_detail_string().find("tls_handshake") !=
                std::string::npos;
      });
}

/// A stalled platform resolver service does not head-of-line block another service.
BOOST_AUTO_TEST_CASE(platform_resolver_workers_are_independent) {
   auto blocker_started =
      std::make_shared<std::promise<void>>();
   std::promise<void> release_blocker;
   auto independent_completed =
      std::make_shared<std::promise<void>>();
   auto blocker_started_future = blocker_started->get_future();
   auto release_blocker_future =
      release_blocker.get_future().share();
   auto independent_completed_future =
      independent_completed->get_future();

   fc::http::detail::
      post_platform_resolver_worker_task_for_testing(
         0,
         [started = std::move(blocker_started),
          release = std::move(release_blocker_future)] {
            started->set_value();
            release.wait();
         });
   const auto started =
      blocker_started_future.wait_for(
         std::chrono::milliseconds(normal_timeout_ms)) ==
      std::future_status::ready;
   fc::http::detail::
      post_platform_resolver_worker_task_for_testing(
         1,
         [completed =
             std::move(independent_completed)] {
            completed->set_value();
         });
   const auto completed =
      independent_completed_future.wait_for(
         std::chrono::milliseconds(normal_timeout_ms)) ==
      std::future_status::ready;
   release_blocker.set_value();

   BOOST_REQUIRE(started);
   BOOST_CHECK(completed);
}

/// Resolver notification retries a descriptor write interrupted before publishing its signal.
BOOST_AUTO_TEST_CASE(resolver_signal_retries_interrupted_write) {
   size_t calls = 0;
   const auto signaled =
      fc::http::detail::write_resolver_signal_for_testing(
         [&]() -> int64_t {
            ++calls;
            if (calls == 1) {
               errno = EINTR;
               return -1;
            }
            return 1;
         });

   BOOST_CHECK(signaled);
   BOOST_CHECK_EQUAL(calls, 2U);
}

/// DNS resolution cancels and returns when an injected platform lookup stalls.
BOOST_AUTO_TEST_CASE(dns_resolution_is_deadline_bounded) {
   std::atomic_bool resolver_started{false};
   std::atomic_bool cancel_called{false};
   fc::http::detail::resolver_complete_fn late_completion;
   {
      auto transport = fc::http::transport_test_access::create(
         {},
         [&](const std::string&,
             const std::string&,
             fc::time_point,
             fc::http::detail::resolver_complete_fn complete) {
            resolver_started = true;
            late_completion = std::move(complete);
            return [&] { cancel_called = true; };
         });
      auto options = tls_request_options();
      options.timeouts.connect = fc::milliseconds(200);
      const auto start = std::chrono::steady_clock::now();

      BOOST_CHECK_EXCEPTION(
         transport.perform(
            fc::http::request{
               .method = fc::http::request_method::get,
               .target =
                  fc::url("http://stalled-resolver.invalid/"),
            },
            options),
         fc::timeout_exception,
         [](const fc::exception& error) {
            return error.to_detail_string().find("timeout_connect") !=
                   std::string::npos;
         });
      const auto elapsed = std::chrono::steady_clock::now() - start;
      BOOST_CHECK(resolver_started.load());
      BOOST_CHECK(cancel_called.load());
      BOOST_CHECK_LT(
         std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
         max_test_elapsed_ms);
   }

   // The platform completion may arrive after its client executor is gone.
   BOOST_REQUIRE(late_completion);
   late_completion(
      std::string("cancelled"),
      {});
}

/// Cancellation remains live while an unbounded resolver callback is pending.
BOOST_AUTO_TEST_CASE(dns_resolution_is_cancellable) {
   std::atomic_bool cancel_called{false};
   delayed_cancellation cancellation;
   auto transport = fc::http::transport_test_access::create(
      {},
      [&](const std::string&,
          const std::string&,
          fc::time_point,
          fc::http::detail::resolver_complete_fn) {
         return [&] { cancel_called = true; };
      });
   auto options = tls_request_options();
   options.timeouts.connect = std::nullopt;
   options.timeouts.total = std::nullopt;
   options.timeouts.inherit_task_deadline = false;
   options.cancel_check = cancellation.check();

   BOOST_CHECK_EXCEPTION(
      transport.perform(
         fc::http::request{
            .method = fc::http::request_method::get,
            .target =
               fc::url("http://cancelled-resolver.invalid/"),
         },
         options),
      fc::canceled_exception,
      [](const fc::exception& error) {
         return error.to_detail_string().find("cancelled") !=
                std::string::npos;
      });
   BOOST_CHECK(cancel_called.load());
}

/// Resolver startup failures are normalized into the transport's fixed DNS category.
BOOST_AUTO_TEST_CASE(dns_resolver_start_failure_is_classified) {
   auto transport = fc::http::transport_test_access::create(
      {},
      [](const std::string&,
         const std::string&,
         fc::time_point,
         fc::http::detail::resolver_complete_fn)
         -> fc::http::detail::resolver_cancel_fn {
         throw std::runtime_error("injected resolver startup failure");
      });

   BOOST_CHECK_EXCEPTION(
      transport.perform(
         fc::http::request{
            .method = fc::http::request_method::get,
            .target = fc::url("http://resolver-start.invalid/"),
         },
         tls_request_options()),
      fc::exception,
      [](const fc::exception& error) {
         return error.to_detail_string().find("dns") != std::string::npos;
      });
}

/// A successful injected DNS result is used for the bounded connection attempt.
BOOST_AUTO_TEST_CASE(dns_resolution_accepts_completed_lookup) {
   boost::asio::io_context io;
   tcp::acceptor closed_listener(
      io,
      tcp::endpoint(
         boost::asio::ip::address_v4::loopback(),
         0));
   const auto closed_port =
      closed_listener.local_endpoint().port();
   boost::system::error_code close_error;
   closed_listener.close(close_error);
   std::atomic_uint32_t resolve_count{0};
   auto resolver =
      [&](const std::string&,
          const std::string&,
          fc::time_point,
          fc::http::detail::resolver_complete_fn complete) {
         ++resolve_count;
         complete(
            std::nullopt,
            {{
               .address = "127.0.0.1",
               .port = closed_port,
            }});
         return [] {};
      };
   auto transport =
      fc::http::transport_test_access::create({}, resolver);

   BOOST_CHECK_EXCEPTION(
      transport.perform(
         fc::http::request{
            .method = fc::http::request_method::get,
            .target = fc::url("http://resolved.invalid/"),
         },
         tls_request_options()),
      fc::exception,
      [](const fc::exception& error) {
         return error.to_detail_string().find("connect") !=
                std::string::npos;
      });
   BOOST_CHECK_EQUAL(resolve_count.load(), 1U);
}

/// DNS TTL and connection-failure refresh are independent cache policies.
BOOST_AUTO_TEST_CASE(dns_cache_refresh_policy_is_preserved) {
   boost::asio::io_context io;
   tcp::acceptor closed_listener(io, tcp::endpoint(boost::asio::ip::address_v4::loopback(), 0));
   const auto closed_port = closed_listener.local_endpoint().port();
   boost::system::error_code close_error;
   closed_listener.close(close_error);

   const auto exercise = [&](std::optional<fc::microseconds> cache_timeout,
                             bool refresh_on_connection_failure) {
      std::atomic_uint32_t resolve_count{0};
      auto transport = fc::http::transport_test_access::create(
         fc::http::transport_options{
            .dns_cache_timeout = cache_timeout,
            .refresh_dns_on_connection_failure = refresh_on_connection_failure,
         },
         [&](const std::string&, const std::string&, fc::time_point, fc::http::detail::resolver_complete_fn complete) {
            ++resolve_count;
            complete(std::nullopt, {
                                      {
                                       .address = "127.0.0.1",
                                       .port = closed_port,
                                       }
            });
            return [] {};
         });
      for (size_t attempt = 0; attempt < 2; ++attempt) {
         BOOST_CHECK_THROW(transport.perform(
                              fc::http::request{
                                 .method = fc::http::request_method::get,
                                 .target = fc::url("http://cache-policy.invalid/"),
                              },
                              tls_request_options()),
                           fc::exception);
      }
      return resolve_count.load();
   };

   BOOST_CHECK_EQUAL(exercise(fc::seconds(60), true), 2U);
   BOOST_CHECK_EQUAL(exercise(std::nullopt, true), 2U);
   BOOST_CHECK_EQUAL(exercise(std::nullopt, false), 1U);
   BOOST_CHECK_EQUAL(
      exercise(fc::microseconds(0), false),
      2U);
   BOOST_CHECK_THROW(
      fc::http::transport(
         fc::http::transport_options{
            .dns_cache_timeout =
               fc::microseconds(-1),
         }),
      fc::exception);
}

/// Non-idempotent requests cannot opt into automatic replay.
BOOST_AUTO_TEST_CASE(retries_require_explicit_idempotency) {
   fc::http::transport transport;
   auto options = tls_request_options();
   options.idempotent = false;
   options.retry.max_attempts = 2;

   BOOST_CHECK_THROW(
      transport.perform(
         fc::http::request{
            .method = fc::http::request_method::post,
            .target = fc::url("http://127.0.0.1:1/"),
            .body = "{}",
         },
         options),
      fc::exception);
}

/// Exhausted retries produce a stable category without replaying more than the configured attempts.
BOOST_AUTO_TEST_CASE(idempotent_retry_exhaustion_is_bounded) {
   boost::asio::io_context io;
   tcp::acceptor closed_listener(
      io,
      tcp::endpoint(
         boost::asio::ip::address_v4::loopback(),
         0));
   const auto closed_port =
      closed_listener.local_endpoint().port();
   boost::system::error_code close_error;
   closed_listener.close(close_error);

   fc::http::transport transport;
   auto options = tls_request_options();
   options.retry.max_attempts = 2;
   options.retry.initial_backoff = fc::microseconds(0);
   options.retry.max_backoff = fc::microseconds(0);

   BOOST_CHECK_EXCEPTION(
      transport.perform(
         fc::http::request{
            .method = fc::http::request_method::get,
            .target =
               fc::url(
                  "http://127.0.0.1:" +
                  std::to_string(closed_port) + "/"),
         },
         options),
      fc::exception,
      [](const fc::exception& error) {
         return error.to_detail_string().find("retry_exhausted") !=
                std::string::npos;
      });
}

/// Caller-provided request headers are bounded before any connection attempt.
BOOST_AUTO_TEST_CASE(oversized_request_headers_are_rejected_before_send) {
   fc::http::transport transport;
   auto options = tls_request_options();
   options.max_request_header_bytes = 512;

   BOOST_CHECK_EXCEPTION(
      transport.perform(
         fc::http::request{
            .method = fc::http::request_method::get,
            .target = fc::url("http://127.0.0.1:1/"),
            .headers = {{"X-Large", std::string(1'024, 'x')}},
         },
         options),
      fc::exception,
      [](const fc::exception& error) {
         return error.to_detail_string().find("request_limit") != std::string::npos;
      });
}

/// A long request target is included in the request-header admission budget.
BOOST_AUTO_TEST_CASE(oversized_request_target_is_rejected_before_send) {
   fc::http::transport transport;
   auto options = tls_request_options();
   options.max_request_header_bytes = 512;
   const auto before = fc::http::get_metrics_snapshot();

   BOOST_CHECK_EXCEPTION(
      transport.perform(
         fc::http::request{
            .method = fc::http::request_method::get,
            .target =
               fc::url(
                  "http://127.0.0.1:1/" +
                  std::string(1'024, 'x')),
         },
         options),
      fc::exception,
      [](const fc::exception& error) {
         return error.to_detail_string().find("request_limit") !=
                std::string::npos;
      });

   const auto after = fc::http::get_metrics_snapshot();
   const auto request_limit_index =
      magic_enum::enum_index(fc::http::failure_kind::request_limit);
   BOOST_REQUIRE(request_limit_index);
   BOOST_CHECK_EQUAL(after.requests, before.requests + 1);
   BOOST_CHECK_EQUAL(after.request_bytes, before.request_bytes);
   BOOST_CHECK_EQUAL(
      after.failures[*request_limit_index],
      before.failures[*request_limit_index] + 1);
}

/// Callers cannot override framing and routing headers owned by the transport.
BOOST_AUTO_TEST_CASE(transport_controlled_request_headers_are_rejected) {
   fc::http::transport transport;

   BOOST_CHECK_EXCEPTION(
      transport.perform(
         fc::http::request{
            .method = fc::http::request_method::post,
            .target = fc::url("http://127.0.0.1:1/"),
            .body = "{}",
            .headers = {{"Content-Length", "0"}},
         },
         tls_request_options()),
      fc::exception,
      [](const fc::exception& error) {
         return error.to_detail_string().find("request_limit") !=
                std::string::npos;
      });
}

/// Aggregate response headers are bounded independently from the response body.
BOOST_AUTO_TEST_CASE(oversized_response_headers_are_rejected) {
   scripted_http_server server([](tcp::socket& socket, const std::atomic_bool&) {
      write_bytes(
         socket,
         "HTTP/1.1 200 OK\r\nX-Large: " + std::string(1'024, 'x') +
            "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
   });
   fc::http::transport transport;
   auto options = tls_request_options();
   options.max_response_header_bytes = 512;

   BOOST_CHECK_EXCEPTION(
      transport.perform(
         fc::http::request{
            .method = fc::http::request_method::get,
            .target =
               fc::url("http://127.0.0.1:" + std::to_string(server.port()) + "/"),
         },
         options),
      fc::exception,
      [](const fc::exception& error) {
         return error.to_detail_string().find("response_limit") != std::string::npos;
      });
}

/// Trickle-fed response headers cannot extend the absolute header-phase budget.
BOOST_AUTO_TEST_CASE(slow_response_headers_time_out) {
   scripted_http_server server([](tcp::socket& socket, const std::atomic_bool& stop) {
      if (!write_bytes(socket, "HTTP/1.1 200 OK\r\nX-Slow: "))
         return;
      while (!stop.load()) {
         if (!write_bytes(socket, "x"))
            return;
         std::this_thread::sleep_for(50ms);
      }
   });
   fc::http::transport transport;
   auto options = tls_request_options();
   options.timeouts.connect = fc::milliseconds(10);
   options.timeouts.header = fc::milliseconds(200);
   const auto start = std::chrono::steady_clock::now();

   BOOST_CHECK_EXCEPTION(
      transport.perform(
         fc::http::request{
            .method = fc::http::request_method::get,
            .target =
               fc::url("http://127.0.0.1:" + std::to_string(server.port()) + "/"),
         },
         options),
      fc::timeout_exception,
      [](const fc::exception& error) {
         return error.to_detail_string().find("timeout_header") != std::string::npos;
      });
   const auto elapsed = std::chrono::steady_clock::now() - start;
   BOOST_CHECK_LT(
      std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
      max_test_elapsed_ms);
}

/// A continuously progressing body cannot extend its aggregate read-phase deadline.
BOOST_AUTO_TEST_CASE(slow_progressing_response_body_times_out) {
   scripted_http_server server([](tcp::socket& socket, const std::atomic_bool& stop) {
      if (!write_bytes(socket, fixed_length_header(1'000)))
         return;
      while (!stop.load()) {
         if (!write_bytes(socket, "x"))
            return;
         std::this_thread::sleep_for(40ms);
      }
   });
   fc::http::transport transport;
   auto options = tls_request_options();
   options.timeouts.read = fc::milliseconds(200);
   options.timeouts.idle = std::nullopt;
   options.timeouts.total = std::nullopt;
   const auto start = std::chrono::steady_clock::now();

   BOOST_CHECK_EXCEPTION(
      transport.perform(
         fc::http::request{
            .method = fc::http::request_method::get,
            .target =
               fc::url(
                  "http://127.0.0.1:" +
                  std::to_string(server.port()) + "/"),
         },
         options),
      fc::timeout_exception,
      [](const fc::exception& error) {
         return error.to_detail_string().find("timeout_read") !=
                std::string::npos;
      });
   const auto elapsed = std::chrono::steady_clock::now() - start;
   BOOST_CHECK_LT(
      std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
      max_test_elapsed_ms);
}

/// Cancellation remains active while a peer accepts but does not drain a bounded request body.
BOOST_AUTO_TEST_CASE(request_upload_can_be_cancelled) {
   scripted_http_server server(
      [](tcp::socket&, const std::atomic_bool& stop) {
         while (!stop.load())
            std::this_thread::sleep_for(10ms);
      },
      false);
   delayed_cancellation cancellation;
   fc::http::transport transport;
   auto options = tls_request_options();
   options.max_request_body_bytes = blocked_request_body_bytes * 2;
   options.cancel_check = cancellation.check();
   const auto start = std::chrono::steady_clock::now();

   BOOST_CHECK_EXCEPTION(
      transport.perform(
         fc::http::request{
            .method = fc::http::request_method::post,
            .target =
               fc::url("http://127.0.0.1:" + std::to_string(server.port()) + "/"),
            .body = std::string(blocked_request_body_bytes, 'x'),
         },
         options),
      fc::canceled_exception,
      [](const fc::exception& error) {
         return error.to_detail_string().find("cancelled") != std::string::npos;
      });
   const auto elapsed = std::chrono::steady_clock::now() - start;
   BOOST_CHECK_LT(
      std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
      max_test_elapsed_ms);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(http_async_client_tests)

/// A continuation hook executes on the client strand rather than the initiating executor.
BOOST_AUTO_TEST_CASE(continuation_hook_runs_on_client_executor) {
   scripted_http_server server(
      [](tcp::socket& socket, const std::atomic_bool&) {
         BOOST_REQUIRE(write_bytes(
            socket,
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: 5\r\n"
            "Connection: keep-alive\r\n\r\n"
            "first"));
         if (!read_request_header(socket).empty()) {
            (void)write_bytes(
               socket,
               "HTTP/1.1 200 OK\r\n"
               "Content-Length: 6\r\n"
               "Connection: close\r\n\r\n"
               "second");
         }
      });

   boost::asio::io_context client_io;
   auto client_work =
      boost::asio::make_work_guard(client_io);
   std::promise<std::thread::id> client_thread;
   auto client_thread_future =
      client_thread.get_future();
   std::jthread client_worker(
      [&] {
         client_thread.set_value(
            std::this_thread::get_id());
         client_io.run();
      });
   const auto expected_thread =
      client_thread_future.get();

   boost::asio::io_context caller_io;
   fc::http::client client(client_io.get_executor());
   const fc::http::request request{
      .method = fc::http::request_method::get,
      .target = server_url(server),
   };
   std::thread::id hook_thread;
   std::exception_ptr failure;
   boost::asio::co_spawn(
      caller_io,
      [&]() -> boost::asio::awaitable<void> {
         const auto response =
            co_await client.async_request_then(
               request,
               tls_request_options(),
               [&](const fc::http::response& first) {
                  hook_thread =
                     std::this_thread::get_id();
                  BOOST_CHECK_EQUAL(first.body, "first");
                  return fc::http::continuation_request{
                     .next_request = request,
                     .options = tls_request_options(),
                  };
               });
         BOOST_CHECK_EQUAL(response.body, "second");
      },
      [&](std::exception_ptr operation_failure) {
         failure = std::move(operation_failure);
      });
   caller_io.run();
   client_work.reset();
   client_worker.join();

   if (failure)
      std::rethrow_exception(failure);
   BOOST_CHECK(hook_thread == expected_thread);
   BOOST_CHECK(hook_thread != std::this_thread::get_id());
}

/// async_open returns after the response head and the pull reader incrementally consumes the body.
BOOST_AUTO_TEST_CASE(open_exposes_headers_before_a_delayed_body) {
   scripted_http_server server(
      [](tcp::socket& socket, const std::atomic_bool& stop) {
         if (!write_bytes(socket, fixed_length_header(exact_body.size())))
            return;
         const auto body_at = std::chrono::steady_clock::now() + 300ms;
         while (!stop.load() &&
                std::chrono::steady_clock::now() < body_at) {
            std::this_thread::sleep_for(5ms);
         }
         if (!stop.load())
            (void)write_bytes(socket, exact_body);
      });

   boost::asio::io_context io;
   fc::http::client client(io.get_executor());
   std::exception_ptr failure;
   boost::asio::co_spawn(
      io,
      [&]() -> boost::asio::awaitable<void> {
         const auto started = std::chrono::steady_clock::now();
         auto reader = co_await client.async_open(
            fc::http::request{
               .method = fc::http::request_method::get,
               .target = server_url(server),
            },
            tls_request_options());
         const auto header_elapsed =
            std::chrono::steady_clock::now() - started;
         BOOST_CHECK_LT(
            std::chrono::duration_cast<std::chrono::milliseconds>(
               header_elapsed)
               .count(),
            250);
         BOOST_CHECK_EQUAL(reader.head().status, 200);
         BOOST_REQUIRE(reader.head().content_length);
         BOOST_CHECK_EQUAL(
            *reader.head().content_length,
            exact_body.size());

         std::string body;
         std::array<char, 3> increment{};
         while (!reader.done()) {
            const auto bytes = co_await reader.async_read_some(
               boost::asio::buffer(increment));
            body.append(increment.data(), bytes);
         }
         BOOST_CHECK_EQUAL(body, exact_body);
      },
      [&](std::exception_ptr operation_failure) {
         failure = std::move(operation_failure);
      });
   io.run();
   if (failure)
      std::rethrow_exception(failure);
}

/// One cancellation slot remains live after async_open returns and interrupts a pending body read.
BOOST_AUTO_TEST_CASE(cancellation_slot_spans_headers_and_body) {
   scripted_http_server server(
      [](tcp::socket& socket, const std::atomic_bool& stop) {
         if (!write_bytes(socket, fixed_length_header(exact_body.size())))
            return;
         while (!stop.load())
            std::this_thread::sleep_for(5ms);
      });

   boost::asio::io_context io;
   fc::http::client client(io.get_executor());
   boost::asio::cancellation_signal cancellation;
   boost::asio::steady_timer cancel_timer(io);
   cancel_timer.expires_after(100ms);
   cancel_timer.async_wait(
      [&](const boost::system::error_code& error) {
         if (!error) {
            cancellation.emit(
               boost::asio::cancellation_type::terminal);
         }
      });

   std::exception_ptr failure;
   boost::asio::co_spawn(
      io,
      [&]() -> boost::asio::awaitable<void> {
         auto reader = co_await client.async_open(
            fc::http::request{
               .method = fc::http::request_method::get,
               .target = server_url(server),
            },
            tls_request_options(),
            cancellation.slot());
         std::array<char, exact_body_bytes> body{};
         bool cancelled = false;
         try {
            (void)co_await reader.async_read_some(
               boost::asio::buffer(body));
         } catch (const fc::canceled_exception& error) {
            cancelled =
               error.to_detail_string().find("cancelled") !=
               std::string::npos;
         }
         BOOST_CHECK(cancelled);
      },
      [&](std::exception_ptr operation_failure) {
         failure = std::move(operation_failure);
      });
   io.run();
   if (failure)
      std::rethrow_exception(failure);
}

/// Destroying a reader while its body read is pending closes safely after the operation resumes.
BOOST_AUTO_TEST_CASE(active_body_read_retains_its_implementation) {
   scripted_http_server server(
      [](tcp::socket& socket, const std::atomic_bool& stop) {
         if (!write_bytes(socket, fixed_length_header(exact_body.size())))
            return;
         while (!stop.load())
            std::this_thread::sleep_for(5ms);
      });

   boost::asio::io_context io;
   fc::http::client client(io.get_executor());
   std::exception_ptr failure;
   boost::asio::co_spawn(
      io,
      [&]() -> boost::asio::awaitable<void> {
         auto reader = co_await client.async_open(
            fc::http::request{
               .method = fc::http::request_method::get,
               .target = server_url(server),
            },
            tls_request_options());
         std::array<char, exact_body_bytes> body{};
         boost::asio::steady_timer read_complete(io);
         read_complete.expires_at(
            std::chrono::steady_clock::time_point::max());
         bool completed = false;
         std::exception_ptr read_failure;
         boost::asio::co_spawn(
            io,
            reader.async_read_some(boost::asio::buffer(body)),
            [&](std::exception_ptr operation_failure, size_t) {
               read_failure = std::move(operation_failure);
               completed = true;
               read_complete.cancel();
            });

         boost::asio::steady_timer let_read_start(io);
         let_read_start.expires_after(50ms);
         co_await let_read_start.async_wait(boost::asio::use_awaitable);
         reader = fc::http::response_reader{};

         boost::system::error_code wait_error;
         co_await read_complete.async_wait(
            boost::asio::redirect_error(
               boost::asio::use_awaitable,
               wait_error));
         BOOST_CHECK(completed);
         BOOST_CHECK(read_failure);
      },
      [&](std::exception_ptr operation_failure) {
         failure = std::move(operation_failure);
      });
   io.run();
   if (failure)
      std::rethrow_exception(failure);
}

/// Destroying a partial reader closes its lease instead of returning that connection to the cache.
BOOST_AUTO_TEST_CASE(abandoned_body_is_never_reused) {
   std::atomic_uint32_t connection_index{0};
   scripted_http_server server(
      [&](tcp::socket& socket, const std::atomic_bool&) {
         if (connection_index.fetch_add(1) == 0) {
            if (!write_bytes(
                   socket,
                   "HTTP/1.1 200 OK\r\n"
                   "Content-Length: 8\r\n"
                   "Connection: keep-alive\r\n\r\n1")) {
               return;
            }
            std::array<char, 256> ignored{};
            boost::system::error_code error;
            (void)socket.read_some(
               boost::asio::buffer(ignored),
               error);
            return;
         }
         (void)write_bytes(
            socket,
            fixed_length_header(exact_body.size()) +
               std::string(exact_body));
      },
      true,
      2);

   boost::asio::io_context io;
   fc::http::client client(io.get_executor());
   std::exception_ptr failure;
   boost::asio::co_spawn(
      io,
      [&]() -> boost::asio::awaitable<void> {
         {
            auto partial = co_await client.async_open(
               fc::http::request{
                  .method = fc::http::request_method::get,
                  .target = server_url(server),
               },
               tls_request_options());
            BOOST_CHECK(!partial.done());
         }

         const auto complete =
            co_await client.async_request(
               fc::http::request{
                  .method = fc::http::request_method::get,
                  .target = server_url(server),
               },
               tls_request_options());
         BOOST_CHECK_EQUAL(complete.status, 200);
         BOOST_CHECK_EQUAL(complete.body, exact_body);
      },
      [&](std::exception_ptr operation_failure) {
         failure = std::move(operation_failure);
      });
   io.run();
   if (failure)
      std::rethrow_exception(failure);
   BOOST_CHECK_EQUAL(connection_index.load(), 2U);
}

/// A failed header attempt must not finalize metrics before a retry succeeds.
BOOST_AUTO_TEST_CASE(header_retry_records_only_the_final_outcome) {
   std::atomic_uint32_t connection_index{0};
   scripted_http_server server(
      [&](tcp::socket& socket, const std::atomic_bool&) {
         const auto request = read_request_header(socket);
         const auto header_end = request.find("\r\n\r\n");
         if (header_end == std::string::npos)
            return;
         const auto buffered_body_bytes =
            request.size() - header_end - 4;
         boost::system::error_code error;
         if (buffered_body_bytes < exact_body.size()) {
            std::array<char, exact_body_bytes> remaining_body{};
            boost::asio::read(
               socket,
               boost::asio::buffer(
                  remaining_body.data(),
                  exact_body.size() - buffered_body_bytes),
               error);
            if (error)
               return;
         }
         if (connection_index.fetch_add(1) == 0) {
            socket.shutdown(tcp::socket::shutdown_both, error);
            return;
         }
         (void)write_bytes(
            socket,
            fixed_length_header(exact_body.size()) +
               std::string(exact_body));
      },
      false,
      2);

   const auto before = fc::http::get_metrics_snapshot();
   boost::asio::io_context io;
   fc::http::client client(io.get_executor());
   std::exception_ptr failure;
   boost::asio::co_spawn(
      io,
      [&]() -> boost::asio::awaitable<void> {
         auto options = tls_request_options();
         options.retry.max_attempts = 2;
         options.retry.initial_backoff = fc::microseconds(0);
         options.retry.max_backoff = fc::microseconds(0);
         const auto response =
            co_await client.async_request(
               fc::http::request{
                  .method = fc::http::request_method::post,
                  .target = server_url(server),
                  .body = std::string(exact_body),
               },
               options);
         BOOST_CHECK_EQUAL(response.status, 200);
         BOOST_CHECK_EQUAL(response.body, exact_body);
      },
      [&](std::exception_ptr operation_failure) {
         failure = std::move(operation_failure);
      });
   io.run();
   if (failure)
      std::rethrow_exception(failure);

   const auto after = fc::http::get_metrics_snapshot();
   BOOST_CHECK_EQUAL(connection_index.load(), 2U);
   BOOST_CHECK_EQUAL(after.requests, before.requests + 1);
   BOOST_CHECK_EQUAL(after.successes, before.successes + 1);
   BOOST_CHECK_EQUAL(
      after.request_bytes,
      before.request_bytes + exact_body.size() * 2);
   BOOST_CHECK_EQUAL_COLLECTIONS(
      after.failures.begin(),
      after.failures.end(),
      before.failures.begin(),
      before.failures.end());
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(http_client_file_download_tests)

/// Snapshot file transfers retain their pre-refactor unbounded deadline behavior.
BOOST_AUTO_TEST_CASE(snapshot_file_download_has_no_transport_deadlines) {
   const auto options = download_options(exact_body_bytes);

   BOOST_CHECK(!options.timeouts.connect);
   BOOST_CHECK(!options.timeouts.header);
   BOOST_CHECK(!options.timeouts.read);
   BOOST_CHECK(!options.timeouts.idle);
   BOOST_CHECK(!options.timeouts.total);
   BOOST_CHECK(!options.timeouts.inherit_task_deadline);
}

/// Snapshot file transfers are not capped by an ambient task deadline.
BOOST_AUTO_TEST_CASE(snapshot_file_download_ignores_ambient_task_deadline) {
   scripted_http_server server([](tcp::socket& socket, const std::atomic_bool&) {
      write_bytes(socket, fixed_length_header(exact_body_bytes) + std::string(exact_body));
   });
   fc::temp_directory temp;
   const auto output = temp.path() / "unbounded-deadline.bin";
   fc::task::deadline_scope expired_deadline(
      fc::time_point::now() - fc::milliseconds(1));

   BOOST_CHECK_NO_THROW(
      download(server, output, download_options(exact_body_bytes)));
   BOOST_CHECK_EQUAL(read_file(output), exact_body);
}

/// A request above its caller budget is rejected before any network write.
BOOST_AUTO_TEST_CASE(oversized_request_body_is_rejected_before_send) {
   std::atomic_bool cancellation_requested{false};
   scripted_http_server server([&cancellation_requested](tcp::socket&, const std::atomic_bool& stop) {
      std::this_thread::sleep_for(std::chrono::milliseconds(cancellation_delay_ms));
      cancellation_requested = true;
      while (!stop.load()) {
         std::this_thread::sleep_for(10ms);
      }
   }, false);
   fc::temp_directory temp;
   const auto output = temp.path() / "request-write-cancelled.bin";
   auto options = download_options(exact_body_bytes);
   const fc::variant payload(std::string(blocked_request_body_bytes, 'a'));

   const auto start = std::chrono::steady_clock::now();
   fc::http_client client;
   client.set_cancel_check([&cancellation_requested]() { return cancellation_requested.load(); });
   BOOST_CHECK_EXCEPTION(
      client.post_to_file(server_url(server), payload, output, options),
      fc::exception,
      [](const fc::exception& error) {
         return error.to_detail_string().find("request_limit") != std::string::npos;
      });
   const auto elapsed = std::chrono::steady_clock::now() - start;

   BOOST_CHECK_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), max_test_elapsed_ms);
   check_download_files_removed(output);
}

/// Metadata requests remain explicitly cancellable within their finite default total deadline.
BOOST_AUTO_TEST_CASE(post_sync_can_be_cancelled) {
   scripted_http_server server([](tcp::socket&, const std::atomic_bool& stop) {
      while (!stop.load()) {
         std::this_thread::sleep_for(10ms);
      }
   });
   delayed_cancellation cancellation;
   fc::http_client client;
   client.set_cancel_check(cancellation.check());

   const auto start = std::chrono::steady_clock::now();
   BOOST_CHECK_EXCEPTION(
      client.post_sync(server_url(server), fc::variant(fc::mutable_variant_object())),
      fc::exception,
      [](const fc::exception& error) {
         return error.to_detail_string().find("cancelled") != std::string::npos;
      });
   const auto elapsed = std::chrono::steady_clock::now() - start;

   BOOST_CHECK_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), max_test_elapsed_ms);
}

/// An unbounded download clears an expired metadata deadline before reusing the connection.
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
   metadata_deadline.safe_add(fc::milliseconds(reuse_metadata_deadline_ms));

   BOOST_REQUIRE_NO_THROW(
      client.post_sync(server_url(server), fc::variant(fc::mutable_variant_object()), metadata_deadline));
   std::this_thread::sleep_for(
      std::chrono::milliseconds(reuse_after_deadline_wait_ms));
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

/// The legacy stale-connection flag does not retry a failure on the first fresh connection.
BOOST_AUTO_TEST_CASE(fresh_download_connection_failure_is_not_retried) {
   boost::asio::io_context io;
   tcp::acceptor closed_listener(
      io,
      tcp::endpoint(
         boost::asio::ip::address_v4::loopback(),
         0));
   const auto closed_port =
      closed_listener.local_endpoint().port();
   boost::system::error_code close_error;
   closed_listener.close(close_error);
   fc::temp_directory temp;
   const auto output = temp.path() / "fresh-connect-failure.bin";
   fc::http_client client;
   auto options = download_options(exact_body_bytes);
   options.retry_failed_reused_connection = true;

   BOOST_CHECK_EXCEPTION(
      client.post_to_file(
         fc::url(
            "http://127.0.0.1:" +
            std::to_string(closed_port) +
            "/download"),
         fc::variant(fc::mutable_variant_object()),
         output,
         options),
      fc::exception,
      [](const fc::exception& error) {
         const auto detail = error.to_detail_string();
         return detail.find("connect") != std::string::npos &&
                detail.find("retry_exhausted") == std::string::npos;
      });
   check_download_files_removed(output);
}

/// An attended caller can cancel while a peer never sends a response header.
BOOST_AUTO_TEST_CASE(response_header_wait_can_be_cancelled) {
   scripted_http_server server([](tcp::socket&, const std::atomic_bool& stop) {
      while (!stop.load()) {
         std::this_thread::sleep_for(10ms);
      }
   });
   fc::temp_directory temp;
   const auto output = temp.path() / "header-cancelled.bin";
   auto options = download_options(exact_body_bytes);
   delayed_cancellation cancellation;

   const auto start = std::chrono::steady_clock::now();
   BOOST_CHECK_THROW(download(server, output, options, cancellation.check()), fc::exception);
   const auto elapsed = std::chrono::steady_clock::now() - start;

   BOOST_CHECK_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), max_test_elapsed_ms);
   check_download_files_removed(output);
}

/// An attended caller can cancel a body that stops after partial progress.
BOOST_AUTO_TEST_CASE(stalled_body_can_be_cancelled_and_removes_partial_file) {
   scripted_http_server server([](tcp::socket& socket, const std::atomic_bool& stop) {
      if (!write_bytes(socket, fixed_length_header(2)) || !write_bytes(socket, "1")) {
         return;
      }
      while (!stop.load()) {
         std::this_thread::sleep_for(10ms);
      }
   });
   fc::temp_directory temp;
   const auto output = temp.path() / "body-cancelled.bin";
   auto options = download_options(exact_body_bytes);
   delayed_cancellation cancellation;

   BOOST_CHECK_THROW(download(server, output, options, cancellation.check()), fc::exception);
   check_download_files_removed(output);
}

/// A continuously progressing transfer has no total deadline but remains interruptible.
BOOST_AUTO_TEST_CASE(progressing_body_has_no_total_deadline_and_can_be_cancelled) {
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
   const auto output = temp.path() / "progressing-body-cancelled.bin";
   auto options = download_options(1'000);
   delayed_cancellation cancellation;

   BOOST_CHECK_THROW(download(server, output, options, cancellation.check()), fc::exception);
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

/// A non-success response includes only a small bounded prefix of its diagnostic body.
BOOST_AUTO_TEST_CASE(error_response_includes_bounded_body_diagnostic) {
   const std::string error_prefix = "snapshot not ready: ";
   const std::string omitted_suffix = "omitted-tail";
   const std::string error_body = error_prefix + std::string(256, 'x') + omitted_suffix;
   scripted_http_server server([&](tcp::socket& socket, const std::atomic_bool&) {
      write_bytes(socket, fixed_length_header(error_body.size(), "409 Conflict") + error_body);
   });
   fc::temp_directory temp;
   const auto output = temp.path() / "error-response.bin";

   BOOST_CHECK_EXCEPTION(
      download(server, output, download_options(error_body.size())),
      fc::exception,
      [&](const fc::exception& error) {
         const auto detail = error.to_detail_string();
         return detail.find("HTTP POST failed with status 409: " + error_prefix) != std::string::npos &&
                detail.find(omitted_suffix) == std::string::npos;
      });
   check_download_files_removed(output);
}

/// A fixed-length response that ends early must fail without retaining either output file.
BOOST_AUTO_TEST_CASE(truncated_fixed_length_response_is_rejected_and_removed) {
   scripted_http_server server([](tcp::socket& socket, const std::atomic_bool&) {
      write_bytes(socket, fixed_length_header(exact_body_bytes) + "short");
      boost::system::error_code ec;
      socket.shutdown(tcp::socket::shutdown_send, ec);
      socket.close(ec);
   });
   fc::temp_directory temp;
   const auto output = temp.path() / "truncated-fixed.bin";

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
   options.timeouts.idle = fc::milliseconds(200);

   const auto start = std::chrono::steady_clock::now();
   BOOST_CHECK_THROW(download(server, output, options), fc::exception);
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
   std::vector<fc::http_file_download_status> statuses;
   auto options = download_options(exact_body_bytes);
   options.status_callback = [&](const fc::http_file_download_status& status) { statuses.push_back(status); };

   BOOST_CHECK_NO_THROW(download(server, output, options));
   BOOST_CHECK(std::filesystem::exists(output));
   BOOST_CHECK(!std::filesystem::exists(partial_path(output)));
   BOOST_CHECK_EQUAL(read_file(output), exact_body);
   BOOST_REQUIRE(!statuses.empty());
   std::vector<fc::http_file_download_phase> observed_phases;
   for (const auto& status : statuses) {
      if (observed_phases.empty() ||
          observed_phases.back() != status.phase) {
         observed_phases.push_back(status.phase);
      }
   }
   const std::vector expected_phases{
      fc::http_file_download_phase::connecting,
      fc::http_file_download_phase::sending_request,
      fc::http_file_download_phase::waiting_for_response,
      fc::http_file_download_phase::downloading,
      fc::http_file_download_phase::complete,
   };
   BOOST_REQUIRE_EQUAL(
      observed_phases.size(),
      expected_phases.size());
   for (size_t index = 0; index < expected_phases.size(); ++index)
      BOOST_CHECK(observed_phases[index] == expected_phases[index]);
   const auto& final_status = statuses.back();
   BOOST_CHECK(final_status.phase == fc::http_file_download_phase::complete);
   BOOST_CHECK_EQUAL(final_status.downloaded_bytes, exact_body_bytes);
   BOOST_REQUIRE(final_status.total_bytes);
   BOOST_CHECK_EQUAL(*final_status.total_bytes, exact_body_bytes);
}

/// Chunked downloads report progress without inventing a total byte count or ETA basis.
BOOST_AUTO_TEST_CASE(chunked_response_reports_unknown_total) {
   scripted_http_server server([](tcp::socket& socket, const std::atomic_bool&) {
      write_bytes(socket, chunked_header() + "8\r\n" + std::string(exact_body) + "\r\n0\r\n\r\n");
   });
   fc::temp_directory temp;
   const auto output = temp.path() / "chunked-progress.bin";
   std::vector<fc::http_file_download_status> statuses;
   auto options = download_options(exact_body_bytes);
   options.status_callback = [&](const fc::http_file_download_status& status) { statuses.push_back(status); };

   BOOST_REQUIRE_NO_THROW(download(server, output, options));
   BOOST_REQUIRE(!statuses.empty());
   const auto& final_status = statuses.back();
   BOOST_CHECK(final_status.phase == fc::http_file_download_phase::complete);
   BOOST_CHECK_EQUAL(final_status.downloaded_bytes, exact_body_bytes);
   BOOST_CHECK(!final_status.total_bytes);
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

   BOOST_REQUIRE_NO_THROW(download(server, output, options));
   BOOST_CHECK_EQUAL(std::filesystem::file_size(output), response_bytes);
}

/// A response that cannot fit on the destination filesystem must fail before writing.
BOOST_AUTO_TEST_CASE(insufficient_disk_space_is_rejected_before_write) {
   scripted_http_server server([](tcp::socket& socket, const std::atomic_bool&) {
      write_bytes(socket, fixed_length_header(1) + "1");
   });
   fc::temp_directory temp;
   const auto output = temp.path() / "disk-headroom.bin";
   auto options = download_options(exact_body_bytes);

   BOOST_CHECK_THROW(download_with_available_disk_space(server, output, options, 0), fc::exception);
   check_download_files_removed(output);
}

/// A write that only barely fits must still leave room for the concurrent-consumer margin.
BOOST_AUTO_TEST_CASE(disk_space_budget_requires_concurrency_margin) {
   scripted_http_server server([](tcp::socket& socket, const std::atomic_bool&) {
      write_bytes(socket, fixed_length_header(exact_body_bytes) + std::string(exact_body));
   });
   fc::temp_directory temp;
   const auto output = temp.path() / "disk-concurrency-margin.bin";
   auto options = download_options(exact_body_bytes);

   BOOST_CHECK_THROW(
      download_with_available_disk_space(server, output, options, exact_body_bytes), fc::exception);
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
   auto options = download_options(response_bytes);

   BOOST_CHECK_EXCEPTION(
      download_with_available_disk_space(
         server, output, options, response_bytes + headroom_without_full_margin),
      fc::exception,
      [](const fc::exception& error) {
         return error.to_detail_string().find("Insufficient disk space") != std::string::npos;
      });
   check_download_files_removed(output);
}

BOOST_AUTO_TEST_SUITE_END()

/**
 *  @file
 *  @copyright defined in LICENSE.txt
 */
#pragma once

#include <fc/static_variant.hpp>
#include <fc/time.hpp>
#include <fc/variant.hpp>
#include <fc/exception/exception.hpp>
#include <fc/network/url.hpp>
#include <fc/crypto/blake3.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/cancellation_signal.hpp>

#include <cstdint>
#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <magic_enum/magic_enum.hpp>

namespace fc {

struct http_file_download_status;

namespace http {

namespace detail {

/** Resolver result used by the transport's deterministic test seam. */
struct resolved_endpoint {
   std::string address;
   uint16_t port = 0;
};

/** Completion callback for an asynchronous outbound resolver operation. */
using resolver_complete_fn =
   std::function<void(std::optional<std::string>,
                      std::vector<resolved_endpoint>)>;

/** Cancellation callback returned by a resolver starter. */
using resolver_cancel_fn = std::function<void()>;

/** Injectable asynchronous resolver starter used by transport regression tests. */
using resolver_start_fn =
   std::function<resolver_cancel_fn(const std::string&,
                                    const std::string&,
                                    time_point,
                                    resolver_complete_fn)>;

} // namespace detail

/** Supported outbound HTTP request methods. */
enum class request_method {
   get,
   put,
   post,
   delete_,
};

/** Fixed-cardinality failure categories exported by the shared transport. */
enum class failure_kind {
   cancelled,
   dns,
   connect,
   tls_ca,
   tls_handshake,
   tls_verification,
   tls_hostname,
   tls_ip,
   timeout_connect,
   timeout_header,
   timeout_read,
   timeout_idle,
   timeout_total,
   request_limit,
   response_limit,
   http_status,
   retry_exhausted,
   io,
};

/** Number of public transport failure categories. */
inline constexpr size_t failure_kind_count = magic_enum::enum_count<failure_kind>();

/** Return a stable metric label for @p failure. */
std::string_view failure_kind_name(failure_kind failure);

/** Return only the scheme, host, and explicit port of @p target for safe diagnostics. */
std::string sanitized_endpoint(const url& target);

/**
 * Additional trust and routing configuration for one transport instance.
 *
 * HTTPS always uses the platform trust store with peer and hostname/IP verification.
 * Custom CA sources augment that store and cannot disable verification.
 */
struct transport_options {
   /// Optional PEM bundle that augments the platform trust store.
   std::optional<std::filesystem::path> additional_ca_file;
   /// Optional hashed certificate directory that augments the platform trust store.
   std::optional<std::filesystem::path> additional_ca_path;
   /// Explicit outbound proxy. Environment proxy variables are ignored when absent.
   std::optional<std::string> proxy;
   /// Resolver cache lifetime in seconds; -1 retains entries for the client lifetime.
   int64_t dns_cache_timeout_seconds = 60;
   /// Invalidate a cached resolver result after connection establishment fails.
   bool refresh_dns_on_connection_failure = true;
};

/** Per-request phase and total deadlines. */
struct timeout_options {
   /// Maximum DNS and connection-establishment time.
   microseconds connect = seconds(10);
   /// Maximum time waiting for complete response headers after request upload.
   microseconds header = seconds(30);
   /// Maximum aggregate response-body read time. nullopt is an explicitly reviewed exemption.
   std::optional<microseconds> read = seconds(30);
   /// Maximum time without upload or download progress after response headers.
   microseconds idle = seconds(30);
   /// Overall request budget. nullopt is permitted only for a reviewed attended operation.
   std::optional<microseconds> total = seconds(30);
};

/** Explicit bounded retry policy. */
struct retry_options {
   /// Total attempts including the first request.
   uint32_t max_attempts = 1;
   /// Delay before the second attempt.
   microseconds initial_backoff = milliseconds(100);
   /// Maximum delay between attempts.
   microseconds max_backoff = seconds(1);
};

/** Resource, deadline, cancellation, and retry policy for one request. */
struct request_options {
   /// Maximum serialized request-header bytes, including transport-supplied fields.
   uint64_t max_request_header_bytes = 64ULL * 1024ULL;
   /// Maximum serialized request body bytes.
   uint64_t max_request_body_bytes = 1ULL * 1024ULL * 1024ULL;
   /// Maximum aggregate response-header bytes.
   uint64_t max_response_header_bytes = 64ULL * 1024ULL;
   /// Maximum decoded response body bytes.
   uint64_t max_response_body_bytes = 1ULL * 1024ULL * 1024ULL;
   /// Per-phase and total deadlines.
   timeout_options timeouts;
   /// Retry policy; attempts above one require idempotent=true.
   retry_options retry;
   /// Whether replaying this request is safe.
   bool idempotent = false;
   /// Restrict retryable failures to attempts made on an existing cached connection.
   bool retry_only_reused_connection = false;
   /// Blocking-adapter cancellation predicate; asynchronous client operations ignore this field.
   std::function<bool()> cancel_check;
};

/** A transport-level HTTP request. */
struct request {
   /// Request method.
   request_method method = request_method::post;
   /// Fully qualified HTTP, HTTPS, or Unix-socket URL.
   url target;
   /// Serialized request body.
   std::string body;
   /// Request Content-Type when body is present.
   std::string content_type = "application/json";
   /// Request User-Agent.
   std::string user_agent;
   /// Additional request headers.
   std::vector<std::pair<std::string, std::string>> headers;
};

/** Buffered HTTP response returned by the shared transport. */
struct response {
   /// Numeric HTTP status.
   uint32_t status = 0;
   /// Sanitized HTTP reason phrase.
   std::string reason;
   /// Decoded response body.
   std::string body;
};

/** Response metadata available before a streamed body is consumed. */
struct response_head {
   /// Numeric HTTP status.
   uint32_t status = 0;
   /// Sanitized HTTP reason phrase.
   std::string reason;
   /// Decoded response size when supplied by the peer.
   std::optional<uint64_t> content_length;
};

/** Optional observation and filesystem hooks for an atomic download. */
struct download_options {
   /// Receive phase transitions and periodic transfer status. The callback must not throw.
   std::function<void(const ::fc::http_file_download_status&)> status_callback;
   /// Override the destination-filesystem space query.
   std::function<uint64_t(const std::filesystem::path&)> space_available_provider;
};

/** Process-wide monotonic transport metrics with fixed cardinality. */
struct metrics_snapshot {
   /// Logical requests started, independent of retry attempts.
   uint64_t requests = 0;
   /// Logical requests whose final HTTP response had a successful status.
   uint64_t successes = 0;
   /// Complete request-body uploads, including completed retry attempts.
   uint64_t request_bytes = 0;
   /// Response-body bytes accepted across final logical request outcomes.
   uint64_t response_bytes = 0;
   /// Final logical request failures by fixed category.
   std::array<uint64_t, failure_kind_count> failures{};
};

/** Return a coherent snapshot of shared outbound transport metrics. */
metrics_snapshot get_metrics_snapshot();

/**
 * Move-only pull reader for one response body and its leased HTTP/1.1 connection.
 *
 * Reaching end-of-body returns a healthy connection to the client cache. Destroying the reader
 * before completion closes the connection so a partially consumed response cannot be reused.
 */
class response_reader {
public:
   response_reader();
   ~response_reader();

   response_reader(const response_reader&) = delete;
   response_reader& operator=(const response_reader&) = delete;
   response_reader(response_reader&&) noexcept;
   response_reader& operator=(response_reader&&) noexcept;

   /** Return response metadata parsed by client::async_open. */
   const response_head& head() const;

   /** Read one decoded response-body increment into caller-owned storage. */
   boost::asio::awaitable<size_t>
   async_read_some(boost::asio::mutable_buffer output);

   /** Return whether the complete response body has been consumed. */
   bool done() const noexcept;

private:
   friend class client;
   friend boost::asio::awaitable<void>
   async_download_atomic(class client&,
                         request,
                         request_options,
                         std::filesystem::path,
                         download_options,
                         boost::asio::cancellation_slot);

   explicit response_reader(
      std::shared_ptr<class response_reader_impl> impl);

   std::shared_ptr<class response_reader_impl> _impl;
};

/**
 * Executor-bound asynchronous HTTP/HTTPS/Unix-socket client.
 *
 * The client owns resolver and idle-connection caches. Operations are serialized through an
 * internal strand while leased response bodies may overlap safely on separate connections.
 */
class client {
public:
   explicit client(boost::asio::any_io_executor executor,
                   transport_options options = {});
   ~client();

   client(const client&) = delete;
   client& operator=(const client&) = delete;
   client(client&&) noexcept;
   client& operator=(client&&) noexcept;

   /** Send a bounded request and return after its response head is parsed. */
   boost::asio::awaitable<response_reader>
   async_open(request req,
              request_options options,
              boost::asio::cancellation_slot cancellation = {});

   /** Send a bounded request and buffer its complete bounded response body. */
   boost::asio::awaitable<response>
   async_request(request req,
                 request_options options,
                 boost::asio::cancellation_slot cancellation = {});

   /** Resolve and cache one endpoint without opening a connection. */
   boost::asio::awaitable<void>
   async_warm_up(const url& target,
                 request_options options,
                 boost::asio::cancellation_slot cancellation = {});

   /** Return the executor used by all client state and socket operations. */
   boost::asio::any_io_executor get_executor() const noexcept;

private:
   friend class transport_impl;
   friend boost::asio::awaitable<void>
   async_download_atomic(client&,
                         request,
                         request_options,
                         std::filesystem::path,
                         download_options,
                         boost::asio::cancellation_slot);

   client(boost::asio::any_io_executor executor,
          transport_options options,
          detail::resolver_start_fn resolver_start);

   std::shared_ptr<class client_impl> _impl;
};

/**
 * Stream a bounded response to a temporary sibling and atomically publish it at completion.
 */
boost::asio::awaitable<void>
async_download_atomic(
   client& source,
   request req,
   request_options policy,
   std::filesystem::path output,
   download_options options = {},
   boost::asio::cancellation_slot cancellation = {});

/**
 * Blocking compatibility adapter over the executor-bound asynchronous client.
 *
 * New coroutine call sites should use client directly. Existing synchronous JSON-RPC, KIOD,
 * and snapshot paths may use this adapter while they migrate.
 */
class transport {
public:
   explicit transport(transport_options options = {});
   ~transport();

   transport(const transport&) = delete;
   transport& operator=(const transport&) = delete;
   transport(transport&&) noexcept;
   transport& operator=(transport&&) noexcept;

   /** Execute one bounded request and buffer its bounded response body. */
   response perform(const request& req, const request_options& options);

   /** Resolve and cache one endpoint under the same DNS/connect deadline policy. */
   void prime_endpoint(const url& target,
                       const request_options& options);

   /**
    * Execute one bounded request and stream a successful response atomically to @p output.
    *
    * The body ceiling, disk checks, progress reporting, and partial-file cleanup are shared
    * with snapshot bootstrap.
    */
   void perform_to_file(const request& req,
                        const request_options& options,
                        const std::filesystem::path& output,
                        const std::function<void(const ::fc::http_file_download_status&)>& status_callback,
                        const std::function<uint64_t(const std::filesystem::path&)>&
                           space_available_provider = {});

private:
   friend struct transport_test_access;

   transport(transport_options options,
             detail::resolver_start_fn resolver_start);

   std::unique_ptr<class transport_impl> _impl;
};

} // namespace http

/** Current phase of a streamed HTTP file download. */
enum class http_file_download_phase {
   connecting,
   sending_request,
   waiting_for_response,
   downloading,
   complete,
};

/** Periodic status for a streamed HTTP file download. */
struct http_file_download_status {
   /// Current request or transfer phase.
   http_file_download_phase phase;
   /// Decoded response-body bytes persisted so far.
   uint64_t downloaded_bytes;
   /// Expected response-body bytes when the peer supplied Content-Length.
   std::optional<uint64_t> total_bytes;
   /// Time elapsed since the download request started.
   microseconds elapsed;
};

/** Resource limits and status reporting for a streamed HTTP file download. */
struct http_file_download_options {
   /// Maximum accepted response-body size in bytes.
   uint64_t max_response_body_bytes;
   /// Retry a failed request once when it used a cached connection. Only enable for idempotent requests.
   bool retry_failed_reused_connection = false;
   /// Explicit phase deadlines. The attended snapshot download intentionally has no total deadline.
   http::timeout_options timeouts{
      .connect = seconds(10),
      .header = seconds(30),
      .read = std::nullopt,
      .idle = seconds(30),
      .total = std::nullopt,
   };
   /// Receive phase transitions and periodic transfer status. The callback must not throw.
   std::function<void(const http_file_download_status&)> status_callback;
};

/**
 * Backwards-compatible JSON POST facade over the shared bounded authenticated transport.
 *
 * New coroutine call sites should use `fc::http::client` directly. This facade remains for
 * KIOD and snapshot call sites while preserving their existing serialization API.
 */
class http_client {
   public:
      http_client();
      explicit http_client(http::transport_options options);
      ~http_client();

      variant post_sync(const url& dest, const variant& payload, const time_point& deadline = time_point::maximum());

      template<typename T>
      variant post_sync(const url& dest, const T& payload, const time_point& deadline = time_point::maximum()) {
         variant payload_v;
         to_variant(payload, payload_v);
         return post_sync(dest, payload_v, deadline);
      }

      /**
       * Download a binary POST response using explicit resource limits.
       *
       * The response is written to a temporary sibling of @p output and renamed only after
       * the complete bounded body has been persisted successfully.
       */
      void post_to_file(const url& dest,
                        const variant& payload,
                        const std::filesystem::path& output,
                        const http_file_download_options& options);

      /**
       * Set a predicate used to cancel synchronous HTTP operations.
       *
       * The predicate is polled while resolver and socket operations are pending. Returning true
       * cancels the active operation so callers can interrupt otherwise unbounded requests.
       */
      void set_cancel_check(std::function<bool()> cancel_check);

      void set_verify_peers(bool enabled);

private:
   friend struct http_client_test_access;

   /// Override the filesystem free-space query for deterministic transport tests.
   void set_space_available_provider_for_testing(
      std::function<uint64_t(const std::filesystem::path&)> provider);

   std::unique_ptr<http::transport> _transport;
   std::function<bool()> _cancel_check;
   std::function<uint64_t(const std::filesystem::path&)> _space_available_provider;
};

}

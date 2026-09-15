#pragma once

/* Internal outbound HTTP transport state shared by the http_*.cpp translation units.
 *
 * Every declaration here has external linkage on purpose: transport_failure crosses
 * translation-unit boundaries in a throw/catch pair, and shared_metrics() must resolve
 * to one process-global counter block.
 */

#include <fc/network/http/http_client.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core/basic_stream.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http.hpp>
#include <boost/system/error_code.hpp>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <sys/socket.h>

namespace fc {
namespace http {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace beast_http = boost::beast::http;
namespace local = boost::asio::local;
using tcp = asio::ip::tcp;

inline namespace transport_internal {

using error_code = boost::system::error_code;

inline constexpr uint64_t disk_space_check_interval_bytes = 64ULL * 1024ULL * 1024ULL;
inline constexpr uint64_t disk_space_concurrency_margin_bytes = 64ULL * 1024ULL * 1024ULL;
inline constexpr size_t body_read_buffer_bytes = 64ULL * 1024ULL;
inline constexpr size_t max_error_response_body_bytes = 200;
inline constexpr auto disk_space_check_interval = std::chrono::seconds(5);
inline constexpr auto download_status_interval = std::chrono::seconds(5);
inline constexpr size_t platform_resolver_workers = 4;
inline constexpr size_t max_resolver_capacity_waiters = 256;
inline constexpr unsigned http_version_1_1 = 11;
inline constexpr uint16_t default_http_port = 80;
inline constexpr uint16_t default_https_port = 443;
inline constexpr std::string_view default_http_service = "80";
inline constexpr std::string_view default_https_service = "443";
inline constexpr std::string_view filesystem_current_directory = ".";
inline constexpr std::string_view scheme_http = "http";
inline constexpr std::string_view scheme_https = "https";
inline constexpr std::string_view scheme_unix = "unix";

/** Internal typed failure retained across retry and public exception conversion. */
class transport_failure : public std::runtime_error {
public:
   transport_failure(failure_kind failure, std::string message, bool retryable_failure = false)
      : std::runtime_error(std::move(message))
      , kind(failure)
      , retryable(retryable_failure) {}

   /** Out-of-line to pin one vtable and typeinfo so a cross-translation-unit catch matches. */
   ~transport_failure() override;

   failure_kind kind;
   bool retryable;
};

/** Process-global metrics storage with only fixed enum-indexed cardinality. */
struct atomic_metrics {
   std::atomic<uint64_t> requests{0};
   std::atomic<uint64_t> successes{0};
   std::atomic<uint64_t> request_bytes{0};
   std::atomic<uint64_t> response_bytes{0};
   std::array<std::atomic<uint64_t>, failure_kind_count> failures{};
};

/** Return the process-global outbound transport metrics. */
atomic_metrics& shared_metrics();

/** Increment one fixed-cardinality failure counter. */
void record_failure(failure_kind kind);

/** Return whether @p status belongs to the HTTP successful response class. */
bool is_success_status(uint32_t status);

/** Throw a public fc exception while retaining timeout and cancellation types. */
[[noreturn]] void throw_public_failure(const transport_failure& failure);

/** Return the earlier configured total timeout and active fc task deadline. */
std::optional<time_point> effective_total_deadline(const request_options& options);

/** One asynchronous operation's absolute deadline and timeout category. */
struct operation_deadline {
   time_point when;
   failure_kind timeout_kind;
};

/** Clamp an optional phase timeout to the request's optional total deadline. */
std::optional<operation_deadline> phase_deadline(const std::optional<microseconds>& phase_timeout,
                                                 failure_kind phase_failure,
                                                 const std::optional<time_point>& total_deadline);

/** Return a bounded printable HTTP reason phrase. */
std::string sanitize_reason(boost::beast::string_view reason);

/** Return whether an Asio error can represent a stale or failed peer connection. */
bool is_retryable_connection_error(const boost::system::error_code& error);

/** Start one platform lookup whose completion state may outlive its caller. */
detail::resolver_cancel_fn start_platform_resolution(const std::string& host, const std::string& service,
                                                     time_point deadline, detail::resolver_complete_fn complete);

/** One plain, TLS, or Unix-domain HTTP/1.1 connection. */
struct connection_state {
   using tcp_stream = beast::tcp_stream;
   using tls_stream = asio::ssl::stream<tcp_stream>;
   using unix_stream = beast::basic_stream<local::stream_protocol>;
   using stream_variant =
      std::variant<std::unique_ptr<tcp_stream>, std::unique_ptr<tls_stream>, std::unique_ptr<unix_stream>>;

   explicit connection_state(stream_variant stream_in)
      : stream(std::move(stream_in)) {}

   /** Return whether the lowest-layer socket remains open. */
   bool open() const {
      return std::visit([](const auto& value) { return beast::get_lowest_layer(*value).socket().is_open(); }, stream);
   }

   /**
    * Return whether an idle socket has no peer close or unexpected input ready.
    *
    * A locally open descriptor does not reveal a peer FIN. A non-blocking
    * peek distinguishes an actually idle socket from FIN/reset and also
    * rejects unsolicited bytes that cannot belong to a fully consumed HTTP
    * response.
    */
   bool healthy_for_reuse() {
      if (!open())
         return false;
      return std::visit(
         [](auto& value) {
            const auto descriptor = beast::get_lowest_layer(*value).socket().native_handle();
            char byte = 0;
            for (;;) {
               const auto result = ::recv(descriptor, &byte, sizeof(byte), MSG_DONTWAIT | MSG_PEEK);
               if (result >= 0)
                  return false;
               if (errno == EINTR)
                  continue;
               return errno == EAGAIN || errno == EWOULDBLOCK;
            }
         },
         stream);
   }

   /** Cancel the active operation without releasing this connection object. */
   void cancel() {
      std::visit(
         [](auto& value) {
            boost::system::error_code ignored;
            beast::get_lowest_layer(*value).socket().cancel(ignored);
         },
         stream);
   }

   /** Close the connection so it cannot return to the idle cache. */
   void close() {
      std::visit(
         [](auto& value) {
            boost::system::error_code ignored;
            beast::get_lowest_layer(*value).socket().close(ignored);
         },
         stream);
   }

   stream_variant stream;
};

/** Monotonic clock used for connection-pool residence time. */
using idle_clock = std::chrono::steady_clock;

/** One persistent connection and the time it entered the idle cache. */
struct idle_connection {
   std::shared_ptr<connection_state> connection;
   idle_clock::time_point idle_since;
};

/**
 * Cancellation ownership for one logical request.
 *
 * The cancellation-slot handler survives the header/body boundary. Each active resolver, timer,
 * or socket operation installs a lifetime-safe callback and clears it before returning.
 */
class request_control : public std::enable_shared_from_this<request_control> {
public:
   static std::shared_ptr<request_control> create(asio::cancellation_slot slot, asio::any_io_executor executor) {
      auto result = std::shared_ptr<request_control>(new request_control(std::move(executor)));
      if (slot.is_connected()) {
         slot.assign([weak = std::weak_ptr<request_control>(result)](asio::cancellation_type_t type) {
            if (type != asio::cancellation_type::none) {
               if (auto control = weak.lock())
                  control->cancel();
            }
         });
      }
      return result;
   }

   request_control(const request_control&) = delete;
   request_control& operator=(const request_control&) = delete;

   /** Throw the stable cancellation category after an emitted cancellation. */
   void throw_if_cancelled(std::string_view phase = "request") const {
      if (_cancelled.load(std::memory_order_acquire)) {
         throw transport_failure(failure_kind::cancelled, std::string(phase) + " cancelled");
      }
   }

   /** Install the cancellation callback for the current operation. */
   void set_active(std::function<void()> cancel_active) {
      bool already_cancelled = false;
      {
         std::scoped_lock lock(_mutex);
         _cancel_active = cancel_active;
         already_cancelled = _cancelled.load(std::memory_order_acquire);
      }
      if (already_cancelled)
         post_active_cancellation();
   }

   /** Remove the current operation callback. */
   void clear_active() {
      std::scoped_lock lock(_mutex);
      _cancel_active = {};
   }

private:
   explicit request_control(asio::any_io_executor executor)
      : _executor(std::move(executor)) {}

   /** Remember cancellation and interrupt the currently active operation. */
   void cancel() {
      _cancelled.store(true, std::memory_order_release);
      post_active_cancellation();
   }

   /** Cancel only the operation still active when this callback reaches the client executor. */
   void post_active_cancellation() {
      asio::post(_executor, [weak = weak_from_this()] {
         auto self = weak.lock();
         if (!self)
            return;
         std::function<void()> cancel_active;
         {
            std::scoped_lock lock(self->_mutex);
            cancel_active = self->_cancel_active;
         }
         if (cancel_active)
            cancel_active();
      });
   }

   asio::any_io_executor _executor;
   std::atomic_bool _cancelled{false};
   std::mutex _mutex;
   std::function<void()> _cancel_active;
};

/** Clear a request's active cancellation target at scope exit. */
class active_cancel_guard {
public:
   active_cancel_guard(std::shared_ptr<request_control> control, std::function<void()> cancel_active)
      : _control(std::move(control)) {
      _control->set_active(std::move(cancel_active));
   }

   active_cancel_guard(const active_cancel_guard&) = delete;
   active_cancel_guard& operator=(const active_cancel_guard&) = delete;

   ~active_cancel_guard() { _control->clear_active(); }

private:
   std::shared_ptr<request_control> _control;
};

/** Metrics finalized exactly once when a response completes, fails, or is abandoned. */
struct request_metrics_state {
   void add_response_bytes(uint64_t bytes) { response_bytes.fetch_add(bytes, std::memory_order_relaxed); }

   void finish_status(uint32_t status) {
      if (finalized.exchange(true, std::memory_order_acq_rel))
         return;
      shared_metrics().response_bytes.fetch_add(response_bytes.load(std::memory_order_relaxed),
                                                std::memory_order_relaxed);
      if (is_success_status(status))
         shared_metrics().successes.fetch_add(1, std::memory_order_relaxed);
      else
         record_failure(failure_kind::http_status);
   }

   void finish_failure(failure_kind failure) {
      if (finalized.exchange(true, std::memory_order_acq_rel))
         return;
      shared_metrics().response_bytes.fetch_add(response_bytes.load(std::memory_order_relaxed),
                                                std::memory_order_relaxed);
      record_failure(failure);
   }

   std::atomic<uint64_t> response_bytes{0};
   std::atomic_bool finalized{false};
};

/** Set or clear a Beast logical-operation deadline, rejecting an expired phase. */
template <typename Stream>
void arm_operation_deadline(Stream& stream, const std::optional<operation_deadline>& deadline) {
   if (!deadline) {
      beast::get_lowest_layer(stream).expires_never();
      return;
   }
   const auto remaining = deadline->when - time_point::now();
   if (remaining.count() <= 0) {
      throw transport_failure(deadline->timeout_kind, "request deadline expired");
   }
   beast::get_lowest_layer(stream).expires_after(std::chrono::microseconds(remaining.count()));
}

/** Convert a Beast/Asio timeout into the selected phase category. */
void throw_if_operation_failed(const error_code& error, const std::optional<operation_deadline>& deadline,
                               const std::shared_ptr<request_control>& control);

/** Write one complete request under an optional total or connect deadline. */
template <typename Stream, typename Body>
asio::awaitable<void> write_request(const std::shared_ptr<connection_state>& connection, Stream& stream,
                                    beast_http::request<Body>& request_message,
                                    std::optional<operation_deadline> deadline,
                                    const std::shared_ptr<request_control>& control) {
   arm_operation_deadline(stream, deadline);
   active_cancel_guard cancel_guard(control, [connection] { connection->cancel(); });
   error_code error;
   (void)co_await beast_http::async_write(stream, request_message, asio::redirect_error(asio::use_awaitable, error));
   throw_if_operation_failed(error, deadline, control);
   if (error) {
      throw transport_failure(failure_kind::io, "request write failed: " + error.message(),
                              is_retryable_connection_error(error));
   }
}

/** Read one response head under the configured header deadline. */
template <typename Stream, typename Parser>
asio::awaitable<void> read_header(const std::shared_ptr<connection_state>& connection, Stream& stream,
                                  beast::flat_buffer& buffer, Parser& parser, const request_options& policy,
                                  std::optional<operation_deadline> deadline,
                                  const std::shared_ptr<request_control>& control) {
   arm_operation_deadline(stream, deadline);
   active_cancel_guard cancel_guard(control, [connection] { connection->cancel(); });
   error_code error;
   (void)co_await beast_http::async_read_header(stream, buffer, parser,
                                                asio::redirect_error(asio::use_awaitable, error));
   throw_if_operation_failed(error, deadline, control);
   if (error == beast_http::error::header_limit) {
      throw transport_failure(failure_kind::response_limit, "response headers exceed configured maximum");
   }
   if (error == beast_http::error::body_limit) {
      throw transport_failure(failure_kind::response_limit, "response body exceeds configured maximum of " +
                                                               std::to_string(policy.max_response_body_bytes) +
                                                               " bytes");
   }
   if (error) {
      throw transport_failure(failure_kind::io, "response header read failed: " + error.message(),
                              is_retryable_connection_error(error));
   }
}

/** Read one decoded response-body increment into caller storage. */
template <typename Stream, typename Parser>
asio::awaitable<size_t> read_body(const std::shared_ptr<connection_state>& connection, Stream& stream,
                                  beast::flat_buffer& buffer, Parser& parser, asio::mutable_buffer output,
                                  const request_options& policy, const std::optional<time_point>& total_deadline,
                                  const std::optional<operation_deadline>& read_deadline,
                                  const std::shared_ptr<request_control>& control) {
   parser.get().body().data = output.data();
   parser.get().body().size = output.size();
   auto deadline = phase_deadline(policy.timeouts.idle, failure_kind::timeout_idle, total_deadline);
   if (read_deadline && (!deadline || read_deadline->when < deadline->when))
      deadline = *read_deadline;
   arm_operation_deadline(stream, deadline);
   active_cancel_guard cancel_guard(control, [connection] { connection->cancel(); });
   error_code error;
   (void)co_await beast_http::async_read_some(stream, buffer, parser, asio::redirect_error(asio::use_awaitable, error));
   throw_if_operation_failed(error, deadline, control);
   if (error == beast_http::error::need_buffer)
      error.clear();
   if (error == beast_http::error::body_limit) {
      throw transport_failure(failure_kind::response_limit, "response body exceeds configured maximum of " +
                                                               std::to_string(policy.max_response_body_bytes) +
                                                               " bytes");
   }
   if (error) {
      throw transport_failure(failure_kind::io, "response body read failed: " + error.message());
   }
   co_return output.size() - parser.get().body().size;
}

} // namespace transport_internal

class response_reader_impl;

/** Executor-bound Beast transport state shared by the client and leased response readers. */
class client_impl : public std::enable_shared_from_this<client_impl> {
public:
   using error_code = boost::system::error_code;

   /** Parsed endpoint details shared by connection and request construction. */
   struct target_info {
      std::string scheme;
      std::string host;
      std::string service;
      std::string host_header;
      std::string request_target;
      std::string connection_key;
      std::optional<std::string> unix_socket_path;
      bool tls = false;
   };

   /** Cached resolver result with a finite or client-lifetime expiry. */
   struct dns_entry {
      std::vector<tcp::endpoint> endpoints;
      std::chrono::steady_clock::time_point expires;
   };

   explicit client_impl(asio::any_io_executor executor, transport_options options_in,
                        detail::resolver_start_fn resolver_start_in = {});

   ~client_impl();

   asio::strand<asio::any_io_executor> strand;
   transport_options options;
   asio::ssl::context tls_context;
   detail::resolver_start_fn resolver_start;
   std::optional<std::string> proxy_host;
   std::optional<std::string> proxy_service;
   std::map<std::string, std::vector<idle_connection>> idle_connections;
   size_t idle_connection_count = 0;
   std::map<std::string, dns_entry> dns_cache;
   std::map<std::string, target_info> unix_target_cache;

   /** Validate request budgets and explicit retry safety. */
   static void validate_policy(const request_options& policy);

   /** Return a conservative serialized request-header byte estimate. */
   static uint64_t estimate_request_header_bytes(const request& req);

   /** Validate a request before any resolver or socket operation begins. */
   static void validate_request(const request& req, const request_options& policy);

   /** Parse an HTTP, HTTPS, or Unix target into connection-safe components. */
   target_info normalize_target(const url& target);

   /** Resolve one host under an optional connection deadline with a bounded cache. */
   asio::awaitable<std::vector<tcp::endpoint>> resolve(const std::string& host, const std::string& service,
                                                       std::optional<operation_deadline> deadline,
                                                       const std::shared_ptr<request_control>& control);

   /** Connect a TCP stream to already-resolved endpoints. */
   asio::awaitable<void> connect_tcp(const std::shared_ptr<connection_state>& connection,
                                     connection_state::tcp_stream& stream, const std::vector<tcp::endpoint>& endpoints,
                                     const std::string& host, const std::string& service,
                                     std::optional<operation_deadline> deadline,
                                     const std::shared_ptr<request_control>& control);

   /** Build a bounded Beast request for a normalized target. */
   beast_http::request<beast_http::string_body> build_request(const request& req, const target_info& target) const;

   /** Send an HTTP CONNECT request before upgrading a proxy socket to TLS. */
   asio::awaitable<void> establish_proxy_tunnel(const std::shared_ptr<connection_state>& connection,
                                                connection_state::tcp_stream& stream, const target_info& target,
                                                const request_options& policy,
                                                std::optional<operation_deadline> connect_deadline,
                                                const std::shared_ptr<request_control>& control);

   /** Connect one new plain, TLS, or Unix-domain connection. */
   asio::awaitable<std::shared_ptr<connection_state>>
   create_connection(const target_info& target, const request_options& policy,
                     const std::optional<time_point>& total_deadline, const std::shared_ptr<request_control>& control);

   /** Close idle connections that exceeded the configured reuse age. */
   void prune_expired_idle_connections();

   /** Lease a recent locally-open idle connection, or create a fresh connection. */
   asio::awaitable<std::pair<std::shared_ptr<connection_state>, bool>>
   acquire_connection(const target_info& target, const request_options& policy,
                      const std::optional<time_point>& total_deadline, const std::shared_ptr<request_control>& control,
                      bool force_fresh);

   /** Return a fully consumed healthy connection to the idle cache. */
   void release_connection(const std::string& key, std::shared_ptr<connection_state> connection);

   /** Sleep through one bounded, cancellable exponential retry backoff. */
   asio::awaitable<void> wait_before_retry(const request_options& policy,
                                           const std::optional<time_point>& total_deadline, uint32_t completed_attempts,
                                           const std::shared_ptr<request_control>& control);

   asio::awaitable<std::shared_ptr<response_reader_impl>>
   async_open(request req, request_options policy, std::shared_ptr<request_control> control,
              std::function<void(http_file_download_phase)> on_phase = {});
};

} // namespace http
} // namespace fc

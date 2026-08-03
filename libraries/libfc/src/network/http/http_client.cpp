#include <fc/network/http/http_client.hpp>

#include <fc/task/deadline.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core/basic_stream.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>

#include <openssl/pem.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace fc {
namespace http {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace beast_http = boost::beast::http;
namespace local = boost::asio::local;
using tcp = asio::ip::tcp;

namespace {

constexpr uint64_t disk_space_check_interval_bytes = 64ULL * 1024ULL * 1024ULL;
constexpr uint64_t disk_space_concurrency_margin_bytes = 64ULL * 1024ULL * 1024ULL;
constexpr size_t body_read_buffer_bytes = 64ULL * 1024ULL;
constexpr size_t max_error_response_body_bytes = 200;
constexpr auto disk_space_check_interval = std::chrono::seconds(5);
constexpr auto download_status_interval = std::chrono::seconds(5);
constexpr size_t platform_resolver_workers = 4;
constexpr size_t max_resolver_capacity_waiters = 256;
constexpr unsigned http_version_1_1 = 11;
constexpr uint16_t default_http_port = 80;
constexpr uint16_t default_https_port = 443;
constexpr std::string_view default_http_service = "80";
constexpr std::string_view default_https_service = "443";
constexpr std::string_view filesystem_current_directory = ".";
constexpr std::string_view scheme_http = "http";
constexpr std::string_view scheme_https = "https";
constexpr std::string_view scheme_unix = "unix";

/** Internal typed failure retained across retry and public exception conversion. */
class transport_failure : public std::runtime_error {
public:
   transport_failure(failure_kind failure,
                     std::string message,
                     bool retryable_failure = false)
      : std::runtime_error(std::move(message))
      , kind(failure)
      , retryable(retryable_failure) {}

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
atomic_metrics& shared_metrics() {
   static atomic_metrics metrics;
   return metrics;
}

/** Increment one fixed-cardinality failure counter. */
void record_failure(failure_kind kind) {
   const auto index = magic_enum::enum_index(kind);
   FC_ASSERT(index, "Unknown outbound HTTP failure kind");
   shared_metrics().failures[*index].fetch_add(1, std::memory_order_relaxed);
}

/** Throw a public fc exception while retaining timeout and cancellation types. */
[[noreturn]] void throw_public_failure(const transport_failure& failure) {
   const auto category = std::string(failure_kind_name(failure.kind));
   if (failure.kind == failure_kind::cancelled) {
      FC_THROW_EXCEPTION(
         fc::canceled_exception,
         "Outbound HTTP {}: {}",
         category,
         failure.what());
   }
   if (failure.kind == failure_kind::timeout_connect ||
       failure.kind == failure_kind::timeout_header ||
       failure.kind == failure_kind::timeout_read ||
       failure.kind == failure_kind::timeout_idle ||
       failure.kind == failure_kind::timeout_total) {
      FC_THROW_EXCEPTION(
         fc::timeout_exception,
         "Outbound HTTP {}: {}",
         category,
         failure.what());
   }
   FC_THROW("Outbound HTTP {}: {}", category, failure.what());
}

/** Return the earlier configured total timeout and active fc task deadline. */
std::optional<time_point> effective_total_deadline(const request_options& options) {
   std::optional<time_point> result;
   if (options.timeouts.total)
      result = time_point::now().safe_add(*options.timeouts.total);
   if (options.timeouts.inherit_task_deadline) {
      if (auto task_deadline = fc::task::current_deadline();
          task_deadline && *task_deadline < time_point::maximum() &&
          (!result || *task_deadline < *result)) {
         result = *task_deadline;
      }
   }
   return result;
}

/** One asynchronous operation's absolute deadline and timeout category. */
struct operation_deadline {
   time_point when;
   failure_kind timeout_kind;
};

/** Clamp an optional phase timeout to the request's optional total deadline. */
std::optional<operation_deadline>
phase_deadline(const std::optional<microseconds>& phase_timeout,
               failure_kind phase_failure,
               const std::optional<time_point>& total_deadline) {
   auto now = time_point::now();
   if (total_deadline && *total_deadline <= now) {
      throw transport_failure(
         failure_kind::timeout_total,
         "total request deadline expired");
   }
   if (!phase_timeout) {
      if (total_deadline)
         return operation_deadline{*total_deadline, failure_kind::timeout_total};
      return std::nullopt;
   }
   auto phase_end = now;
   phase_end.safe_add(*phase_timeout);
   if (total_deadline && *total_deadline < phase_end)
      return operation_deadline{*total_deadline, failure_kind::timeout_total};
   return operation_deadline{phase_end, phase_failure};
}

/** Reject control characters in caller-controlled HTTP header material. */
void validate_header_component(std::string_view value, std::string_view label) {
   const auto invalid = std::find_if(
      value.begin(),
      value.end(),
      [](unsigned char character) {
         return (character < 0x20 && character != '\t') ||
                character == 0x7f;
      });
   if (invalid != value.end()) {
      throw transport_failure(
         failure_kind::request_limit,
         "request " + std::string(label) +
            " contains a forbidden control character");
   }
}

/** Return a bounded printable HTTP reason phrase. */
std::string sanitize_reason(boost::beast::string_view reason) {
   constexpr size_t max_reason_bytes = 128;
   std::string result;
   result.reserve(std::min(reason.size(), max_reason_bytes));
   for (const unsigned char value : reason) {
      if (result.size() == max_reason_bytes)
         break;
      result.push_back(value >= 0x20 && value <= 0x7e ? static_cast<char>(value) : '?');
   }
   return result;
}

/** Return whether an Asio error can represent a stale or failed peer connection. */
bool is_retryable_connection_error(const boost::system::error_code& error) {
   return error == asio::error::eof ||
          error == asio::error::connection_reset ||
          error == asio::error::connection_aborted ||
          error == asio::error::broken_pipe ||
          error == asio::error::not_connected ||
          error == asio::error::shut_down ||
          error == beast_http::error::end_of_stream;
}

/** Return whether @p status belongs to the HTTP successful response class. */
bool is_success_status(uint32_t status) {
   return beast_http::to_status_class(status) ==
          beast_http::status_class::successful;
}

/** Return the integer value of a named HTTP status. */
constexpr uint32_t status_value(beast_http::status status) {
   return static_cast<uint32_t>(status);
}

/** Return whether @p host is safe to serialize into an HTTP authority and TLS identity. */
bool is_safe_network_host_impl(std::string_view host) {
   if (host.empty())
      return false;

   boost::system::error_code address_error;
   asio::ip::make_address(host, address_error);
   if (!address_error)
      return true;
   if (host.find(':') != std::string_view::npos)
      return false;

   return std::all_of(
      host.begin(),
      host.end(),
      [](unsigned char character) {
         return std::isalnum(character) ||
                character == '-' ||
                character == '.' ||
                character == '_';
      });
}

/** Return whether @p directory contains a parseable OpenSSL hashed CA entry. */
bool contains_hashed_ca_certificate(
   const std::filesystem::path& directory) {
   std::error_code iteration_error;
   std::filesystem::directory_iterator entries(
      directory,
      iteration_error);
   if (iteration_error)
      return false;

   for (const auto& entry : entries) {
      std::error_code type_error;
      if (!entry.is_regular_file(type_error) || type_error)
         continue;
      const auto name = entry.path().filename().string();
      if (name.size() < 10 || name[8] != '.' ||
          !std::all_of(
             name.begin(),
             name.begin() + 8,
             [](unsigned char character) {
                return std::isxdigit(character);
             }) ||
          !std::all_of(
             name.begin() + 9,
             name.end(),
             [](unsigned char character) {
                return std::isdigit(character);
             })) {
         continue;
      }

      ERR_clear_error();
      std::unique_ptr<BIO, decltype(&BIO_free)> input(
         BIO_new_file(entry.path().c_str(), "r"),
         &BIO_free);
      if (!input)
         continue;
      std::unique_ptr<X509, decltype(&X509_free)> certificate(
         PEM_read_bio_X509(
            input.get(),
            nullptr,
            nullptr,
            nullptr),
         &X509_free);
      if (certificate) {
         ERR_clear_error();
         return true;
      }
   }
   ERR_clear_error();
   return false;
}

/** Return the positive standard-library duration remaining before @p deadline. */
std::chrono::microseconds resolver_time_remaining(time_point deadline) {
   const auto now = time_point::now();
   if (deadline <= now) {
      throw transport_failure(
         failure_kind::timeout_connect,
         "DNS resolution deadline expired");
   }
   return std::chrono::microseconds((deadline - now).count());
}

class resolver_notifier;

/** Publish one resolver signal, retrying an interrupted descriptor write. */
template <typename WriteOnce>
bool write_resolver_signal(WriteOnce&& write_once) {
   for (;;) {
      const auto written = std::invoke(write_once);
      if (written == 1)
         return true;
      if (written < 0 && errno == EINTR)
         continue;
      return written < 0 &&
             (errno == EAGAIN || errno == EWOULDBLOCK);
   }
}

/** Create a non-blocking close-on-exec pipe without an inheritance race where supported. */
bool create_resolver_pipe(std::array<int, 2>& descriptors) {
#if defined(__linux__)
   return ::pipe2(descriptors.data(), O_CLOEXEC | O_NONBLOCK) == 0;
#else
   if (::pipe(descriptors.data()) != 0)
      return false;
   for (const auto descriptor : descriptors) {
      const auto status_flags =
         ::fcntl(descriptor, F_GETFL, 0);
      const auto descriptor_flags =
         ::fcntl(descriptor, F_GETFD, 0);
      if (status_flags < 0 ||
          descriptor_flags < 0 ||
          ::fcntl(
             descriptor,
             F_SETFL,
             status_flags | O_NONBLOCK) != 0 ||
          ::fcntl(
             descriptor,
             F_SETFD,
             descriptor_flags | FD_CLOEXEC) != 0) {
         return false;
      }
   }
   return true;
#endif
}

/**
 * One process-lifetime resolver service with its own platform lookup thread.
 *
 * Boost.Asio serializes getaddrinfo work within one execution_context even
 * when that context has several executor threads. Separate contexts preserve
 * the intended four-way lookup isolation.
 */
struct platform_resolver_worker {
   platform_resolver_worker()
      : context(std::make_shared<asio::io_context>())
      , work(asio::make_work_guard(*context)) {
      std::thread(
         [context = context] {
            context->run();
         })
         .detach();
   }

   std::shared_ptr<asio::io_context> context;
   asio::executor_work_guard<asio::io_context::executor_type> work;
};

/** Process-lifetime worker state with bounded concurrent platform DNS work. */
struct platform_resolver_runtime {
   platform_resolver_runtime() {
      for (auto& worker : workers)
         worker = std::make_unique<platform_resolver_worker>();
   }

   std::mutex mutex;
   std::array<std::unique_ptr<platform_resolver_worker>, platform_resolver_workers> workers;
   std::array<bool, platform_resolver_workers> active{};
   std::vector<std::weak_ptr<resolver_notifier>> waiters;

   /** Reserve one resolver worker without queueing behind a stalled lookup. */
   std::optional<size_t> try_acquire() {
      std::scoped_lock lock(mutex);
      const auto available =
         std::find(
            active.begin(),
            active.end(),
            false);
      if (available == active.end())
         return std::nullopt;
      *available = true;
      return static_cast<size_t>(
         std::distance(
            active.begin(),
            available));
   }

   /** Register an executor-independent waiter, closing the release-before-register race. */
   bool wait_for_capacity(
      const std::shared_ptr<resolver_notifier>& waiter);

   /** Release one worker and wake waiters without polling. */
   void release(size_t worker);

   /** Return the independent executor reserved for @p worker. */
   asio::any_io_executor executor(size_t worker) const {
      FC_ASSERT(worker < workers.size(), "Platform resolver worker index is invalid");
      return workers[worker]->context->get_executor();
   }
};

/** Internal signal that every platform resolver worker is currently occupied. */
class resolver_capacity_unavailable : public std::runtime_error {
public:
   resolver_capacity_unavailable()
      : std::runtime_error("platform resolver worker is busy") {}
};

/**
 * Executor-independent writer for one pipe notification.
 *
 * Resolver threads may retain this object after the client executor is gone.
 * It therefore owns only a pipe writer and plain atomic state.
 */
class resolver_notifier {
public:
   explicit resolver_notifier(int descriptor)
      : descriptor_number(descriptor) {}

   resolver_notifier(const resolver_notifier&) = delete;
   resolver_notifier& operator=(const resolver_notifier&) = delete;

   ~resolver_notifier() { disable(); }

   /** Wake the waiting coroutine once. */
   void notify() {
      std::scoped_lock lock(mutex);
      if (descriptor_number < 0 ||
          notified.load(std::memory_order_acquire))
         return;
      constexpr uint8_t signal = 1;
      if (write_resolver_signal(
             [&] {
                return ::write(
                   descriptor_number,
                   &signal,
                   sizeof(signal));
             })) {
         notified.store(true, std::memory_order_release);
      }
   }

   /** Prevent late background completions from writing after the reader closes. */
   void disable() {
      std::scoped_lock lock(mutex);
      if (descriptor_number < 0)
         return;
      ::close(descriptor_number);
      descriptor_number = -1;
   }

   /** Return whether this notifier has published its one signal. */
   bool was_notified() const {
      return notified.load(std::memory_order_acquire);
   }

private:
   mutable std::mutex mutex;
   int descriptor_number = -1;
   std::atomic_bool notified{false};
};

/** Executor-bound reader paired with an executor-independent notifier. */
class resolver_event {
public:
   explicit resolver_event(asio::any_io_executor executor)
      : event(std::move(executor)) {
      std::array<int, 2> descriptors{-1, -1};
      const bool configured =
         create_resolver_pipe(descriptors);
      if (!configured) {
         for (const auto descriptor : descriptors) {
            if (descriptor >= 0)
               ::close(descriptor);
         }
      }
      FC_ASSERT(
         configured,
         "Failed to create resolver notification pipe");
      notifier =
         std::make_shared<resolver_notifier>(
            descriptors[1]);

      boost::system::error_code error;
      event.assign(descriptors[0], error);
      if (error) {
         ::close(descriptors[0]);
         notifier.reset();
      }
      FC_ASSERT(
         !error,
         "Failed to attach resolver event: {}",
         error.message());
   }

   /** Disable the writer while the executor-bound read descriptor still exists. */
   ~resolver_event() {
      if (notifier)
         notifier->disable();
   }

   asio::posix::stream_descriptor event;
   std::shared_ptr<resolver_notifier> notifier;
};

bool platform_resolver_runtime::wait_for_capacity(
   const std::shared_ptr<resolver_notifier>& waiter) {
   bool notify_now = false;
   {
      std::scoped_lock lock(mutex);
      std::erase_if(
         waiters,
         [](const auto& candidate) {
            return candidate.expired();
         });
      if (std::find(active.begin(), active.end(), false) != active.end())
         notify_now = true;
      else if (
         waiters.size() <
         max_resolver_capacity_waiters)
         waiters.emplace_back(waiter);
      else
         return false;
   }
   if (notify_now)
      waiter->notify();
   return true;
}

void platform_resolver_runtime::release(size_t worker) {
   std::vector<std::shared_ptr<resolver_notifier>> live_waiters;
   {
      std::scoped_lock lock(mutex);
      FC_ASSERT(
         worker < active.size() && active[worker],
         "Platform resolver permit underflow");
      active[worker] = false;
      live_waiters.reserve(waiters.size());
      for (auto& weak : waiters) {
         if (auto waiter = weak.lock())
            live_waiters.push_back(std::move(waiter));
      }
      waiters.clear();
   }
   for (const auto& waiter : live_waiters)
      waiter->notify();
}

/** Permit preventing unbounded work from queuing behind stalled platform lookups. */
class platform_resolver_permit {
public:
   /** Acquire one platform resolver slot without blocking an I/O executor. */
   explicit platform_resolver_permit(platform_resolver_runtime& runtime)
      : _runtime(runtime) {
      const auto worker = _runtime.try_acquire();
      if (!worker)
         throw resolver_capacity_unavailable();
      _worker = *worker;
   }

   platform_resolver_permit(const platform_resolver_permit&) = delete;
   platform_resolver_permit& operator=(const platform_resolver_permit&) = delete;

   /** Release the resolver slot after the platform lookup really completes. */
   ~platform_resolver_permit() { _runtime.release(_worker); }

   /** Return this permit's independently serviced resolver executor. */
   asio::any_io_executor executor() const {
      return _runtime.executor(_worker);
   }

private:
   platform_resolver_runtime& _runtime;
   size_t _worker = 0;
};

/**
 * Return intentionally process-lifetime resolver state.
 *
 * Joining an executor whose platform resolver is stuck would reintroduce an
 * unbounded process-exit wait.
 */
platform_resolver_runtime& resolver_runtime() {
   static auto* runtime = new platform_resolver_runtime;
   return *runtime;
}

/** Start one platform lookup whose completion state may outlive its caller. */
detail::resolver_cancel_fn start_platform_resolution(
   const std::string& host,
   const std::string& service,
   time_point deadline,
   detail::resolver_complete_fn complete) {
   auto& runtime = resolver_runtime();
   auto permit =
      std::make_shared<platform_resolver_permit>(
         runtime);
   (void)resolver_time_remaining(deadline);

   auto resolver =
      std::make_shared<tcp::resolver>(permit->executor());
   resolver->async_resolve(
      host,
      service,
      [resolver,
       permit,
       complete = std::move(complete)](
         const boost::system::error_code& error,
         tcp::resolver::results_type results) mutable {
         (void)resolver;
         (void)permit;
         if (error) {
            complete(error.message(), {});
            return;
         }

         std::vector<detail::resolved_endpoint> endpoints;
         endpoints.reserve(results.size());
         for (const auto& result : results) {
            const auto address =
               result.endpoint().address().to_string();
            endpoints.push_back(
               detail::resolved_endpoint{
                  .address = address,
                  .port = result.endpoint().port(),
               });
         }
         complete(std::nullopt, std::move(endpoints));
      });
   return [resolver] {
      asio::post(
         resolver->get_executor(),
         [resolver] { resolver->cancel(); });
   };
}

/** Convert the public request method enum to a Beast verb by enum spelling. */
beast_http::verb to_beast_verb(request_method method) {
   std::string name(magic_enum::enum_name(method));
   FC_ASSERT(!name.empty(), "Unknown outbound HTTP method");
   if (name.back() == '_')
      name.pop_back();
   std::transform(
      name.begin(),
      name.end(),
      name.begin(),
      [](unsigned char value) { return static_cast<char>(std::toupper(value)); });
   const auto verb = beast_http::string_to_verb(name);
   FC_ASSERT(verb != beast_http::verb::unknown, "Unsupported outbound HTTP method {}", name);
   return verb;
}

/** Disk-checked atomic file sink used by transport::perform_to_file. */
class file_sink {
public:
   file_sink(const std::filesystem::path& output,
             uint64_t max_response_body_bytes,
             std::function<void(const http_file_download_status&)> status_callback,
             std::function<uint64_t(const std::filesystem::path&)> space_available_provider)
      : _output(output)
      , _temp(output)
      , _max_response_body_bytes(max_response_body_bytes)
      , _status_callback(std::move(status_callback))
      , _space_available_provider(std::move(space_available_provider))
      , _started(std::chrono::steady_clock::now())
      , _next_status(_started) {
      _temp += ".downloading";
   }

   file_sink(const file_sink&) = delete;
   file_sink& operator=(const file_sink&) = delete;

   /** Remove an incomplete temporary file. */
   ~file_sink() {
      if (!_complete) {
         if (_file.is_open())
            _file.close();
         std::error_code error;
         std::filesystem::remove(_temp, error);
      }
   }

   /** Report connection-establishment phase. */
   void connecting() {
      std::scoped_lock lock(_mutex);
      transition(http_file_download_phase::connecting);
   }

   /** Report request-upload phase. */
   void sending_request() {
      std::scoped_lock lock(_mutex);
      transition(http_file_download_phase::sending_request);
   }

   /** Report response-header phase. */
   void waiting_for_response() {
      std::scoped_lock lock(_mutex);
      transition(http_file_download_phase::waiting_for_response);
   }

   /** Validate response metadata and open the temporary file for a successful response. */
   void headers(uint32_t status,
                std::optional<uint64_t> content_length) {
      std::scoped_lock lock(_mutex);
      _status = status;
      _content_length = content_length;
      if (_content_length && *_content_length > _max_response_body_bytes) {
         throw transport_failure(
            failure_kind::response_limit,
            "response body exceeds configured maximum of " +
               std::to_string(_max_response_body_bytes) + " bytes");
      }
      if (_status != status_value(beast_http::status::ok))
         return;

      if (_content_length) {
         FC_ASSERT(
            *_content_length <=
               std::numeric_limits<uint64_t>::max() -
                  disk_space_concurrency_margin_bytes,
            "HTTP response Content-Length {} is too large for the {}-byte disk safety margin",
            *_content_length,
            disk_space_concurrency_margin_bytes);
         require_disk_space(
            *_content_length == 0
               ? 0
               : *_content_length + disk_space_concurrency_margin_bytes);
         _disk_space_write_budget =
            std::min(*_content_length, disk_space_check_interval_bytes);
      } else {
         _disk_space_write_budget =
            refresh_disk_space_write_budget(_max_response_body_bytes);
      }
      _next_disk_space_check =
         std::chrono::steady_clock::now() + disk_space_check_interval;

      _file.open(_temp, std::ios::binary | std::ios::trunc);
      FC_ASSERT(
         _file.is_open(),
         "Failed to open temp file {} for writing",
         _temp.string());
      transition(http_file_download_phase::downloading);
   }

   /** Consume one decoded response-body block; false stops a bounded error response. */
   bool body(const char* data, size_t bytes) {
      std::scoped_lock lock(_mutex);
      if (_status != status_value(beast_http::status::ok)) {
         const auto room = max_error_response_body_bytes - _error_body.size();
         _error_body.append(data, std::min(bytes, room));
         return _error_body.size() < max_error_response_body_bytes;
      }
      if (bytes == 0)
         return true;

      if (_disk_space_write_budget < bytes ||
          std::chrono::steady_clock::now() >= _next_disk_space_check) {
         const auto response_limit =
            _content_length.value_or(_max_response_body_bytes);
         const auto remaining = response_limit - _downloaded_bytes;
         _disk_space_write_budget =
            refresh_disk_space_write_budget(remaining);
         _next_disk_space_check =
            std::chrono::steady_clock::now() + disk_space_check_interval;
      }

      _file.write(data, static_cast<std::streamsize>(bytes));
      FC_ASSERT(
         _file.good(),
         "Failed to write {} bytes to temp file {}",
         bytes,
         _temp.string());
      _downloaded_bytes += bytes;
      _disk_space_write_budget -= bytes;
      report(false);
      return true;
   }

   /** Emit a periodic download status while a socket operation is pending. */
   void progress() {
      std::scoped_lock lock(_mutex);
      report(false);
   }

   /** Validate status, atomically publish a completed file, and report completion. */
   void finish() {
      std::scoped_lock lock(_mutex);
      if (_status != status_value(beast_http::status::ok)) {
         auto message =
            "HTTP POST failed with status " + std::to_string(_status);
         if (!_error_body.empty())
            message += ": " + _error_body;
         throw transport_failure(failure_kind::http_status, std::move(message));
      }

      _file.close();
      FC_ASSERT(
         _file.good(),
         "Failed to close temp file {} after HTTP download",
         _temp.string());
      std::error_code error;
      std::filesystem::rename(_temp, _output, error);
      FC_ASSERT(!error, "Failed to rename downloaded file: {}", error.message());
      _complete = true;
      transition(http_file_download_phase::complete);
   }

   /** Return decoded response bytes retained or written by this sink. */
   uint64_t received_bytes() const {
      std::scoped_lock lock(_mutex);
      return _status == status_value(beast_http::status::ok)
                ? _downloaded_bytes
                : _error_body.size();
   }

private:
   /** Return a path on the destination filesystem suitable for a space query. */
   std::filesystem::path space_query_path() const {
      if (_output.has_parent_path())
         return _output.parent_path();
      std::error_code error;
      auto current = std::filesystem::current_path(error);
      FC_ASSERT(
         !error,
         "Failed to resolve current directory for download disk-space check: {}",
         error.message());
      return current;
   }

   /** Return currently available destination-filesystem bytes. */
   uint64_t available_disk_space() const {
      const auto query_path = space_query_path();
      if (_space_available_provider)
         return _space_available_provider(query_path);
      std::error_code error;
      const auto space = std::filesystem::space(query_path, error);
      FC_ASSERT(
         !error,
         "Failed to query free disk space at {}: {}",
         query_path.string(),
         error.message());
      return space.available;
   }

   /** Require @p required bytes on the destination filesystem. */
   void require_disk_space(uint64_t required) const {
      const auto available = available_disk_space();
      FC_ASSERT(
         required <= available,
         "Insufficient disk space at {} for HTTP download: {} bytes available and {} bytes required",
         space_query_path().string(),
         available,
         required);
   }

   /** Verify and return the next amortized disk-space write budget. */
   uint64_t refresh_disk_space_write_budget(uint64_t remaining) const {
      const auto budget =
         std::min(remaining, disk_space_check_interval_bytes);
      if (budget == 0) {
         require_disk_space(0);
         return 0;
      }
      require_disk_space(budget + disk_space_concurrency_margin_bytes);
      return budget;
   }

   /** Change phase and force a status notification. */
   void transition(http_file_download_phase phase) {
      _phase = phase;
      report(true);
   }

   /** Emit a rate-limited, non-throwing status notification. */
   void report(bool force) {
      if (!_status_callback)
         return;
      const auto now = std::chrono::steady_clock::now();
      if (!force && now < _next_status)
         return;
      const auto elapsed =
         std::chrono::duration_cast<std::chrono::microseconds>(now - _started);
      try {
         _status_callback(http_file_download_status{
            .phase = _phase,
            .downloaded_bytes = _downloaded_bytes,
            .total_bytes = _content_length,
            .elapsed = microseconds(elapsed.count()),
         });
      } catch (...) {
      }
      _next_status = now + download_status_interval;
   }

   std::filesystem::path _output;
   std::filesystem::path _temp;
   uint64_t _max_response_body_bytes;
   std::function<void(const http_file_download_status&)> _status_callback;
   std::function<uint64_t(const std::filesystem::path&)>
      _space_available_provider;
   std::ofstream _file;
   uint32_t _status = 0;
   std::optional<uint64_t> _content_length;
   std::string _error_body;
   uint64_t _downloaded_bytes = 0;
   uint64_t _disk_space_write_budget = 0;
   std::chrono::steady_clock::time_point _next_disk_space_check;
   http_file_download_phase _phase = http_file_download_phase::connecting;
   std::chrono::steady_clock::time_point _started;
   std::chrono::steady_clock::time_point _next_status;
   bool _complete = false;
   mutable std::mutex _mutex;
};

/** Periodically report an attended download even while one socket read is pending. */
class download_progress_reporter
   : public std::enable_shared_from_this<download_progress_reporter> {
public:
   download_progress_reporter(
      asio::any_io_executor executor,
      std::shared_ptr<file_sink> sink)
      : _timer(std::move(executor))
      , _sink(std::move(sink)) {}

   /** Start rate-limited progress observation. */
   void start() {
      asio::post(
         _timer.get_executor(),
         [self = shared_from_this()] {
            if (!self->_stopped)
               self->arm();
         });
   }

   /** Stop observation without retaining the download sink. */
   void stop() {
      asio::post(
         _timer.get_executor(),
         [self = shared_from_this()] {
            self->_stopped = true;
            self->_timer.cancel();
            self->_sink.reset();
         });
   }

private:
   /** Schedule the next observation on the client executor. */
   void arm() {
      _timer.expires_after(download_status_interval);
      _timer.async_wait(
         [self = shared_from_this()](
            const boost::system::error_code& error) {
            if (error || self->_stopped)
               return;
            if (self->_sink)
               self->_sink->progress();
            self->arm();
         });
   }

   asio::steady_timer _timer;
   std::shared_ptr<file_sink> _sink;
   bool _stopped = false;
};

/** Stop a periodic reporter whenever its owning coroutine leaves scope. */
class download_progress_scope {
public:
   explicit download_progress_scope(
      std::shared_ptr<download_progress_reporter> reporter)
      : _reporter(std::move(reporter)) {
      _reporter->start();
   }

   download_progress_scope(const download_progress_scope&) = delete;
   download_progress_scope& operator=(const download_progress_scope&) = delete;

   ~download_progress_scope() { _reporter->stop(); }

private:
   std::shared_ptr<download_progress_reporter> _reporter;
};

} // namespace
namespace {

/** One plain, authenticated TLS, or Unix-domain HTTP/1.1 connection. */
struct connection_state {
   using tcp_stream = beast::tcp_stream;
   using tls_stream = asio::ssl::stream<tcp_stream>;
   using unix_stream = beast::basic_stream<local::stream_protocol>;
   using stream_variant = std::variant<
      std::unique_ptr<tcp_stream>,
      std::unique_ptr<tls_stream>,
      std::unique_ptr<unix_stream>>;

   explicit connection_state(stream_variant stream_in)
      : stream(std::move(stream_in)) {}

   /** Return whether the lowest-layer socket remains open. */
   bool open() const {
      return std::visit(
         [](const auto& value) {
            return beast::get_lowest_layer(*value).socket().is_open();
         },
         stream);
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
            const auto descriptor =
               beast::get_lowest_layer(*value)
                  .socket()
                  .native_handle();
            char byte = 0;
            for (;;) {
               const auto result =
                  ::recv(
                     descriptor,
                     &byte,
                     sizeof(byte),
                     MSG_DONTWAIT | MSG_PEEK);
               if (result >= 0)
                  return false;
               if (errno == EINTR)
                  continue;
               return errno == EAGAIN ||
                      errno == EWOULDBLOCK;
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

/** Opaque in-process lease retained across a connection-affine continuation. */
struct connection_affinity {
   std::shared_ptr<connection_state> connection;
   std::string connection_key;

   explicit operator bool() const noexcept {
      return static_cast<bool>(connection);
   }
};

/**
 * Cancellation ownership for one logical request.
 *
 * The cancellation-slot handler survives the header/body boundary. Each active resolver, timer,
 * or socket operation installs a lifetime-safe callback and clears it before returning.
 */
class request_control : public std::enable_shared_from_this<request_control> {
public:
   static std::shared_ptr<request_control>
   create(asio::cancellation_slot slot,
          asio::any_io_executor executor) {
      auto result = std::shared_ptr<request_control>(
         new request_control(std::move(executor)));
      if (slot.is_connected()) {
         slot.assign(
            [weak = std::weak_ptr<request_control>(result)](
               asio::cancellation_type_t type) {
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
         throw transport_failure(
            failure_kind::cancelled,
            std::string(phase) + " cancelled");
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
      asio::post(
         _executor,
         [weak = weak_from_this()] {
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
   active_cancel_guard(
      std::shared_ptr<request_control> control,
      std::function<void()> cancel_active)
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
   void add_response_bytes(uint64_t bytes) {
      response_bytes.fetch_add(bytes, std::memory_order_relaxed);
   }

   void finish_status(uint32_t status) {
      if (finalized.exchange(true, std::memory_order_acq_rel))
         return;
      shared_metrics().response_bytes.fetch_add(
         response_bytes.load(std::memory_order_relaxed),
         std::memory_order_relaxed);
      if (is_success_status(status))
         shared_metrics().successes.fetch_add(1, std::memory_order_relaxed);
      else
         record_failure(failure_kind::http_status);
   }

   void finish_failure(failure_kind failure) {
      if (finalized.exchange(true, std::memory_order_acq_rel))
         return;
      shared_metrics().response_bytes.fetch_add(
         response_bytes.load(std::memory_order_relaxed),
         std::memory_order_relaxed);
      record_failure(failure);
   }

   std::atomic<uint64_t> response_bytes{0};
   std::atomic_bool finalized{false};
};

/** Executor-independent result that may briefly outlive its requesting client. */
struct async_resolution_result {
   std::mutex mutex;
   bool complete = false;
   bool deadline_expired = false;
   std::optional<std::string> error;
   std::vector<detail::resolved_endpoint> endpoints;
};

/** Resolver wait state whose Asio objects remain owned by the client executor. */
struct async_resolution_state {
   explicit async_resolution_state(asio::any_io_executor executor)
      : notification(executor)
      , deadline_timer(std::move(executor)) {}

   resolver_event notification;
   asio::steady_timer deadline_timer;
   std::shared_ptr<async_resolution_result> result =
      std::make_shared<async_resolution_result>();
   detail::resolver_cancel_fn cancel;
};

} // namespace

bool is_safe_network_host(std::string_view host) {
   return is_safe_network_host_impl(host);
}

void detail::post_platform_resolver_worker_task_for_testing(
   size_t worker,
   std::function<void()> task) {
   FC_ASSERT(
      worker < platform_resolver_workers,
      "Platform resolver test worker index is invalid");
   asio::post(
      resolver_runtime().executor(worker),
      std::move(task));
}

bool detail::write_resolver_signal_for_testing(
   const std::function<int64_t()>& write_once) {
   return write_resolver_signal(write_once);
}

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

   explicit client_impl(
      asio::any_io_executor executor,
      transport_options options_in,
      detail::resolver_start_fn resolver_start_in = {})
      : strand(asio::make_strand(std::move(executor)))
      , options(std::move(options_in))
      , tls_context(asio::ssl::context::tls_client)
      , resolver_start(std::move(resolver_start_in)) {
      FC_ASSERT(
         !options.dns_cache_timeout ||
            options.dns_cache_timeout->count() >= 0,
         "Outbound HTTP DNS cache timeout cannot be negative");
      FC_ASSERT(
         options.max_idle_connection_age.count() > 0,
         "Outbound HTTP idle connection age must be positive");
      tls_context.set_verify_mode(asio::ssl::verify_peer);
      error_code error;
      tls_context.set_default_verify_paths(error);
      FC_ASSERT(
         !error,
         "Outbound HTTPS tls_ca failure loading system trust roots: {}",
         error.message());

      const auto ca_file = options.additional_ca_file
                              ? options.additional_ca_file->c_str()
                              : nullptr;
      const auto ca_path = options.additional_ca_path
                              ? options.additional_ca_path->c_str()
                              : nullptr;
      if (options.additional_ca_path) {
         FC_ASSERT(
            contains_hashed_ca_certificate(*options.additional_ca_path),
            "Outbound HTTPS tls_ca directory is empty, malformed, or unreadable");
      }
      if (ca_file || ca_path) {
         ERR_clear_error();
         FC_ASSERT(
            SSL_CTX_load_verify_locations(
               tls_context.native_handle(),
               ca_file,
               ca_path) == 1,
            "Outbound HTTPS tls_ca configuration is malformed or unreadable");
         ERR_clear_error();
      }

      if (options.proxy) {
         const fc::url parsed(*options.proxy);
         FC_ASSERT(
            parsed.proto() == scheme_http,
            "Outbound HTTP proxy must use the http scheme");
         FC_ASSERT(
            parsed.host() && !parsed.host()->empty(),
            "Outbound HTTP proxy URL is missing a host");
         FC_ASSERT(
            is_safe_network_host(*parsed.host()),
            "Outbound HTTP proxy URL has an invalid host");
         FC_ASSERT(
            !parsed.user() && !parsed.pass(),
            "Outbound HTTP proxy credentials are not supported");
         proxy_host = *parsed.host();
         proxy_service =
            parsed.port()
               ? std::to_string(*parsed.port())
               : std::string(default_http_service);
      }
      if (!resolver_start)
         resolver_start = start_platform_resolution;
   }

   ~client_impl() {
      for (auto& [key, connections] : idle_connections) {
         (void)key;
         for (auto& connection : connections)
            connection.connection->close();
      }
   }

   asio::strand<asio::any_io_executor> strand;
   transport_options options;
   asio::ssl::context tls_context;
   detail::resolver_start_fn resolver_start;
   std::optional<std::string> proxy_host;
   std::optional<std::string> proxy_service;
   std::map<std::string, std::vector<idle_connection>>
      idle_connections;
   size_t idle_connection_count = 0;
   std::map<std::string, dns_entry> dns_cache;
   std::map<std::string, target_info> unix_target_cache;

   /** Validate request budgets and explicit retry safety. */
   static void validate_policy(const request_options& policy) {
      FC_ASSERT(
         policy.max_request_header_bytes > 0,
         "Outbound HTTP request-header limit must be positive");
      FC_ASSERT(
         policy.max_response_header_bytes > 0,
         "Outbound HTTP response-header limit must be positive");
      FC_ASSERT(
         !policy.timeouts.connect || policy.timeouts.connect->count() > 0,
         "Outbound HTTP connect timeout must be positive when present");
      FC_ASSERT(
         !policy.timeouts.header || policy.timeouts.header->count() > 0,
         "Outbound HTTP header timeout must be positive when present");
      FC_ASSERT(
         !policy.timeouts.read || policy.timeouts.read->count() > 0,
         "Outbound HTTP read timeout must be positive when present");
      FC_ASSERT(
         !policy.timeouts.idle || policy.timeouts.idle->count() > 0,
         "Outbound HTTP idle timeout must be positive when present");
      FC_ASSERT(
         !policy.timeouts.total || policy.timeouts.total->count() > 0,
         "Outbound HTTP total timeout must be positive when present");
      FC_ASSERT(
         policy.retry.max_attempts > 0,
         "Outbound HTTP retry attempts must be positive");
      FC_ASSERT(
         policy.retry.max_attempts == 1 || policy.idempotent,
         "Outbound HTTP retries require an explicitly idempotent request");
      FC_ASSERT(
         policy.retry.initial_backoff.count() >= 0 &&
            policy.retry.max_backoff.count() >= 0,
         "Outbound HTTP retry backoff cannot be negative");
   }

   /** Return a conservative serialized request-header byte estimate. */
   static uint64_t estimate_request_header_bytes(const request& req) {
      constexpr uint64_t transport_header_allowance = 256;
      uint64_t result = transport_header_allowance;
      auto add = [&](uint64_t bytes) {
         FC_ASSERT(
            bytes <= std::numeric_limits<uint64_t>::max() - result,
            "Outbound HTTP request header byte count overflow");
         result += bytes;
      };
      add(req.target.proto().size());
      if (const auto host = req.target.host())
         add(host->size());
      if (const auto path = req.target.path())
         add(path->generic_string().size());
      if (const auto query = req.target.query())
         add(query->size());
      add(req.user_agent.size());
      add(req.content_type.size());
      for (const auto& [name, value] : req.headers) {
         add(name.size());
         add(value.size());
         add(4);
      }
      return result;
   }

   /** Validate a request before any resolver or socket operation begins. */
   static void validate_request(const request& req,
                                const request_options& policy) {
      auto fail = [](std::string message) {
         throw transport_failure(
            failure_kind::request_limit,
            std::move(message));
      };
      if (estimate_request_header_bytes(req) >
          policy.max_request_header_bytes) {
         fail(
            "request headers exceed configured maximum of " +
            std::to_string(policy.max_request_header_bytes) + " bytes");
      }
      if (req.body.size() > policy.max_request_body_bytes) {
         fail(
            "request body exceeds configured maximum of " +
            std::to_string(policy.max_request_body_bytes) + " bytes");
      }

      validate_header_component(req.user_agent, "User-Agent");
      validate_header_component(req.content_type, "Content-Type");
      for (const auto& [name, value] : req.headers) {
         validate_header_component(name, "header name");
         validate_header_component(value, "header value");
         const auto valid_name_character = [](unsigned char character) {
            return std::isalnum(character) ||
                   std::string_view("!#$%&'*+-.^_`|~").find(
                      static_cast<char>(character)) != std::string_view::npos;
         };
         if (name.empty() ||
             !std::all_of(name.begin(), name.end(), valid_name_character)) {
            fail("request contains an invalid header name");
         }
         const auto field = beast_http::string_to_field(name);
         if (field == beast_http::field::host ||
             field == beast_http::field::content_length ||
             field == beast_http::field::transfer_encoding ||
             field == beast_http::field::connection ||
             field == beast_http::field::proxy_connection ||
             field == beast_http::field::proxy_authorization ||
             field == beast_http::field::expect) {
            fail("request attempts to override a transport-controlled header");
         }
      }
   }

   /** Parse an HTTP, HTTPS, or Unix target into connection-safe components. */
   target_info normalize_target(const url& target) {
      FC_ASSERT(target.host(), "Outbound HTTP URL is missing a host");
      target_info result{
         .scheme = target.proto(),
         .host = *target.host(),
      };
      if (result.scheme == scheme_unix) {
         if (auto found = unix_target_cache.find(result.host);
             found != unix_target_cache.end()) {
            result = found->second;
            if (target.query())
               result.request_target += "?" + *target.query();
            return result;
         }

         const auto complete_path =
            std::filesystem::path(result.host);
         std::filesystem::path socket_file(result.host);
         FC_ASSERT(
            socket_file.is_absolute(),
            "Unix-socket URL cannot be relative");
         while (!socket_file.empty()) {
            std::error_code socket_error;
            if (std::filesystem::is_socket(socket_file, socket_error) &&
                !socket_error) {
               break;
            }
            const auto parent = socket_file.parent_path();
            if (parent.empty() || parent == socket_file) {
               socket_file.clear();
               break;
            }
            socket_file = parent;
         }
         if (socket_file.empty()) {
            throw transport_failure(
               failure_kind::connect,
               "Unix socket path does not exist");
         }
         result.unix_socket_path = socket_file.string();
         result.host = "localhost";
         result.host_header = "localhost";
         const auto request_path =
            complete_path.lexically_relative(socket_file);
         const bool root_request =
            request_path.empty() ||
            request_path ==
               std::filesystem::path{filesystem_current_directory};
         result.request_target =
            root_request
               ? "/"
               : "/" + request_path.generic_string();
         result.connection_key = "unix|" + *result.unix_socket_path;
         unix_target_cache.emplace(*target.host(), result);
      } else {
         FC_ASSERT(
            result.scheme == scheme_http || result.scheme == scheme_https,
            "Unsupported outbound HTTP URL scheme: {}",
            result.scheme);
         if (!is_safe_network_host(result.host)) {
            throw transport_failure(
               failure_kind::request_limit,
               "request URL has an invalid host");
         }
         result.tls = result.scheme == scheme_https;
         result.service = target.port()
                             ? std::to_string(*target.port())
                             : std::string(
                                  result.tls
                                     ? default_https_service
                                     : default_http_service);
         const bool ipv6 = result.host.find(':') != std::string::npos;
         result.host_header =
            ipv6 ? "[" + result.host + "]" : result.host;
         const bool default_port =
            !target.port() ||
            (result.tls &&
             *target.port() == default_https_port) ||
            (!result.tls &&
             *target.port() == default_http_port);
         if (!default_port)
            result.host_header += ":" + result.service;
         result.request_target =
            target.path() ? target.path()->generic_string() : "/";
         if (result.request_target.empty())
            result.request_target = "/";
         result.connection_key =
            result.scheme + "|" + result.host + "|" + result.service;
      }
      if (target.query())
         result.request_target += "?" + *target.query();
      return result;
   }

   /** Set or clear a Beast logical-operation deadline, rejecting an expired phase. */
   template <typename Stream>
   static void arm_operation_deadline(
      Stream& stream,
      const std::optional<operation_deadline>& deadline) {
      if (!deadline) {
         beast::get_lowest_layer(stream).expires_never();
         return;
      }
      const auto remaining = deadline->when - time_point::now();
      if (remaining.count() <= 0) {
         throw transport_failure(
            deadline->timeout_kind,
            "request deadline expired");
      }
      beast::get_lowest_layer(stream).expires_after(
         std::chrono::microseconds(remaining.count()));
   }

   /** Convert a Beast/Asio timeout into the selected phase category. */
   static void throw_if_operation_failed(
      const error_code& error,
      const std::optional<operation_deadline>& deadline,
      const std::shared_ptr<request_control>& control) {
      control->throw_if_cancelled();
      if (deadline &&
          (error == beast::error::timeout ||
           error == asio::error::timed_out)) {
         throw transport_failure(
            deadline->timeout_kind,
            "request deadline expired");
      }
   }

   /** Resolve one host under an optional connection deadline with a bounded cache. */
   asio::awaitable<std::vector<tcp::endpoint>>
   resolve(const std::string& host,
           const std::string& service,
           std::optional<operation_deadline> deadline,
           const std::shared_ptr<request_control>& control) {
      const auto cache_key = host + "|" + service;
      if (auto found = dns_cache.find(cache_key);
          found != dns_cache.end() &&
          (!options.dns_cache_timeout ||
           found->second.expires > std::chrono::steady_clock::now())) {
         co_return found->second.endpoints;
      }
      dns_cache.erase(cache_key);
      control->throw_if_cancelled("DNS resolution");

      detail::resolver_cancel_fn cancel_resolution;
      auto state = std::make_shared<async_resolution_state>(strand);
      for (;;) {
         if (deadline && deadline->when <= time_point::now()) {
            throw transport_failure(
               deadline->timeout_kind,
               "DNS resolution deadline expired");
         }
         bool capacity_unavailable = false;
         try {
            auto weak_result =
               std::weak_ptr<async_resolution_result>(
                  state->result);
            auto weak_notifier =
               std::weak_ptr<resolver_notifier>(
                  state->notification.notifier);
            cancel_resolution = resolver_start(
               host,
               service,
               deadline ? deadline->when : time_point::maximum(),
               [weak_result,
                weak_notifier](
                  std::optional<std::string> error,
                  std::vector<detail::resolved_endpoint> endpoints) mutable {
                  auto result = weak_result.lock();
                  auto notifier = weak_notifier.lock();
                  if (!result || !notifier)
                     return;
                  {
                     std::scoped_lock lock(result->mutex);
                     if (result->complete)
                        return;
                     result->complete = true;
                     result->error = std::move(error);
                     result->endpoints =
                        std::move(endpoints);
                  }
                  notifier->notify();
               });
            break;
         } catch (const resolver_capacity_unavailable&) {
            capacity_unavailable = true;
         } catch (const transport_failure&) {
            throw;
         } catch (...) {
            throw transport_failure(
               failure_kind::dns,
               "DNS resolver failed to start",
               true);
         }
         if (capacity_unavailable) {
            auto waiter =
               std::make_shared<resolver_event>(
                  strand);
            auto timeout =
               std::make_shared<asio::steady_timer>(
                  strand);
            auto timed_out =
               std::make_shared<std::atomic_bool>(
                  false);
            if (deadline) {
               const auto remaining = deadline->when - time_point::now();
               if (remaining.count() <= 0) {
                  throw transport_failure(
                     deadline->timeout_kind,
                     "DNS resolution deadline expired");
               }
               timeout->expires_after(
                  std::chrono::microseconds(
                     remaining.count()));
               timeout->async_wait(
                  [waiter, timed_out](
                     const error_code& error) {
                     if (error)
                        return;
                     timed_out->store(
                        true,
                        std::memory_order_release);
                     waiter->event.cancel();
                  });
            }
            if (!resolver_runtime()
                    .wait_for_capacity(
                       waiter->notifier)) {
               throw transport_failure(
                  failure_kind::dns,
                  "DNS resolver admission queue is full",
                  true);
            }
            active_cancel_guard cancel_guard(
               control,
               [waiter, timeout] {
                  timeout->cancel();
                  waiter->event.cancel();
               });
            uint8_t signal = 0;
            error_code error;
            (void)co_await waiter->event.async_read_some(
               asio::buffer(&signal, sizeof(signal)),
               asio::redirect_error(asio::use_awaitable, error));
            timeout->cancel();
            control->throw_if_cancelled("DNS resolution");
            if (timed_out->load(
                   std::memory_order_acquire)) {
               throw transport_failure(
                  deadline->timeout_kind,
                  "DNS resolution deadline expired");
            }
            if (!waiter->notifier
                    ->was_notified()) {
               throw transport_failure(
                  failure_kind::dns,
                  "DNS resolver capacity wait ended",
                  true);
            }
         }
      }

      state->cancel = std::move(cancel_resolution);
      if (deadline) {
         const auto remaining = deadline->when - time_point::now();
         if (remaining.count() <= 0) {
            if (state->cancel)
               state->cancel();
            throw transport_failure(
               deadline->timeout_kind,
               "DNS resolution deadline expired");
         }
         state->deadline_timer.expires_after(
            std::chrono::microseconds(remaining.count()));
         auto weak =
            std::weak_ptr<async_resolution_state>(
               state);
         state->deadline_timer.async_wait(
            [weak](const error_code& error) {
               if (error)
                  return;
               auto current = weak.lock();
               if (!current)
                  return;
               {
                  std::scoped_lock lock(
                     current->result->mutex);
                  if (current->result->complete)
                     return;
                  current->result->complete = true;
                  current->result->deadline_expired = true;
               }
               if (current->cancel)
                  current->cancel();
               current->notification.event.cancel();
            });
      }
      active_cancel_guard cancel_guard(
         control,
         [state] {
            if (state->cancel)
               state->cancel();
            state->deadline_timer.cancel();
            state->notification.event.cancel();
         });
      uint8_t signal = 0;
      error_code wait_error;
      (void)co_await
         state->notification.event.async_read_some(
            asio::buffer(&signal, sizeof(signal)),
         asio::redirect_error(asio::use_awaitable, wait_error));
      state->deadline_timer.cancel();
      control->throw_if_cancelled("DNS resolution");
      std::optional<std::string> resolution_error;
      std::vector<detail::resolved_endpoint>
         resolved_endpoints;
      bool resolution_complete = false;
      bool deadline_expired = false;
      {
         std::scoped_lock lock(state->result->mutex);
         resolution_complete = state->result->complete;
         deadline_expired =
            state->result->deadline_expired;
         if (resolution_complete) {
            resolution_error = state->result->error;
            resolved_endpoints =
               std::move(state->result->endpoints);
         } else {
            state->result->complete = true;
         }
      }
      if (deadline_expired) {
         throw transport_failure(
            deadline->timeout_kind,
            "DNS resolution deadline expired");
      }
      if (!resolution_complete) {
         if (state->cancel)
            state->cancel();
         if (deadline) {
            throw transport_failure(
               deadline->timeout_kind,
               "DNS resolution deadline expired");
         }
         throw transport_failure(
            failure_kind::dns,
            "DNS resolution wait ended before completion",
            true);
      }
      state->cancel = {};
      if (resolution_error) {
         throw transport_failure(
            failure_kind::dns,
            "DNS resolution failed",
            true);
      }
      if (resolved_endpoints.empty()) {
         throw transport_failure(
            failure_kind::dns,
            "DNS resolution returned no endpoints",
            true);
      }

      std::vector<tcp::endpoint> endpoints;
      endpoints.reserve(resolved_endpoints.size());
      for (const auto& entry : resolved_endpoints) {
         error_code address_error;
         const auto address =
            asio::ip::make_address(entry.address, address_error);
         if (address_error) {
            throw transport_failure(
               failure_kind::dns,
               "DNS resolution returned an invalid endpoint",
               true);
         }
         endpoints.emplace_back(address, entry.port);
      }
      if (!options.dns_cache_timeout ||
          options.dns_cache_timeout->count() != 0) {
         const auto expires =
            !options.dns_cache_timeout
               ? std::chrono::steady_clock::time_point::max()
               : std::chrono::steady_clock::now() +
                    std::chrono::microseconds(
                       options.dns_cache_timeout->count());
         dns_cache.insert_or_assign(
            cache_key,
            dns_entry{endpoints, expires});
      }
      co_return endpoints;
   }

   /** Connect a TCP stream to already-resolved endpoints. */
   asio::awaitable<void>
   connect_tcp(const std::shared_ptr<connection_state>& connection,
               connection_state::tcp_stream& stream,
               const std::vector<tcp::endpoint>& endpoints,
               const std::string& host,
               const std::string& service,
               std::optional<operation_deadline> deadline,
               const std::shared_ptr<request_control>& control) {
      arm_operation_deadline(stream, deadline);
      active_cancel_guard cancel_guard(
         control,
         [connection] { connection->cancel(); });
      error_code error;
      (void)co_await stream.async_connect(
         endpoints,
         asio::redirect_error(asio::use_awaitable, error));
      throw_if_operation_failed(error, deadline, control);
      if (error) {
         if (options.refresh_dns_on_connection_failure)
            dns_cache.erase(host + "|" + service);
         throw transport_failure(
            failure_kind::connect,
            "Failed to connect: " + error.message(),
            true);
      }
   }

   /** Write one complete request under an optional total or connect deadline. */
   template <typename Stream, typename Body>
   asio::awaitable<void>
   write_request(const std::shared_ptr<connection_state>& connection,
                 Stream& stream,
                 beast_http::request<Body>& request_message,
                 std::optional<operation_deadline> deadline,
                 const std::shared_ptr<request_control>& control) {
      arm_operation_deadline(stream, deadline);
      active_cancel_guard cancel_guard(
         control,
         [connection] { connection->cancel(); });
      error_code error;
      (void)co_await beast_http::async_write(
         stream,
         request_message,
         asio::redirect_error(asio::use_awaitable, error));
      throw_if_operation_failed(error, deadline, control);
      if (error) {
         throw transport_failure(
            failure_kind::io,
            "request write failed: " + error.message(),
            is_retryable_connection_error(error));
      }
   }

   /** Read one response head under the configured header deadline. */
   template <typename Stream, typename Parser>
   asio::awaitable<void>
   read_header(const std::shared_ptr<connection_state>& connection,
               Stream& stream,
               beast::flat_buffer& buffer,
               Parser& parser,
               const request_options& policy,
               std::optional<operation_deadline> deadline,
               const std::shared_ptr<request_control>& control) {
      arm_operation_deadline(stream, deadline);
      active_cancel_guard cancel_guard(
         control,
         [connection] { connection->cancel(); });
      error_code error;
      (void)co_await beast_http::async_read_header(
         stream,
         buffer,
         parser,
         asio::redirect_error(asio::use_awaitable, error));
      throw_if_operation_failed(error, deadline, control);
      if (error == beast_http::error::header_limit) {
         throw transport_failure(
            failure_kind::response_limit,
            "response headers exceed configured maximum");
      }
      if (error == beast_http::error::body_limit) {
         throw transport_failure(
            failure_kind::response_limit,
            "response body exceeds configured maximum of " +
               std::to_string(policy.max_response_body_bytes) + " bytes");
      }
      if (error) {
         throw transport_failure(
            failure_kind::io,
            "response header read failed: " + error.message(),
            is_retryable_connection_error(error));
      }
   }

   /** Read one decoded response-body increment into caller storage. */
   template <typename Stream, typename Parser>
   asio::awaitable<size_t>
   read_body(const std::shared_ptr<connection_state>& connection,
             Stream& stream,
             beast::flat_buffer& buffer,
             Parser& parser,
             asio::mutable_buffer output,
             const request_options& policy,
             const std::optional<time_point>& total_deadline,
             const std::optional<operation_deadline>& read_deadline,
             const std::shared_ptr<request_control>& control) {
      parser.get().body().data = output.data();
      parser.get().body().size = output.size();
      auto deadline = phase_deadline(
         policy.timeouts.idle,
         failure_kind::timeout_idle,
         total_deadline);
      if (read_deadline &&
          (!deadline || read_deadline->when < deadline->when))
         deadline = *read_deadline;
      arm_operation_deadline(stream, deadline);
      active_cancel_guard cancel_guard(
         control,
         [connection] { connection->cancel(); });
      error_code error;
      (void)co_await beast_http::async_read_some(
         stream,
         buffer,
         parser,
         asio::redirect_error(asio::use_awaitable, error));
      throw_if_operation_failed(error, deadline, control);
      if (error == beast_http::error::need_buffer)
         error.clear();
      if (error == beast_http::error::body_limit) {
         throw transport_failure(
            failure_kind::response_limit,
            "response body exceeds configured maximum of " +
               std::to_string(policy.max_response_body_bytes) + " bytes");
      }
      if (error) {
         throw transport_failure(
            failure_kind::io,
            "response body read failed: " + error.message());
      }
      co_return output.size() - parser.get().body().size;
   }

   /** Build a bounded Beast request for a normalized target. */
   beast_http::request<beast_http::string_body>
   build_request(const request& req,
                 const target_info& target) const {
      std::string request_target = target.request_target;
      if (proxy_host && target.scheme == scheme_http) {
         request_target =
            "http://" + target.host_header + target.request_target;
      }
      if (std::any_of(
             request_target.begin(),
             request_target.end(),
             [](unsigned char character) {
                return character <= 0x20 || character == 0x7f;
             })) {
         throw transport_failure(
            failure_kind::request_limit,
            "request target contains a forbidden control or space character");
      }
      beast_http::request<beast_http::string_body> result{
         to_beast_verb(req.method),
         request_target,
         http_version_1_1};
      result.set(beast_http::field::host, target.host_header);
      result.set(
         beast_http::field::user_agent,
         req.user_agent.empty() ? BOOST_BEAST_VERSION_STRING : req.user_agent);
      if (!req.content_type.empty() && !req.body.empty())
         result.set(beast_http::field::content_type, req.content_type);
      for (const auto& [name, value] : req.headers)
         result.set(name, value);
      result.keep_alive(true);
      result.body() = req.body;
      result.prepare_payload();
      return result;
   }

   /** Send an HTTP CONNECT request before upgrading a proxy socket to TLS. */
   asio::awaitable<void>
   establish_proxy_tunnel(
      const std::shared_ptr<connection_state>& connection,
      connection_state::tcp_stream& stream,
      const target_info& target,
      const request_options& policy,
      std::optional<operation_deadline> connect_deadline,
      const std::shared_ptr<request_control>& control) {
      const bool ipv6 = target.host.find(':') != std::string::npos;
      const auto connect_authority =
         (ipv6 ? "[" + target.host + "]" : target.host) +
         ":" + target.service;
      beast_http::request<beast_http::empty_body> connect_request{
         beast_http::verb::connect,
         connect_authority,
         http_version_1_1};
      connect_request.set(beast_http::field::host, connect_authority);
      connect_request.set(
         beast_http::field::user_agent,
         BOOST_BEAST_VERSION_STRING);
      co_await write_request(
         connection,
         stream,
         connect_request,
         connect_deadline,
         control);

      beast::flat_buffer buffer(policy.max_response_header_bytes);
      beast_http::response_parser<beast_http::empty_body> parser;
      parser.header_limit(policy.max_response_header_bytes);
      co_await read_header(
         connection,
         stream,
         buffer,
         parser,
         policy,
         connect_deadline,
         control);
      if (parser.get().result() != beast_http::status::ok) {
         throw transport_failure(
            failure_kind::connect,
            "proxy tunnel failed with HTTP status " +
               std::to_string(parser.get().result_int()));
      }
   }

   /** Connect one new plain, authenticated TLS, or Unix-domain connection. */
   asio::awaitable<std::shared_ptr<connection_state>>
   create_connection(
      const target_info& target,
      const request_options& policy,
      const std::optional<time_point>& total_deadline,
      const std::shared_ptr<request_control>& control) {
      const auto connect_deadline = phase_deadline(
         policy.timeouts.connect,
         failure_kind::timeout_connect,
         total_deadline);
      if (target.scheme == scheme_unix) {
         auto connection = std::make_shared<connection_state>(
            connection_state::stream_variant{
               std::make_unique<connection_state::unix_stream>(strand)});
         auto& stream =
            *std::get<std::unique_ptr<connection_state::unix_stream>>(
               connection->stream);
         arm_operation_deadline(stream, connect_deadline);
         active_cancel_guard cancel_guard(
            control,
            [connection] { connection->cancel(); });
         error_code error;
         co_await stream.async_connect(
            local::stream_protocol::endpoint(*target.unix_socket_path),
            asio::redirect_error(asio::use_awaitable, error));
         throw_if_operation_failed(error, connect_deadline, control);
         if (error) {
            throw transport_failure(
               failure_kind::connect,
               "Unix-socket connection failed: " + error.message(),
               true);
         }
         co_return connection;
      }

      const auto connect_host = proxy_host.value_or(target.host);
      const auto connect_service = proxy_service.value_or(target.service);
      const auto endpoints =
         co_await resolve(
            connect_host,
            connect_service,
            connect_deadline,
            control);

      if (!target.tls) {
         auto connection = std::make_shared<connection_state>(
            connection_state::stream_variant{
               std::make_unique<connection_state::tcp_stream>(strand)});
         auto& stream =
            *std::get<std::unique_ptr<connection_state::tcp_stream>>(
               connection->stream);
         co_await connect_tcp(
            connection,
            stream,
            endpoints,
            connect_host,
            connect_service,
            connect_deadline,
            control);
         co_return connection;
      }

      auto connection = std::make_shared<connection_state>(
         connection_state::stream_variant{
            std::make_unique<connection_state::tls_stream>(
               strand,
               tls_context)});
      auto& stream =
         *std::get<std::unique_ptr<connection_state::tls_stream>>(
            connection->stream);
      co_await connect_tcp(
         connection,
         stream.next_layer(),
         endpoints,
         connect_host,
         connect_service,
         connect_deadline,
         control);
      if (proxy_host) {
         co_await establish_proxy_tunnel(
            connection,
            stream.next_layer(),
            target,
            policy,
            connect_deadline,
            control);
      }

      error_code address_error;
      asio::ip::make_address(target.host, address_error);
      const bool ip_literal = !address_error;
      if (!ip_literal) {
         ERR_clear_error();
         if (SSL_set_tlsext_host_name(
                stream.native_handle(),
                target.host.c_str()) != 1) {
            ERR_clear_error();
            throw transport_failure(
               failure_kind::tls_handshake,
               "failed to configure TLS server name");
         }
      }
      stream.set_verify_mode(asio::ssl::verify_peer);
      auto identity_failure = std::make_shared<bool>(false);
      auto chain_failure =
         std::make_shared<bool>(false);
      auto verification_observed =
         std::make_shared<bool>(false);
      stream.set_verify_callback(
         [verify_identity =
             asio::ssl::host_name_verification(target.host),
          identity_failure,
          chain_failure,
          verification_observed](
             bool preverified,
             asio::ssl::verify_context& context) mutable {
            *verification_observed = true;
            if (!preverified) {
               *chain_failure = true;
               return false;
            }
            const bool verified = verify_identity(preverified, context);
            if (!verified &&
                X509_STORE_CTX_get_error_depth(
                   context.native_handle()) == 0) {
               *identity_failure = true;
            }
            return verified;
         });

      arm_operation_deadline(stream, connect_deadline);
      active_cancel_guard cancel_guard(
         control,
         [connection] { connection->cancel(); });
      error_code error;
      co_await stream.async_handshake(
         asio::ssl::stream_base::client,
         asio::redirect_error(asio::use_awaitable, error));
      throw_if_operation_failed(error, connect_deadline, control);
      if (error) {
         const auto failure =
            *identity_failure
               ? (ip_literal
                     ? failure_kind::tls_ip
                     : failure_kind::tls_hostname)
               : (*verification_observed &&
                  (*chain_failure ||
                   SSL_get_verify_result(
                      stream.native_handle()) != X509_V_OK))
                    ? failure_kind::tls_verification
                    : failure_kind::tls_handshake;
         throw transport_failure(
            failure,
            "TLS handshake or peer verification failed");
      }
      co_return connection;
   }

   /** Close idle connections that exceeded the configured reuse age. */
   void prune_expired_idle_connections() {
      const auto now = idle_clock::now();
      const auto max_idle_age =
         std::chrono::microseconds{
            options.max_idle_connection_age.count()};
      for (auto entry = idle_connections.begin();
           entry != idle_connections.end();) {
         auto& connections = entry->second;
         for (auto connection = connections.begin();
              connection != connections.end();) {
            if (!connection->connection->open() ||
                now - connection->idle_since >
                   max_idle_age) {
               connection->connection->close();
               connection = connections.erase(connection);
               FC_ASSERT(
                  idle_connection_count > 0,
                  "Outbound HTTP idle connection count underflow");
               --idle_connection_count;
            } else {
               ++connection;
            }
         }
         if (connections.empty())
            entry = idle_connections.erase(entry);
         else
            ++entry;
      }
   }

   /** Lease a recent locally-open idle connection, or create a fresh connection. */
   asio::awaitable<std::pair<std::shared_ptr<connection_state>, bool>>
   acquire_connection(
      const target_info& target,
      const request_options& policy,
      const std::optional<time_point>& total_deadline,
      const std::shared_ptr<request_control>& control,
      bool force_fresh) {
      if (!force_fresh) {
         prune_expired_idle_connections();
         auto found = idle_connections.find(target.connection_key);
         while (found != idle_connections.end() &&
                !found->second.empty()) {
            auto connection =
               std::move(found->second.back().connection);
            found->second.pop_back();
            FC_ASSERT(
               idle_connection_count > 0,
               "Outbound HTTP idle connection count underflow");
            --idle_connection_count;
            if (found->second.empty())
               idle_connections.erase(found);
            if (connection->healthy_for_reuse())
               co_return std::pair{std::move(connection), true};
            connection->close();
            found = idle_connections.find(target.connection_key);
         }
      }
      co_return std::pair{
         co_await create_connection(
            target,
            policy,
            total_deadline,
            control),
         false};
   }

   /** Return a fully consumed healthy connection to the idle cache. */
   void release_connection(
      const std::string& key,
      std::shared_ptr<connection_state> connection) {
      prune_expired_idle_connections();
      if (!connection->open()) {
         connection->close();
         return;
      }
      if (idle_connection_count >=
          options.max_idle_connections) {
         connection->close();
         return;
      }
      idle_connections[key].push_back(
         idle_connection{
            .connection = std::move(connection),
            .idle_since = idle_clock::now(),
         });
      ++idle_connection_count;
   }

   /** Sleep through one bounded, cancellable exponential retry backoff. */
   asio::awaitable<void>
   wait_before_retry(
      const request_options& policy,
      const std::optional<time_point>& total_deadline,
      uint32_t completed_attempts,
      const std::shared_ptr<request_control>& control) {
      auto delay = policy.retry.initial_backoff;
      for (uint32_t index = 1; index < completed_attempts; ++index)
         delay = std::min(delay * 2, policy.retry.max_backoff);
      if (total_deadline) {
         const auto remaining = *total_deadline - time_point::now();
         if (remaining.count() <= 0) {
            throw transport_failure(
               failure_kind::timeout_total,
               "total request deadline expired during retry backoff");
         }
         delay = std::min(delay, remaining);
      }
      auto timer = std::make_shared<asio::steady_timer>(strand);
      timer->expires_after(std::chrono::microseconds(delay.count()));
      active_cancel_guard cancel_guard(
         control,
         [timer] { timer->cancel(); });
      error_code error;
      co_await timer->async_wait(
         asio::redirect_error(asio::use_awaitable, error));
      control->throw_if_cancelled("retry backoff");
      if (error)
         throw transport_failure(failure_kind::io, "retry timer failed");
      if (total_deadline && time_point::now() >= *total_deadline) {
         throw transport_failure(
            failure_kind::timeout_total,
            "total request deadline expired during retry backoff");
      }
   }

   asio::awaitable<std::shared_ptr<response_reader_impl>>
   async_open(
      request req,
      request_options policy,
      std::shared_ptr<request_control> control,
      std::function<void(http_file_download_phase)> on_phase = {},
      connection_affinity affinity = {},
      bool retain_connection = false);

   asio::awaitable<void>
   async_warm_up(
      url target,
      request_options policy,
      std::shared_ptr<request_control> control) {
      validate_policy(policy);
      const auto total_deadline = effective_total_deadline(policy);
      const auto connect_deadline = phase_deadline(
         policy.timeouts.connect,
         failure_kind::timeout_connect,
         total_deadline);
      const auto normalized = normalize_target(target);
      if (normalized.scheme == scheme_unix)
         co_return;
      (void)co_await resolve(
         proxy_host.value_or(normalized.host),
         proxy_service.value_or(normalized.service),
         connect_deadline,
         control);
   }
};

/** Parser, connection lease, and policy for one opened response. */
class response_reader_impl
   : public std::enable_shared_from_this<response_reader_impl> {
public:
   response_reader_impl(
      std::shared_ptr<client_impl> client_in,
      std::shared_ptr<connection_state> connection_in,
      std::string connection_key_in,
      request_options policy_in,
      std::optional<time_point> total_deadline_in,
      std::shared_ptr<request_control> control_in,
      std::shared_ptr<request_metrics_state> metrics_in,
      bool retain_connection_in)
      : client(std::move(client_in))
      , connection(std::move(connection_in))
      , connection_key(std::move(connection_key_in))
      , buffer(static_cast<size_t>(
           std::min<uint64_t>(
              policy_in.max_response_header_bytes,
              std::numeric_limits<size_t>::max())))
      , policy(std::move(policy_in))
      , total_deadline(total_deadline_in)
      , control(std::move(control_in))
      , metrics(std::move(metrics_in))
      , retain_connection(retain_connection_in) {
      parser.header_limit(policy.max_response_header_bytes);
      parser.body_limit(policy.max_response_body_bytes);
   }

   ~response_reader_impl() {
      if (complete.load(std::memory_order_acquire))
         return;
      connection->close();
      // Before async_read_header succeeds, async_open owns failure
      // categorization and may retry with another connection. Do not let a
      // temporary reader suppress that final outcome.
      if (!opened.load(std::memory_order_acquire))
         return;
      metrics->finish_failure(
         is_success_status(value_head.status)
            ? failure_kind::io
            : failure_kind::http_status);
   }

   /** Initialize public metadata and the aggregate body-read deadline. */
   void header_complete() {
      value_head.status = parser.get().result_int();
      value_head.reason = sanitize_reason(parser.get().reason());
      if (const auto length = parser.content_length())
         value_head.content_length = *length;
      if (policy.timeouts.read) {
         read_deadline = phase_deadline(
            policy.timeouts.read,
            failure_kind::timeout_read,
            total_deadline);
      }
      opened.store(true, std::memory_order_release);
      finish_if_complete();
   }

   /** Read one body increment on the client's strand. */
   asio::awaitable<size_t> read_some(asio::mutable_buffer output) {
      if (complete.load(std::memory_order_acquire))
         co_return 0;
      if (abandoned.load(std::memory_order_acquire)) {
         throw transport_failure(
            failure_kind::cancelled,
            "response reader was abandoned");
      }
      if (output.size() == 0)
         co_return 0;
      control->throw_if_cancelled("response body read");
      if (reading) {
         throw transport_failure(
            failure_kind::io,
            "concurrent response-body reads are not supported");
      }
      reading = true;

      try {
         const auto bytes = co_await std::visit(
            [&](auto& stream) {
               return client->read_body(
                  connection,
                  *stream,
                  buffer,
                  parser,
                  output,
                  policy,
                  total_deadline,
                  read_deadline,
                  control);
            },
            connection->stream);
         metrics->add_response_bytes(bytes);
         finish_if_complete();
         reading = false;
         co_return bytes;
      } catch (const transport_failure& failure) {
         reading = false;
         connection->close();
         complete.store(true, std::memory_order_release);
         metrics->finish_failure(failure.kind);
         throw;
      } catch (...) {
         reading = false;
         connection->close();
         complete.store(true, std::memory_order_release);
         metrics->finish_failure(failure_kind::io);
         throw;
      }
   }

   /** Close a partially consumed response without touching unrelated leases. */
   void abandon() {
      if (complete.load(std::memory_order_acquire) ||
          abandoned.exchange(true, std::memory_order_acq_rel)) {
         return;
      }
      auto self = shared_from_this();
      asio::post(
         client->strand,
         [self] {
            if (self->complete.exchange(true, std::memory_order_acq_rel))
               return;
            self->connection->close();
            self->metrics->finish_failure(
               is_success_status(
                  self->value_head.status)
                  ? failure_kind::io
                  : failure_kind::http_status);
         });
   }

   /** Return the leased connection only after Beast confirms end-of-message. */
   void finish_if_complete() {
      if (!parser.is_done() ||
          complete.exchange(true, std::memory_order_acq_rel)) {
         return;
      }
      if (!parser.get().keep_alive())
         connection->close();
      else if (!retain_connection)
         client->release_connection(connection_key, connection);
      metrics->finish_status(value_head.status);
   }

   /** Return the completed response's retained connection lease. */
   connection_affinity retained_affinity() const {
      FC_ASSERT(
         retain_connection &&
            complete.load(std::memory_order_acquire),
         "Outbound HTTP response has no completed retained connection");
      return connection_affinity{
         .connection = connection,
         .connection_key = connection_key,
      };
   }

   /** Close a retained connection after a continuation hook rejects it. */
   void close_retained_connection() {
      FC_ASSERT(
         retain_connection,
         "Outbound HTTP response connection was not retained");
      connection->close();
   }

   std::shared_ptr<client_impl> client;
   std::shared_ptr<connection_state> connection;
   std::string connection_key;
   beast::flat_buffer buffer;
   beast_http::response_parser<beast_http::buffer_body> parser;
   request_options policy;
   std::optional<time_point> total_deadline;
   std::optional<operation_deadline> read_deadline;
   std::shared_ptr<request_control> control;
   std::shared_ptr<request_metrics_state> metrics;
   response_head value_head;
   std::atomic_bool opened{false};
   std::atomic_bool complete{false};
   std::atomic_bool abandoned{false};
   bool retain_connection = false;
   bool reading = false;
};

asio::awaitable<std::shared_ptr<response_reader_impl>>
client_impl::async_open(
   request req,
   request_options policy,
   std::shared_ptr<request_control> control,
   std::function<void(http_file_download_phase)> on_phase,
   connection_affinity affinity,
   bool retain_connection) {
   validate_policy(policy);
   auto metrics = std::make_shared<request_metrics_state>();
   shared_metrics().requests.fetch_add(1, std::memory_order_relaxed);
   try {
      validate_request(req, policy);
   } catch (const transport_failure& failure) {
      metrics->finish_failure(failure.kind);
      throw;
   } catch (...) {
      metrics->finish_failure(failure_kind::request_limit);
      throw;
   }
   const auto total_deadline = effective_total_deadline(policy);
   target_info target;
   try {
      target = normalize_target(req.target);
   } catch (const transport_failure& failure) {
      metrics->finish_failure(failure.kind);
      throw;
   } catch (...) {
      metrics->finish_failure(failure_kind::request_limit);
      throw;
   }
   transport_failure last_failure(
      failure_kind::io,
      "request did not start");
   const auto max_attempts =
      affinity ? 1U : policy.retry.max_attempts;
   for (uint32_t attempt = 1;
        attempt <= max_attempts;
        ++attempt) {
      std::shared_ptr<connection_state> connection;
      bool reused = false;
      bool retry = false;
      try {
         if (on_phase)
            on_phase(http_file_download_phase::connecting);
         if (affinity) {
            connection = affinity.connection;
            reused = true;
            if (target.connection_key != affinity.connection_key) {
               throw transport_failure(
                  failure_kind::request_limit,
                  "connection-affine follow-up target does not match the retained endpoint");
            }
            if (!connection->open()) {
               throw transport_failure(
                  failure_kind::connect,
                  "connection-affine follow-up connection is no longer open");
            }
         } else {
            auto acquired = co_await acquire_connection(
               target,
               policy,
               total_deadline,
               control,
               attempt > 1);
            connection = std::move(acquired.first);
            reused = acquired.second;
         }

         auto request_message = build_request(req, target);
         if (on_phase)
            on_phase(http_file_download_phase::sending_request);
         control->throw_if_cancelled(
            affinity
               ? "connection-affine follow-up"
               : "request send");
         co_await std::visit(
            [&](auto& stream) {
               const auto upload_deadline =
                  total_deadline
                     ? std::optional<operation_deadline>{
                          operation_deadline{
                             .when = *total_deadline,
                             .timeout_kind =
                                failure_kind::timeout_total,
                          }}
                     : std::nullopt;
               return write_request(
                  connection,
                  *stream,
                  request_message,
                  upload_deadline,
                  control);
            },
            connection->stream);
         shared_metrics().request_bytes.fetch_add(
            req.body.size(),
            std::memory_order_relaxed);

         auto reader = std::make_shared<response_reader_impl>(
            shared_from_this(),
            connection,
            target.connection_key,
            policy,
            total_deadline,
            control,
            metrics,
            retain_connection);
         if (on_phase)
            on_phase(http_file_download_phase::waiting_for_response);
         co_await std::visit(
            [&](auto& stream) {
               return read_header(
                  connection,
                  *stream,
                  reader->buffer,
                  reader->parser,
                  policy,
                  phase_deadline(
                     policy.timeouts.header,
                     failure_kind::timeout_header,
                     total_deadline),
                  control);
            },
            connection->stream);
         reader->header_complete();
         co_return reader;
      } catch (transport_failure& failure) {
         if (connection)
            connection->close();
         if (failure.retryable && policy.retry.allow_retry) {
            try {
               failure.retryable =
                  policy.retry.allow_retry(
                     retry_context{
                        .failure = failure.kind,
                        .attempt = attempt,
                        .reused_connection = reused,
                     });
            } catch (...) {
               metrics->finish_failure(failure_kind::io);
               throw transport_failure(
                  failure_kind::io,
                  "retry decision hook threw");
            }
         }
         last_failure = failure;
         if (attempt == max_attempts ||
             !failure.retryable) {
            const auto final_failure =
               max_attempts > 1 && failure.retryable
                  ? transport_failure(
                       failure_kind::retry_exhausted,
                       "retry attempts exhausted after " +
                          std::to_string(attempt) + " attempts: " +
                          failure.what())
                  : failure;
            metrics->finish_failure(final_failure.kind);
            throw final_failure;
         }
         retry = true;
      } catch (...) {
         if (connection)
            connection->close();
         metrics->finish_failure(failure_kind::io);
         throw;
      }
      if (retry) {
         try {
            co_await wait_before_retry(
               policy,
               total_deadline,
               attempt,
               control);
         } catch (const transport_failure& backoff_failure) {
            metrics->finish_failure(backoff_failure.kind);
            throw;
         }
      }
   }

   metrics->finish_failure(last_failure.kind);
   throw last_failure;
}

response_reader::response_reader() = default;

response_reader::response_reader(
   std::shared_ptr<response_reader_impl> impl)
   : _impl(std::move(impl)) {}

response_reader::~response_reader() {
   if (_impl)
      _impl->abandon();
}

response_reader::response_reader(response_reader&& other) noexcept
   : _impl(std::move(other._impl)) {}

response_reader&
response_reader::operator=(response_reader&& other) noexcept {
   if (this == &other)
      return *this;
   if (_impl)
      _impl->abandon();
   _impl = std::move(other._impl);
   return *this;
}

const response_head& response_reader::head() const {
   FC_ASSERT(_impl, "Outbound HTTP response reader is empty");
   return _impl->value_head;
}

/** Translate an implementation read while retaining its state for the whole operation. */
asio::awaitable<size_t>
async_read_some_public(
   std::shared_ptr<response_reader_impl> impl,
   asio::mutable_buffer output) {
   try {
      co_return co_await impl->read_some(output);
   } catch (const transport_failure& failure) {
      throw_public_failure(failure);
   }
}

asio::awaitable<size_t>
response_reader::async_read_some(asio::mutable_buffer output) {
   FC_ASSERT(_impl, "Outbound HTTP response reader is empty");
   auto impl = _impl;
   auto executor = impl->client->strand;
   return asio::co_spawn(
      std::move(executor),
      async_read_some_public(std::move(impl), output),
      asio::use_awaitable);
}

bool response_reader::done() const noexcept {
   return !_impl ||
          _impl->complete.load(std::memory_order_acquire);
}

/** Buffer one public response reader under its existing bounded policy. */
asio::awaitable<response>
async_buffer_response(response_reader& reader) {
   response result{
      .status = reader.head().status,
      .reason = reader.head().reason,
   };
   std::array<char, body_read_buffer_bytes> body_buffer{};
   while (!reader.done()) {
      const auto bytes = co_await reader.async_read_some(
         asio::buffer(body_buffer));
      result.body.append(body_buffer.data(), bytes);
   }
   co_return result;
}

/** Invoke one continuation hook on the client executor. */
asio::awaitable<continuation_request>
async_select_continuation(
   continuation_hook continue_with,
   response first_response) {
   co_return continue_with(first_response);
}

/** Close one retained connection on its owning strand. */
asio::awaitable<void>
async_close_retained_response(
   std::shared_ptr<response_reader_impl> reader) {
   reader->close_retained_connection();
   co_return;
}

client::client(asio::any_io_executor executor,
               transport_options options)
   : client(std::move(executor), std::move(options), {}) {}

client::client(
   asio::any_io_executor executor,
   transport_options options,
   detail::resolver_start_fn resolver_start)
   : _impl(
        std::make_shared<client_impl>(
           std::move(executor),
           std::move(options),
           std::move(resolver_start))) {}

client::~client() = default;
client::client(client&&) noexcept = default;
client& client::operator=(client&&) noexcept = default;

asio::awaitable<response_reader>
client::async_open(request req,
                   request_options options,
                   asio::cancellation_slot cancellation) {
   FC_ASSERT(_impl, "Outbound HTTP client is empty");
   auto control = request_control::create(
      cancellation,
      _impl->strand);
   try {
      auto impl = co_await asio::co_spawn(
         _impl->strand,
         _impl->async_open(
            std::move(req),
            std::move(options),
            std::move(control)),
         asio::use_awaitable);
      co_return response_reader(std::move(impl));
   } catch (const transport_failure& failure) {
      throw_public_failure(failure);
   }
}

asio::awaitable<response>
client::async_request(request req,
                      request_options options,
                      asio::cancellation_slot cancellation) {
   auto reader = co_await async_open(
      std::move(req),
      std::move(options),
      cancellation);
   co_return co_await async_buffer_response(reader);
}

asio::awaitable<response>
client::async_request_then(
   request req,
   request_options options,
   continuation_hook continue_with,
   asio::cancellation_slot cancellation) {
   FC_ASSERT(_impl, "Outbound HTTP client is empty");
   FC_ASSERT(
      static_cast<bool>(continue_with),
      "Outbound HTTP continuation hook must be configured");
   auto control = request_control::create(
      cancellation,
      _impl->strand);

   std::shared_ptr<response_reader_impl> first_impl;
   try {
      first_impl = co_await asio::co_spawn(
         _impl->strand,
         _impl->async_open(
            std::move(req),
            std::move(options),
            control,
            {},
            {},
            true),
         asio::use_awaitable);
   } catch (const transport_failure& failure) {
      throw_public_failure(failure);
   }

   response_reader first_reader(first_impl);
   auto first_response =
      co_await async_buffer_response(first_reader);
   const auto affinity = first_impl->retained_affinity();

   std::optional<continuation_request> continuation;
   std::exception_ptr continuation_failure;
   try {
      continuation.emplace(
         co_await asio::co_spawn(
            _impl->strand,
            async_select_continuation(
               std::move(continue_with),
               std::move(first_response)),
            asio::use_awaitable));
      FC_ASSERT(
         continuation->options.retry.max_attempts == 1,
         "Outbound HTTP connection-affine follow-up cannot retry");
   } catch (...) {
      continuation_failure = std::current_exception();
   }
   if (continuation_failure) {
      co_await asio::co_spawn(
         _impl->strand,
         async_close_retained_response(first_impl),
         asio::use_awaitable);
      std::rethrow_exception(continuation_failure);
   }

   std::shared_ptr<response_reader_impl> next_impl;
   std::exception_ptr next_failure;
   try {
      next_impl = co_await asio::co_spawn(
         _impl->strand,
         _impl->async_open(
            std::move(continuation->next_request),
            std::move(continuation->options),
            control,
            {},
            affinity),
         asio::use_awaitable);
   } catch (...) {
      next_failure = std::current_exception();
   }
   if (next_failure) {
      co_await asio::co_spawn(
         _impl->strand,
         async_close_retained_response(first_impl),
         asio::use_awaitable);
      try {
         std::rethrow_exception(next_failure);
      } catch (const transport_failure& failure) {
         throw_public_failure(failure);
      }
      std::rethrow_exception(next_failure);
   }

   response_reader next_reader(std::move(next_impl));
   co_return co_await async_buffer_response(next_reader);
}

asio::awaitable<void>
client::async_warm_up(const url& target,
                      request_options options,
                      asio::cancellation_slot cancellation) {
   FC_ASSERT(_impl, "Outbound HTTP client is empty");
   auto control = request_control::create(
      cancellation,
      _impl->strand);
   try {
      co_await asio::co_spawn(
         _impl->strand,
         _impl->async_warm_up(
            target,
            std::move(options),
            std::move(control)),
         asio::use_awaitable);
   } catch (const transport_failure& failure) {
      record_failure(failure.kind);
      throw_public_failure(failure);
   }
}

asio::any_io_executor client::get_executor() const noexcept {
   return _impl ? asio::any_io_executor(_impl->strand)
                : asio::any_io_executor{};
}

asio::awaitable<void>
async_download_atomic(
   client& source,
   request req,
   request_options policy,
   std::filesystem::path output,
   download_options options,
   asio::cancellation_slot cancellation) {
   FC_ASSERT(source._impl, "Outbound HTTP client is empty");
   auto sink = std::make_shared<file_sink>(
      output,
      policy.max_response_body_bytes,
      std::move(options.status_callback),
      std::move(options.space_available_provider));
   auto progress_reporter =
      std::make_shared<download_progress_reporter>(
         source._impl->strand,
         sink);
   download_progress_scope progress_scope(progress_reporter);
   auto control = request_control::create(
      cancellation,
      source._impl->strand);
   std::shared_ptr<response_reader_impl> opened;
   try {
      opened = co_await asio::co_spawn(
         source._impl->strand,
         source._impl->async_open(
            std::move(req),
            std::move(policy),
            std::move(control),
            [&](http_file_download_phase phase) {
               switch (phase) {
                  case http_file_download_phase::connecting:
                     sink->connecting();
                     break;
                  case http_file_download_phase::sending_request:
                     sink->sending_request();
                     break;
                  case http_file_download_phase::waiting_for_response:
                     sink->waiting_for_response();
                     break;
                  case http_file_download_phase::downloading:
                  case http_file_download_phase::complete:
                     FC_ASSERT(
                        false,
                        "Transport emitted an invalid pre-response download phase");
               }
            }),
         asio::use_awaitable);
   } catch (const transport_failure& failure) {
      throw_public_failure(failure);
   }

   response_reader reader(std::move(opened));
   try {
      sink->headers(
         reader.head().status,
         reader.head().content_length);
      std::array<char, body_read_buffer_bytes> body_buffer{};
      while (!reader.done()) {
         const auto bytes = co_await reader.async_read_some(
            asio::buffer(body_buffer));
         if (bytes != 0 &&
             !sink->body(body_buffer.data(), bytes)) {
            break;
         }
      }
      sink->finish();
   } catch (const transport_failure& failure) {
      throw_public_failure(failure);
   }
}

std::string_view failure_kind_name(failure_kind failure) {
   return magic_enum::enum_name(failure);
}

std::string sanitized_endpoint(const url& target) {
   if (target.proto() == scheme_unix)
      return "unix://local-socket";

   std::string result = target.proto() + "://";
   if (!target.host() || target.host()->empty())
      return result + "<missing-host>";
   const bool ipv6 =
      target.host()->find(':') != std::string::npos;
   result +=
      ipv6 ? "[" + *target.host() + "]" : *target.host();
   if (target.port())
      result += ":" + std::to_string(*target.port());
   return result;
}

metrics_snapshot get_metrics_snapshot() {
   auto& source = shared_metrics();
   metrics_snapshot result{
      .requests = source.requests.load(std::memory_order_relaxed),
      .successes = source.successes.load(std::memory_order_relaxed),
      .request_bytes = source.request_bytes.load(std::memory_order_relaxed),
      .response_bytes = source.response_bytes.load(std::memory_order_relaxed),
   };
   for (const auto failure : magic_enum::enum_values<failure_kind>()) {
      const auto index = magic_enum::enum_index(failure);
      FC_ASSERT(index, "Unknown outbound HTTP failure kind");
      result.failures[*index] =
         source.failures[*index].load(std::memory_order_relaxed);
   }
   return result;
}

} // namespace http
} // namespace fc

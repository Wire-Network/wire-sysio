#include "http_transport_internal.hpp"

#include <boost/asio/co_spawn.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>

namespace fc {
namespace http {

namespace {

/** Return the integer value of a named HTTP status. */
constexpr uint32_t status_value(beast_http::status status) {
   return static_cast<uint32_t>(status);
}

/** Disk-checked atomic file sink used by transport::perform_to_file. */
class file_sink {
public:
   file_sink(const std::filesystem::path& output, uint64_t max_response_body_bytes,
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
   void headers(uint32_t status, std::optional<uint64_t> content_length) {
      std::scoped_lock lock(_mutex);
      _status = status;
      _content_length = content_length;
      if (_content_length && *_content_length > _max_response_body_bytes) {
         throw transport_failure(failure_kind::response_limit, "response body exceeds configured maximum of " +
                                                                  std::to_string(_max_response_body_bytes) + " bytes");
      }
      if (_status != status_value(beast_http::status::ok))
         return;

      if (_content_length) {
         FC_ASSERT(*_content_length <= std::numeric_limits<uint64_t>::max() - disk_space_concurrency_margin_bytes,
                   "HTTP response Content-Length {} is too large for the {}-byte disk safety margin", *_content_length,
                   disk_space_concurrency_margin_bytes);
         require_disk_space(*_content_length == 0 ? 0 : *_content_length + disk_space_concurrency_margin_bytes);
         _disk_space_write_budget = std::min(*_content_length, disk_space_check_interval_bytes);
      } else {
         _disk_space_write_budget = refresh_disk_space_write_budget(_max_response_body_bytes);
      }
      _next_disk_space_check = std::chrono::steady_clock::now() + disk_space_check_interval;

      _file.open(_temp, std::ios::binary | std::ios::trunc);
      FC_ASSERT(_file.is_open(), "Failed to open temp file {} for writing", _temp.string());
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

      if (_disk_space_write_budget < bytes || std::chrono::steady_clock::now() >= _next_disk_space_check) {
         const auto response_limit = _content_length.value_or(_max_response_body_bytes);
         const auto remaining = response_limit - _downloaded_bytes;
         _disk_space_write_budget = refresh_disk_space_write_budget(remaining);
         _next_disk_space_check = std::chrono::steady_clock::now() + disk_space_check_interval;
      }

      _file.write(data, static_cast<std::streamsize>(bytes));
      FC_ASSERT(_file.good(), "Failed to write {} bytes to temp file {}", bytes, _temp.string());
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
         auto message = "HTTP POST failed with status " + std::to_string(_status);
         if (!_error_body.empty())
            message += ": " + _error_body;
         throw transport_failure(failure_kind::http_status, std::move(message));
      }

      _file.close();
      FC_ASSERT(_file.good(), "Failed to close temp file {} after HTTP download", _temp.string());
      std::error_code error;
      std::filesystem::rename(_temp, _output, error);
      FC_ASSERT(!error, "Failed to rename downloaded file: {}", error.message());
      _complete = true;
      transition(http_file_download_phase::complete);
   }

private:
   /** Return a path on the destination filesystem suitable for a space query. */
   std::filesystem::path space_query_path() const {
      if (_output.has_parent_path())
         return _output.parent_path();
      std::error_code error;
      auto current = std::filesystem::current_path(error);
      FC_ASSERT(!error, "Failed to resolve current directory for download disk-space check: {}", error.message());
      return current;
   }

   /** Return currently available destination-filesystem bytes. */
   uint64_t available_disk_space() const {
      const auto query_path = space_query_path();
      if (_space_available_provider)
         return _space_available_provider(query_path);
      std::error_code error;
      const auto space = std::filesystem::space(query_path, error);
      FC_ASSERT(!error, "Failed to query free disk space at {}: {}", query_path.string(), error.message());
      return space.available;
   }

   /** Require @p required bytes on the destination filesystem. */
   void require_disk_space(uint64_t required) const {
      const auto available = available_disk_space();
      FC_ASSERT(required <= available,
                "Insufficient disk space at {} for HTTP download: {} bytes available and {} bytes required",
                space_query_path().string(), available, required);
   }

   /** Verify and return the next amortized disk-space write budget. */
   uint64_t refresh_disk_space_write_budget(uint64_t remaining) const {
      const auto budget = std::min(remaining, disk_space_check_interval_bytes);
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
      const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - _started);
      try {
         _status_callback(http_file_download_status{
            .phase = _phase,
            .downloaded_bytes = _downloaded_bytes,
            .total_bytes = _content_length,
            .elapsed = microseconds(elapsed.count()),
         });
      } catch (...) {}
      _next_status = now + download_status_interval;
   }

   std::filesystem::path _output;
   std::filesystem::path _temp;
   uint64_t _max_response_body_bytes;
   std::function<void(const http_file_download_status&)> _status_callback;
   std::function<uint64_t(const std::filesystem::path&)> _space_available_provider;
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
class download_progress_reporter : public std::enable_shared_from_this<download_progress_reporter> {
public:
   download_progress_reporter(asio::any_io_executor executor, std::shared_ptr<file_sink> sink)
      : _timer(std::move(executor))
      , _sink(std::move(sink)) {}

   /** Start rate-limited progress observation. */
   void start() {
      asio::post(_timer.get_executor(), [self = shared_from_this()] {
         if (!self->_stopped)
            self->arm();
      });
   }

   /** Stop observation without retaining the download sink. */
   void stop() {
      asio::post(_timer.get_executor(), [self = shared_from_this()] {
         self->_stopped = true;
         self->_timer.cancel();
         self->_sink.reset();
      });
   }

private:
   /** Schedule the next observation on the client executor. */
   void arm() {
      _timer.expires_after(download_status_interval);
      _timer.async_wait([self = shared_from_this()](const boost::system::error_code& error) {
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
   explicit download_progress_scope(std::shared_ptr<download_progress_reporter> reporter)
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

asio::awaitable<void> async_download_atomic(client& source, request req, request_options policy,
                                            std::filesystem::path output, download_options options,
                                            asio::cancellation_slot cancellation) {
   FC_ASSERT(source._impl, "Outbound HTTP client is empty");
   auto sink = std::make_shared<file_sink>(output, policy.max_response_body_bytes, std::move(options.status_callback),
                                           std::move(options.space_available_provider));
   auto progress_reporter = std::make_shared<download_progress_reporter>(source._impl->strand, sink);
   download_progress_scope progress_scope(progress_reporter);
   auto control = request_control::create(cancellation, source._impl->strand);
   std::shared_ptr<response_reader_impl> opened;
   try {
      opened = co_await asio::co_spawn(
         source._impl->strand,
         source._impl->async_open(std::move(req), std::move(policy), std::move(control),
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
                                        FC_ASSERT(false, "Transport emitted an invalid pre-response download phase");
                                     }
                                  }),
         asio::use_awaitable);
   } catch (const transport_failure& failure) {
      throw_public_failure(failure);
   }

   response_reader reader(std::move(opened));
   try {
      sink->headers(reader.head().status, reader.head().content_length);
      std::array<char, body_read_buffer_bytes> body_buffer{};
      while (!reader.done()) {
         const auto bytes = co_await reader.async_read_some(asio::buffer(body_buffer));
         if (bytes != 0 && !sink->body(body_buffer.data(), bytes)) {
            break;
         }
      }
      sink->finish();
   } catch (const transport_failure& failure) {
      throw_public_failure(failure);
   }
}

} // namespace http
} // namespace fc

#include <fc/network/http/http_client.hpp>

#include <fc/io/json.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <chrono>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace fc {
namespace http {

namespace asio = boost::asio;

namespace {

constexpr auto cancellation_poll_interval =
   std::chrono::milliseconds(50);

} // namespace

/**
 * Synchronous event-loop ownership retained only for legacy call sites.
 *
 * All resolver, connection, parser, retry, and download behavior lives in the asynchronous
 * client. This adapter owns only blocking execution and predicate-to-slot cancellation.
 */
class transport_impl {
public:
   explicit transport_impl(
      transport_options options,
      detail::resolver_start_fn resolver_start = {})
      : async_client(
           io.get_executor(),
           std::move(options),
           std::move(resolver_start))
      , poll_timer(io) {}

   /** Buffer one asynchronous request while polling a legacy cancellation predicate. */
   response perform(const request& req,
                    const request_options& options) {
      std::scoped_lock lock(use_mutex);
      auto async_options = options;
      async_options.cancel_check = {};
      return run<response>(
         [&](asio::cancellation_slot slot) {
            return async_client.async_request(
               req,
               std::move(async_options),
               slot);
         },
         options.cancel_check);
   }

   /** Resolve one endpoint through the same asynchronous core. */
   void prime_endpoint(const url& target,
                       const request_options& options) {
      std::scoped_lock lock(use_mutex);
      auto async_options = options;
      async_options.cancel_check = {};
      run_void(
         [&](asio::cancellation_slot slot) {
            return async_client.async_warm_up(
               target,
               std::move(async_options),
               slot);
         },
         options.cancel_check);
   }

   /** Stream one response through the pull reader and atomic-file helper. */
   void perform_to_file(
      const request& req,
      const request_options& options,
      const std::filesystem::path& output,
      const std::function<void(const http_file_download_status&)>&
         status_callback,
      const std::function<uint64_t(const std::filesystem::path&)>&
         space_available_provider) {
      std::scoped_lock lock(use_mutex);
      auto async_options = options;
      async_options.cancel_check = {};
      run_void(
         [&](asio::cancellation_slot slot) {
            return async_download_atomic(
               async_client,
               req,
               std::move(async_options),
               output,
               download_options{
                  .status_callback = status_callback,
                  .space_available_provider =
                     space_available_provider,
               },
               slot);
         },
         options.cancel_check);
   }

private:
   /** Arm the only legacy-specific behavior: predicate-to-slot cancellation. */
   void arm_cancel_poll(
      const std::function<bool()>& cancel_check,
      asio::cancellation_signal& cancellation,
      bool& complete) {
      if (!cancel_check)
         return;
      poll_timer.expires_after(cancellation_poll_interval);
      poll_timer.async_wait(
         [this, &cancel_check, &cancellation, &complete](
            const boost::system::error_code& error) {
            if (error || complete)
               return;
            bool cancel = false;
            try {
               cancel = cancel_check();
            } catch (...) {
               cancel = true;
            }
            if (cancel) {
               cancellation.emit(asio::cancellation_type::terminal);
               return;
            }
            arm_cancel_poll(
               cancel_check,
               cancellation,
               complete);
         });
   }

   /** Run one result-bearing awaitable to completion on the private event loop. */
   template <typename Result, typename Factory>
   Result run(
      Factory&& factory,
      const std::function<bool()>& cancel_check) {
      asio::cancellation_signal cancellation;
      std::optional<Result> result;
      std::exception_ptr failure;
      bool complete = false;
      asio::co_spawn(
         io,
         factory(cancellation.slot()),
         [&](std::exception_ptr operation_failure,
             Result operation_result) {
            failure = std::move(operation_failure);
            if (!failure)
               result.emplace(std::move(operation_result));
            complete = true;
            poll_timer.cancel();
         });
      arm_cancel_poll(cancel_check, cancellation, complete);
      io.restart();
      io.run();
      if (failure)
         std::rethrow_exception(failure);
      FC_ASSERT(result, "Outbound HTTP coroutine did not complete");
      return std::move(*result);
   }

   /** Run one void awaitable to completion on the private event loop. */
   template <typename Factory>
   void run_void(
      Factory&& factory,
      const std::function<bool()>& cancel_check) {
      asio::cancellation_signal cancellation;
      std::exception_ptr failure;
      bool complete = false;
      asio::co_spawn(
         io,
         factory(cancellation.slot()),
         [&](std::exception_ptr operation_failure) {
            failure = std::move(operation_failure);
            complete = true;
            poll_timer.cancel();
         });
      arm_cancel_poll(cancel_check, cancellation, complete);
      io.restart();
      io.run();
      if (failure)
         std::rethrow_exception(failure);
   }

   asio::io_context io;
   client async_client;
   asio::steady_timer poll_timer;
   std::mutex use_mutex;
};

transport::transport(transport_options options)
   : transport(std::move(options), {}) {}

transport::transport(
   transport_options options,
   detail::resolver_start_fn resolver_start)
   : _impl(
        std::make_unique<transport_impl>(
           std::move(options),
           std::move(resolver_start))) {}

transport::~transport() = default;
transport::transport(transport&&) noexcept = default;
transport& transport::operator=(transport&&) noexcept = default;

response transport::perform(const request& req,
                            const request_options& options) {
   return _impl->perform(req, options);
}

void transport::prime_endpoint(
   const url& target,
   const request_options& options) {
   _impl->prime_endpoint(target, options);
}

void transport::perform_to_file(
   const request& req,
   const request_options& options,
   const std::filesystem::path& output,
   const std::function<void(const http_file_download_status&)>&
      status_callback,
   const std::function<uint64_t(const std::filesystem::path&)>&
      space_available_provider) {
   _impl->perform_to_file(
      req,
      options,
      output,
      status_callback,
      space_available_provider);
}

} // namespace http

namespace {

constexpr uint64_t legacy_max_request_body_bytes =
   1ULL * 1024ULL * 1024ULL;
constexpr uint64_t legacy_max_response_body_bytes =
   1ULL * 1024ULL * 1024ULL;

/** Return phase timeouts honoring the legacy absolute-deadline argument. */
http::timeout_options legacy_timeouts(time_point deadline) {
   http::timeout_options result;
   if (deadline < time_point::maximum()) {
      const auto now = time_point::now();
      FC_ASSERT(
         now < deadline,
         "HTTP request deadline already expired");
      result.total = deadline - now;
   }
   return result;
}

} // namespace

http_client::http_client()
   : http_client(http::transport_options{}) {}

http_client::http_client(http::transport_options options)
   : _transport(
        std::make_unique<http::transport>(
           std::move(options))) {}

http_client::~http_client() = default;

void http_client::post_to_file(
   const url& dest,
   const variant& payload,
   const std::filesystem::path& output,
   const http_file_download_options& options) {
   http::request req{
      .method = http::request_method::post,
      .target = dest,
      .body =
         json::to_string(
            payload,
            time_point::maximum()),
      .content_type = "application/json",
      .user_agent = "wire-libfc-http",
   };
   http::request_options policy{
      .max_request_body_bytes =
         legacy_max_request_body_bytes,
      .max_response_body_bytes =
         options.max_response_body_bytes,
      .timeouts = options.timeouts,
      .retry =
         http::retry_options{
            .max_attempts =
               options.retry_failed_reused_connection
                  ? 2U
                  : 1U,
         },
      .idempotent =
         options.retry_failed_reused_connection,
      .retry_only_reused_connection =
         options.retry_failed_reused_connection,
      .cancel_check = _cancel_check,
   };
   _transport->perform_to_file(
      req,
      policy,
      output,
      options.status_callback,
      _space_available_provider);
}

void http_client::set_cancel_check(
   std::function<bool()> cancel_check) {
   _cancel_check = std::move(cancel_check);
}

void http_client::set_space_available_provider_for_testing(
   std::function<uint64_t(
      const std::filesystem::path&)> provider) {
   _space_available_provider = std::move(provider);
}

variant http_client::post_sync(
   const url& dest,
   const variant& payload,
   const time_point& deadline) {
   const auto timeouts = legacy_timeouts(deadline);
   const auto serialization_deadline =
      deadline < time_point::maximum()
         ? deadline
         : time_point::now().safe_add(*timeouts.total);
   http::request req{
      .method = http::request_method::post,
      .target = dest,
      .body =
         json::to_string(
            payload,
            serialization_deadline),
      .content_type = "application/json",
      .user_agent = "wire-libfc-http",
   };
   http::request_options policy{
      .max_request_body_bytes =
         legacy_max_request_body_bytes,
      .max_response_body_bytes =
         legacy_max_response_body_bytes,
      .timeouts = timeouts,
      .cancel_check = _cancel_check,
   };
   const auto response = _transport->perform(req, policy);

   variant result;
   if (!response.body.empty()) {
      try {
         result = json::from_string(response.body);
      } catch (...) {
      }
   }
   if (response.status == 500) {
      exception_ptr remote;
      try {
         const auto error =
            result.get_object()["error"].get_object();
         remote = std::make_shared<exception>(
            error["code"].as_int64(),
            error["name"].as_string(),
            error["what"].as_string());
         if (error.contains("details")) {
            for (const auto& detail :
                 error["details"].get_array()) {
               remote->append_log(
                  FC_LOG_MESSAGE(
                     error,
                     "{}",
                     detail.get_object()["message"]
                        .as_string()));
            }
         }
      } catch (...) {
      }
      if (remote)
         remote->rethrow();
      FC_THROW(
         "Request failed with 500 response, but response was not parseable");
   }
   if (response.status == 404)
      FC_THROW("URL not found");
   if (response.status < 200 ||
       response.status >= 300) {
      FC_THROW(
         "HTTP POST failed with status {}",
         response.status);
   }
   return result;
}

void http_client::set_verify_peers(bool enabled) {
   FC_ASSERT(
      enabled,
      "Outbound HTTPS peer and hostname verification cannot be disabled");
}

} // namespace fc

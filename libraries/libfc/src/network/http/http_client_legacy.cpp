#include <fc/network/http/http_client.hpp>

#include <fc/io/json.hpp>
#include <fc/task/deadline.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/http/status.hpp>

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
constexpr uint32_t internal_server_error_status =
   static_cast<uint32_t>(
      boost::beast::http::status::internal_server_error);
constexpr uint32_t not_found_status =
   static_cast<uint32_t>(
      boost::beast::http::status::not_found);
thread_local const void* active_transport = nullptr;

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
      std::optional<connection_validation> validation = std::nullopt,
      detail::resolver_start_fn resolver_start = {})
      : async_client(
           io.get_executor(),
           std::move(options),
           std::move(validation),
           std::move(resolver_start))
      , poll_timer(io) {}

   /**
    * Serialize access to the private event loop within the caller's budget.
    *
    * The synchronous facade cannot safely run nested operations on the same
    * io_context. Same-thread re-entry therefore fails before lock acquisition,
    * while ordinary contention consumes the request's total/task budget.
    */
   class use_guard {
   public:
      use_guard(transport_impl& owner,
                request_options& options)
         : _owner(owner)
         , _lock(owner.use_mutex, std::defer_lock)
         , _previous(active_transport) {
         if (active_transport == &_owner) {
            FC_THROW(
               "Outbound HTTP synchronous transport cannot be re-entered");
         }

         const auto started = time_point::now();
         std::optional<time_point> deadline;
         if (options.timeouts.total) {
            auto configured_deadline = started;
            configured_deadline.safe_add(
               *options.timeouts.total);
            deadline = configured_deadline;
         }
         if (options.timeouts.inherit_task_deadline) {
            if (const auto task_deadline =
                   fc::task::current_deadline();
                task_deadline &&
                (!deadline || *task_deadline < *deadline)) {
               deadline = task_deadline;
            }
         }

         if (!deadline && !options.cancel_check) {
            if (!_lock.try_lock()) {
               FC_THROW(
                  "Outbound HTTP synchronous transport is busy and the request has no queue deadline");
            }
         } else {
            while (!_lock.owns_lock()) {
               bool cancelled = false;
               try {
                  cancelled =
                     options.cancel_check &&
                     options.cancel_check();
               } catch (...) {
                  cancelled = true;
               }
               if (cancelled) {
                  FC_THROW_EXCEPTION(
                     fc::canceled_exception,
                     "Outbound HTTP request cancelled while waiting for the synchronous transport");
               }

               auto wait_for = cancellation_poll_interval;
               if (deadline) {
                  const auto remaining =
                     *deadline - time_point::now();
                  if (remaining.count() <= 0) {
                     FC_THROW_EXCEPTION(
                        fc::timeout_exception,
                        "Outbound HTTP total request deadline expired while waiting for the synchronous transport");
                  }
                  wait_for = std::min(
                     wait_for,
                     std::chrono::duration_cast<
                        std::chrono::milliseconds>(
                        std::chrono::microseconds(
                           remaining.count())));
                  if (wait_for.count() <= 0)
                     wait_for =
                        std::chrono::milliseconds(1);
               }
               (void)_lock.try_lock_for(wait_for);
            }
         }

         if (options.timeouts.total) {
            const auto elapsed =
               time_point::now() - started;
            if (elapsed >= *options.timeouts.total) {
               FC_THROW_EXCEPTION(
                  fc::timeout_exception,
                  "Outbound HTTP total request deadline expired while waiting for the synchronous transport");
            }
            options.timeouts.total =
               *options.timeouts.total - elapsed;
         }
         active_transport = &_owner;
      }

      use_guard(const use_guard&) = delete;
      use_guard& operator=(const use_guard&) = delete;

      ~use_guard() {
         active_transport = _previous;
      }

   private:
      transport_impl& _owner;
      std::unique_lock<std::timed_mutex> _lock;
      const void* _previous;
   };

   /** Buffer one asynchronous request while polling a legacy cancellation predicate. */
   response perform(const request& req,
                    const request_options& options) {
      auto async_options = options;
      use_guard guard(*this, async_options);
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

   /** Run one hook-selected follow-up on the first response's exact connection. */
   response perform_then(
      const request& req,
      const request_options& options,
      const continuation_hook& continue_with) {
      auto async_options = options;
      use_guard guard(*this, async_options);
      async_options.cancel_check = {};
      auto active_cancel =
         std::make_shared<std::function<bool()>>(
            options.cancel_check);
      auto async_continue =
         [continue_with,
          active_cancel](
            const response& first_response) mutable {
            auto continuation =
               continue_with(first_response);
            if (continuation.options.cancel_check) {
               auto first_cancel =
                  std::move(*active_cancel);
               auto next_cancel =
                  std::move(
                     continuation.options.cancel_check);
               *active_cancel =
                  [first_cancel =
                      std::move(first_cancel),
                   next_cancel =
                      std::move(next_cancel)] {
                     return (first_cancel &&
                             first_cancel()) ||
                            (next_cancel &&
                             next_cancel());
                  };
            }
            continuation.options.cancel_check = {};
            bool cancelled = false;
            try {
               cancelled =
                  static_cast<bool>(*active_cancel) &&
                  (*active_cancel)();
            } catch (...) {
               cancelled = true;
            }
            if (cancelled) {
               FC_THROW_EXCEPTION(
                  fc::canceled_exception,
                  "Outbound HTTP connection-affine follow-up cancelled");
            }
            return continuation;
         };
      return run<response>(
         [&](asio::cancellation_slot slot) {
            return async_client.async_request_then(
               req,
               std::move(async_options),
               std::move(async_continue),
               slot);
         },
         [active_cancel] {
            return static_cast<bool>(*active_cancel) &&
                   (*active_cancel)();
         });
   }

   /** Resolve one endpoint through the same asynchronous core. */
   void prime_endpoint(const url& target,
                       const request_options& options) {
      auto async_options = options;
      use_guard guard(*this, async_options);
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
      auto async_options = options;
      use_guard guard(*this, async_options);
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
   std::timed_mutex use_mutex;
};

transport::transport(
   transport_options options)
   : transport(
        std::move(options),
        std::nullopt,
        {}) {}

transport::transport(
   transport_options options,
   std::optional<connection_validation> validation)
   : transport(
        std::move(options),
        std::move(validation),
        {}) {}

transport::transport(
   transport_options options,
   detail::resolver_start_fn resolver_start)
   : transport(
        std::move(options),
        std::nullopt,
        std::move(resolver_start)) {}

transport::transport(
   transport_options options,
   std::optional<connection_validation> validation,
   detail::resolver_start_fn resolver_start)
   : _impl(
        std::make_unique<transport_impl>(
           std::move(options),
           std::move(validation),
           std::move(resolver_start))) {}

transport::~transport() = default;
transport::transport(transport&&) noexcept = default;
transport& transport::operator=(transport&&) noexcept = default;

response transport::perform(const request& req,
                            const request_options& options) {
   return _impl->perform(req, options);
}

response transport::perform_then(
   const request& req,
   const request_options& options,
   const continuation_hook& continue_with) {
   return _impl->perform_then(
      req,
      options,
      continue_with);
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
            .allow_retry =
               options.retry_failed_reused_connection
                  ? std::function<bool(const http::retry_context&)>{
                       [](const http::retry_context& context) {
                          return context.reused_connection;
                       }}
                  : std::function<bool(const http::retry_context&)>{},
         },
      .idempotent =
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
   if (response.status ==
       http::internal_server_error_status) {
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
   if (response.status == http::not_found_status)
      FC_THROW("URL not found");
   if (boost::beast::http::to_status_class(
          response.status) !=
       boost::beast::http::status_class::successful) {
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

void http_client::set_transport_options(
   http::transport_options options) {
   _transport =
      std::make_unique<http::transport>(
         std::move(options));
}

} // namespace fc

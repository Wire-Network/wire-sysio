#include "_http_transport_internal.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/beast/version.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <string>

namespace fc {
namespace http {

namespace {

/** Convert the public request method enum to a Beast verb by enum spelling. */
beast_http::verb to_beast_verb(request_method method) {
   std::string name(magic_enum::enum_name(method));
   FC_ASSERT(!name.empty(), "Unknown outbound HTTP method");
   if (name.back() == '_')
      name.pop_back();
   std::transform(name.begin(), name.end(), name.begin(),
                  [](unsigned char value) { return static_cast<char>(std::toupper(value)); });
   const auto verb = beast_http::string_to_verb(name);
   FC_ASSERT(verb != beast_http::verb::unknown, "Unsupported outbound HTTP method {}", name);
   return verb;
}

} // namespace

beast_http::request<beast_http::string_body> client_impl::build_request(const request& req,
                                                                        const target_info& target) const {
   std::string request_target = target.request_target;
   if (proxy_host && target.scheme == scheme_http) {
      request_target = "http://" + target.host_header + target.request_target;
   }
   if (std::any_of(request_target.begin(), request_target.end(),
                   [](unsigned char character) { return character <= 0x20 || character == 0x7f; })) {
      throw transport_failure(failure_kind::request_limit,
                              "request target contains a forbidden control or space character");
   }
   beast_http::request<beast_http::string_body> result{to_beast_verb(req.method), request_target, http_version_1_1};
   result.set(beast_http::field::host, target.host_header);
   result.set(beast_http::field::user_agent, req.user_agent.empty() ? BOOST_BEAST_VERSION_STRING : req.user_agent);
   if (!req.content_type.empty() && !req.body.empty())
      result.set(beast_http::field::content_type, req.content_type);
   for (const auto& [name, value] : req.headers)
      result.set(name, value);
   result.keep_alive(true);
   result.body() = req.body;
   result.prepare_payload();
   return result;
}

asio::awaitable<void> client_impl::wait_before_retry(const request_options& policy,
                                                     const std::optional<time_point>& total_deadline,
                                                     uint32_t completed_attempts,
                                                     const std::shared_ptr<request_control>& control) {
   auto delay = policy.retry.initial_backoff;
   for (uint32_t index = 1; index < completed_attempts; ++index)
      delay = std::min(delay * 2, policy.retry.max_backoff);
   if (total_deadline) {
      const auto remaining = *total_deadline - time_point::now();
      if (remaining.count() <= 0) {
         throw transport_failure(failure_kind::timeout_total, "total request deadline expired during retry backoff");
      }
      delay = std::min(delay, remaining);
   }
   auto timer = std::make_shared<asio::steady_timer>(strand);
   timer->expires_after(std::chrono::microseconds(delay.count()));
   active_cancel_guard cancel_guard(control, [timer] { timer->cancel(); });
   error_code error;
   co_await timer->async_wait(asio::redirect_error(asio::use_awaitable, error));
   control->throw_if_cancelled("retry backoff");
   if (error)
      throw transport_failure(failure_kind::io, "retry timer failed");
   if (total_deadline && time_point::now() >= *total_deadline) {
      throw transport_failure(failure_kind::timeout_total, "total request deadline expired during retry backoff");
   }
}

/** Parser, connection lease, and policy for one opened response. */
class response_reader_impl : public std::enable_shared_from_this<response_reader_impl> {
public:
   response_reader_impl(std::shared_ptr<client_impl> client_in, std::shared_ptr<connection_state> connection_in,
                        std::string connection_key_in, request_options policy_in,
                        std::optional<time_point> total_deadline_in, std::shared_ptr<request_control> control_in,
                        std::shared_ptr<request_metrics_state> metrics_in)
      : client(std::move(client_in))
      , connection(std::move(connection_in))
      , connection_key(std::move(connection_key_in))
      , buffer(static_cast<size_t>(
           std::min<uint64_t>(policy_in.max_response_header_bytes, std::numeric_limits<size_t>::max())))
      , policy(std::move(policy_in))
      , total_deadline(total_deadline_in)
      , control(std::move(control_in))
      , metrics(std::move(metrics_in)) {
      restart_response_parser(parser, policy);
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
      metrics->finish_failure(is_success_status(value_head.status) ? failure_kind::io : failure_kind::http_status);
   }

   /** Initialize public metadata and the aggregate body-read deadline. */
   void header_complete() {
      value_head.status = parser->get().result_int();
      value_head.reason = sanitize_reason(parser->get().reason());
      if (const auto length = parser->content_length())
         value_head.content_length = *length;
      if (policy.timeouts.read) {
         read_deadline = phase_deadline(policy.timeouts.read, failure_kind::timeout_read, total_deadline);
      }
      opened.store(true, std::memory_order_release);
      finish_if_complete();
   }

   /** Read one body increment on the client's strand. */
   asio::awaitable<size_t> read_some(asio::mutable_buffer output) {
      if (complete.load(std::memory_order_acquire))
         co_return 0;
      if (abandoned.load(std::memory_order_acquire)) {
         throw transport_failure(failure_kind::cancelled, "response reader was abandoned");
      }
      if (output.size() == 0)
         co_return 0;
      control->throw_if_cancelled("response body read");
      if (reading) {
         throw transport_failure(failure_kind::io, "concurrent response-body reads are not supported");
      }
      reading = true;

      try {
         const auto bytes = co_await std::visit(
            [&](auto& stream) {
               return read_body(connection, *stream, buffer, *parser, output, policy, total_deadline, read_deadline,
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
      if (complete.load(std::memory_order_acquire) || abandoned.exchange(true, std::memory_order_acq_rel)) {
         return;
      }
      auto self = shared_from_this();
      asio::post(client->strand, [self] {
         if (self->complete.exchange(true, std::memory_order_acq_rel))
            return;
         self->connection->close();
         self->metrics->finish_failure(is_success_status(self->value_head.status) ? failure_kind::io
                                                                                  : failure_kind::http_status);
      });
   }

   /** Return the leased connection only after Beast confirms end-of-message. */
   void finish_if_complete() {
      if (!parser->is_done() || complete.exchange(true, std::memory_order_acq_rel)) {
         return;
      }
      if (!parser->get().keep_alive())
         connection->close();
      else
         client->release_connection(connection_key, connection);
      metrics->finish_status(value_head.status);
   }

   std::shared_ptr<client_impl> client;
   std::shared_ptr<connection_state> connection;
   std::string connection_key;
   beast::flat_buffer buffer;
   std::optional<beast_http::response_parser<beast_http::buffer_body>> parser;
   request_options policy;
   std::optional<time_point> total_deadline;
   std::optional<operation_deadline> read_deadline;
   std::shared_ptr<request_control> control;
   std::shared_ptr<request_metrics_state> metrics;
   response_head value_head;
   std::atomic_bool opened{false};
   std::atomic_bool complete{false};
   std::atomic_bool abandoned{false};
   bool reading = false;
};

asio::awaitable<std::shared_ptr<response_reader_impl>>
client_impl::async_open(request req, request_options policy, std::shared_ptr<request_control> control,
                        std::function<void(http_file_download_phase)> on_phase) {
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
   transport_failure last_failure(failure_kind::io, "request did not start");
   for (uint32_t attempt = 1; attempt <= policy.retry.max_attempts; ++attempt) {
      std::shared_ptr<connection_state> connection;
      bool reused = false;
      bool retry = false;
      try {
         if (on_phase)
            on_phase(http_file_download_phase::connecting);
         auto acquired = co_await acquire_connection(target, policy, total_deadline, control, attempt > 1);
         connection = std::move(acquired.first);
         reused = acquired.second;

         auto request_message = build_request(req, target);
         if (on_phase)
            on_phase(http_file_download_phase::sending_request);
         control->throw_if_cancelled("request send");
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
               return write_request(connection, *stream, request_message, upload_deadline, control);
            },
            connection->stream);
         shared_metrics().request_bytes.fetch_add(req.body.size(), std::memory_order_relaxed);

         auto reader = std::make_shared<response_reader_impl>(shared_from_this(), connection, target.connection_key,
                                                              policy, total_deadline, control, metrics);
         if (on_phase)
            on_phase(http_file_download_phase::waiting_for_response);
         // One budget covers every header read below, interim responses included, so a peer
         // cannot extend the header phase by trickling 1xx responses.
         const auto header_deadline =
            phase_deadline(policy.timeouts.header, failure_kind::timeout_header, total_deadline);
         co_await std::visit(
            [&](auto& stream) {
               return read_final_header(connection, *stream, reader->buffer, reader->parser, policy, header_deadline,
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
               failure.retryable = policy.retry.allow_retry(retry_context{
                  .failure = failure.kind,
                  .attempt = attempt,
                  .reused_connection = reused,
               });
            } catch (...) {
               metrics->finish_failure(failure_kind::io);
               throw transport_failure(failure_kind::io, "retry decision hook threw");
            }
         }
         last_failure = failure;
         if (attempt == policy.retry.max_attempts || !failure.retryable) {
            const auto final_failure =
               policy.retry.max_attempts > 1 && failure.retryable
                  ? transport_failure(failure_kind::retry_exhausted, "retry attempts exhausted after " +
                                                                        std::to_string(attempt) +
                                                                        " attempts: " + failure.what())
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
            co_await wait_before_retry(policy, total_deadline, attempt, control);
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

response_reader::response_reader(std::shared_ptr<response_reader_impl> impl)
   : _impl(std::move(impl)) {}

response_reader::~response_reader() {
   if (_impl)
      _impl->abandon();
}

response_reader::response_reader(response_reader&& other) noexcept
   : _impl(std::move(other._impl)) {}

response_reader& response_reader::operator=(response_reader&& other) noexcept {
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
asio::awaitable<size_t> async_read_some_public(std::shared_ptr<response_reader_impl> impl,
                                               asio::mutable_buffer output) {
   try {
      co_return co_await impl->read_some(output);
   } catch (const transport_failure& failure) {
      throw_public_failure(failure);
   }
}

asio::awaitable<size_t> response_reader::async_read_some(asio::mutable_buffer output) {
   FC_ASSERT(_impl, "Outbound HTTP response reader is empty");
   auto impl = _impl;
   auto executor = impl->client->strand;
   return asio::co_spawn(std::move(executor), async_read_some_public(std::move(impl), output), asio::use_awaitable);
}

bool response_reader::done() const noexcept {
   return !_impl || _impl->complete.load(std::memory_order_acquire);
}

/** Buffer one public response reader under its existing bounded policy. */
asio::awaitable<response> async_buffer_response(response_reader& reader) {
   response result{
      .status = reader.head().status,
      .reason = reader.head().reason,
   };
   std::array<char, body_read_buffer_bytes> body_buffer{};
   while (!reader.done()) {
      const auto bytes = co_await reader.async_read_some(asio::buffer(body_buffer));
      result.body.append(body_buffer.data(), bytes);
   }
   co_return result;
}

client::client(asio::any_io_executor executor, transport_options options)
   : client(std::move(executor), std::move(options), {}) {}

client::client(asio::any_io_executor executor, transport_options options, detail::resolver_start_fn resolver_start)
   : _impl(std::make_shared<client_impl>(std::move(executor), std::move(options), std::move(resolver_start))) {}

client::~client() = default;
client::client(client&&) noexcept = default;
client& client::operator=(client&&) noexcept = default;

asio::awaitable<response_reader> client::async_open(request req, request_options options,
                                                    asio::cancellation_slot cancellation) {
   FC_ASSERT(_impl, "Outbound HTTP client is empty");
   auto control = request_control::create(cancellation, _impl->strand);
   try {
      auto impl = co_await asio::co_spawn(
         _impl->strand, _impl->async_open(std::move(req), std::move(options), std::move(control)), asio::use_awaitable);
      co_return response_reader(std::move(impl));
   } catch (const transport_failure& failure) {
      throw_public_failure(failure);
   }
}

asio::awaitable<response> client::async_request(request req, request_options options,
                                                asio::cancellation_slot cancellation) {
   auto reader = co_await async_open(std::move(req), std::move(options), cancellation);
   co_return co_await async_buffer_response(reader);
}

} // namespace http
} // namespace fc

#include "http_transport_internal.hpp"

#include <algorithm>
#include <boost/asio/ip/address.hpp>
#include <cctype>
#include <fc/task/deadline.hpp>
#include <limits>
#include <string>
#include <string_view>

namespace fc {
namespace http {

namespace {

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

   return std::all_of(host.begin(), host.end(), [](unsigned char character) {
      return std::isalnum(character) || character == '-' || character == '.' || character == '_';
   });
}

} // namespace

inline namespace transport_internal {

transport_failure::~transport_failure() = default;

atomic_metrics& shared_metrics() {
   static atomic_metrics metrics;
   return metrics;
}

void record_failure(failure_kind kind) {
   const auto index = magic_enum::enum_index(kind);
   FC_ASSERT(index, "Unknown outbound HTTP failure kind");
   shared_metrics().failures[*index].fetch_add(1, std::memory_order_relaxed);
}

bool is_success_status(uint32_t status) {
   return beast_http::to_status_class(status) == beast_http::status_class::successful;
}

void throw_public_failure(const transport_failure& failure) {
   const auto category = std::string(failure_kind_name(failure.kind));
   if (failure.kind == failure_kind::cancelled) {
      FC_THROW_EXCEPTION(fc::canceled_exception, "Outbound HTTP {}: {}", category, failure.what());
   }
   if (failure.kind == failure_kind::timeout_connect || failure.kind == failure_kind::timeout_header ||
       failure.kind == failure_kind::timeout_read || failure.kind == failure_kind::timeout_idle ||
       failure.kind == failure_kind::timeout_total) {
      FC_THROW_EXCEPTION(fc::timeout_exception, "Outbound HTTP {}: {}", category, failure.what());
   }
   FC_THROW("Outbound HTTP {}: {}", category, failure.what());
}

std::optional<time_point> effective_total_deadline(const request_options& options) {
   std::optional<time_point> result;
   if (options.timeouts.total)
      result = time_point::now().safe_add(*options.timeouts.total);
   if (options.timeouts.inherit_task_deadline) {
      if (auto task_deadline = fc::task::current_deadline();
          task_deadline && *task_deadline < time_point::maximum() && (!result || *task_deadline < *result)) {
         result = *task_deadline;
      }
   }
   return result;
}

std::optional<operation_deadline> phase_deadline(const std::optional<microseconds>& phase_timeout,
                                                 failure_kind phase_failure,
                                                 const std::optional<time_point>& total_deadline) {
   auto now = time_point::now();
   if (total_deadline && *total_deadline <= now) {
      throw transport_failure(failure_kind::timeout_total, "total request deadline expired");
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
namespace {
void validate_header_component(std::string_view value, std::string_view label) {
   const auto invalid = std::find_if(value.begin(), value.end(), [](unsigned char character) {
      return (character < 0x20 && character != '\t') || character == 0x7f;
   });
   if (invalid != value.end()) {
      throw transport_failure(failure_kind::request_limit,
                              "request " + std::string(label) + " contains a forbidden control character");
   }
}
} // namespace

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

bool is_retryable_connection_error(const boost::system::error_code& error) {
   return error == asio::error::eof || error == asio::error::connection_reset ||
          error == asio::error::connection_aborted || error == asio::error::broken_pipe ||
          error == asio::error::not_connected || error == asio::error::shut_down ||
          error == beast_http::error::end_of_stream;
}

void throw_if_operation_failed(const error_code& error, const std::optional<operation_deadline>& deadline,
                               const std::shared_ptr<request_control>& control) {
   control->throw_if_cancelled();
   if (deadline && (error == beast::error::timeout || error == asio::error::timed_out)) {
      throw transport_failure(deadline->timeout_kind, "request deadline expired");
   }
}

} // namespace transport_internal

bool is_safe_network_host(std::string_view host) {
   return is_safe_network_host_impl(host);
}

void client_impl::validate_policy(const request_options& policy) {
   FC_ASSERT(policy.max_request_header_bytes > 0, "Outbound HTTP request-header limit must be positive");
   FC_ASSERT(policy.max_response_header_bytes > 0, "Outbound HTTP response-header limit must be positive");
   FC_ASSERT(!policy.timeouts.connect || policy.timeouts.connect->count() > 0,
             "Outbound HTTP connect timeout must be positive when present");
   FC_ASSERT(!policy.timeouts.header || policy.timeouts.header->count() > 0,
             "Outbound HTTP header timeout must be positive when present");
   FC_ASSERT(!policy.timeouts.read || policy.timeouts.read->count() > 0,
             "Outbound HTTP read timeout must be positive when present");
   FC_ASSERT(!policy.timeouts.idle || policy.timeouts.idle->count() > 0,
             "Outbound HTTP idle timeout must be positive when present");
   FC_ASSERT(!policy.timeouts.total || policy.timeouts.total->count() > 0,
             "Outbound HTTP total timeout must be positive when present");
   FC_ASSERT(policy.retry.max_attempts > 0, "Outbound HTTP retry attempts must be positive");
   FC_ASSERT(policy.retry.max_attempts == 1 || policy.idempotent,
             "Outbound HTTP retries require an explicitly idempotent request");
   FC_ASSERT(policy.retry.initial_backoff.count() >= 0 && policy.retry.max_backoff.count() >= 0,
             "Outbound HTTP retry backoff cannot be negative");
}

uint64_t client_impl::estimate_request_header_bytes(const request& req) {
   constexpr uint64_t transport_header_allowance = 256;
   uint64_t result = transport_header_allowance;
   auto add = [&](uint64_t bytes) {
      FC_ASSERT(bytes <= std::numeric_limits<uint64_t>::max() - result,
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

void client_impl::validate_request(const request& req, const request_options& policy) {
   auto fail = [](std::string message) { throw transport_failure(failure_kind::request_limit, std::move(message)); };
   if (estimate_request_header_bytes(req) > policy.max_request_header_bytes) {
      fail("request headers exceed configured maximum of " + std::to_string(policy.max_request_header_bytes) +
           " bytes");
   }
   if (req.body.size() > policy.max_request_body_bytes) {
      fail("request body exceeds configured maximum of " + std::to_string(policy.max_request_body_bytes) + " bytes");
   }

   validate_header_component(req.user_agent, "User-Agent");
   validate_header_component(req.content_type, "Content-Type");
   for (const auto& [name, value] : req.headers) {
      validate_header_component(name, "header name");
      validate_header_component(value, "header value");
      const auto valid_name_character = [](unsigned char character) {
         return std::isalnum(character) ||
                std::string_view("!#$%&'*+-.^_`|~").find(static_cast<char>(character)) != std::string_view::npos;
      };
      if (name.empty() || !std::all_of(name.begin(), name.end(), valid_name_character)) {
         fail("request contains an invalid header name");
      }
      const auto field = beast_http::string_to_field(name);
      if (field == beast_http::field::host || field == beast_http::field::content_length ||
          field == beast_http::field::transfer_encoding || field == beast_http::field::connection ||
          field == beast_http::field::proxy_connection || field == beast_http::field::proxy_authorization ||
          field == beast_http::field::expect) {
         fail("request attempts to override a transport-controlled header");
      }
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
   const bool ipv6 = target.host()->find(':') != std::string::npos;
   result += ipv6 ? "[" + *target.host() + "]" : *target.host();
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
      result.failures[*index] = source.failures[*index].load(std::memory_order_relaxed);
   }
   return result;
}

} // namespace http
} // namespace fc

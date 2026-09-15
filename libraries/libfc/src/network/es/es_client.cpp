#include <algorithm>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <chrono>
#include <cstddef>
#include <exception>
#include <fc/crypto/base64.hpp>
#include <fc/exception/exception.hpp>
#include <fc/io/json.hpp>
#include <fc/log/logger_config.hpp> // fc::set_thread_name
#include <fc/network/es/es_client.hpp>
#include <fc/scoped_exit.hpp>
#include <fc/variant.hpp>
#include <fc/variant_object.hpp>
#include <fmt/format.h>
#include <string_view>

namespace fc::network::es {

namespace {

/// The action line around the encoded index: `{"index":{"_index":<index>}}` plus the newline _bulk requires.
constexpr std::string_view es_action_line_prefix = R"({"index":{"_index":)";
constexpr std::string_view es_action_line_suffix = "}}\n";
constexpr std::string_view es_bulk_path = "/_bulk";
/// What probe() requests: the base URL's root, the one path every OpenSearch/Elasticsearch endpoint answers.
constexpr std::string_view es_probe_path = "/";
/// The reason probe() reports when a failure carries no message of its own.
constexpr std::string_view es_unknown_failure = "unknown failure";
constexpr std::string_view es_content_type = "application/x-ndjson";
constexpr std::string_view es_user_agent = "wire-es-client";
/// OS and fc name of the client's io thread.
constexpr std::string_view es_io_thread_name = "es-client";
constexpr std::string_view es_scheme_http = "http";
constexpr std::string_view es_scheme_https = "https";
/// The top-level "errors" flag precedes "items" in a bulk response, so probing the head of the body for
/// the success token is authoritative; every other outcome (item errors, non-bulk 2xx, unexpected
/// whitespace) falls through to a full parse.
constexpr std::size_t es_errors_probe_bytes = 256;
constexpr std::string_view es_errors_false_token = R"("errors":false)";
constexpr std::chrono::milliseconds es_max_backoff{es_max_retry_backoff_ms};
/// Clamp for the backoff doubling exponent: `1u << attempt` is UB once `attempt` reaches the width of
/// unsigned (max_retries is operator-controlled and may exceed 32). es_max_backoff already caps the RESULT
/// after ~4 doublings, so clamping the exponent changes no observable behavior.
constexpr uint32_t es_max_backoff_exponent = 16;
/// Bulk responses carry one item per document; cap them well above realistic sizes.
constexpr uint64_t es_max_response_body_bytes = 4ULL * 1024ULL * 1024ULL;
constexpr uint32_t es_http_status_ok_min = 200;
constexpr uint32_t es_http_status_ok_max = 299;
/// Authenticated but not authorized. probe() counts it as reachable: its GET of the root is the cluster-info
/// path, which OpenSearch fine-grained access control gates behind the cluster `monitor/main` permission, so a
/// least-privilege credential scoped to writing _bulk is answered with a 403 while _bulk itself works.
constexpr uint32_t es_http_status_forbidden = 403;
constexpr uint32_t es_http_status_too_many_requests = 429;
constexpr uint32_t es_http_status_server_error_min = 500;

} // anonymous namespace

es_client_options es_client::validate(es_client_options options) {
   while (!options.url.empty() && options.url.back() == '/')
      options.url.pop_back();
   FC_ASSERT(!options.url.empty(), "es_client: url is required");
   const fc::url parsed{options.url};
   FC_ASSERT(parsed.proto() == es_scheme_http || parsed.proto() == es_scheme_https,
             "es_client: url scheme must be http or https, got '{}'", parsed.proto());
   FC_ASSERT(!options.index.empty(), "es_client: index is required");
   FC_ASSERT(options.max_batch_bytes > 0 && options.max_batch_bytes <= es_max_batch_bytes_ceiling,
             "es_client: max_batch_bytes must be in (0, {}]", es_max_batch_bytes_ceiling);
   FC_ASSERT(options.max_doc_bytes > 0 && options.max_doc_bytes <= options.max_batch_bytes,
             "es_client: max_doc_bytes must be in (0, max_batch_bytes]");
   FC_ASSERT(options.username.has_value() == options.password.has_value(),
             "es_client: username and password must be provided together");
   return options;
}

std::optional<std::string> es_client::auth_header_for(const es_client_options& options) {
   if (!options.username)
      return std::nullopt;
   return "Basic " + fc::base64_encode(*options.username + ":" + *options.password);
}

es_client::es_client(es_client_options options)
   : _options(validate(std::move(options)))
   // The index is encoded by fc::json -- the one JSON encoder every producer's documents go through as well.
   , _action_line(std::string{es_action_line_prefix} +
                  fc::json::to_string(fc::variant{_options.index}, fc::time_point::maximum()) +
                  std::string{es_action_line_suffix})
   , _bulk_url(_options.url + std::string{es_bulk_path})
   , _root_url(_options.url + std::string{es_probe_path})
   , _endpoint(http::sanitized_endpoint(_bulk_url))
   , _auth_header(auth_header_for(_options))
   , _work(boost::asio::make_work_guard(_io))
   , _http(_io.get_executor())
   , _io_thread([this] {
      // An exception leaving a thread function terminates the process, so every statement here is guarded --
      // including the thread-name allocation, whose failure would otherwise take the node down.
      try {
         fc::set_thread_name(std::string{es_io_thread_name});
      } catch (...) {}
      // Nothing posted here throws (async_bulk returns every outcome as a value and the cancel emit is
      // noexcept); the loop only guards the thread against an unexpected handler exception ending delivery.
      while (!_io.stopped()) {
         try {
            _io.run();
         } catch (...) {}
      }
   }) {}

es_client::~es_client() {
   cancel();      // aborts an in-flight request or backoff on the io thread
   _work.reset(); // run() returns once the (now canceled) work has drained
   if (_io_thread.joinable())
      _io_thread.join();
}

void es_client::cancel() noexcept {
   if (_cancel_requested.exchange(true, std::memory_order_relaxed))
      return;
   // cancellation_signal is not thread-safe: emit it on the io thread, where every operation that connects
   // to its slot runs. The flag above covers the window in which no operation is connected.
   try {
      boost::asio::post(_io, [this] { _cancellation.emit(boost::asio::cancellation_type::all); });
   } catch (...) {
      // Allocation failure only loses the abort of an in-flight request: the flag above already cancels later ones.
   }
}

es_bulk_result es_client::canceled_result(uint32_t doc_count, uint32_t attempts, std::string detail) {
   es_bulk_result result;
   result.outcome = es_bulk_result::status::canceled;
   result.attempts = attempts;
   result.failed_docs = doc_count;
   result.detail = std::move(detail);
   return result;
}

boost::asio::awaitable<es_bulk_result> es_client::async_bulk(std::string body, uint32_t doc_count) {
   // Every member this coroutine touches -- the http client, the cancellation signal -- is io-thread-only, so a
   // caller that co_spawns it onto another executor is a programming error. Checked before the flag below, so a
   // misuse leaves _in_flight clear.
   FC_ASSERT(_io.get_executor().running_in_this_thread(), "es_client: async_bulk() must run on get_executor()");
   // The cancellation signal holds one slot: a second request would displace the first one's cancel handler.
   // When this fires the flag was already set, so it is left set -- the request that owns it still clears it.
   FC_ASSERT(!_in_flight.exchange(true),
             "es_client: one request at a time -- async_bulk() must not overlap another request");
   auto in_flight_guard = fc::make_scoped_exit([this] { _in_flight.store(false); });

   http::request req;
   req.method = http::request_method::post;
   req.target = _bulk_url;
   req.body = std::move(body);
   req.content_type = std::string{es_content_type};
   req.user_agent = std::string{es_user_agent};
   if (_auth_header)
      req.headers.emplace_back("Authorization", *_auth_header);

   http::request_options opt;
   // A body may exceed max_batch_bytes by one action/document pair (the producer closes a body only when the
   // NEXT pair would overflow it), so the client's cap allows exactly that much more.
   opt.max_request_body_bytes = uint64_t{_options.max_batch_bytes} + _options.max_doc_bytes + _action_line.size();
   opt.max_response_body_bytes = es_max_response_body_bytes;
   opt.timeouts.connect = fc::milliseconds(_options.connect_timeout_ms);
   opt.timeouts.header = opt.timeouts.read = opt.timeouts.idle = opt.timeouts.total =
      fc::milliseconds(_options.request_timeout_ms);
   // Delivery runs on the client's own thread: an ambient fc task deadline must not bound it.
   opt.timeouts.inherit_task_deadline = false;
   // The http client's retry stays at one attempt (a _bulk POST is not idempotent); this loop owns retries.
   // Cancellation arrives through the slot: the asynchronous client ignores request_options::cancel_check.
   // async_request takes the request by value, so every attempt copies the body (at most max_batch_bytes plus
   // one action/document pair): a retry is a replay of the same bytes, never a re-assembly.

   std::string detail;
   uint32_t attempts = 0;
   for (uint32_t attempt = 0;; ++attempt) {
      if (_cancel_requested.load(std::memory_order_relaxed))
         co_return canceled_result(doc_count, attempts, std::move(detail));
      try {
         ++attempts;
         const auto resp = co_await _http.async_request(req, opt, _cancellation.slot());
         if (resp.status >= es_http_status_ok_min && resp.status <= es_http_status_ok_max)
            co_return account_response(resp.body, doc_count, attempts);
         detail = fmt::format("HTTP {} from {}", resp.status, _endpoint);
         // 429 is endpoint back-pressure -- the one 4xx worth retrying; other 4xx are terminal (retrying a
         // rejected request only repeats the rejection).
         if (resp.status < es_http_status_server_error_min && resp.status != es_http_status_too_many_requests) {
            es_bulk_result result;
            result.outcome = es_bulk_result::status::rejected;
            result.attempts = attempts;
            result.failed_docs = doc_count;
            result.detail = std::move(detail);
            co_return result;
         }
      } catch (const fc::canceled_exception&) {
         co_return canceled_result(doc_count, attempts, std::move(detail)); // cancel() emitted the signal
      } catch (const fc::timeout_exception& e) {
         detail = fmt::format("timeout delivering to {}: {}", _endpoint, e.top_message());
      } catch (const fc::exception& e) {
         // connect / DNS / TLS / io failures -- message-classified only at this layer.
         detail = fmt::format("delivery to {} failed: {}", _endpoint, e.top_message());
      } catch (const std::exception& e) {
         // Anything the http client does not funnel through its own classification (an allocation failure, an
         // asio error on a path outside it): the outcome stays a value, as promised to both entry points.
         detail = fmt::format("delivery to {} failed: {}", _endpoint, e.what());
      } catch (...) {
         detail = fmt::format("delivery to {} failed", _endpoint);
      }
      if (attempt >= _options.max_retries) {
         es_bulk_result result;
         result.outcome = es_bulk_result::status::unavailable;
         result.attempts = attempts;
         result.failed_docs = doc_count;
         result.detail = std::move(detail);
         co_return result;
      }
      // Capped exponential backoff as a timer on this executor, interruptible by cancel() through the same
      // slot; an aborted wait (or a flag raised while nothing was connected to the slot) means canceled.
      const auto backoff = std::min<std::chrono::milliseconds>(std::chrono::milliseconds(_options.retry_backoff_ms) *
                                                                  (1u << std::min(attempt, es_max_backoff_exponent)),
                                                               es_max_backoff);
      boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor);
      timer.expires_after(backoff);
      boost::system::error_code wait_error;
      co_await timer.async_wait(boost::asio::bind_cancellation_slot(
         _cancellation.slot(), boost::asio::redirect_error(boost::asio::use_awaitable, wait_error)));
      if (wait_error || _cancel_requested.load(std::memory_order_relaxed))
         co_return canceled_result(doc_count, attempts, std::move(detail));
   }
}

es_bulk_result es_client::bulk(std::string body, uint32_t doc_count) {
   // This blocks the calling thread on the coroutine's result, so the io thread calling it would wait on the
   // one thread that has to run the coroutine. Outside the try below: a programming error is not a delivery
   // outcome and must propagate rather than fold into an `unavailable` value.
   FC_ASSERT(!_io.get_executor().running_in_this_thread(),
             "es_client: bulk() must not be called from the client's io thread -- co_spawn async_bulk() instead");
   if (_cancel_requested.load(std::memory_order_relaxed))
      return canceled_result(doc_count, 0);
   try {
      return boost::asio::co_spawn(_io, async_bulk(std::move(body), doc_count), boost::asio::use_future).get();
   } catch (const std::exception& e) {
      // async_bulk returns every delivery outcome as a value; only a failure to run the coroutine lands here.
      es_bulk_result result;
      result.outcome = es_bulk_result::status::unavailable;
      result.failed_docs = doc_count;
      result.detail = fmt::format("delivery could not run: {}", e.what());
      return result;
   } catch (...) {
      // A throw that is not a std::exception must not escape either: bulk() never throws.
      es_bulk_result result;
      result.outcome = es_bulk_result::status::unavailable;
      result.failed_docs = doc_count;
      result.detail = "delivery could not run";
      return result;
   }
}

void es_client::probe() {
   // Same affinity rule as bulk(): this blocks the calling thread on the coroutine's result, so the io thread
   // calling it would wait on the one thread that has to run the coroutine.
   FC_ASSERT(!_io.get_executor().running_in_this_thread(),
             "es_client: probe() must not be called from the client's io thread");
   // Same guard bulk() applies to the canceled client, as an assert rather than a value: cancel() is permanent,
   // so a probe after it would never send a request, and a connectivity check that cannot check is a
   // programming error rather than a result to report.
   FC_ASSERT(!_cancel_requested.load(std::memory_order_relaxed), "es_client: probe() after cancel()");
   // Unlike bulk(), every failure is an exception: the future rethrows whatever async_probe() threw, and a
   // failure to run the coroutine at all is just as fatal to the caller's initialization.
   boost::asio::co_spawn(_io, async_probe(), boost::asio::use_future).get();
}

boost::asio::awaitable<void> es_client::async_probe() {
   FC_ASSERT(_io.get_executor().running_in_this_thread(), "es_client: async_probe() must run on get_executor()");
   // The same single cancellation slot async_bulk() guards, for the same reason: a concurrent request would
   // displace the other one's cancel handler.
   FC_ASSERT(!_in_flight.exchange(true), "es_client: one request at a time -- probe() must not overlap a bulk request");
   auto in_flight_guard = fc::make_scoped_exit([this] { _in_flight.store(false); });

   http::request req;
   req.method = http::request_method::get;
   req.target = _root_url;
   req.user_agent = std::string{es_user_agent};
   if (_auth_header)
      req.headers.emplace_back("Authorization", *_auth_header);

   http::request_options opt;
   opt.max_response_body_bytes = es_max_response_body_bytes;
   opt.timeouts.connect = fc::milliseconds(_options.connect_timeout_ms);
   opt.timeouts.header = opt.timeouts.read = opt.timeouts.idle = opt.timeouts.total =
      fc::milliseconds(_options.request_timeout_ms);
   // The check runs on the client's own thread: an ambient fc task deadline must not bound it.
   opt.timeouts.inherit_task_deadline = false;
   // One attempt: request_options::retry stays at its single-attempt default and this function has no loop.

   std::string reason;
   bool canceled = false;
   try {
      const auto resp = co_await _http.async_request(req, opt, _cancellation.slot());
      // A 403 answers the only question this check asks: the endpoint is up and it accepted the credential.
      // Reading the cluster info is a separate permission a write-only credential is not required to hold.
      if ((resp.status >= es_http_status_ok_min && resp.status <= es_http_status_ok_max) ||
          resp.status == es_http_status_forbidden)
         co_return;
      reason = fmt::format("HTTP {}", resp.status);
   } catch (const fc::canceled_exception& e) {
      // cancel() aborted the check; the endpoint was never judged, so it must not be called unreachable.
      canceled = true;
      reason = e.top_message();
   } catch (const fc::exception& e) {
      // connect / DNS / TLS / timeout / io failures -- all carry their own message.
      reason = e.top_message();
   } catch (const std::exception& e) {
      reason = e.what();
   } catch (...) {
      reason = std::string{es_unknown_failure};
   }
   // Thrown outside the handlers above so the exception does not nest inside one.
   if (canceled)
      FC_THROW("es_client: probe of endpoint {}{} was canceled: {}", _endpoint, es_probe_path, reason);
   FC_THROW("es_client: endpoint {}{} is not reachable: {}", _endpoint, es_probe_path, reason);
}

es_bulk_result es_client::account_response(const std::string& body, uint32_t doc_count, uint32_t attempts) const {
   es_bulk_result result;
   result.attempts = attempts;
   const std::string_view probe = std::string_view{body}.substr(0, es_errors_probe_bytes);
   if (probe.find(es_errors_false_token) != std::string_view::npos) {
      result.outcome = es_bulk_result::status::indexed;
      result.indexed_docs = doc_count;
      return result;
   }
   // "errors":true, or neither token in the probe window (a proxy landing page, a response with
   // unexpected whitespace, ...) -- parse to find out. Never retry after a 2xx: a partial success
   // replayed duplicates the documents that DID index.
   try {
      const auto parsed = fc::json::from_string(body).get_object();
      if (!parsed.contains("errors")) {
         result.outcome = es_bulk_result::status::rejected;
         result.failed_docs = doc_count;
         result.detail = fmt::format("2xx from {} is not a bulk response", _endpoint);
         return result;
      }
      if (!parsed["errors"].as_bool()) {
         result.outcome = es_bulk_result::status::indexed;
         result.indexed_docs = doc_count;
         return result;
      }
      uint64_t failed_docs = 0;
      std::string first_reason;
      if (parsed.contains("items")) {
         for (const auto& item : parsed["items"].get_array()) {
            for (const auto& entry : item.get_object()) {
               const auto& outcome = entry.value().get_object();
               if (!outcome.contains("error"))
                  continue;
               ++failed_docs;
               if (first_reason.empty())
                  first_reason = fc::json::to_string(outcome["error"], fc::time_point::maximum());
            }
         }
      }
      failed_docs = std::min<uint64_t>(failed_docs, doc_count);
      result.outcome = es_bulk_result::status::partial;
      result.indexed_docs = doc_count - static_cast<uint32_t>(failed_docs);
      result.failed_docs = static_cast<uint32_t>(failed_docs);
      // "errors":true with no item carrying an error: report the contradiction instead of a "0 of N documents
      // rejected" line whose first error would be empty. Outcome and counts are unchanged.
      result.detail = failed_docs == 0 ? fmt::format("{} reported errors with no rejected items", _endpoint)
                                       : fmt::format("{} of {} documents rejected by {}; first error: {}", failed_docs,
                                                     doc_count, _endpoint, first_reason);
   } catch (const fc::exception&) {
      result.outcome = es_bulk_result::status::rejected;
      result.failed_docs = doc_count;
      result.detail = fmt::format("unparseable 2xx response from {}", _endpoint);
   } catch (const std::exception&) {
      // A 2xx is never retried, whatever the parse throws.
      result.outcome = es_bulk_result::status::rejected;
      result.failed_docs = doc_count;
      result.detail = fmt::format("unparseable 2xx response from {}", _endpoint);
   }
   return result;
}

} // namespace fc::network::es

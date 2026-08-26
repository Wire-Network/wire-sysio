#include <fc/log/es_sink.hpp>
#include <fc/log/json_escape.hpp>
#include <fc/log/json_formatter.hpp>

#include <fc/crypto/base64.hpp>
#include <fc/exception/exception.hpp>
#include <fc/io/json.hpp>
#include <fc/variant_object.hpp>

#include <algorithm>
#include <iostream>

namespace fc {

namespace {

using log::detail::json_escape_into;

constexpr std::string_view es_bulk_path     = "/_bulk";
constexpr std::string_view es_content_type  = "application/x-ndjson";
constexpr std::string_view es_user_agent    = "wire-es-sink";
constexpr std::string_view es_scheme_http   = "http";
constexpr std::string_view es_scheme_https  = "https";
/// The top-level "errors" flag precedes "items" in a bulk response, so probing the
/// head of the body for the success token is authoritative; every other outcome
/// (item errors, non-bulk 2xx, unexpected whitespace) falls through to a full parse.
constexpr std::size_t      es_errors_probe_bytes = 256;
constexpr std::string_view es_errors_false_token = R"("errors":false)";
constexpr auto es_warn_interval = std::chrono::seconds(5);
constexpr auto es_drain_poll    = std::chrono::milliseconds(25);
constexpr auto es_max_backoff   = std::chrono::milliseconds(2000);
/// Clamp for the backoff doubling exponent: `1u << attempt` is UB once `attempt`
/// reaches the width of unsigned (max_retries is operator-controlled and may exceed
/// 32). es_max_backoff already caps the RESULT after ~4 doublings, so clamping the
/// exponent changes no observable behavior.
constexpr uint32_t es_max_backoff_exponent = 16;
/// Bulk responses carry one item per document; cap them well above realistic sizes.
constexpr uint64_t es_max_response_body_bytes = 4ULL * 1024ULL * 1024ULL;
constexpr uint32_t http_status_ok_min            = 200;
constexpr uint32_t http_status_ok_max            = 299;
constexpr uint32_t http_status_too_many_requests = 429;
constexpr uint32_t http_status_server_error_min  = 500;

/// Fields used to sample-render the formatter for configure-time validation.
constexpr std::string_view sample_source_file = "es_sink";
constexpr std::string_view sample_logger_name = "es_sink";
constexpr std::string_view sample_payload     = "sample";
constexpr int              sample_source_line = 1;

/// Increments the completed-batches counter when a delivery attempt exits (any path).
struct delivery_completed_guard {
   explicit delivery_completed_guard(std::atomic<uint64_t>& counter) : _counter(counter) {}
   ~delivery_completed_guard() { _counter.fetch_add(1, std::memory_order_acq_rel); }
   std::atomic<uint64_t>& _counter;
};

} // anonymous namespace

fc::sink::es_sink_config es_sink_mt::validate(fc::sink::es_sink_config cfg) {
   while (!cfg.url.empty() && cfg.url.back() == '/')
      cfg.url.pop_back();
   FC_ASSERT(!cfg.url.empty(), "es_sink: url is required");
   const fc::url parsed{cfg.url};
   FC_ASSERT(parsed.proto() == es_scheme_http || parsed.proto() == es_scheme_https,
             "es_sink: url scheme must be http or https, got '{}'", parsed.proto());
   FC_ASSERT(!cfg.index.empty(), "es_sink: index is required");
   FC_ASSERT(cfg.batch_size > 0, "es_sink: batch_size must be greater than zero");
   FC_ASSERT(cfg.max_batch_bytes > 0 && cfg.max_batch_bytes <= fc::sink::es_max_batch_bytes_ceiling,
             "es_sink: max_batch_bytes must be in (0, {}]", fc::sink::es_max_batch_bytes_ceiling);
   FC_ASSERT(cfg.max_doc_bytes > 0 && cfg.max_doc_bytes <= cfg.max_batch_bytes,
             "es_sink: max_doc_bytes must be in (0, max_batch_bytes]");
   FC_ASSERT(cfg.flush_interval_ms > 0, "es_sink: flush_interval_ms must be greater than zero");
   FC_ASSERT(cfg.max_pending_batches > 0, "es_sink: max_pending_batches must be greater than zero");
   FC_ASSERT(cfg.username.has_value() == cfg.password.has_value(),
             "es_sink: username and password must be provided together");
   return cfg;
}

es_sink_mt::es_sink_mt(fc::sink::es_sink_config cfg)
   : spdlog::sinks::base_sink<std::mutex>(std::make_unique<fc::log::json_formatter>(
        std::map<std::string, std::string>{}, std::string{fc::log::es_default_layout}))
   , _cfg(validate(std::move(cfg))) {
   spdlog::memory_buf_t action;
   log::detail::append_sv(action, R"({"index":{"_index":")");
   json_escape_into(action, _cfg.index);
   log::detail::append_sv(action, "\"}}\n");
   _action_line.assign(action.data(), action.size());

   _bulk_url = fc::url{_cfg.url + std::string{es_bulk_path}};
   if (_cfg.username)
      _auth_header = "Basic " + fc::base64_encode(*_cfg.username + ":" + *_cfg.password);
   _transport = std::make_unique<http::transport>();

   // Thread-bearing members are created LAST, inside a guard: a throw after the queue
   // exists (e.g. std::system_error from std::thread) would otherwise skip the
   // destructor while the queue's worker still holds a raw `this`.
   try {
      _queue = parallel::worker_task_queue<batch>::create(
         {.max_threads = 1, .max_pending_items = _cfg.max_pending_batches},
         [this](batch& delivery) { deliver(delivery); });
      _timer_thread = std::thread([this] { timer_loop(); });
   } catch (...) {
      _cancel_requested.store(true, std::memory_order_relaxed);
      if (_queue) {
         _queue->discard_pending();
         _queue->stop();
      }
      throw;
   }
}

es_sink_mt::~es_sink_mt() {
   // 1. Stop the interval timer; it re-checks the flag before every tick.
   {
      std::lock_guard<std::mutex> lk(_timer_mtx);
      _shutting_down = true;
   }
   _timer_cv.notify_all();
   if (_timer_thread.joinable())
      _timer_thread.join();

   // 2. Enqueue the tail batch. No logger references this sink anymore (destruction
   //    runs at refcount zero on the reconfigure path), but keep the lock invariant.
   {
      std::lock_guard<std::mutex> lk(mutex_);
      if (_pending.doc_count > 0)
         enqueue_pending_locked();
   }

   // 3. Graceful drain, bounded by shutdown_flush_timeout_ms. The enqueued/completed
   //    counter pair is essential: queue size() drops to zero the instant the worker
   //    POPS the tail -- before it is sent -- so a size-based predicate would cancel
   //    the very batch this drain exists to save.
   const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(_cfg.shutdown_flush_timeout_ms);
   while (std::chrono::steady_clock::now() < deadline) {
      if (_batches_completed.load(std::memory_order_acquire) == _batches_enqueued.load(std::memory_order_acquire))
         break;
      std::this_thread::sleep_for(es_drain_poll);
   }

   // 4. Hard-stop anything still in flight: the cancel_check poll aborts an active
   //    perform(); the backoff wait's predicate aborts a retry sleep.
   _cancel_requested.store(true, std::memory_order_relaxed);
   _timer_cv.notify_all();

   // 5. Account then discard whatever the drain window did not deliver (stop()
   //    discards silently), then join the worker.
   _dropped_batches.fetch_add(_queue->discard_pending(), std::memory_order_relaxed);
   _queue->stop();

   // 6. Final report + transport teardown.
   emit_pending_warnings();
   _transport.reset();
}

void es_sink_mt::sink_it_(const spdlog::details::log_msg& msg) {
   spdlog::memory_buf_t formatted;
   formatter_->format(msg, formatted);
   if (formatted.size() > _cfg.max_doc_bytes) {
      // Truncating mid-JSON would corrupt the document; drop it whole and count.
      _dropped_docs.fetch_add(1, std::memory_order_relaxed);
      _warn_events.fetch_add(1, std::memory_order_relaxed);
      return;
   }
   const std::size_t incoming = _action_line.size() + formatted.size();
   // Ship the current batch first when appending would exceed the byte cap, so every
   // request body stays <= max_batch_bytes (+ at most one max_doc_bytes document).
   if (_pending.doc_count > 0 && _pending.body.size() + incoming > _cfg.max_batch_bytes)
      enqueue_pending_locked();
   _pending.body.append(_action_line);
   _pending.body.append(formatted.data(), formatted.size());
   ++_pending.doc_count;
   if (_pending.doc_count >= _cfg.batch_size || _pending.body.size() >= _cfg.max_batch_bytes)
      enqueue_pending_locked();
}

void es_sink_mt::flush_() {
   // Intentionally empty -- see the header doc: flush_on(info) fires this per record
   // on the logging thread with mutex_ held; delivery cadence lives elsewhere.
}

void es_sink_mt::enqueue_pending_locked() {
   if (_queue->try_push(std::move(_pending))) {
      _batches_enqueued.fetch_add(1, std::memory_order_acq_rel);
   } else {
      _dropped_batches.fetch_add(1, std::memory_order_relaxed);
      _warn_events.fetch_add(1, std::memory_order_relaxed);
   }
   _pending = {};
}

void es_sink_mt::timer_loop() {
   std::unique_lock<std::mutex> lk(_timer_mtx);
   while (!_shutting_down) {
      _timer_cv.wait_for(lk, std::chrono::milliseconds(_cfg.flush_interval_ms));
      if (_shutting_down)
         return;
      lk.unlock();
      {
         std::lock_guard<std::mutex> pending_lk(mutex_);
         if (_pending.doc_count > 0)
            enqueue_pending_locked();
      }
      emit_pending_warnings();
      lk.lock();
   }
}

void es_sink_mt::deliver(batch& delivery) {
   const delivery_completed_guard completion{_batches_completed};
   const uint32_t                 doc_count = delivery.doc_count;

   http::request req;
   req.method       = http::request_method::post;
   req.target       = _bulk_url;
   req.body         = std::move(delivery.body);
   req.content_type = std::string{es_content_type};
   req.user_agent   = std::string{es_user_agent};
   if (_auth_header)
      req.headers.emplace_back("Authorization", *_auth_header);

   http::request_options opt;
   opt.max_request_body_bytes  = uint64_t{_cfg.max_batch_bytes} + _cfg.max_doc_bytes;
   opt.max_response_body_bytes = es_max_response_body_bytes;
   opt.timeouts.connect        = fc::milliseconds(_cfg.connect_timeout_ms);
   opt.timeouts.header = opt.timeouts.read = opt.timeouts.idle = opt.timeouts.total =
      fc::milliseconds(_cfg.request_timeout_ms);
   // Background worker: an ambient fc task deadline must not bound a log flush.
   opt.timeouts.inherit_task_deadline = false;
   opt.cancel_check = [this] { return _cancel_requested.load(std::memory_order_relaxed); };
   // Transport retry stays at one attempt (a _bulk POST is not idempotent); the sink
   // owns its retry loop below.

   for (uint32_t attempt = 0;; ++attempt) {
      if (_cancel_requested.load(std::memory_order_relaxed)) {
         _failed_batches.fetch_add(1, std::memory_order_relaxed);
         return;
      }
      bool retryable = false;
      try {
         const auto resp = _transport->perform(req, opt);
         if (resp.status >= http_status_ok_min && resp.status <= http_status_ok_max) {
            handle_bulk_response_body(resp.body, doc_count);
            return;
         }
         // 429 is endpoint back-pressure -- the one 4xx worth retrying; other 4xx are
         // terminal (retrying a rejected request only repeats the rejection).
         retryable = resp.status >= http_status_server_error_min || resp.status == http_status_too_many_requests;
         if (!retryable) {
            _failed_batches.fetch_add(1, std::memory_order_relaxed);
            note_warning(fmt::format("HTTP {} from {}", resp.status, http::sanitized_endpoint(_bulk_url)));
            return;
         }
         note_warning(fmt::format("HTTP {} from {} (attempt {})", resp.status,
                                  http::sanitized_endpoint(_bulk_url), attempt + 1));
      } catch (const fc::canceled_exception&) {
         // Shutdown abort via cancel_check.
         _failed_batches.fetch_add(1, std::memory_order_relaxed);
         return;
      } catch (const fc::timeout_exception& e) {
         retryable = true;
         note_warning(fmt::format("timeout delivering to {}: {}", http::sanitized_endpoint(_bulk_url),
                                  e.top_message()));
      } catch (const fc::exception& e) {
         // connect / DNS / TLS / io failures -- message-classified only at this layer.
         retryable = true;
         note_warning(fmt::format("delivery to {} failed: {}", http::sanitized_endpoint(_bulk_url),
                                  e.top_message()));
      }
      if (!retryable || attempt >= _cfg.max_retries) {
         _failed_batches.fetch_add(1, std::memory_order_relaxed);
         return;
      }
      // Capped exponential backoff, interruptible by shutdown's cancel request. The
      // predicate deliberately keys on _cancel_requested (set at drain-timeout), NOT
      // _shutting_down (set at teardown start) -- otherwise retries would spin at zero
      // backoff for the whole graceful-drain window.
      const auto backoff = std::min<std::chrono::milliseconds>(
         std::chrono::milliseconds(_cfg.retry_backoff_ms) * (1u << std::min(attempt, es_max_backoff_exponent)),
         es_max_backoff);
      std::unique_lock<std::mutex> lk(_timer_mtx);
      if (_timer_cv.wait_for(lk, backoff, [this] { return _cancel_requested.load(std::memory_order_relaxed); })) {
         _failed_batches.fetch_add(1, std::memory_order_relaxed);
         return;
      }
   }
}

void es_sink_mt::handle_bulk_response_body(const std::string& body, uint32_t doc_count) {
   const std::string_view probe = std::string_view{body}.substr(0, es_errors_probe_bytes);
   if (probe.find(es_errors_false_token) != std::string_view::npos) {
      _indexed_docs.fetch_add(doc_count, std::memory_order_relaxed);
      return;
   }
   // "errors":true, or neither token in the probe window (a proxy landing page, a
   // response with unexpected whitespace, ...) -- parse to find out. Never retry after
   // a 2xx: a partial success replayed duplicates the documents that DID index.
   try {
      const auto parsed = fc::json::from_string(body).get_object();
      if (!parsed.contains("errors")) {
         _failed_batches.fetch_add(1, std::memory_order_relaxed);
         note_warning(fmt::format("2xx from {} is not a bulk response", http::sanitized_endpoint(_bulk_url)));
         return;
      }
      if (!parsed["errors"].as_bool()) {
         _indexed_docs.fetch_add(doc_count, std::memory_order_relaxed);
         return;
      }
      uint64_t    failed_docs = 0;
      std::string first_reason;
      if (parsed.contains("items")) {
         for (const auto& item : parsed["items"].get_array()) {
            const auto& item_obj = item.get_object();
            for (const auto& entry : item_obj) {
               const auto& result = entry.value().get_object();
               if (!result.contains("error"))
                  continue;
               ++failed_docs;
               if (first_reason.empty())
                  first_reason = fc::json::to_string(result["error"], fc::time_point::maximum());
            }
         }
      }
      failed_docs = std::min<uint64_t>(failed_docs, doc_count);
      _indexed_docs.fetch_add(doc_count - failed_docs, std::memory_order_relaxed);
      _failed_batches.fetch_add(1, std::memory_order_relaxed);
      note_warning(fmt::format("{} of {} documents rejected by {}; first error: {}", failed_docs, doc_count,
                               http::sanitized_endpoint(_bulk_url), first_reason));
   } catch (const fc::exception&) {
      _failed_batches.fetch_add(1, std::memory_order_relaxed);
      note_warning(fmt::format("unparseable 2xx response from {}", http::sanitized_endpoint(_bulk_url)));
   }
}

void es_sink_mt::note_warning(std::string detail) {
   _warn_events.fetch_add(1, std::memory_order_relaxed);
   std::lock_guard<std::mutex> lk(_timer_mtx);
   _warn_detail = std::move(detail);
}

void es_sink_mt::emit_pending_warnings() {
   if (_warn_events.load(std::memory_order_relaxed) == 0)
      return;
   const auto now_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
         .count();
   auto last = _last_warn_ns.load(std::memory_order_relaxed);
   if (now_ns - last < std::chrono::duration_cast<std::chrono::nanoseconds>(es_warn_interval).count())
      return;
   if (!_last_warn_ns.compare_exchange_strong(last, now_ns, std::memory_order_relaxed))
      return;
   const auto  events = _warn_events.exchange(0, std::memory_order_relaxed);
   std::string detail;
   {
      std::lock_guard<std::mutex> lk(_timer_mtx);
      detail = _warn_detail;
   }
   // std::cerr on purpose -- NEVER fc loggers from sink internals (recursion; see the
   // class doc). Matches the logger_config.cpp warning convention.
   std::cerr << "\nWARNING: es_sink: " << events << " delivery warning(s) since last report"
             << " (dropped_docs=" << _dropped_docs.load(std::memory_order_relaxed)
             << " dropped_batches=" << _dropped_batches.load(std::memory_order_relaxed)
             << " failed_batches=" << _failed_batches.load(std::memory_order_relaxed)
             << " indexed_docs=" << _indexed_docs.load(std::memory_order_relaxed) << ")"
             << (detail.empty() ? "" : " -- last: ") << detail << std::endl;
}

bool es_sink_mt::wait_until_idle(std::chrono::milliseconds timeout) {
   const auto deadline = std::chrono::steady_clock::now() + timeout;
   while (true) {
      bool pending_empty = false;
      {
         std::lock_guard<std::mutex> lk(mutex_);
         pending_empty = _pending.doc_count == 0;
      }
      if (pending_empty &&
          _batches_completed.load(std::memory_order_acquire) == _batches_enqueued.load(std::memory_order_acquire))
         return true;
      if (std::chrono::steady_clock::now() >= deadline)
         return false;
      std::this_thread::sleep_for(es_drain_poll);
   }
}

void es_sink_mt::assert_formatter_renders_json() {
   spdlog::memory_buf_t rendered;
   {
      std::lock_guard<std::mutex> lk(mutex_);
      const spdlog::details::log_msg sample{
         spdlog::source_loc{sample_source_file.data(), sample_source_line, sample_source_file.data()},
         spdlog::string_view_t{sample_logger_name.data(), sample_logger_name.size()}, spdlog::level::info,
         spdlog::string_view_t{sample_payload.data(), sample_payload.size()}};
      formatter_->format(sample, rendered);
   }
   const std::string_view line{rendered.data(), rendered.size()};
   FC_ASSERT(!line.empty() && line.back() == '\n' && line.find('\n') == line.size() - 1,
             "es_sink: formatter must render exactly one newline-terminated line per record");
   try {
      const auto parsed = fc::json::from_string(std::string{line.substr(0, line.size() - 1)});
      FC_ASSERT(parsed.is_object(), "es_sink: formatter output is not a JSON object");
   } catch (const fc::exception& e) {
      FC_THROW_EXCEPTION(assert_exception,
                         "es_sink: formatter output is not valid one-line JSON (bulk NDJSON requires it): {}",
                         e.top_message());
   }
}

} // namespace fc

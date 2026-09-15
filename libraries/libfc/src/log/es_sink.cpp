#include <fc/exception/exception.hpp>
#include <fc/io/json.hpp>
#include <fc/log/es_sink.hpp>
#include <fc/log/json_formatter.hpp>
#include <iostream>

namespace fc {

namespace {

constexpr auto es_warn_interval = std::chrono::seconds(5);
constexpr auto es_drain_poll = std::chrono::milliseconds(25);

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

/// The delivery half of an es_sink_config, as the shared client consumes it.
fc::network::es::es_client_options to_es_client_options(const fc::sink::es_sink_config& cfg) {
   fc::network::es::es_client_options delivery;
   delivery.url = cfg.url;
   delivery.index = cfg.index;
   delivery.username = cfg.username;
   delivery.password = cfg.password;
   delivery.max_batch_bytes = cfg.max_batch_bytes;
   delivery.max_doc_bytes = cfg.max_doc_bytes;
   delivery.max_retries = cfg.max_retries;
   delivery.retry_backoff_ms = cfg.retry_backoff_ms;
   delivery.connect_timeout_ms = cfg.connect_timeout_ms;
   delivery.request_timeout_ms = cfg.request_timeout_ms;
   return delivery;
}

} // anonymous namespace

fc::sink::es_sink_config es_sink_mt::validate(fc::sink::es_sink_config cfg) {
   // Endpoint, byte-cap, and auth invariants are the client's; the sink adds its batching invariants and
   // keeps the client's normalized url.
   cfg.url = fc::network::es::es_client::validate(to_es_client_options(cfg)).url;
   FC_ASSERT(cfg.batch_size > 0, "es_sink: batch_size must be greater than zero");
   FC_ASSERT(cfg.flush_interval_ms > 0, "es_sink: flush_interval_ms must be greater than zero");
   FC_ASSERT(cfg.max_pending_batches > 0, "es_sink: max_pending_batches must be greater than zero");
   return cfg;
}

es_sink_mt::es_sink_mt(fc::sink::es_sink_config cfg)
   : spdlog::sinks::base_sink<std::mutex>(std::make_unique<fc::log::json_formatter>(
        std::map<std::string, std::string>{}, std::string{fc::log::es_default_layout}))
   , _cfg(validate(std::move(cfg))) {
   _client = std::make_unique<fc::network::es::es_client>(to_es_client_options(_cfg));

   // The es client starts its own io thread, but its destructor cancels and joins it during unwinding. The
   // queue and the timer thread need the guard: their workers hold a raw `this`, and a throw after the queue
   // exists (e.g. std::system_error from std::thread) would otherwise skip the destructor.
   try {
      _queue = parallel::worker_task_queue<batch>::create(
         {.max_threads = 1, .max_pending_items = _cfg.max_pending_batches},
         [this](batch& delivery) { deliver(delivery); });
      _timer_thread = std::thread([this] { timer_loop(); });
   } catch (...) {
      _client->cancel();
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

   // 4. Hard-stop anything still in flight: the client's cancellation signal aborts an active request and
   //    its backoff wait on the client's io thread; the timer is already stopped, so nothing else needs waking.
   _client->cancel();

   // 5. Account then discard whatever the drain window did not deliver (stop()
   //    discards silently), then join the worker.
   _dropped_batches.fetch_add(_queue->discard_pending(), std::memory_order_relaxed);
   _queue->stop();

   // 6. Final report + client teardown.
   emit_pending_warnings();
   _client.reset();
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
   const std::size_t incoming = _client->action_line().size() + formatted.size();
   // Ship the current batch first when appending would exceed the byte cap, so every
   // request body stays <= max_batch_bytes (+ at most one max_doc_bytes document).
   if (_pending.doc_count > 0 && _pending.body.size() + incoming > _cfg.max_batch_bytes)
      enqueue_pending_locked();
   _pending.body.append(_client->action_line());
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
   const auto result = _client->bulk(std::move(delivery.body), delivery.doc_count);
   _indexed_docs.fetch_add(result.indexed_docs, std::memory_order_relaxed);
   if (result.outcome == fc::network::es::es_bulk_result::status::indexed)
      return;
   // partial, rejected, unavailable: the batch counts as failed (partial also credited the indexed subset
   // above); canceled is shutdown's doing and warrants no warning.
   _failed_batches.fetch_add(1, std::memory_order_relaxed);
   if (result.outcome != fc::network::es::es_bulk_result::status::canceled)
      note_warning(result.detail);
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

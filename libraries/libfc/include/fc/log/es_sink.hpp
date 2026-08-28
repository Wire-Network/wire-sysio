#pragma once
#include <fc/spdlog.hpp>
#include <spdlog/sinks/base_sink.h>

#include <fc/log/logger_config.hpp>
#include <fc/network/http/http_client.hpp>
#include <fc/network/url.hpp>
#include <fc/parallel/worker_task_queue.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace fc {

/// spdlog sink that batches formatted document lines and ships them to an
/// OpenSearch/Elasticsearch _bulk endpoint from a dedicated worker thread.
///
/// The logging thread never performs network I/O: sink_it_ formats the record and
/// appends it to an in-memory batch; a bounded single-worker queue delivers batches
/// (size-, byte-, and interval-triggered) with retry/backoff on the worker thread.
/// The ctor installs fc::log::json_formatter with the fc::log::es_default_layout
/// template as the default formatter (dmlog_sink precedent -- configure_logging must
/// not overwrite it with the default pattern); an explicit json "format" block
/// overrides it, which is the supported way to stamp identity extra_fields or reshape
/// documents. Each formatter output line is used verbatim as one bulk document source
/// line; the sink owns the action line built from its `index` argument.
///
/// Lock order: _timer_mtx -> base_sink::mutex_ -> worker_task_queue internals.
///  - logging threads: mutex_ -> queue (try_push); never _timer_mtx
///  - timer thread:    _timer_mtx, then mutex_ -> queue on each tick
///  - worker thread:   queue alone (pop) or _timer_mtx alone (backoff / warn detail);
///                     NEVER mutex_, so a slow endpoint can never stall logging
///  - destructor:      each lock briefly; no lock held across a join
///
/// Failure diagnostics NEVER go through fc loggers (this sink may be attached to the
/// "default" logger -- an fc log call from sink internals would recurse into
/// sink_it_). The hot path only increments counters; the timer thread emits one
/// rate-limited std::cerr summary line.
class es_sink_mt : public spdlog::sinks::base_sink<std::mutex> {
public:
   /// Validates the config (FC_ASSERT -> configure_logging returns false on a bad
   /// value) and starts the delivery worker + interval-flush timer. No network
   /// connection is opened until the first batch is delivered.
   explicit es_sink_mt(fc::sink::es_sink_config cfg);
   ~es_sink_mt() override;

   // --- observability / test support (safe from any thread) ---

   /// Batches rejected by the full delivery queue, or discarded at shutdown.
   uint64_t dropped_batches() const noexcept { return _dropped_batches.load(std::memory_order_relaxed); }
   /// Single documents whose formatted size exceeded max_doc_bytes.
   uint64_t dropped_docs() const noexcept { return _dropped_docs.load(std::memory_order_relaxed); }
   /// Batches that exhausted retries, got a terminal 4xx, or a non-bulk 2xx response;
   /// also counted (alongside partial indexed_docs credit) on 2xx-with-item-errors.
   uint64_t failed_batches() const noexcept { return _failed_batches.load(std::memory_order_relaxed); }
   /// Documents positively acknowledged by the endpoint ("errors":false, or the
   /// non-erroring subset of a partial-failure response).
   uint64_t indexed_docs() const noexcept { return _indexed_docs.load(std::memory_order_relaxed); }

   /// Block until the pending batch is empty AND every enqueued batch has completed
   /// delivery -- the same predicate the destructor's graceful drain uses -- or until
   /// @p timeout elapses. Returns whether the sink went idle. (Enqueued/completed
   /// counters, not queue size: size() drops the moment the worker POPS a batch,
   /// before it is delivered, so a size-based predicate has an idle-looking window.)
   bool wait_until_idle(std::chrono::milliseconds timeout);

   /// Format a synthetic sample record through the CURRENT formatter and FC_ASSERT the
   /// output is a single line parsing as a JSON object -- the bulk-NDJSON requirement.
   /// configure_logging calls this after formatter attachment so a broken or non-JSON
   /// layout template fails configuration loudly instead of silently 400-ing every
   /// batch; any custom template that renders valid one-line JSON passes.
   void assert_formatter_renders_json();

protected:
   void sink_it_(const spdlog::details::log_msg& msg) override;
   /// Intentionally a no-op: every configured logger runs flush_on(info), firing
   /// flush_() synchronously after each info+ record with mutex_ held. Delivery
   /// cadence is owned by the size/byte thresholds and the interval timer.
   void flush_() override;

private:
   /// One assembled _bulk request body (action/document line pairs) plus its count.
   struct batch {
      std::string body;
      uint32_t    doc_count = 0;
   };

   /// FC_ASSERTs endpoint/batching invariants and strips a trailing '/' from url.
   static fc::sink::es_sink_config validate(fc::sink::es_sink_config cfg);

   /// Move the pending batch onto the delivery queue. Requires mutex_ held. Counters
   /// only on the drop path -- no I/O, no stream writes.
   void enqueue_pending_locked();
   /// Deliver one batch (worker thread only; never takes mutex_).
   void deliver(batch& delivery);
   /// Interval flush + rate-limited warning reporter (dedicated timer thread).
   void timer_loop();
   /// Account a 2xx bulk response: positive-signal probe for "errors":false /
   /// "errors":true, full parse on the uncertain paths (worker thread only).
   void handle_bulk_response_body(const std::string& body, uint32_t doc_count);
   /// Record a warning event with detail text for the timer thread's next report.
   void note_warning(std::string detail);
   /// Emit at most one std::cerr summary per warn interval (timer thread + dtor only).
   void emit_pending_warnings();

   const fc::sink::es_sink_config _cfg;         ///< validated configuration
   std::string                _action_line; ///< {"index":{"_index":"<escaped>"}}\n, built once
   fc::url                    _bulk_url;    ///< <url>/_bulk
   std::optional<std::string> _auth_header; ///< "Basic <base64(user:pass)>" when configured

   batch _pending; ///< guarded by base_sink::mutex_

   std::shared_ptr<parallel::worker_task_queue<batch>> _queue;
   std::unique_ptr<http::transport>                    _transport; ///< worker thread only

   std::thread             _timer_thread;
   std::mutex              _timer_mtx;
   std::condition_variable _timer_cv;
   bool                    _shutting_down = false; ///< guarded by _timer_mtx; stops the TIMER only
   std::string             _warn_detail;           ///< guarded by _timer_mtx; latest warning detail
   std::atomic<bool>       _cancel_requested{false}; ///< aborts in-flight HTTP + retry backoff
   std::atomic<uint64_t>   _batches_enqueued{0};     ///< successful try_push count (gap-free vs completed)
   std::atomic<uint64_t>   _batches_completed{0};    ///< deliver() exits (any outcome)

   std::atomic<uint64_t> _dropped_batches{0};
   std::atomic<uint64_t> _dropped_docs{0};
   std::atomic<uint64_t> _failed_batches{0};
   std::atomic<uint64_t> _indexed_docs{0};
   std::atomic<uint64_t> _warn_events{0};   ///< events since the last report
   std::atomic<int64_t>  _last_warn_ns{0};  ///< steady_clock ns of the last stderr report
};

} // namespace fc

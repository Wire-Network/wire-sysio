#pragma once

#include <cstddef>
#include <cstdint>
#include <fc/exception/exception.hpp>
#include <fc/parallel/detail/task_queue_base.hpp>
#include <functional>
#include <memory>
#include <optional>
#include <queue>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace fc::parallel {

/// Construction-time settings for batch_task_queue. Every field has a usable default; create() rejects a
/// max_threads or a max_items_per_task of zero.
struct batch_task_queue_config {
   /// Worker threads in the pool. With one thread batches arrive in queue order; with more, they may
   /// interleave. Must be greater than zero.
   uint64_t max_threads = 1;
   /// Upper bound on the items handed to one callback invocation. The worker never waits for a full batch:
   /// it drains whatever is queued when it wakes, up to this many. Must be greater than zero.
   std::size_t max_items_per_task = 1;
   /// When set, create() leaves the workers unstarted and the owner calls start() itself.
   bool skip_autostart = false;
   /// Maximum waiting items, excluding a batch already handed to a callback. Null means unbounded.
   std::optional<std::size_t> max_pending_items;
};

/// Name this queue reports in the configuration asserts of its create().
inline constexpr std::string_view batch_task_queue_name = "batch_task_queue";

/// Thread-pool-backed work queue that delivers items of type T to a callback in BATCHES: each invocation
/// receives a std::span<T> over the items drained from the queue (at most max_items_per_task, in queue
/// order). The span views the worker's own buffer -- the callback may move items out of it, and every
/// item is destroyed when the callback returns. With one thread batches arrive in queue order; with more,
/// batches may interleave. The callback must not throw: it runs unguarded on a boost::asio::thread_pool
/// thread (as worker_task_queue's does), where an escaping exception terminates the process.
///
/// The queue storage, admission and the start/stop lifecycle contract live in
/// fc::parallel::detail::task_queue_base; this class adds the batching callback and its worker loop.
///
/// Managed exclusively via shared_ptr -- use the static create() factory. Non-copyable, non-movable.
///
/// Example:
///   auto q = batch_task_queue<document>::create(
///      {.max_items_per_task = 100, .max_pending_items = 256},
///      [](std::span<document> batch) { ship(batch); });
///   q->try_push(doc);
///   q->stop();
template <typename T>
class batch_task_queue : public detail::task_queue_base<T, batch_task_queue<T>, batch_task_queue_config> {
   using base_t = detail::task_queue_base<T, batch_task_queue<T>, batch_task_queue_config>;
   /// The base posts one worker_loop() call per pool thread.
   friend base_t;

public:
   /// Receives each drained batch; the span views the worker's buffer and is valid only during the call.
   using callback_t = std::function<void(std::span<T>)>;

   /// Factory -- the only way to obtain an instance. FC_ASSERTs max_items_per_task > 0, and the base
   /// FC_ASSERTs max_threads > 0.
   /// @throw fc::assert_exception when either bound is zero.
   static std::shared_ptr<batch_task_queue> create(batch_task_queue_config config, callback_t cb) {
      FC_ASSERT(config.max_items_per_task > 0, "{}: max_items_per_task must be greater than zero",
                batch_task_queue_name);
      return base_t::start_if_configured(
         std::shared_ptr<batch_task_queue>(new batch_task_queue(std::move(config), std::move(cb))),
         batch_task_queue_name);
   }

private:
   batch_task_queue(batch_task_queue_config config, callback_t cb)
      : base_t(std::move(config))
      , _callback(std::move(cb)) {}

   /// Drain up to max_items_per_task items per callback invocation until stop() clears the running flag. The
   /// items are taken under the queue mutex and the callback runs after it is released, so a slow callback
   /// blocks only the worker running it.
   void worker_loop() {
      // Grows on demand, never reserved up front: max_items_per_task is operator-controlled, and a
      // reservation sized from it would be an allocation nothing guards. clear() keeps the capacity, so the
      // vector reallocates only while the first few batches establish its high-water mark.
      std::vector<T> batch;
      while (true) {
         const bool taken = this->wait_and_drain([this, &batch](std::queue<T>& queue) {
            while (!queue.empty() && batch.size() < this->_config.max_items_per_task) {
               batch.push_back(std::move(queue.front()));
               queue.pop();
            }
         });
         if (!taken)
            return;
         _callback(std::span<T>{batch});
         batch.clear();
      }
   }

   /// Receives each drained batch; runs on a pool thread and must not throw.
   callback_t _callback;
};

} // namespace fc::parallel

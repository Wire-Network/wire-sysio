#pragma once

#include <cstddef>
#include <cstdint>
#include <fc/parallel/detail/task_queue_base.hpp>
#include <functional>
#include <memory>
#include <optional>
#include <queue>
#include <string_view>
#include <utility>

namespace fc::parallel {

struct worker_task_queue_config {
   bool     dynamic_size   = false;
   bool     reuse_thread   = true;
   uint64_t max_threads    = 1;
   bool     prune_threads  = false;
   bool     skip_autostart = false;
   /// Maximum waiting items, excluding callbacks already in progress. Null means unbounded.
   std::optional<std::size_t> max_pending_items;
};

/// Name this queue reports in the configuration assert of its create().
inline constexpr std::string_view worker_task_queue_name = "worker_task_queue";

/// Thread-pool-backed work queue that delivers items of type T to a callback.
///
/// The queue storage, admission and the start/stop lifecycle contract live in
/// fc::parallel::detail::task_queue_base; this class adds the single-item callback and its worker loop.
///
/// Managed exclusively via shared_ptr — use the static create() factory.
/// Non-copyable, non-movable.
///
/// Example:
///   auto q = worker_task_queue<MyEvent>::create(
///      {.max_threads = 4},
///      [](MyEvent& e) { process(e); });
///   q->push(event);
///   q->stop();
template <typename T>
class worker_task_queue : public detail::task_queue_base<T, worker_task_queue<T>, worker_task_queue_config> {
   using base_t = detail::task_queue_base<T, worker_task_queue<T>, worker_task_queue_config>;
   /// The base posts one worker_loop() call per pool thread.
   friend base_t;

public:
   /// The one-item callback type: the item is delivered by reference, outside the queue mutex.
   using callback_t = std::function<void(T&)>;

   /// Factory — the only way to obtain an instance.
   /// @throw fc::assert_exception when max_threads is zero.
   static std::shared_ptr<worker_task_queue> create(worker_task_queue_config config, callback_t cb) {
      return base_t::start_if_configured(
         std::shared_ptr<worker_task_queue>(new worker_task_queue(std::move(config), std::move(cb))),
         worker_task_queue_name);
   }

   /// Alias for stop().
   void destroy() { this->stop(); }

private:
   worker_task_queue(worker_task_queue_config config, callback_t cb)
      : base_t(std::move(config))
      , _callback(std::move(cb)) {}

   /// Deliver one item per callback invocation until stop() clears the running flag. The item is taken
   /// under the queue mutex and the callback runs after it is released, so a slow callback blocks only the
   /// worker running it.
   void worker_loop() {
      while (true) {
         std::optional<T> item;
         const bool taken = this->wait_and_drain([&item](std::queue<T>& queue) {
            item.emplace(std::move(queue.front()));
            queue.pop();
         });
         if (!taken)
            return;
         _callback(*item);
      }
   }

   /// Receives each dequeued item; runs on a pool thread and must not throw.
   callback_t _callback;
};

} // namespace fc::parallel

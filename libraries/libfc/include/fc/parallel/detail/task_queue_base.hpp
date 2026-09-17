#pragma once

#include <boost/asio/post.hpp>
#include <boost/asio/thread_pool.hpp>
#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <fc/exception/exception.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string_view>
#include <utility>

namespace fc::parallel::detail {

/// Shared storage, admission protocol and lifecycle for the thread-pool-backed work queues in fc::parallel.
///
/// The base owns the item queue, the pool, the synchronization state and every start/stop/push operation; a
/// concrete queue supplies only its callback type, its create() factory and its worker_loop(), which reaches
/// the queue exclusively through wait_and_drain(). Template parameters:
///   - T       the queued item type.
///   - Derived the concrete queue, as a curiously-recurring template parameter. It declares worker_loop()
///             non-public and befriends this base, which posts one worker_loop() call per thread.
///   - Config  the concrete queue's construction-time settings. The requires-clause pins the three members
///             this base reads; anything else on it belongs to the concrete queue alone.
///
/// Lifecycle contract, identical for every concrete queue:
///   - The owner must call stop() before releasing its last shared_ptr: every worker holds a shared_ptr to
///     the queue, so dropping the owner's reference alone neither destroys the queue nor joins its threads.
///   - stop() joins the pool, so it must never be called from the callback.
///   - The callback runs unguarded on a pool thread and must not throw; an escaping exception terminates the
///     process.
///   - start() and stop() are lifecycle calls for one owning thread; they are not synchronized against each
///     other. Two concurrent stop() calls are outside the contract for the same reason: the loser returns as
///     soon as it observes the cleared running flag, which is before the winner's join has completed.
///   - A stopped queue stays stopped: start() afterwards is a no-op and no further item is admitted.
///
/// Managed exclusively via shared_ptr -- each concrete queue exposes a static create() factory that routes
/// through start_if_configured(). Non-copyable, non-movable.
template <typename T, typename Derived, typename Config>
   requires requires(const Config& config) {
      { config.max_threads } -> std::convertible_to<std::size_t>;
      { config.skip_autostart } -> std::convertible_to<bool>;
      { static_cast<bool>(config.max_pending_items) } -> std::same_as<bool>;
      { *config.max_pending_items } -> std::convertible_to<std::size_t>;
   }
class task_queue_base : public std::enable_shared_from_this<Derived> {
public:
   task_queue_base(const task_queue_base&) = delete;
   task_queue_base& operator=(const task_queue_base&) = delete;
   task_queue_base(task_queue_base&&) = delete;
   task_queue_base& operator=(task_queue_base&&) = delete;

   /// Enqueue an item. No-op if the queue has been stopped or is at capacity.
   void push(const T& item) { (void)try_push(item); }

   /// Enqueue an item (move). No-op if the queue has been stopped or is at capacity.
   void push(T&& item) { (void)try_push(std::move(item)); }

   /// Attempt to enqueue a copied item.
   /// @return True when admitted; false when stopped or at the pending-item limit.
   bool try_push(const T& item) { return try_push_impl(item); }

   /// Attempt to enqueue a moved item.
   /// @return True when admitted; false when stopped or at the pending-item limit.
   bool try_push(T&& item) { return try_push_impl(std::move(item)); }

   /// Start the thread pool and worker loops. Called automatically by the concrete create() unless
   /// skip_autostart is set; a no-op while running and after stop().
   /// @throw Anything the pool or a post throws; the queue is then left stopped and admits no further item.
   void start() {
      {
         std::lock_guard<std::mutex> lock(_mtx);
         if (_running || _stopped)
            return;
         _running = true;
      }
      // The pool and every post allocate. Without this guard a throw part-way through the loop would leave
      // _running set and the workers posted so far waiting on a queue that nothing joins.
      try {
         _pool.emplace(static_cast<std::size_t>(_config.max_threads));
         for (uint64_t i = 0; i < _config.max_threads; ++i) {
            boost::asio::post(*_pool, [self = this->shared_from_this()]() { self->worker_loop(); });
         }
      } catch (...) {
         stop();
         throw;
      }
   }

   /// Mark the queue as stopped and join all worker threads. Items still queued are left in place (see
   /// discard_pending()); work already handed to a callback completes.
   void stop() {
      {
         std::lock_guard<std::mutex> lock(_mtx);
         _stopped = true;
         if (!_running)
            return;
         _running = false;
      }
      _cv.notify_all();
      if (_pool) {
         _pool->join();
         _pool.reset();
      }
   }

   /// True while the workers are running: after start() and before stop(). False for a never-started queue.
   bool running() const {
      std::lock_guard<std::mutex> lock(_mtx);
      return _running;
   }

   /// Number of items waiting in the queue, excluding work already handed to a callback.
   std::size_t size() const {
      std::lock_guard<std::mutex> lock(_mtx);
      return _queue.size();
   }

   /// Discard every pending item and return how many were discarded; an active callback is unaffected.
   std::size_t discard_pending() {
      std::queue<T> discarded;
      {
         std::lock_guard<std::mutex> lock(_mtx);
         _queue.swap(discarded);
      }
      return discarded.size();
   }

protected:
   /// Take the concrete queue's settings; the concrete constructor stores its own callback.
   explicit task_queue_base(Config config)
      : _config(std::move(config)) {}

   /// Stops a never-started queue. A started queue reaches its destructor only after stop() has joined the
   /// workers, because they held the remaining references. Non-virtual and protected: the queues are owned
   /// through shared_ptr<Derived> and are never deleted through a base pointer.
   ~task_queue_base() { stop(); }

   /// Validate @p queue's thread count, start it unless its configuration opts out, then hand it back. This
   /// is the tail of every concrete create(), which differs from this only in the validation it performs
   /// first. @p queue_name labels the assert so a failure names the concrete queue.
   /// @throw fc::assert_exception when max_threads is zero.
   static std::shared_ptr<Derived> start_if_configured(std::shared_ptr<Derived> queue, std::string_view queue_name) {
      static_assert(std::derived_from<Derived, task_queue_base>,
                    "task_queue_base: Derived must be the concrete queue that derives from this base");
      // boost::asio::thread_pool accepts a count of zero and runs nothing, so an unchecked zero would leave a
      // queue that reports itself running while it accumulates every pushed item and delivers none.
      FC_ASSERT(queue->_config.max_threads > 0, "{}: max_threads must be greater than zero", queue_name);
      if (!queue->_config.skip_autostart)
         queue->start();
      return queue;
   }

   /// Wait for work or shutdown, then let @p drain take items with the queue mutex held.
   /// @return False once stop() has cleared the running flag; the worker then returns.
   template <typename Drain>
   bool wait_and_drain(Drain&& drain) {
      std::unique_lock<std::mutex> lock(_mtx);
      _cv.wait(lock, [this] { return !_queue.empty() || !_running; });
      if (!_running)
         return false;
      drain(_queue);
      return true;
   }

   /// Construction-time settings, read by this base and by the concrete worker loop.
   const Config _config;

private:
   /// Admit an item while holding the capacity check and queue mutation under the same lock.
   template <typename U>
   bool try_push_impl(U&& item) {
      {
         std::lock_guard<std::mutex> lock(_mtx);
         if (_stopped || (_config.max_pending_items && _queue.size() >= *_config.max_pending_items)) {
            return false;
         }
         _queue.push(std::forward<U>(item));
      }
      _cv.notify_one();
      return true;
   }

   /// Guards _queue, _running and _stopped, and is the wait mutex for _cv.
   mutable std::mutex _mtx;
   /// Signals an admitted item to a waiting worker, and the end of the run to every worker.
   std::condition_variable _cv;
   /// Items waiting for a worker, in push order.
   std::queue<T> _queue;
   /// True between start() and stop(); a worker returns as soon as it observes false.
   bool _running = false;
   /// Latched by stop(): a stopped queue neither restarts nor admits another item.
   bool _stopped = false;
   /// Holds the worker threads while running; reset by stop() once the join has returned.
   std::optional<boost::asio::thread_pool> _pool;
};

} // namespace fc::parallel::detail

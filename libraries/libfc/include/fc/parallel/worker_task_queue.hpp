#pragma once

#include <boost/asio/post.hpp>
#include <boost/asio/thread_pool.hpp>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>

namespace fc::parallel {

struct worker_task_queue_config {
   bool     dynamic_size   = false;
   bool     reuse_thread   = true;
   uint64_t max_threads    = 1;
   bool     prune_threads  = false;
   bool     skip_autostart = false;
};

/// Thread-pool-backed work queue that delivers items of type T to a callback.
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
class worker_task_queue : public std::enable_shared_from_this<worker_task_queue<T>> {
public:
   using callback_t = std::function<void(T&)>;

   /// Factory — the only way to obtain an instance.
   static std::shared_ptr<worker_task_queue> create(worker_task_queue_config config, callback_t cb) {
      auto ptr = std::shared_ptr<worker_task_queue>(new worker_task_queue(std::move(config), std::move(cb)));
      if (!ptr->_config.skip_autostart)
         ptr->start();
      return ptr;
   }

   worker_task_queue(const worker_task_queue&)            = delete;
   worker_task_queue& operator=(const worker_task_queue&) = delete;
   worker_task_queue(worker_task_queue&&)                 = delete;
   worker_task_queue& operator=(worker_task_queue&&)      = delete;

   ~worker_task_queue() { stop(); }

   /// Enqueue an item. No-op if the queue has been stopped.
   void push(const T& item) {
      {
         std::lock_guard<std::mutex> lock(_mtx);
         if (_stopped)
            return;
         _queue.push(item);
      }
      _cv.notify_one();
   }

   /// Enqueue an item (move). No-op if the queue has been stopped.
   void push(T&& item) {
      {
         std::lock_guard<std::mutex> lock(_mtx);
         if (_stopped)
            return;
         _queue.push(std::move(item));
      }
      _cv.notify_one();
   }

   /// Start the thread pool and worker loops.
   /// Called automatically by create() unless skip_autostart is set.
   void start() {
      {
         std::lock_guard<std::mutex> lock(_mtx);
         if (_running)
            return;
         _running = true;
      }
      _pool.emplace(static_cast<std::size_t>(_config.max_threads));
      for (uint64_t i = 0; i < _config.max_threads; ++i) {
         boost::asio::post(*_pool, [self = this->shared_from_this()]() { self->worker_loop(); });
      }
   }

   /// Mark the queue as stopped and join all worker threads.
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

   /// Alias for stop().
   void destroy() { stop(); }

   bool running() const {
      std::lock_guard<std::mutex> lock(_mtx);
      return _running;
   }

   std::size_t size() const {
      std::lock_guard<std::mutex> lock(_mtx);
      return _queue.size();
   }

private:
   worker_task_queue(worker_task_queue_config config, callback_t cb)
      : _config(std::move(config))
      , _callback(std::move(cb)) {}

   void worker_loop() {
      while (true) {
         std::optional<T> item;
         {
            std::unique_lock<std::mutex> lock(_mtx);
            _cv.wait(lock, [this] { return !_queue.empty() || !_running; });
            if (!_running)
               return;
            item.emplace(std::move(_queue.front()));
            _queue.pop();
         }
         _callback(*item);
      }
   }

   worker_task_queue_config                  _config;
   callback_t                                _callback;
   mutable std::mutex                        _mtx;
   std::condition_variable                   _cv;
   std::queue<T>                             _queue;
   bool                                      _running = false;
   bool                                      _stopped = false;
   std::optional<boost::asio::thread_pool>   _pool;
};

} // namespace fc::parallel

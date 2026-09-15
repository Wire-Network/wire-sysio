#include "_http_transport_internal.hpp"

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>

#include <algorithm>
#include <array>
#include <thread>

#include <fcntl.h>
#include <unistd.h>

namespace fc {
namespace http {

namespace {

/** Return the positive standard-library duration remaining before @p deadline. */
std::chrono::microseconds resolver_time_remaining(time_point deadline) {
   const auto now = time_point::now();
   if (deadline <= now) {
      throw transport_failure(failure_kind::timeout_connect, "DNS resolution deadline expired");
   }
   return std::chrono::microseconds((deadline - now).count());
}

class resolver_notifier;

/** Publish one resolver signal, retrying an interrupted descriptor write. */
template <typename WriteOnce>
bool write_resolver_signal(WriteOnce&& write_once) {
   for (;;) {
      const auto written = std::invoke(write_once);
      if (written == 1)
         return true;
      if (written < 0 && errno == EINTR)
         continue;
      return written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
   }
}

/** Create a non-blocking close-on-exec pipe without an inheritance race where supported. */
bool create_resolver_pipe(std::array<int, 2>& descriptors) {
#if defined(__linux__)
   return ::pipe2(descriptors.data(), O_CLOEXEC | O_NONBLOCK) == 0;
#else
   if (::pipe(descriptors.data()) != 0)
      return false;
   for (const auto descriptor : descriptors) {
      const auto status_flags = ::fcntl(descriptor, F_GETFL, 0);
      const auto descriptor_flags = ::fcntl(descriptor, F_GETFD, 0);
      if (status_flags < 0 || descriptor_flags < 0 || ::fcntl(descriptor, F_SETFL, status_flags | O_NONBLOCK) != 0 ||
          ::fcntl(descriptor, F_SETFD, descriptor_flags | FD_CLOEXEC) != 0) {
         return false;
      }
   }
   return true;
#endif
}

/**
 * One process-lifetime resolver service with its own platform lookup thread.
 *
 * Boost.Asio serializes getaddrinfo work within one execution_context even
 * when that context has several executor threads. Separate contexts preserve
 * the intended four-way lookup isolation.
 */
struct platform_resolver_worker {
   platform_resolver_worker()
      : context(std::make_shared<asio::io_context>())
      , work(asio::make_work_guard(*context)) {
      std::thread([context = context] { context->run(); }).detach();
   }

   std::shared_ptr<asio::io_context> context;
   asio::executor_work_guard<asio::io_context::executor_type> work;
};

/** Process-lifetime worker state with bounded concurrent platform DNS work. */
struct platform_resolver_runtime {
   platform_resolver_runtime() {
      for (auto& worker : workers)
         worker = std::make_unique<platform_resolver_worker>();
   }

   std::mutex mutex;
   std::array<std::unique_ptr<platform_resolver_worker>, platform_resolver_workers> workers;
   std::array<bool, platform_resolver_workers> active{};
   std::vector<std::weak_ptr<resolver_notifier>> waiters;

   /** Reserve one resolver worker without queueing behind a stalled lookup. */
   std::optional<size_t> try_acquire() {
      std::scoped_lock lock(mutex);
      const auto available = std::find(active.begin(), active.end(), false);
      if (available == active.end())
         return std::nullopt;
      *available = true;
      return static_cast<size_t>(std::distance(active.begin(), available));
   }

   /** Register an executor-independent waiter, closing the release-before-register race. */
   bool wait_for_capacity(const std::shared_ptr<resolver_notifier>& waiter);

   /** Release one worker and wake waiters without polling. */
   void release(size_t worker);

   /** Return the independent executor reserved for @p worker. */
   asio::any_io_executor executor(size_t worker) const {
      FC_ASSERT(worker < workers.size(), "Platform resolver worker index is invalid");
      return workers[worker]->context->get_executor();
   }
};

/** Internal signal that every platform resolver worker is currently occupied. */
class resolver_capacity_unavailable : public std::runtime_error {
public:
   resolver_capacity_unavailable()
      : std::runtime_error("platform resolver worker is busy") {}
};

/**
 * Executor-independent writer for one pipe notification.
 *
 * Resolver threads may retain this object after the client executor is gone.
 * It therefore owns only a pipe writer and plain atomic state.
 */
class resolver_notifier {
public:
   explicit resolver_notifier(int descriptor)
      : descriptor_number(descriptor) {}

   resolver_notifier(const resolver_notifier&) = delete;
   resolver_notifier& operator=(const resolver_notifier&) = delete;

   ~resolver_notifier() { disable(); }

   /** Wake the waiting coroutine once. */
   void notify() {
      std::scoped_lock lock(mutex);
      if (descriptor_number < 0 || notified.load(std::memory_order_acquire))
         return;
      constexpr uint8_t signal = 1;
      if (write_resolver_signal([&] { return ::write(descriptor_number, &signal, sizeof(signal)); })) {
         notified.store(true, std::memory_order_release);
      }
   }

   /** Prevent late background completions from writing after the reader closes. */
   void disable() {
      std::scoped_lock lock(mutex);
      if (descriptor_number < 0)
         return;
      ::close(descriptor_number);
      descriptor_number = -1;
   }

   /** Return whether this notifier has published its one signal. */
   bool was_notified() const { return notified.load(std::memory_order_acquire); }

private:
   mutable std::mutex mutex;
   int descriptor_number = -1;
   std::atomic_bool notified{false};
};

/** Executor-bound reader paired with an executor-independent notifier. */
class resolver_event {
public:
   explicit resolver_event(asio::any_io_executor executor)
      : event(std::move(executor)) {
      std::array<int, 2> descriptors{-1, -1};
      const bool configured = create_resolver_pipe(descriptors);
      if (!configured) {
         for (const auto descriptor : descriptors) {
            if (descriptor >= 0)
               ::close(descriptor);
         }
      }
      FC_ASSERT(configured, "Failed to create resolver notification pipe");
      notifier = std::make_shared<resolver_notifier>(descriptors[1]);

      boost::system::error_code error;
      event.assign(descriptors[0], error);
      if (error) {
         ::close(descriptors[0]);
         notifier.reset();
      }
      FC_ASSERT(!error, "Failed to attach resolver event: {}", error.message());
   }

   /** Disable the writer while the executor-bound read descriptor still exists. */
   ~resolver_event() {
      if (notifier)
         notifier->disable();
   }

   asio::posix::stream_descriptor event;
   std::shared_ptr<resolver_notifier> notifier;
};

bool platform_resolver_runtime::wait_for_capacity(const std::shared_ptr<resolver_notifier>& waiter) {
   bool notify_now = false;
   {
      std::scoped_lock lock(mutex);
      std::erase_if(waiters, [](const auto& candidate) { return candidate.expired(); });
      if (std::find(active.begin(), active.end(), false) != active.end())
         notify_now = true;
      else if (waiters.size() < max_resolver_capacity_waiters)
         waiters.emplace_back(waiter);
      else
         return false;
   }
   if (notify_now)
      waiter->notify();
   return true;
}

void platform_resolver_runtime::release(size_t worker) {
   std::vector<std::shared_ptr<resolver_notifier>> live_waiters;
   {
      std::scoped_lock lock(mutex);
      FC_ASSERT(worker < active.size() && active[worker], "Platform resolver permit underflow");
      active[worker] = false;
      live_waiters.reserve(waiters.size());
      for (auto& weak : waiters) {
         if (auto waiter = weak.lock())
            live_waiters.push_back(std::move(waiter));
      }
      waiters.clear();
   }
   for (const auto& waiter : live_waiters)
      waiter->notify();
}

/** Permit preventing unbounded work from queuing behind stalled platform lookups. */
class platform_resolver_permit {
public:
   /** Acquire one platform resolver slot without blocking an I/O executor. */
   explicit platform_resolver_permit(platform_resolver_runtime& runtime)
      : _runtime(runtime) {
      const auto worker = _runtime.try_acquire();
      if (!worker)
         throw resolver_capacity_unavailable();
      _worker = *worker;
   }

   platform_resolver_permit(const platform_resolver_permit&) = delete;
   platform_resolver_permit& operator=(const platform_resolver_permit&) = delete;

   /** Release the resolver slot after the platform lookup really completes. */
   ~platform_resolver_permit() { _runtime.release(_worker); }

   /** Return this permit's independently serviced resolver executor. */
   asio::any_io_executor executor() const { return _runtime.executor(_worker); }

private:
   platform_resolver_runtime& _runtime;
   size_t _worker = 0;
};

/**
 * Return intentionally process-lifetime resolver state.
 *
 * Joining an executor whose platform resolver is stuck would reintroduce an
 * unbounded process-exit wait.
 */
platform_resolver_runtime& resolver_runtime() {
   static auto* runtime = new platform_resolver_runtime;
   return *runtime;
}

/** Executor-independent result that may briefly outlive its requesting client. */
struct async_resolution_result {
   std::mutex mutex;
   bool complete = false;
   bool deadline_expired = false;
   std::optional<std::string> error;
   std::vector<detail::resolved_endpoint> endpoints;
};

/** Resolver wait state whose Asio objects remain owned by the client executor. */
struct async_resolution_state {
   explicit async_resolution_state(asio::any_io_executor executor)
      : notification(executor)
      , deadline_timer(std::move(executor)) {}

   resolver_event notification;
   asio::steady_timer deadline_timer;
   std::shared_ptr<async_resolution_result> result = std::make_shared<async_resolution_result>();
   detail::resolver_cancel_fn cancel;
};

} // namespace

inline namespace transport_internal {

detail::resolver_cancel_fn start_platform_resolution(const std::string& host, const std::string& service,
                                                     time_point deadline, detail::resolver_complete_fn complete) {
   auto& runtime = resolver_runtime();
   auto permit = std::make_shared<platform_resolver_permit>(runtime);
   (void)resolver_time_remaining(deadline);

   auto resolver = std::make_shared<tcp::resolver>(permit->executor());
   resolver->async_resolve(host, service,
                           [resolver, permit, complete = std::move(complete)](
                              const boost::system::error_code& error, tcp::resolver::results_type results) mutable {
                              (void)resolver;
                              (void)permit;
                              if (error) {
                                 complete(error.message(), {});
                                 return;
                              }

                              std::vector<detail::resolved_endpoint> endpoints;
                              endpoints.reserve(results.size());
                              for (const auto& result : results) {
                                 const auto address = result.endpoint().address().to_string();
                                 endpoints.push_back(detail::resolved_endpoint{
                                    .address = address,
                                    .port = result.endpoint().port(),
                                 });
                              }
                              complete(std::nullopt, std::move(endpoints));
                           });
   return [resolver] { asio::post(resolver->get_executor(), [resolver] { resolver->cancel(); }); };
}

} // namespace transport_internal

void detail::post_platform_resolver_worker_task_for_testing(size_t worker, std::function<void()> task) {
   FC_ASSERT(worker < platform_resolver_workers, "Platform resolver test worker index is invalid");
   asio::post(resolver_runtime().executor(worker), std::move(task));
}

bool detail::write_resolver_signal_for_testing(const std::function<int64_t()>& write_once) {
   return write_resolver_signal(write_once);
}

asio::awaitable<std::vector<tcp::endpoint>> client_impl::resolve(const std::string& host, const std::string& service,
                                                                 std::optional<operation_deadline> deadline,
                                                                 const std::shared_ptr<request_control>& control) {
   const auto cache_key = host + "|" + service;
   if (auto found = dns_cache.find(cache_key);
       found != dns_cache.end() &&
       (!options.dns_cache_timeout || found->second.expires > std::chrono::steady_clock::now())) {
      co_return found->second.endpoints;
   }
   dns_cache.erase(cache_key);
   control->throw_if_cancelled("DNS resolution");

   detail::resolver_cancel_fn cancel_resolution;
   auto state = std::make_shared<async_resolution_state>(strand);
   for (;;) {
      if (deadline && deadline->when <= time_point::now()) {
         throw transport_failure(deadline->timeout_kind, "DNS resolution deadline expired");
      }
      bool capacity_unavailable = false;
      try {
         auto weak_result = std::weak_ptr<async_resolution_result>(state->result);
         auto weak_notifier = std::weak_ptr<resolver_notifier>(state->notification.notifier);
         cancel_resolution =
            resolver_start(host, service, deadline ? deadline->when : time_point::maximum(),
                           [weak_result, weak_notifier](std::optional<std::string> error,
                                                        std::vector<detail::resolved_endpoint> endpoints) mutable {
                              auto result = weak_result.lock();
                              auto notifier = weak_notifier.lock();
                              if (!result || !notifier)
                                 return;
                              {
                                 std::scoped_lock lock(result->mutex);
                                 if (result->complete)
                                    return;
                                 result->complete = true;
                                 result->error = std::move(error);
                                 result->endpoints = std::move(endpoints);
                              }
                              notifier->notify();
                           });
         break;
      } catch (const resolver_capacity_unavailable&) {
         capacity_unavailable = true;
      } catch (const transport_failure&) {
         throw;
      } catch (...) {
         throw transport_failure(failure_kind::dns, "DNS resolver failed to start", true);
      }
      if (capacity_unavailable) {
         auto waiter = std::make_shared<resolver_event>(strand);
         auto timeout = std::make_shared<asio::steady_timer>(strand);
         auto timed_out = std::make_shared<std::atomic_bool>(false);
         if (deadline) {
            const auto remaining = deadline->when - time_point::now();
            if (remaining.count() <= 0) {
               throw transport_failure(deadline->timeout_kind, "DNS resolution deadline expired");
            }
            timeout->expires_after(std::chrono::microseconds(remaining.count()));
            timeout->async_wait([waiter, timed_out](const error_code& error) {
               if (error)
                  return;
               timed_out->store(true, std::memory_order_release);
               waiter->event.cancel();
            });
         }
         if (!resolver_runtime().wait_for_capacity(waiter->notifier)) {
            throw transport_failure(failure_kind::dns, "DNS resolver admission queue is full", true);
         }
         active_cancel_guard cancel_guard(control, [waiter, timeout] {
            timeout->cancel();
            waiter->event.cancel();
         });
         uint8_t signal = 0;
         error_code error;
         (void)co_await waiter->event.async_read_some(asio::buffer(&signal, sizeof(signal)),
                                                      asio::redirect_error(asio::use_awaitable, error));
         timeout->cancel();
         control->throw_if_cancelled("DNS resolution");
         if (timed_out->load(std::memory_order_acquire)) {
            throw transport_failure(deadline->timeout_kind, "DNS resolution deadline expired");
         }
         if (!waiter->notifier->was_notified()) {
            throw transport_failure(failure_kind::dns, "DNS resolver capacity wait ended", true);
         }
      }
   }

   state->cancel = std::move(cancel_resolution);
   if (deadline) {
      const auto remaining = deadline->when - time_point::now();
      if (remaining.count() <= 0) {
         if (state->cancel)
            state->cancel();
         throw transport_failure(deadline->timeout_kind, "DNS resolution deadline expired");
      }
      state->deadline_timer.expires_after(std::chrono::microseconds(remaining.count()));
      auto weak = std::weak_ptr<async_resolution_state>(state);
      state->deadline_timer.async_wait([weak](const error_code& error) {
         if (error)
            return;
         auto current = weak.lock();
         if (!current)
            return;
         {
            std::scoped_lock lock(current->result->mutex);
            if (current->result->complete)
               return;
            current->result->complete = true;
            current->result->deadline_expired = true;
         }
         if (current->cancel)
            current->cancel();
         current->notification.event.cancel();
      });
   }
   active_cancel_guard cancel_guard(control, [state] {
      if (state->cancel)
         state->cancel();
      state->deadline_timer.cancel();
      state->notification.event.cancel();
   });
   uint8_t signal = 0;
   error_code wait_error;
   (void)co_await state->notification.event.async_read_some(asio::buffer(&signal, sizeof(signal)),
                                                            asio::redirect_error(asio::use_awaitable, wait_error));
   state->deadline_timer.cancel();
   control->throw_if_cancelled("DNS resolution");
   std::optional<std::string> resolution_error;
   std::vector<detail::resolved_endpoint> resolved_endpoints;
   bool resolution_complete = false;
   bool deadline_expired = false;
   {
      std::scoped_lock lock(state->result->mutex);
      resolution_complete = state->result->complete;
      deadline_expired = state->result->deadline_expired;
      if (resolution_complete) {
         resolution_error = state->result->error;
         resolved_endpoints = std::move(state->result->endpoints);
      } else {
         state->result->complete = true;
      }
   }
   if (deadline_expired) {
      throw transport_failure(deadline->timeout_kind, "DNS resolution deadline expired");
   }
   if (!resolution_complete) {
      if (state->cancel)
         state->cancel();
      if (deadline) {
         throw transport_failure(deadline->timeout_kind, "DNS resolution deadline expired");
      }
      throw transport_failure(failure_kind::dns, "DNS resolution wait ended before completion", true);
   }
   state->cancel = {};
   if (resolution_error) {
      throw transport_failure(failure_kind::dns, "DNS resolution failed", true);
   }
   if (resolved_endpoints.empty()) {
      throw transport_failure(failure_kind::dns, "DNS resolution returned no endpoints", true);
   }

   std::vector<tcp::endpoint> endpoints;
   endpoints.reserve(resolved_endpoints.size());
   for (const auto& entry : resolved_endpoints) {
      error_code address_error;
      const auto address = asio::ip::make_address(entry.address, address_error);
      if (address_error) {
         throw transport_failure(failure_kind::dns, "DNS resolution returned an invalid endpoint", true);
      }
      endpoints.emplace_back(address, entry.port);
   }
   if (!options.dns_cache_timeout || options.dns_cache_timeout->count() != 0) {
      const auto expires =
         !options.dns_cache_timeout
            ? std::chrono::steady_clock::time_point::max()
            : std::chrono::steady_clock::now() + std::chrono::microseconds(options.dns_cache_timeout->count());
      dns_cache.insert_or_assign(cache_key, dns_entry{endpoints, expires});
   }
   co_return endpoints;
}

} // namespace http
} // namespace fc

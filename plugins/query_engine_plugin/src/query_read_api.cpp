#include <sysio/query_engine_plugin/query_read_api.hpp>

#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>

namespace sysio::query_engine {
namespace {
constexpr auto read_wait_interval = std::chrono::milliseconds(10);
}

/// Shared by queued reads so shutdown can invalidate controller access before those callbacks drain.
struct query_read_api::impl : std::enable_shared_from_this<impl> {
   std::shared_ptr<local_table_source> source;
   const read_scheduler post_read;
   const std::thread::id application_thread;
   const read_window_remaining window_remaining;
   mutable std::shared_mutex mutex;

   impl(std::shared_ptr<local_table_source> source, read_scheduler scheduler, std::thread::id application_thread,
        read_window_remaining window_remaining)
      : source(std::move(source))
      , post_read(std::move(scheduler))
      , application_thread(application_thread)
      , window_remaining(std::move(window_remaining)) {
      if (!this->source || !post_read)
         throw query_error(error_kind::INVALID_PARAMS, "Query source and read scheduler are required");
   }

   /// Own the operation and task across cancellation; a worker never lends its stack to a queued read.
   /// An attempt the read window cuts short is queued again for the next window while the request
   /// deadline allows, as producer_plugin re-queues a read-only transaction the window exhausted; the
   /// waiting caller keeps the attempt alive, so an abandoned read is never retried.
   template <typename Result, typename Operation>
   Result read(std::shared_ptr<query_task> task, Operation operation) {
      task->budget->check();
      auto completion = std::make_shared<std::promise<Result>>();
      auto future = completion->get_future();
      auto attempt = std::make_shared<std::function<void()>>();
      *attempt = [self = shared_from_this(), task, completion, operation = std::move(operation),
                  retry = std::weak_ptr<std::function<void()>>(attempt)] {
         const auto charged = task->budget->checkpoint();
         const auto cuts = task->budget->read_window_cuts.load(std::memory_order_relaxed);
         try {
            std::shared_lock lock(self->mutex);
            task->budget->check();
            if (!self->source)
               throw query_error(error_kind::QUERY_CANCELLED, "Query read API is stopped");
            // The callback is bounded by the read window's own end as well as by the capture budget,
            // the same pair producer_plugin applies to a read-only transaction.
            if (self->window_remaining)
               task->budget->capture_deadline =
                  task->budget->now() +
                  std::chrono::duration_cast<query_budget::clock::duration>(self->window_remaining());
            auto result = operation(*self->source, *task->budget);
            task->budget->cut_by_read_window = false;
            completion->set_value(std::move(result));
         } catch (const query_error&) {
            // The window's end, not the query's own budgets, ended this attempt: its partial result goes
            // with its charges, and the attempt is queued for the next window while the request
            // deadline allows. A deadline that passes meanwhile names the window.
            if (task->budget->read_window_cuts.load(std::memory_order_relaxed) > cuts) {
               if (const auto next = retry.lock()) {
                  try {
                     task->budget->restore(charged);
                     task->budget->check();
                     self->post_read(*next);
                  } catch (...) {
                     completion->set_exception(std::current_exception());
                  }
                  return;
               }
            }
            completion->set_exception(std::current_exception());
         } catch (...) {
            completion->set_exception(std::current_exception());
         }
      };
      post_read(*attempt);
      while (future.wait_for(read_wait_interval) != std::future_status::ready)
         task->budget->check();
      task->budget->check();
      return future.get();
   }
};

query_read_api::query_read_api(std::shared_ptr<local_table_source> source, read_scheduler scheduler,
                               std::thread::id application_thread, read_window_remaining window_remaining)
   : state(std::make_shared<impl>(std::move(source), std::move(scheduler), application_thread,
                                  std::move(window_remaining))) {}

std::vector<table_schema> query_read_api::describe(std::shared_ptr<const ast_query> ast,
                                                   std::shared_ptr<query_task> task) const {
   assert_can_wait();
   // The read callback only copies bytes; hashing and decoding each ABI happens here, on the worker.
   auto schemas = state->read<std::vector<table_schema>>(
      task, [ast](auto& source, auto& budget) { return source.capture_abis(*ast, budget); });
   for (auto& schema : schemas)
      resolve_schema(schema, ast->table, *task->budget);
   return schemas;
}

captured_input query_read_api::capture(std::shared_ptr<const typed_plan> plan, std::shared_ptr<query_task> task) const {
   assert_can_wait();
   return state->read<captured_input>(
      std::move(task), [plan = std::move(plan)](auto& source, auto& budget) { return source.capture(*plan, budget); });
}

void query_read_api::assert_can_wait() const {
   if (std::this_thread::get_id() == state->application_thread)
      throw query_error(error_kind::INVALID_PARAMS, "Blocking query execution requires a worker thread");
}

void query_read_api::stop() {
   std::unique_lock lock(state->mutex);
   state->source.reset();
}
} // namespace sysio::query_engine

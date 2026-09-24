#include <sysio/query_engine_plugin/query_read_api.hpp>

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
   mutable std::shared_mutex mutex;

   impl(std::shared_ptr<local_table_source> source, read_scheduler scheduler, std::thread::id application_thread)
      : source(std::move(source))
      , post_read(std::move(scheduler))
      , application_thread(application_thread) {
      if (!this->source || !post_read)
         throw query_error(error_kind::INVALID_PARAMS, "Query source and read scheduler are required");
   }

   /// Own the operation and task across cancellation; a worker never lends its stack to a queued read.
   template <typename Result, typename Operation>
   Result read(std::shared_ptr<query_task> task, Operation operation) {
      task->budget->check();
      auto completion = std::make_shared<std::promise<Result>>();
      auto future = completion->get_future();
      post_read([self = shared_from_this(), task, completion, operation = std::move(operation)] {
         try {
            std::shared_lock lock(self->mutex);
            task->budget->check();
            if (!self->source)
               throw query_error(error_kind::QUERY_CANCELLED, "Query read API is stopped");
            completion->set_value(operation(*self->source, *task->budget));
         } catch (...) {
            completion->set_exception(std::current_exception());
         }
      });
      while (future.wait_for(read_wait_interval) != std::future_status::ready)
         task->budget->check();
      task->budget->check();
      return future.get();
   }
};

query_read_api::query_read_api(std::shared_ptr<local_table_source> source, read_scheduler scheduler,
                               std::thread::id application_thread)
   : state(std::make_shared<impl>(std::move(source), std::move(scheduler), application_thread)) {}

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

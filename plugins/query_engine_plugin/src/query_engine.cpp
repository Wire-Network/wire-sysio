#include <fc/parallel/worker_task_queue.hpp>
#include <sysio/query_engine_plugin/query_engine.hpp>

#include <map>
#include <mutex>

namespace sysio::query_engine {
namespace {
constexpr auto completion_wait_interval = std::chrono::milliseconds(10);
constexpr auto failure_log_interval = std::chrono::seconds(60);
} // namespace

/// The queue owns complete SQL executions; the read API owns synchronization with the controller.
struct query_engine::impl : std::enable_shared_from_this<impl> {
   using task_ptr = std::shared_ptr<query_task>;
   using queue_type = fc::parallel::worker_task_queue<task_ptr>;
   const query_config config;
   const std::shared_ptr<query_read_api> reads;
   const query_budget::now_function now;
   const stage_observer observe;
   std::shared_ptr<queue_type> queue;
   mutable std::mutex mutex;
   std::mutex lifecycle_mutex;
   std::map<const query_task*, std::weak_ptr<query_task>> requests;
   bool stopping = false;
   service_stats counters;
   fc::logger log{constants::logger};
   query_budget::clock::time_point last_failure_log{};
   uint64_t suppressed_failures = 0;
   const std::exception_ptr cancelled_error =
      std::make_exception_ptr(query_error(error_kind::QUERY_CANCELLED, "Query engine is stopping"));
   const std::exception_ptr allocation_error = std::make_exception_ptr(
      query_error(error_kind::QUERY_LIMIT, "Memory allocation failed", std::nullopt, option::max_memory_bytes));
   const std::exception_ptr internal_error =
      std::make_exception_ptr(query_error(error_kind::INTERNAL_ERROR, "Internal query failure"));

   impl(query_config config, std::shared_ptr<query_read_api> reads, query_budget::now_function now,
        stage_observer observe)
      : config(config)
      , reads(std::move(reads))
      , now(std::move(now))
      , observe(std::move(observe)) {
      config.validate();
      if (!this->reads)
         throw query_error(error_kind::INVALID_PARAMS, "Query read API is required");
   }

   /// The owning engine stops this queue before releasing the implementation.
   void start() {
      queue = queue_type::create({.max_threads = config.worker_threads, .max_pending_items = config.max_in_flight},
                                 [this](task_ptr& task) { run(task); });
   }

   /// Retirement runs after the last worker/read/waiter reference, including abandoned read callbacks.
   void retire(const query_task& task) {
      std::lock_guard lock(mutex);
      counters.scanned_rows += task.succeeded ? task.scanned_rows : task.budget->scanned_rows;
      counters.capture_bytes += task.succeeded ? task.raw_bytes : task.budget->raw_bytes;
      counters.peak_accounted_bytes = std::max(
         counters.peak_accounted_bytes, task.succeeded ? task.peak_accounted_bytes : task.budget->peak_accounted_bytes);
      counters.returned_rows += task.returned_rows;
      counters.read_window_cuts += task.budget->read_window_cuts.load(std::memory_order_relaxed);
      requests.erase(&task);
      counters.in_flight = requests.size();
   }

   /// A single completion winner publishes either owned rows or the original typed exception.
   void fail(const task_ptr& task, std::exception_ptr error, error_kind kind) {
      if (task->completed.exchange(true))
         return;
      {
         std::lock_guard lock(mutex);
         ++counters.completed;
         ++counters.failed[*magic_enum::enum_index(kind)];
         counters.timed_out += kind == error_kind::QUERY_TIMEOUT;
         counters.cancelled += kind == error_kind::QUERY_CANCELLED;
      }
      task->completion.set_exception(std::move(error));
   }

   /// Snapshot accounting before returning to an HTTP caller that may charge serialization to the budget.
   void succeed(const task_ptr& task, query_result result) {
      if (task->completed.exchange(true))
         return;
      task->succeeded = true;
      task->returned_rows = result.rows.size();
      task->scanned_rows = task->budget->scanned_rows;
      task->raw_bytes = task->budget->raw_bytes;
      task->peak_accounted_bytes = task->budget->peak_accounted_bytes;
      {
         std::lock_guard lock(mutex);
         ++counters.completed;
      }
      task->completion.set_value(std::move(result));
   }

   /// Keep SQL and internal controller details out of client errors; throttle unexpected diagnostics.
   void report_internal(const std::string& detail) {
      std::lock_guard lock(mutex);
      const auto time = query_budget::clock::now();
      if (last_failure_log.time_since_epoch().count() == 0 || time - last_failure_log >= failure_log_interval) {
         fc_elog(log, "Query internal failure: {}; further_failures_suppressed={}", detail, suppressed_failures);
         last_failure_log = time;
         suppressed_failures = 0;
      } else
         ++suppressed_failures;
   }

   /// Stage observation happens on query workers, including calls into the synchronized read API.
   void stage(const task_ptr& task, query_stage current) {
      task->budget->check();
      if (observe)
         observe(current);
      task->budget->check();
   }

   /// No exception escapes a worker_task_queue callback.
   void run(const task_ptr& task) {
      try {
         stage(task, query_stage::parse);
         auto ast = std::make_shared<ast_query>(parse_query(task->query, *task->budget));
         stage(task, query_stage::describe);
         auto schemas = reads->describe(ast, task);
         stage(task, query_stage::plan);
         auto plan = std::make_shared<typed_plan>(create_plan(std::move(*ast), std::move(schemas), *task->budget));
         ast.reset();
         stage(task, query_stage::capture);
         auto input = reads->capture(plan, task);
         stage(task, query_stage::evaluate);
         auto result = evaluate(*plan, std::move(input), *task->budget);
         plan.reset();
         task->budget->check();
         succeed(task, std::move(result));
      } catch (const query_error& error) {
         fail(task, std::current_exception(), error.kind);
      } catch (const std::bad_alloc&) {
         fail(task, allocation_error, error_kind::QUERY_LIMIT);
      } catch (const fc::exception& error) {
         report_internal(error.to_detail_string());
         fail(task, internal_error, error_kind::INTERNAL_ERROR);
      } catch (const std::exception& error) {
         report_internal(error.what());
         fail(task, internal_error, error_kind::INTERNAL_ERROR);
      } catch (...) {
         report_internal("Unknown query execution failure");
         fail(task, internal_error, error_kind::INTERNAL_ERROR);
      }
   }

   /// Admission covers queued work and retained read callbacks; the caller waits without touching the controller.
   query_result execute(const std::string& sql, std::shared_ptr<query_budget> budget) {
      {
         std::lock_guard lock(mutex);
         if (stopping)
            std::rethrow_exception(cancelled_error);
      }
      reads->assert_can_wait();
      budget->assert_limit(sql.size(), config.max_query_bytes, option::max_query_bytes);
      budget->charge_memory(sql.size() + sizeof(query_task));
      auto task = std::make_shared<query_task>();
      task->query = sql;
      task->budget = std::move(budget);
      auto future = task->completion.get_future();
      std::function<void(const query_task&)> retirement = [self = shared_from_this()](const query_task& retired) {
         self->retire(retired);
      };
      {
         std::lock_guard lock(mutex);
         if (stopping)
            std::rethrow_exception(cancelled_error);
         if (requests.size() >= config.max_in_flight) {
            ++counters.rejected_busy;
            throw query_error(error_kind::QUERY_BUSY, "Query capacity exhausted");
         }
         requests.emplace(task.get(), task);
         task->retire = std::move(retirement);
         ++counters.accepted;
         counters.in_flight = requests.size();
      }
      try {
         if (!queue->try_push(task))
            std::rethrow_exception(cancelled_error);
      } catch (const query_error& error) {
         fail(task, std::current_exception(), error.kind);
      } catch (const std::bad_alloc&) {
         fail(task, allocation_error, error_kind::QUERY_LIMIT);
      }
      while (future.wait_for(completion_wait_interval) != std::future_status::ready) {
         try {
            task->budget->check();
         } catch (const query_error& error) {
            task->budget->cancelled = true;
            fail(task, std::current_exception(), error.kind);
         }
      }
      return future.get();
   }

   /// Cancellation precedes joining: no worker needs the application thread to drain a pending read.
   void stop() {
      std::lock_guard lifecycle_lock(lifecycle_mutex);
      std::vector<task_ptr> pending;
      {
         std::lock_guard lock(mutex);
         if (stopping)
            return;
         stopping = true;
         for (const auto& [key, weak] : requests)
            if (auto task = weak.lock()) {
               task->budget->cancelled = true;
               pending.push_back(std::move(task));
            }
      }
      for (const auto& task : pending)
         fail(task, cancelled_error, error_kind::QUERY_CANCELLED);
      if (queue) {
         queue->stop();
         queue->discard_pending();
      }
      reads->stop();
      pending.clear();
      std::lock_guard lock(mutex);
      fc_ilog(log,
              "Query shutdown: accepted={}, completed={}, busy={}, timed_out={}, cancelled={}, scanned_rows={}, "
              "returned_rows={}, capture_bytes={}, peak_accounted_bytes={}, read_window_cuts={}",
              counters.accepted, counters.completed, counters.rejected_busy, counters.timed_out, counters.cancelled,
              counters.scanned_rows, counters.returned_rows, counters.capture_bytes, counters.peak_accounted_bytes,
              counters.read_window_cuts);
   }
};

query_engine::query_engine(query_config config, std::shared_ptr<query_read_api> reads, query_budget::now_function now,
                           stage_observer observe)
   : state(std::make_shared<impl>(config, std::move(reads), std::move(now), std::move(observe))) {
   state->start();
}
query_engine::~query_engine() {
   stop();
}
query_result query_engine::execute(const std::string& sql, const std::optional<query_options>& options) {
   return execute(sql, create_budget(options));
}
query_result query_engine::execute(const std::string& sql, std::shared_ptr<query_budget> budget) {
   if (!budget)
      throw query_error(error_kind::INVALID_PARAMS, "Query budget is required");
   if (budget->claimed.exchange(true))
      throw query_error(error_kind::INVALID_PARAMS, "Query budget was already used");
   return state->execute(sql, std::move(budget));
}
std::shared_ptr<query_budget> query_engine::create_budget(const std::optional<query_options>& options) const {
   return std::make_shared<query_budget>(state->config, state->now, options.value_or(query_options{}));
}
const query_config& query_engine::config() const {
   return state->config;
}
void query_engine::stop() {
   state->stop();
}
void query_engine::set_logger(fc::logger logger) {
   std::lock_guard lock(state->mutex);
   state->log = std::move(logger);
}
void query_engine::report_internal(const std::string& detail) {
   state->report_internal(detail);
}
service_stats query_engine::stats() const {
   std::lock_guard lock(state->mutex);
   return state->counters;
}
} // namespace sysio::query_engine

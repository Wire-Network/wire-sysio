#include <fc/io/json.hpp>
#include <fc/parallel/worker_task_queue.hpp>
#include <sysio/query_engine_plugin/query_http_handler.hpp>

#include <boost/beast/http/status.hpp>

#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

namespace sysio::query_engine_plugin {
namespace {
constexpr auto deadline_poll_interval = std::chrono::milliseconds(10);
constexpr uint64_t json_escape_bound = 6;
constexpr uint64_t json_scalar_bound = 64;
constexpr uint64_t serialization_copies = 2;

/// Reserve the JSON serialization bound before fc::json allocates its output buffer.
void charge_serialization(const fc::variant& value, query_budget& budget) {
   budget.check();
   budget.charge_memory(json_scalar_bound * serialization_copies);
   if (value.is_string())
      budget.charge_memory(value.get_string().size() * json_escape_bound * serialization_copies);
   else if (value.is_array())
      for (const auto& child : value.get_array())
         charge_serialization(child, budget);
   else if (value.is_object())
      for (const auto& entry : value.get_object()) {
         budget.charge_memory(entry.key().size() * json_escape_bound * serialization_copies);
         charge_serialization(entry.value(), budget);
      }
}

/// Respect notification semantics on both success and invocation failure.
void respond(const query_request* request, url_response_callback callback, fc::variant response) {
   if (request && request->notification)
      callback(magic_enum::enum_integer(boost::beast::http::status::no_content), std::nullopt);
   else
      callback(magic_enum::enum_integer(boost::beast::http::status::ok), std::move(response));
}
} // namespace

/// HTTP workers wait on a separate query queue, so saturation cannot consume the workers needed to execute SQL.
struct query_http_handler::impl {
   struct http_task {
      query_request request;
      std::shared_ptr<query_budget> budget;
      url_response_callback callback;
      fc::variant allocation_error;
      std::atomic<bool> completed{false};
   };
   using task_ptr = std::shared_ptr<http_task>;
   using queue_type = fc::parallel::worker_task_queue<task_ptr>;
   const std::shared_ptr<query_engine> engine;
   std::shared_ptr<queue_type> queue;
   std::mutex mutex;
   std::mutex lifecycle_mutex;
   std::condition_variable ready;
   std::map<const http_task*, task_ptr> requests;
   std::thread timer;
   bool stopping = false;

   explicit impl(std::shared_ptr<query_engine> engine)
      : engine(std::move(engine)) {
      if (!this->engine)
         throw query_error(error_kind::INVALID_PARAMS, "Query engine is required");
      const auto& config = this->engine->config();
      queue = queue_type::create({.max_threads = config.worker_threads, .max_pending_items = config.max_in_flight},
                                 [this](task_ptr& task) { run(task); });
      try {
         timer = std::thread([this] { deadlines(); });
      } catch (...) {
         queue->stop();
         throw;
      }
   }

   /// Exactly one completion wins even when a timeout races engine completion or shutdown.
   void finish(const task_ptr& task, std::optional<query_error> error = std::nullopt, fc::variant response = {}) {
      if (task->completed.exchange(true))
         return;
      if (error) {
         try {
            response = create_error(&task->request, *error);
         } catch (const std::bad_alloc&) {
            response = std::move(task->allocation_error);
         }
      }
      try {
         respond(&task->request, std::move(task->callback), std::move(response));
      } catch (const std::exception& failure) {
         engine->report_internal(failure.what());
      } catch (...) {
         engine->report_internal("Query HTTP completion failed");
      }
   }

   /// Deadline completion stays live while every HTTP worker is blocked in execute().
   void deadlines() {
      for (;;) {
         std::vector<task_ptr> expired;
         {
            std::unique_lock lock(mutex);
            ready.wait_for(lock, deadline_poll_interval, [&] { return stopping; });
            if (stopping)
               return;
            for (const auto& [key, task] : requests)
               if (!task->completed && task->budget->now() >= task->budget->deadline)
                  expired.push_back(task);
         }
         for (const auto& task : expired) {
            task->budget->cancelled = true;
            finish(task, query_error(error_kind::QUERY_TIMEOUT, "Query deadline exceeded"));
         }
      }
   }

   /// HTTP workers invoke queued SQL execution, then serialize its owned result.
   void run(const task_ptr& task) {
      try {
         if (!task->completed) {
            auto result = engine->execute(task->request.query, task->budget);
            auto response = create_success(task->request, std::move(result));
            charge_serialization(response, *task->budget);
            const auto yield = [&](size_t size) {
               task->budget->assert_limit(size, engine->config().max_response_bytes, option::max_response_bytes);
            };
            const auto encoded = fc::json::to_string(response, yield);
            yield(encoded.size());
            finish(task, std::nullopt, std::move(response));
         }
      } catch (const query_error& error) {
         finish(task, error);
      } catch (const std::bad_alloc&) {
         finish(task, query_error(error_kind::QUERY_LIMIT, "Memory allocation failed", std::nullopt,
                                  option::max_memory_bytes));
      } catch (const fc::exception& error) {
         engine->report_internal(error.to_detail_string());
         finish(task, query_error(error_kind::INTERNAL_ERROR, "Internal query failure"));
      } catch (const std::exception& error) {
         engine->report_internal(error.what());
         finish(task, query_error(error_kind::INTERNAL_ERROR, "Internal query failure"));
      } catch (...) {
         engine->report_internal("Unknown query HTTP failure");
         finish(task, query_error(error_kind::INTERNAL_ERROR, "Internal query failure"));
      }
      std::lock_guard lock(mutex);
      requests.erase(task.get());
   }

   /// Bound ingress independently of engine admission, including queued HTTP bodies and response buffers.
   void submit(std::string body, url_response_callback callback) {
      auto budget = engine->create_budget(query_options{.timeout_ms = engine->config().timeout_ms});
      query_request parsed;
      try {
         parsed = parse_request(body, *budget);
      } catch (const query_error& error) {
         respond(nullptr, std::move(callback), create_error(nullptr, error));
         return;
      }
      if (parsed.invocation_error) {
         respond(&parsed, std::move(callback), create_error(&parsed, *parsed.invocation_error));
         return;
      }
      auto task = std::make_shared<http_task>();
      task->request = std::move(parsed);
      task->budget = std::move(budget);
      task->callback = std::move(callback);
      task->allocation_error =
         create_error(&task->request, query_error(error_kind::QUERY_LIMIT, "Memory allocation failed", std::nullopt,
                                                  option::max_memory_bytes));
      std::optional<query_error> rejected;
      {
         std::lock_guard lock(mutex);
         if (stopping)
            rejected.emplace(error_kind::QUERY_CANCELLED, "Query HTTP handler is stopping");
         else if (requests.size() >= engine->config().max_in_flight)
            rejected.emplace(error_kind::QUERY_BUSY, "Query HTTP capacity exhausted");
         else
            requests.emplace(task.get(), task);
      }
      if (rejected) {
         finish(task, *rejected);
         return;
      }
      try {
         if (!queue->try_push(task)) {
            finish(task, query_error(error_kind::QUERY_CANCELLED, "Query HTTP handler is stopping"));
            std::lock_guard lock(mutex);
            requests.erase(task.get());
         }
      } catch (...) {
         finish(task, query_error(error_kind::QUERY_LIMIT, "Memory allocation failed", std::nullopt,
                                  option::max_memory_bytes));
         std::lock_guard lock(mutex);
         requests.erase(task.get());
      }
   }

   /// Wake execute() through its shared cancellation flag before joining adapter workers.
   void stop() {
      std::lock_guard lifecycle_lock(lifecycle_mutex);
      std::vector<task_ptr> pending;
      {
         std::lock_guard lock(mutex);
         if (stopping)
            return;
         stopping = true;
         for (const auto& [key, task] : requests) {
            task->budget->cancelled = true;
            pending.push_back(task);
         }
      }
      for (const auto& task : pending)
         finish(task, query_error(error_kind::QUERY_CANCELLED, "Query HTTP handler is stopping"));
      ready.notify_all();
      timer.join();
      queue->stop();
      queue->discard_pending();
      std::lock_guard lock(mutex);
      requests.clear();
   }
};

query_http_handler::query_http_handler(std::shared_ptr<query_engine> engine)
   : state(std::make_unique<impl>(std::move(engine))) {}
query_http_handler::~query_http_handler() {
   stop();
}
void query_http_handler::submit(std::string body, url_response_callback callback) {
   state->submit(std::move(body), std::move(callback));
}
void query_http_handler::stop() {
   state->stop();
}
api_description query_http_handler::create_api() {
   return {
      {constants::endpoint, api_category::chain_ro,
       [self = shared_from_this()](std::string&&, std::string&& body, url_response_callback&& callback) {
          self->submit(std::move(body), std::move(callback));
       }}
   };
}
} // namespace sysio::query_engine_plugin

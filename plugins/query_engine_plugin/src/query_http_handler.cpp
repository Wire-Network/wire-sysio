#include <fc/io/json.hpp>
#include <fc/io/json_stream.hpp>
#include <fc/parallel/worker_task_queue.hpp>
#include <sysio/query_engine_plugin/query_http_handler.hpp>

#include <boost/beast/http/status.hpp>

#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

namespace sysio::query_engine {
namespace {
constexpr auto deadline_poll_interval = std::chrono::milliseconds(10);
constexpr uint64_t json_escape_bound = 6;
constexpr uint64_t json_scalar_bound = 64;
/// The encoded body, and http_plugin's response buffer while the body is copied into it.
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

/// Encode a response envelope with the encoder http_plugin applies to a variant response.
std::string encode(const fc::variant& response) {
   return fc::json::to_string(response, fc::time_point::maximum());
}

/// Respect notification semantics on both success and invocation failure. A body is charged to the in-flight
/// budget while it waits for an http_plugin thread, and released once the response buffer holds a copy.
void respond(const query_request* request, url_response_stream_callback callback, std::string body,
             const query_http_handler::reserve_bytes_in_flight& reserve) {
   if (request && request->notification) {
      callback(magic_enum::enum_integer(boost::beast::http::status::no_content), [](fc::json_writer&) {});
      return;
   }
   auto reservation = reserve ? reserve(body.size()) : nullptr;
   callback(magic_enum::enum_integer(boost::beast::http::status::ok),
            [body = std::move(body), reservation = std::move(reservation)](fc::json_writer& writer) mutable {
               const auto encoded = std::move(body);
               const auto charge = std::move(reservation);
               writer.raw_value(encoded);
            });
}
} // namespace

/// HTTP workers wait on a separate query queue, so saturation cannot consume the workers needed to execute SQL.
struct query_http_handler::impl {
   struct http_task {
      query_request request;
      std::shared_ptr<query_budget> budget;
      url_response_stream_callback callback;
      std::string allocation_error;
      std::atomic<bool> completed{false};
   };
   using task_ptr = std::shared_ptr<http_task>;
   using queue_type = fc::parallel::worker_task_queue<task_ptr>;
   const std::shared_ptr<query_engine> engine;
   const query_http_handler::reserve_bytes_in_flight reserve;
   std::shared_ptr<queue_type> queue;
   std::mutex mutex;
   std::mutex lifecycle_mutex;
   std::condition_variable ready;
   std::map<const http_task*, task_ptr> requests;
   std::thread timer;
   bool stopping = false;

   impl(std::shared_ptr<query_engine> engine, query_http_handler::reserve_bytes_in_flight reserve)
      : engine(std::move(engine))
      , reserve(std::move(reserve)) {
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
   void finish(const task_ptr& task, std::optional<query_error> error = std::nullopt, std::string body = {}) {
      if (task->completed.exchange(true))
         return;
      if (error) {
         try {
            body = encode(create_error(&task->request, *error));
         } catch (const std::bad_alloc&) {
            body = std::move(task->allocation_error);
         }
      }
      try {
         respond(&task->request, std::move(task->callback), std::move(body), reserve);
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
            finish(task, task->budget->deadline_error());
         }
      }
   }

   /// Execute the query and encode its owned result, bounded by the response limit. The result is released
   /// before the body is handed to http_plugin.
   std::string encode_success(const task_ptr& task) {
      auto result = engine->execute(task->request.query, task->budget);
      const auto response = create_success(task->request, std::move(result));
      charge_serialization(response, *task->budget);
      const auto yield = [&](size_t size) {
         task->budget->assert_limit(size, engine->config().max_response_bytes, option::max_response_bytes);
      };
      auto body = fc::json::to_string(response, yield);
      yield(body.size());
      return body;
   }

   /// HTTP workers invoke queued SQL execution, then serialize its owned result.
   void run(const task_ptr& task) {
      try {
         if (!task->completed)
            finish(task, std::nullopt, encode_success(task));
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
   /// The request's own `limit`, `offset` and `timeout_ms` are applied to the budget before admission.
   void submit(std::string body, url_response_stream_callback callback) {
      auto budget = engine->create_budget(query_options{.timeout = engine->config().timeout});
      query_request parsed;
      try {
         parsed = parse_request(body, *budget);
      } catch (const query_error& error) {
         respond(nullptr, std::move(callback), encode(create_error(nullptr, error)), reserve);
         return;
      }
      if (parsed.invocation_error) {
         respond(&parsed, std::move(callback), encode(create_error(&parsed, *parsed.invocation_error)), reserve);
         return;
      }
      // Cannot throw: timeout_ms is at most the configured timeout, whose deadline this budget already holds.
      budget->apply_request_options(parsed.options);
      auto task = std::make_shared<http_task>();
      task->request = std::move(parsed);
      task->budget = std::move(budget);
      task->callback = std::move(callback);
      task->allocation_error =
         encode(create_error(&task->request, query_error(error_kind::QUERY_LIMIT, "Memory allocation failed",
                                                         std::nullopt, option::max_memory_bytes)));
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

query_http_handler::query_http_handler(std::shared_ptr<query_engine> engine, reserve_bytes_in_flight reserve)
   : state(std::make_unique<impl>(std::move(engine), std::move(reserve))) {}
query_http_handler::~query_http_handler() {
   stop();
}
void query_http_handler::submit(std::string body, url_response_stream_callback callback) {
   state->submit(std::move(body), std::move(callback));
}
void query_http_handler::stop() {
   state->stop();
}
api_description_stream query_http_handler::create_api() {
   return {
      {constants::endpoint, api_category::chain_ro,
       [self = shared_from_this()](std::string&&, std::string&& body, url_response_stream_callback&& callback) {
          self->submit(std::move(body), std::move(callback));
       }}
   };
}
} // namespace sysio::query_engine

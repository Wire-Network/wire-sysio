#include <sysio/chain/exceptions.hpp>
#include <sysio/producer_plugin/producer_plugin.hpp>
#include <sysio/query_engine_plugin/query_config.hpp>
#include <sysio/query_engine_plugin/query_engine_plugin.hpp>
#include <sysio/query_engine_plugin/query_http_handler.hpp>

#include <magic_enum/magic_enum.hpp>

#include <algorithm>
#include <chrono>
#include <mutex>

namespace sysio {
namespace {
/// Why a query fails on a node whose executor has no read-only threads to drain the read-exclusive queue.
constexpr auto no_read_threads_message =
   "No read-only threads: chain reads run on the read-exclusive queue, which only read-only threads drain";
} // namespace

/// Service shutdown invalidates its source even while posted cancelled callbacks remain queued.
/// The engine handle is guarded by a mutex: libc++ does not implement std::atomic<std::shared_ptr>.
struct query_engine_plugin::impl {
   fc::logger log;
   query_engine::query_config config;
   bool capture_configured = false;
   mutable std::mutex engine_mutex;
   std::shared_ptr<query_engine::query_engine> engine;
   std::shared_ptr<query_engine::query_http_handler> http_handler;

   std::shared_ptr<query_engine::query_engine> current_engine() const {
      std::lock_guard lock(engine_mutex);
      return engine;
   }
   std::shared_ptr<query_engine::query_engine> exchange_engine(std::shared_ptr<query_engine::query_engine> next) {
      std::lock_guard lock(engine_mutex);
      engine.swap(next);
      return next;
   }
};

query_engine_plugin::query_engine_plugin()
   : _impl(std::make_unique<impl>()) {}
query_engine_plugin::~query_engine_plugin() {
   plugin_shutdown();
}
void query_engine_plugin::set_program_options(options_description&, options_description& cfg) {
   query_engine::add_options(cfg);
}
void query_engine_plugin::plugin_initialize(const variables_map& options) {
   try {
      _impl->config = query_engine::parse_config(options);
      _impl->capture_configured = options.count(query_engine::option::max_capture_ms) > 0;
      handle_sighup();
   } catch (const query_engine::query_error& error) {
      SYS_THROW(chain::plugin_config_exception, "Invalid query configuration: {}", error.what());
   }
}
void query_engine_plugin::plugin_startup() {
   auto& controller = app().get_plugin<chain_plugin>().chain();
   SYS_ASSERT(controller.get_read_mode() != chain::db_read_mode::SPECULATIVE, chain::plugin_config_exception,
              "Query engine requires head or irreversible read mode");
   // Chain reads run on the read-exclusive queue, which only read-only threads drain. producer_plugin sizes
   // that pool from read-only-threads and refuses it on a producing node; a node without it still starts,
   // and its read scheduler fails every query instead of queueing a read nothing would ever run.
   const bool has_read_threads = app().executor().get_read_threads() > 0;
   auto& producer = app().get_plugin<producer_plugin>();
   if (has_read_threads) {
      // One capture callback may hold a read window for at most the budget producer_plugin enforces on a
      // read-only transaction, so it cannot delay the next write window. An explicit query-max-capture-ms
      // may only tighten that bound.
      const std::chrono::microseconds read_budget(producer.get_read_only_max_transaction_time().count());
      SYS_ASSERT(read_budget > std::chrono::microseconds::zero(), chain::plugin_config_exception,
                 "Query engine requires a positive read-only transaction time from producer_plugin");
      if (_impl->capture_configured)
         SYS_ASSERT(_impl->config.max_capture <= read_budget, chain::plugin_config_exception,
                    "{} ({} us) exceeds producer_plugin's read-only transaction time of {} us",
                    query_engine::option::max_capture_ms, _impl->config.max_capture.count(), read_budget.count());
      else
         _impl->config.max_capture =
            std::min(read_budget, std::chrono::duration_cast<std::chrono::microseconds>(_impl->config.timeout));
   }
   try {
      _impl->config.validate();
   } catch (const query_engine::query_error& error) {
      SYS_THROW(chain::plugin_config_exception, "Invalid query configuration: {}", error.what());
   }
   const auto post_read = [](std::function<void()> callback) {
      app().executor().post(priority::medium_low, exec_queue::read_exclusive, std::move(callback));
   };
   // Without read-only threads the scheduler throws before anything is queued, so the query fails at once
   // with STATE_UNAVAILABLE and its admission is released.
   const auto fail_read = [](std::function<void()>) {
      throw query_engine::query_error(query_engine::error_kind::STATE_UNAVAILABLE, no_read_threads_message,
                                      std::nullopt, query_engine::bound::read_only_threads);
   };
   auto scheduler = has_read_threads ? query_engine::query_read_api::read_scheduler{post_read}
                                     : query_engine::query_read_api::read_scheduler{fail_read};
   auto reads = std::make_shared<query_engine::query_read_api>(
      std::make_shared<query_engine::local_table_source>(controller), std::move(scheduler),
      // Startup runs on the application loop thread before its executor refreshes the thread ID.
      std::this_thread::get_id(),
      // A read-exclusive task runs inside a read window; the window's end bounds the capture like the
      // deadline producer_plugin passes to a read-only transaction.
      [&producer] { return read_window_remaining(producer); });
   auto engine = std::make_shared<query_engine::query_engine>(_impl->config, std::move(reads));
   engine->set_logger(_impl->log);
   _impl->exchange_engine(engine);
   auto* http = app().find_plugin<http_plugin>();
   if (http &&
       (http->get_state() == appbase::abstract_plugin::initialized ||
        http->get_state() == appbase::abstract_plugin::started) &&
       http->is_enabled(api_category::chain_ro)) {
      _impl->http_handler = std::make_shared<query_engine::query_http_handler>(
         engine, [http](size_t size) { return http->reserve_bytes_in_flight(size); });
      http->add_async_api_stream(_impl->http_handler->create_api());
      fc_ilog(_impl->log, "Query HTTP endpoint {} enabled", query_engine::constants::endpoint);
   }
   fc_ilog(_impl->log,
           "Query service enabled; read_mode={}, workers={}, max_in_flight={}, max_capture_us={}, "
           "admitted_memory_bytes={}",
           magic_enum::enum_name(controller.get_read_mode()), _impl->config.worker_threads, _impl->config.max_in_flight,
           _impl->config.max_capture.count(), _impl->config.max_memory_bytes * _impl->config.max_in_flight);
   if (!has_read_threads)
      fc_wlog(_impl->log, "Query service has no {}: every query fails with {}", query_engine::bound::read_only_threads,
              magic_enum::enum_name(query_engine::error_kind::STATE_UNAVAILABLE));
}
void query_engine_plugin::plugin_shutdown() {
   if (auto engine = _impl->exchange_engine(nullptr)) {
      engine->stop();
   }
   if (_impl->http_handler) {
      _impl->http_handler->stop();
      _impl->http_handler.reset();
   }
}
std::shared_ptr<query_engine::query_service> query_engine_plugin::get_query_service() const {
   return _impl->current_engine();
}
std::chrono::microseconds query_engine_plugin::read_window_remaining(const producer_plugin& producer) {
   return std::chrono::microseconds((producer.get_read_only_window_deadline() - fc::time_point::now()).count());
}
void query_engine_plugin::handle_sighup() {
   fc::logger::update(query_engine::constants::logger, _impl->log);
   if (auto engine = _impl->current_engine())
      engine->set_logger(_impl->log);
}
} // namespace sysio

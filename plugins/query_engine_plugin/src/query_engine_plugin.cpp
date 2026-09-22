#include <sysio/chain/exceptions.hpp>
#include <sysio/query_engine_plugin/query_config.hpp>
#include <sysio/query_engine_plugin/query_engine_plugin.hpp>
#include <sysio/query_engine_plugin/query_http_handler.hpp>

#include <magic_enum/magic_enum.hpp>

namespace sysio::query_engine_plugin {
namespace {
constexpr auto producer_option = "producer-name";
} // namespace

/// Service shutdown invalidates its source even while posted cancelled callbacks remain queued.
struct query_engine_plugin::impl {
   fc::logger log;
   query_config config;
   std::atomic<std::shared_ptr<query_engine>> engine;
   std::shared_ptr<query_http_handler> http_handler;
};

query_engine_plugin::query_engine_plugin()
   : _impl(std::make_unique<impl>()) {}
query_engine_plugin::~query_engine_plugin() {
   plugin_shutdown();
}
void query_engine_plugin::set_program_options(options_description&, options_description& cfg) {
   add_options(cfg);
}
void query_engine_plugin::plugin_initialize(const variables_map& options) {
   try {
      _impl->config = parse_config(options);
      SYS_ASSERT(!options.count(producer_option) || options.at(producer_option).as<std::vector<std::string>>().empty(),
                 chain::plugin_config_exception, "Query engine requires a non-producing node");
      handle_sighup();
   } catch (const query_error& error) {
      SYS_THROW(chain::plugin_config_exception, "Invalid query configuration: {}", error.what());
   }
}
void query_engine_plugin::plugin_startup() {
   auto& controller = app().get_plugin<chain_plugin>().chain();
   SYS_ASSERT(controller.get_read_mode() != chain::db_read_mode::SPECULATIVE, chain::plugin_config_exception,
              "Query engine requires head or irreversible read mode");
   auto reads = std::make_shared<query_read_api>(
      std::make_shared<local_table_source>(controller),
      [](std::function<void()> callback) {
         app().executor().post(priority::medium_low, exec_queue::read_only, std::move(callback));
      },
      // Startup runs on the application loop thread before its executor refreshes the thread ID.
      std::this_thread::get_id());
   auto engine = std::make_shared<query_engine>(_impl->config, std::move(reads));
   engine->set_logger(_impl->log);
   _impl->engine.store(engine);
   auto* http = app().find_plugin<http_plugin>();
   if (http &&
       (http->get_state() == appbase::abstract_plugin::initialized ||
        http->get_state() == appbase::abstract_plugin::started) &&
       http->is_enabled(api_category::chain_ro)) {
      _impl->http_handler = std::make_shared<query_http_handler>(engine);
      http->add_async_api(_impl->http_handler->create_api());
      fc_ilog(_impl->log, "Query HTTP endpoint {} enabled", constants::endpoint);
   }
   fc_ilog(_impl->log,
           "Query service enabled; read_mode={}, workers={}, max_in_flight={}, max_capture_ms={}, "
           "admitted_memory_bytes={}",
           magic_enum::enum_name(controller.get_read_mode()), _impl->config.worker_threads, _impl->config.max_in_flight,
           _impl->config.max_capture_ms, _impl->config.max_memory_bytes * _impl->config.max_in_flight);
}
void query_engine_plugin::plugin_shutdown() {
   if (auto engine = _impl->engine.exchange(nullptr)) {
      engine->stop();
   }
   if (_impl->http_handler) {
      _impl->http_handler->stop();
      _impl->http_handler.reset();
   }
}
std::shared_ptr<query_service> query_engine_plugin::get_query_service() const {
   return _impl->engine.load();
}
void query_engine_plugin::handle_sighup() {
   fc::logger::update(constants::logger, _impl->log);
   if (auto engine = _impl->engine.load())
      engine->set_logger(_impl->log);
}
} // namespace sysio::query_engine_plugin

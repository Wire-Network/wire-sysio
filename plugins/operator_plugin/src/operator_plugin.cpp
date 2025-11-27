#include <fc/log/logger.hpp>

#include <sysio/operator_plugin/operator_plugin.hpp>

namespace sysio::operator_plugin {
  static auto _operator_plugin = application::register_plugin<operator_plugin>();

  namespace {
    [[maybe_unused]] inline fc::logger& logger() {
      static fc::logger log{ "operator_plugin" };
      return log;
    }
  }


  void operator_plugin::plugin_initialize(const variables_map& options) {

  }


  void operator_plugin::plugin_startup() {
    ilog("Starting operator plugin");
    auto chain = app().find_plugin<sysio::chain_plugin>();
    auto& controller = chain->chain();
    events.irreversible_block.set_on_subscribe([&] (auto) {
      if (!_irreversible_block_connection) {
        ilog("Subscribing to irreversible block events");
        _irreversible_block_connection.emplace(
         controller.irreversible_block().connect([this](const auto& bsp) {
          events.irreversible_block.publish(bsp);
         }));
      }
    });

    events.irreversible_block.set_on_unsubscribe([&] (auto emitter) {
      if (_irreversible_block_connection && emitter->get_subscriptions().empty()) {
        ilog("Unsubscribing to irreversible block events");
        _irreversible_block_connection.reset();
      }
    });
  }


  void operator_plugin::set_program_options(options_description& cli, options_description& cfg) {
  }


  void operator_plugin::plugin_shutdown() {
    ilog("Shutdown operator plugin");
  }
}

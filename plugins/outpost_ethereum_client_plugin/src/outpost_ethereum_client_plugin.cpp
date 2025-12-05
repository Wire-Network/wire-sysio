#include <fc/log/logger.hpp>

#include <sysio/outpost_ethereum_client_plugin.hpp>

namespace sysio {
  static auto _outpost_ethereum_client_plugin = application::register_plugin<outpost_ethereum_client_plugin>();

  namespace {
    inline fc::logger& logger() {
      static fc::logger log{ "outpost_ethereum_client_plugin" };
      return log;
    }
  }


  void outpost_ethereum_client_plugin::plugin_initialize(const variables_map& options) {
    
  }


  void outpost_ethereum_client_plugin::plugin_startup() {
    ilog("Starting outpost client plugin");
    auto chain = app().find_plugin<sysio::chain_plugin>();
    // auto& controller = chain->chain();
    // events.irreversible_block.set_on_subscribe([&] (auto) {
    //   if (!_irreversible_block_connection) {
    //     ilog("Subscribing to irreversible block events");
    //     _irreversible_block_connection.emplace(
    //      controller.irreversible_block().connect([this](const auto& bsp) {
    //       events.irreversible_block.publish(bsp);
    //      }));
    //   }
    // });
    //
    // events.irreversible_block.set_on_unsubscribe([&] (auto emitter) {
    //   if (_irreversible_block_connection && emitter->get_subscriptions().empty()) {
    //     ilog("Unsubscribing to irreversible block events");
    //     _irreversible_block_connection.reset();
    //   }
    // });
  }


  void outpost_ethereum_client_plugin::set_program_options(options_description& cli, options_description& cfg) {
  }


  void outpost_ethereum_client_plugin::plugin_shutdown() {
    ilog("Shutdown outpost client plugin");
  }
}

#include <fc/log/logger.hpp>

#include <sysio/batch_operator_plugin/batch_operator_plugin.hpp>

namespace sysio {
namespace {
[[maybe_unused]] auto _batch_operator_plugin = application::register_plugin<batch_operator_plugin>();

inline fc::logger &logger() {
  static fc::logger log{"batch_operator_plugin"};
  return log;
}
} // namespace

void batch_operator_plugin::set_program_options(options_description &cli,
                                                options_description &cfg) {}

void batch_operator_plugin::plugin_initialize(const variables_map &options) {}

void batch_operator_plugin::plugin_startup() {
  ilog("Starting batch operator plugin");
  auto &op = app().get_plugin<operator_plugin>();
  op.events.irreversible_block.subscribe(
      [&](const chain::block_signal_params &bsp) {
        ilog("irreversible block");
      });
}

void batch_operator_plugin::plugin_shutdown() {}
} // namespace sysio

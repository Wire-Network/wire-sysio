#include <sysio/sub_chain_plugin/sub_chain_plugin.hpp>
#include <sysio/http_plugin/http_plugin.hpp>
#include <sysio/chain_plugin/chain_plugin.hpp>

#include <boost/signals2/connection.hpp>
#include <fc/log/logger.hpp>
#include <vector>
#include <variant>
#include <sysio/chain/merkle.hpp>
#include <fc/io/raw.hpp>
#include <fc/log/logger_config.hpp>

namespace sysio {
static auto _sub_chain_plugin = application::register_plugin<sub_chain_plugin>();

using namespace chain;

sub_chain_plugin::sub_chain_plugin() {}

sub_chain_plugin::~sub_chain_plugin() {
}

void sub_chain_plugin::set_program_options(options_description&, options_description& cfg) {
}

void sub_chain_plugin::plugin_initialize(const variables_map& options) {
   try {

   } FC_LOG_AND_RETHROW()
}
void sub_chain_plugin::plugin_startup() {
}

void sub_chain_plugin::plugin_shutdown() {
    // Cleanup code
}
} // namespace sysio
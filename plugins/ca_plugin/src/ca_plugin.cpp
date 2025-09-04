#include <sysio/ca_plugin/ca_plugin.hpp>
#include <fc/log/logger.hpp>
#include <boost/program_options.hpp>

using namespace appbase;
using namespace boost::program_options;

namespace sysio {

ca_plugin::ca_plugin() {}
ca_plugin::~ca_plugin() {}

void ca_plugin::set_program_options(options_description& cli, options_description& cfg) {
  options_description opts("CA Plugin");
  opts.add_options()
    ("ca-key-path", bpo::value<std::string>(), "Path to CA private key (PEM)")
    ("ca-cert-path", bpo::value<std::string>(), "Path to CA certificate (PEM)");
  cli.add(opts);
  cfg.add(opts);
}

void ca_plugin::plugin_initialize(const variables_map& options) {
  ilog("ca_plugin initialized");
}

void ca_plugin::plugin_startup() {
  ilog("ca_plugin started");
}

void ca_plugin::plugin_shutdown() {
  ilog("ca_plugin shutdown");
}

}

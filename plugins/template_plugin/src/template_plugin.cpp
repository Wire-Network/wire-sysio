#include <sysio/template_plugin/template_plugin.hpp>

namespace sysio {

struct template_plugin::impl {
};

template_plugin::template_plugin()
   : _impl(std::make_unique<impl>()) {}

template_plugin::~template_plugin() = default;

void template_plugin::set_program_options(options_description& cli,
                                           options_description& cfg) {
}

void template_plugin::plugin_initialize(const variables_map& options) {
   try {

   }
   FC_LOG_AND_RETHROW()
}

void template_plugin::plugin_startup() {
}

void template_plugin::plugin_shutdown() {
}

} // namespace sysio

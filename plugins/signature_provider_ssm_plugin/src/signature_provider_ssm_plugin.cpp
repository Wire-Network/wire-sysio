#include <sysio/signature_provider_ssm_plugin/signature_provider_ssm_plugin.hpp>
#include <sysio/signature_provider_ssm_plugin/ssm_signature_provider.hpp>

namespace sysio {

namespace {
/// The `<provider-type>` scheme this plugin supplies.
constexpr std::string_view ssm_spec_scheme = "SSM";
} // namespace

signature_provider_ssm_plugin::signature_provider_ssm_plugin() {
   // Register the SSM handler at construction. appbase constructs every registered plugin before it initializes any, so
   // the manager sees this handler at its own plugin_initialize and creates the configured SSM providers there --
   // before any consumer -- if this plugin is enabled via --plugin. Registering is cheap and side-effect-free (no AWS,
   // no network): the SecureString fetch happens only when the manager actually creates a provider from an SSM: spec.
   sigprov::register_scheme_handler(ssm_spec_scheme, &sysio::sigprov::ssm::create_ssm_provider, name());
}

signature_provider_ssm_plugin::~signature_provider_ssm_plugin() = default;

void signature_provider_ssm_plugin::set_program_options(options_description&, options_description&) {}

void signature_provider_ssm_plugin::plugin_initialize(const variables_map&) {}

void signature_provider_ssm_plugin::plugin_startup() {}

void signature_provider_ssm_plugin::plugin_shutdown() {}

} // namespace sysio

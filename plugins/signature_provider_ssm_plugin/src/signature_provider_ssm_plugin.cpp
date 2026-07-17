#include <sysio/signature_provider_ssm_plugin/signature_provider_ssm_plugin.hpp>
#include <sysio/signature_provider_ssm_plugin/ssm_signature_provider.hpp>

namespace sysio {

namespace {
/// The `<provider-type>` scheme this plugin supplies.
constexpr auto ssm_spec_scheme = "SSM";
} // namespace

signature_provider_ssm_plugin::signature_provider_ssm_plugin() {
   // Constructor-time announcement: appbase constructs every registered
   // plugin before any plugin_initialize runs, so this executes even when the
   // plugin is never enabled -- which is when it matters, letting the
   // manager's unclaimed-spec boot error name the missing `plugin =` line.
   // Error-text only; the handler registration happens in plugin_initialize.
   sigprov::announce_scheme_plugin(ssm_spec_scheme, name());
}

signature_provider_ssm_plugin::~signature_provider_ssm_plugin() = default;

void signature_provider_ssm_plugin::set_program_options(options_description&, options_description&) {}

void signature_provider_ssm_plugin::plugin_initialize(const variables_map&) {
   // Dependency-first initialization guarantees the manager has already
   // parsed --signature-provider and retained any SSM: specs; register the
   // scheme, then claim those specs. The claim performs the one-time
   // SecureString fetch per spec -- a fetch or validation failure throws out
   // of this plugin's initialize and aborts boot with the AWS error.
   auto& manager = app().get_plugin<signature_provider_manager_plugin>();
   manager.register_spec_handler(ssm_spec_scheme, &sysio::sigprov::ssm::create_ssm_provider);
   manager.create_configured_providers(ssm_spec_scheme);
}

void signature_provider_ssm_plugin::plugin_startup() {}

void signature_provider_ssm_plugin::plugin_shutdown() {}

} // namespace sysio

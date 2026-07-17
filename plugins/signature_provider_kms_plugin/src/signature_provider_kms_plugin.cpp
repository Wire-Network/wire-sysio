#include <sysio/signature_provider_kms_plugin/signature_provider_kms_plugin.hpp>
#include <sysio/signature_provider_kms_plugin/kms_signature_provider.hpp>

namespace sysio {

namespace {
/// The `<provider-type>` scheme this plugin supplies.
constexpr auto kms_spec_scheme = "KMS";
} // namespace

signature_provider_kms_plugin::signature_provider_kms_plugin() {
   // Constructor-time announcement: appbase constructs every registered
   // plugin before any plugin_initialize runs, so this executes even when the
   // plugin is never enabled -- which is when it matters, letting the
   // manager's unclaimed-spec boot error name the missing `plugin =` line.
   // Error-text only; the handler registration happens in plugin_initialize.
   sigprov::announce_scheme_plugin(kms_spec_scheme, name());
}

signature_provider_kms_plugin::~signature_provider_kms_plugin() = default;

void signature_provider_kms_plugin::set_program_options(options_description&, options_description&) {}

void signature_provider_kms_plugin::plugin_initialize(const variables_map&) {
   // Dependency-first initialization guarantees the manager has already
   // parsed --signature-provider and retained any KMS: specs; register the
   // scheme, then claim those specs. Claiming stays offline (client
   // construction resolves no credentials and touches no network); each
   // claimed provider attaches its startup probe via provider_spec_result,
   // which the manager runs unconditionally at its plugin_startup -- see the
   // class doc for why there is no skip flag.
   auto& manager = app().get_plugin<signature_provider_manager_plugin>();
   manager.register_spec_handler(kms_spec_scheme, &sysio::sigprov::kms::create_kms_provider);
   manager.create_configured_providers(kms_spec_scheme);
}

void signature_provider_kms_plugin::plugin_startup() {}

void signature_provider_kms_plugin::plugin_shutdown() {}

} // namespace sysio

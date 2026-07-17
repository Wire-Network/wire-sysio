#include <sysio/signature_provider_kms_plugin/signature_provider_kms_plugin.hpp>
#include <sysio/signature_provider_kms_plugin/kms_signature_provider.hpp>

namespace sysio {

namespace {
/// The `<provider-type>` scheme this plugin supplies.
constexpr std::string_view kms_spec_scheme = "KMS";
} // namespace

signature_provider_kms_plugin::signature_provider_kms_plugin() {
   // Register the KMS handler at construction. appbase constructs every
   // registered plugin before it initializes any, so the manager sees this
   // handler at its own plugin_initialize and creates the configured KMS
   // providers there -- before any consumer -- if this plugin is enabled via
   // --plugin. Registering is cheap and side-effect-free: KMS client
   // construction is offline, and each created provider attaches its startup
   // probe via provider_spec_result, which the manager runs at its startup.
   sigprov::register_scheme_handler(kms_spec_scheme, &sysio::sigprov::kms::create_kms_provider, name());
}

signature_provider_kms_plugin::~signature_provider_kms_plugin() = default;

void signature_provider_kms_plugin::set_program_options(options_description&, options_description&) {}

void signature_provider_kms_plugin::plugin_initialize(const variables_map&) {}

void signature_provider_kms_plugin::plugin_startup() {}

void signature_provider_kms_plugin::plugin_shutdown() {}

} // namespace sysio

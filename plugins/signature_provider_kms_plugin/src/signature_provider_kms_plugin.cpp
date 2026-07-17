#include <sysio/signature_provider_kms_plugin/signature_provider_kms_plugin.hpp>
#include <sysio/signature_provider_kms_plugin/kms_signature_provider.hpp>

namespace sysio {

namespace {
/// The `<provider-type>` scheme this plugin supplies.
constexpr auto kms_spec_scheme = "KMS";

/// Opt-in flag: when set, this plugin enables the manager's startup-probe
/// pass, so every claimed `KMS:` key is validated at startup with a (free)
/// `GetPublicKey` call. Lives on this plugin -- not the manager -- because
/// KMS is the only probe-attaching provider; the manager keeps only the
/// generic run-probes-at-startup hook.
constexpr auto option_name_kms_startup_check = "signature-provider-kms-startup-check";
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

void signature_provider_kms_plugin::set_program_options(options_description&, options_description& cfg) {
   cfg.add_options()(
      option_name_kms_startup_check,
      boost::program_options::value<bool>()->default_value(false),
      "Probe every KMS: signing key at startup with a (free) GetPublicKey call "
      "so a credentials / region / IAM / pinned-key misconfiguration fails at "
      "boot instead of on the first sign. Off by default. Only effective when "
      "this plugin is enabled.");
}

void signature_provider_kms_plugin::plugin_initialize(const variables_map& options) {
   // Dependency-first initialization guarantees the manager has already
   // parsed --signature-provider and retained any KMS: specs; register the
   // scheme, then claim those specs. Claiming stays offline (client
   // construction resolves no credentials and touches no network); each
   // claimed provider attaches its startup probe via provider_spec_result,
   // and enabling the probe pass below is what makes the manager run them.
   auto& manager = app().get_plugin<signature_provider_manager_plugin>();
   manager.register_spec_handler(kms_spec_scheme, &sysio::sigprov::kms::create_kms_provider);
   manager.create_configured_providers(kms_spec_scheme);

   if (options.contains(option_name_kms_startup_check) &&
       options.at(option_name_kms_startup_check).as<bool>()) {
      manager.enable_startup_probes();
   }
}

void signature_provider_kms_plugin::plugin_startup() {}

void signature_provider_kms_plugin::plugin_shutdown() {}

} // namespace sysio

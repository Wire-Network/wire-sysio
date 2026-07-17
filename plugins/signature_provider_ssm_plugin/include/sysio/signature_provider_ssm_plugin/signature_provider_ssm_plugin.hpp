#pragma once

#include <sysio/signature_provider_manager_plugin/signature_provider_manager_plugin.hpp>

namespace sysio {

using namespace appbase;

/**
 * Provides the `SSM:<param-ref>` signature-provider scheme: the private key is fetched once from AWS SSM Parameter
 * Store (a KMS-encrypted `SecureString`) while this plugin initializes, and signing is local thereafter -- `KEY:`
 * semantics without the key material ever appearing in config files, so it suits every signing path including producer
 * block signing. Enable with
 *
 *   plugin = sysio::signature_provider_ssm_plugin
 *
 * The constructor registers the `SSM` handler with the manager (via `sigprov::register_scheme_handler`); the manager
 * creates the configured `SSM:` providers at its own init when this plugin is enabled, so the one-time Parameter Store
 * fetch happens there and a fetch/validation failure aborts boot with the AWS error. `plugin_initialize` is empty. See
 * `ssm_signature_provider.hpp` for the provider machinery and `test/README.md` for the operator runbook.
 */
class signature_provider_ssm_plugin : public appbase::plugin<signature_provider_ssm_plugin> {
public:
   signature_provider_ssm_plugin();
   virtual ~signature_provider_ssm_plugin();

   APPBASE_PLUGIN_REQUIRES((signature_provider_manager_plugin))
   virtual void set_program_options(options_description&, options_description& cfg) override;

   void plugin_initialize(const variables_map& options);
   void plugin_startup();
   void plugin_shutdown();
};

} // namespace sysio

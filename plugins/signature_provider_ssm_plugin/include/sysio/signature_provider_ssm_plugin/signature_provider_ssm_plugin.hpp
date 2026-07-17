#pragma once

#include <sysio/signature_provider_manager_plugin/signature_provider_manager_plugin.hpp>

namespace sysio {

using namespace appbase;

/**
 * Provides the `SSM:<param-ref>` signature-provider scheme: the private key
 * is fetched once from AWS SSM Parameter Store (a KMS-encrypted
 * `SecureString`) while this plugin initializes, and signing is local
 * thereafter -- `KEY:` semantics without the key material ever appearing in
 * config files, so it suits every signing path including producer block
 * signing. Enable with
 *
 *   plugin = sysio::signature_provider_ssm_plugin
 *
 * `plugin_initialize` registers the `SSM` spec handler with the manager and
 * then claims the configured `SSM:` specs (the one-time Parameter Store
 * fetch happens inside that claim; a fetch or validation failure aborts boot
 * here, attributed to this plugin). The constructor announces the scheme so
 * the manager's unclaimed-spec boot error can name this plugin -- and the
 * exact `plugin =` line to add -- even when it is registered but not
 * enabled. See `ssm_signature_provider.hpp` for the provider machinery and
 * `test/README.md` for the operator runbook.
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

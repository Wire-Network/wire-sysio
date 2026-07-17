#pragma once

#include <sysio/signature_provider_manager_plugin/signature_provider_manager_plugin.hpp>

namespace sysio {

using namespace appbase;

/**
 * Provides the `KMS:<key-ref>` signature-provider scheme: every signature is
 * a remote AWS KMS `Sign` call, so the private key never exists on the host
 * or in process memory. Scope is secp256k1/ethereum keys only (the provider
 * hard-rejects other key types), and the per-signature network round-trip
 * (typically 30-100 ms) makes it unsuitable for block production -- which
 * ethereum-only scoping already rules out structurally. Enable with
 *
 *   plugin = sysio::signature_provider_kms_plugin
 *
 * `plugin_initialize` registers the `KMS` spec handler with the manager and
 * then claims the configured `KMS:` specs. Claiming stays offline; every
 * claimed key attaches a startup probe -- a (free) `GetPublicKey` call the
 * manager runs at startup -- so a credentials / region / IAM / pinned-key
 * misconfiguration fails at boot instead of on the first sign. There is
 * deliberately no flag to skip the probe: a transient AWS error at startup
 * is logged and deferred to the lazy first-sign check (a blip never blocks
 * a boot), and a permanent one means the configured signer could never
 * sign. The constructor announces the scheme so the manager's
 * unclaimed-spec boot error can name this plugin -- and the exact
 * `plugin =` line to add -- even when it is registered but not enabled. See
 * `kms_signature_provider.hpp` for the provider machinery and
 * `test/README.md` for the operator runbook.
 */
class signature_provider_kms_plugin : public appbase::plugin<signature_provider_kms_plugin> {
public:
   signature_provider_kms_plugin();
   virtual ~signature_provider_kms_plugin();

   APPBASE_PLUGIN_REQUIRES((signature_provider_manager_plugin))
   virtual void set_program_options(options_description&, options_description& cfg) override;

   void plugin_initialize(const variables_map& options);
   void plugin_startup();
   void plugin_shutdown();
};

} // namespace sysio

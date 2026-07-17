#pragma once
#include <boost/program_options/options_description.hpp>
#include <sysio/chain/application.hpp>
#include <sysio/http_client_plugin/http_client_plugin.hpp>
#include <fc/crypto/chain_types_reflect.hpp>
#include <fc/crypto/private_key.hpp>
#include <fc/crypto/public_key.hpp>
#include <fc/crypto/signature.hpp>
#include <fc/crypto/signature_provider.hpp>

#include <string>
#include <string_view>

namespace sysio {

using namespace appbase;

/**
 * Result of building a signing provider from a `<provider-type>:<spec_data>`
 * spec body.
 *
 *   - `signer`: the signing closure (required).
 *   - `private_key`: a local private key, if the scheme exposes one
 *     (e.g. `KEY:`); empty for remote signers (`KIOD:`, `KMS:`, ...).
 *   - `startup_probe`: an optional one-shot callback the plugin runs from
 *     `plugin_startup()` after the provider has been successfully inserted
 *     into the registry -- e.g. a KMS `GetPublicKey` credential check. The
 *     plugin defers appending the probe until insertion succeeds, so a
 *     provider rejected as a duplicate leaves no orphan probe behind.
 */
struct provider_spec_result {
   fc::crypto::sign_fn                    signer;
   std::optional<fc::crypto::private_key> private_key;
   std::function<void()>                  startup_probe;  ///< empty if not applicable
};

/**
 * Handler invoked when the plugin encounters an unrecognised
 * `<provider-type>:` scheme in a signature-provider spec. The handler owns
 * parsing of its own `spec_data` and returns a built `provider_spec_result`.
 *
 * Register handlers via
 * `signature_provider_manager_plugin::register_spec_handler(scheme, handler)`.
 * The packaged way to ship a scheme is a provider plugin (e.g.
 * `signature_provider_ssm_plugin`) that registers from its
 * `plugin_initialize` and then claims its configured specs via
 * `create_configured_providers(scheme)`; host applications that do not use
 * the plugins may instead register from `main()` before
 * `app().initialize(...)`. The built-in `KEY:` and `KIOD:` schemes are not
 * registrable -- they are handled directly by the plugin.
 */
using spec_handler = std::function<provider_spec_result(
   fc::crypto::chain_key_type_t,
   const fc::crypto::public_key&,
   std::string_view /*spec_data*/)>;

/**
 * Redact any inline private key from a signature-provider spec so it can be safely logged.
 *
 * A spec has the form `<name>,<chain-kind>,<key-type>,<public-key>,<provider>`, where the final
 * comma-separated field is the provider. Only a `KEY:<private-key>` provider embeds secret key material;
 * `KIOD:`/other providers reference external material and malformed specs carry no inline secret, so both are
 * returned unchanged. When the provider field is `KEY:...`, everything after the `KEY:` marker is replaced
 * with `<redacted>`. Only the final field is inspected, so a `KEY:`-prefixed name never triggers redaction.
 *
 * @param spec the signature-provider spec, exactly as supplied to `--signature-provider`.
 * @return a copy of @p spec with any inline private key masked.
 */
inline std::string redact_signature_provider_spec(const std::string& spec) {
   constexpr std::string_view key_provider_prefix = "KEY:";
   const std::string::size_type last_comma = spec.rfind(',');
   const std::string::size_type provider_start = (last_comma == std::string::npos) ? 0 : last_comma + 1;
   const std::string_view provider{spec.data() + provider_start, spec.size() - provider_start};
   if (provider.substr(0, key_provider_prefix.size()) == key_provider_prefix) {
      return spec.substr(0, provider_start + key_provider_prefix.size()) + "<redacted>";
   }
   return spec;
}

/**
 * Plugin responsible for managing signature providers.
 * 
 * This plugin handles the creation, management and querying of signature providers
 * which are used for signing transactions and messages. It supports multiple chain
 * types and key formats, allowing flexible key management across different blockchain
 * protocols.
 * 
 * Key features:
 * - Creation of signature providers from specifications
 * - Management of named and anonymous providers
 * - Query interface for finding providers by various criteria
 * - Support for multiple chain types and key formats
 *
 * Concurrency contract: the provider set is IMMUTABLE AFTER STARTUP. Every
 * mutator (`create_provider`, `register_spec_handler`,
 * `create_configured_providers`, `register_default_signature_providers`)
 * asserts it is called before this plugin has started; all mutation happens
 * on the main thread during the sequential initialize/startup phases.
 * Runtime access from other threads is read-only and therefore needs no
 * synchronization.
 */
class signature_provider_manager_plugin : public appbase::plugin<signature_provider_manager_plugin> {
public:
   constexpr static auto option_name_provider = "signature-provider";
   signature_provider_manager_plugin();
   virtual ~signature_provider_manager_plugin();

   APPBASE_PLUGIN_REQUIRES((http_client_plugin))
   virtual void set_program_options(boost::program_options::options_description&,
                                    boost::program_options::options_description& cfg) override;

   void plugin_initialize(const variables_map& options);

   /**
    * Plugin startup hook. Two passes, in order:
    *
    * 1. Unclaimed-spec accounting: every `--signature-provider` spec whose
    *    scheme had no registered handler at parse time was retained (not
    *    thrown); by startup each such spec must have been claimed by its
    *    provider plugin via `create_configured_providers()`. Any spec still
    *    unclaimed aborts startup with an error naming the plugin that
    *    provides the scheme (when announced -- see
    *    `sigprov::announce_scheme_plugin`) and the exact `plugin =` line to
    *    add. `register_default_signature_providers()` runs the same check
    *    earlier, during chain_plugin's initialize, so a doomed boot cannot
    *    generate and persist anonymous default keys as a side effect.
    *
    * 2. The startup-probe pass: any provider whose `spec_handler` supplied a
    *    `startup_probe` in its `provider_spec_result` (today this is only
    *    the KMS handler, which issues a free `GetPublicKey` to validate
    *    credentials, region, IAM, and pinned key) has that probe invoked
    *    here. Aborts startup loudly on permanent misconfiguration; transient
    *    errors are logged and deferred to the first sign, so an AWS blip at
    *    restart never blocks a boot. Attaching a probe IS the opt-in --
    *    there is deliberately no enable flag. A no-op when no probes were
    *    attached.
    */
   void plugin_startup();
   void plugin_shutdown() {}

   const char* signature_provider_help_text() const;

   /**
    * Create a known signature provider from a specification string.
    *
    * @param spec A string specification in the format "key_name:chain:key_type:public_key:private_key_spec"
    * @return A signature provider pointer that can be used for signing operations
    * @throws fc::exception if the specification is invalid or provider creation fails
    */
   fc::crypto::signature_provider_ptr create_provider(const std::string& spec);

   /**
    * Create a signature provider with explicit parameters.
    *
    * @param key_name Name to identify the provider
    * @param target_chain The target blockchain protocol (e.g., sys, eth)
    * @param key_type The type of key (e.g., k1, r1)
    * @param public_key_text The public key in text format
    * @param private_key_provider_spec The private key or provider specification
    * @return A signature provider pointer that can be used for signing operations
    * @throws fc::exception if provider creation fails with given parameters
    */
   fc::crypto::signature_provider_ptr
   create_provider(
      const std::string& key_name,
      fc::crypto::chain_kind_t target_chain,
      fc::crypto::chain_key_type_t key_type,
      const std::string& public_key_text,
      const std::string& private_key_provider_spec
      );

   /**
    * Create a signature provider for use by the caller and only the caller
    *
    * @param priv private key to use
    * @return an anonymous signature provider
    */
   fc::crypto::sign_fn create_anonymous_provider_from_private_key(fc::crypto::private_key priv) const;

   /**
    * Check for the existence of a provider
    *
    * @param key public key or key name to check for existence of a signature provider
    * @return true if provider exists, false otherwise
    */
   bool has_provider(const fc::crypto::signature_provider_id_t& key);

   /**
    * Get a provider by public key or key name
    *
    * @param key to lookup provider
    * @return provider if found, otherwise assert/throw
    */
   fc::crypto::signature_provider_ptr get_provider(const fc::crypto::signature_provider_id_t& key);

   /**
    * List all existing registered signature providers,
    * with both as `pair<key_name,public_key>`
    *
    * @return list of available signature providers
    */
   std::vector<fc::crypto::signature_provider_ptr> query_providers(
      const std::optional<fc::crypto::signature_provider_id_t>& id_opt = std::nullopt,
      std::optional<fc::crypto::chain_kind_t> target_chain = std::nullopt,
      std::optional<fc::crypto::chain_key_type_t> target_key_type = std::nullopt
      );

   template <std::ranges::input_range R>
   bool has_signature_providers(const R& range) {
      return std::ranges::all_of(range, [&](auto& criteria) {
         if constexpr (std::is_same_v<std::decay_t<decltype(criteria)>, fc::crypto::signature_provider_id_t>) {
            return !query_providers(criteria).empty();
         }

         if constexpr (std::is_same_v<std::decay_t<decltype(criteria)>, fc::crypto::chain_kind_t>) {
            return !query_providers(std::nullopt, criteria, std::nullopt).empty();
         }

         if constexpr (std::is_same_v<std::decay_t<decltype(criteria)>, fc::crypto::chain_key_type_t>) {
            return !query_providers(std::nullopt, std::nullopt, criteria).empty();
         }

         FC_ASSERT(false, "Invalid criteria type");
      });
   }

   void register_default_signature_providers(
      const std::vector<fc::crypto::chain_key_type_t>& key_types);

   /**
    * Register a handler for an unrecognised `<provider-type>:` scheme.
    *
    * Built-in schemes (`KEY:`, `KIOD:`) are not registrable -- attempting to
    * register them throws. Each non-built-in scheme may be registered at
    * most once. Registration is a pure map insert with no side effects; it
    * does NOT create providers for retained specs -- pair it with
    * `create_configured_providers()` for that. Two supported call sites:
    *
    *   - A provider plugin's `plugin_initialize` (the packaged path):
    *       auto& mgr = app().get_plugin<signature_provider_manager_plugin>();
    *       mgr.register_spec_handler("SSM", &sysio::sigprov::ssm::create_ssm_provider);
    *       mgr.create_configured_providers("SSM");
    *
    *   - A host application's `main()` BEFORE `app().initialize(...)`, so the
    *     handler is already in place when `plugin_initialize()` parses each
    *     `--signature-provider` option and every matching spec creates
    *     eagerly (no claim step needed):
    *       auto& plugin = app()._register_plugin<signature_provider_manager_plugin>();
    *       plugin.register_spec_handler(
    *          "KMS", &sysio::sigprov::kms::create_kms_provider);
    *
    * `register_plugin<>` (static, used elsewhere in main()) only enqueues
    * the plugin name in the static registration list; it does NOT
    * construct the plugin instance. Use `_register_plugin<>` (instance
    * method, idempotent) to construct or fetch the instance before calling
    * `register_spec_handler`.
    *
    * @param scheme  the `<provider-type>` token (e.g. "KMS"); case-sensitive
    * @param handler the parser/builder for `scheme:<spec_data>` specs
    * @throws fc::exception if `scheme` is empty, names a built-in, has
    *         already been registered, or `handler` is empty
    */
   void register_spec_handler(std::string scheme, sysio::spec_handler handler);

   /**
    * Create providers for every retained `--signature-provider` spec of
    * @p scheme, consuming those specs from the retained store.
    *
    * `plugin_initialize()` retains (rather than throws on) any configured
    * spec whose scheme has no registered handler at parse time -- the
    * provider plugins that supply those schemes initialize AFTER this
    * plugin, since they depend on it. Each provider plugin claims its own
    * scheme's specs by calling this immediately after its
    * `register_spec_handler()` call. Creation runs the full
    * `create_provider()` path, so validation and errors are identical to an
    * eagerly-created spec; a creation failure (bad pubkey, provider fetch
    * failure, duplicate name) propagates out of the calling plugin's
    * `plugin_initialize` and aborts boot there, attributing the error to
    * the plugin that owns the scheme.
    *
    * Anonymous retained specs draw their `key-N` names when claimed, so
    * numbering can interleave differently than pure config order;
    * deterministic for a given config, and names were never contractual.
    *
    * Specs still unclaimed after the initialize phase fail the boot -- see
    * `plugin_startup()`.
    *
    * @param scheme the `<provider-type>` token whose retained specs to create
    * @return the created providers (empty if no spec of @p scheme was retained)
    */
   std::vector<fc::crypto::signature_provider_ptr> create_configured_providers(std::string_view scheme);

private:
   /**
    * Enforce the immutable-after-startup contract at the mutator boundary:
    * throws `plugin_config_exception` unless this plugin is still in the
    * `registered` or `initialized` state. A future runtime spec-reload
    * feature must remove this (and add the synchronization the containers
    * currently need none of) rather than work around it.
    *
    * @param operation the public mutator's name, for the error message
    */
   void assert_pre_startup(std::string_view operation) const;

   std::unique_ptr<class signature_provider_manager_plugin_impl> my;
};

namespace sigprov {

/**
 * Record that @p plugin_name is the plugin providing `<scheme>:` specs.
 *
 * Call from a provider plugin's constructor: appbase constructs every
 * registered plugin before any `plugin_initialize` runs, so announcements are
 * present even for plugins that are never enabled -- which is exactly when
 * they matter, letting the manager's unclaimed-spec boot error name the
 * `plugin =` line the operator forgot. Process-wide (a static registry, NOT
 * per-application state -- the mapping is a fact about the binary) and
 * idempotent via insert-or-assign because test binaries construct plugin
 * objects repeatedly across scoped_app instances. Deliberately
 * unsynchronized: writes (plugin construction) and reads (boot diagnostics)
 * all happen on the main thread in sequential appbase lifecycle phases. Used
 * ONLY to improve error text; never for control flow.
 *
 * @param scheme      the `<provider-type>` token (e.g. "SSM"); case-sensitive
 * @param plugin_name the providing plugin's demangled name as `--plugin`
 *                    expects it (e.g. "sysio::signature_provider_ssm_plugin")
 */
void announce_scheme_plugin(std::string_view scheme, std::string_view plugin_name);

/**
 * The plugin previously announced for @p scheme, if any.
 *
 * @param scheme the `<provider-type>` token to look up
 * @return the announced plugin name, or nullopt if none was announced
 */
std::optional<std::string> announced_scheme_plugin(std::string_view scheme);

} // namespace sigprov

/**
 * @brief Get a signature provider
 *
 * if exactly 1 provider matches, it will be returned, otherwise throws
 *
 * @param id
 * @param target_chain
 * @return if exactly 1 provider matches, it will be returned, otherwise throws
 */
fc::crypto::signature_provider_ptr get_signature_provider(
   const fc::crypto::signature_provider_id_t& id,
   std::optional<fc::crypto::chain_kind_t> target_chain = std::nullopt
   );

/**
 * @brief Query signature providers
 *
 * @param id_opt
 * @param target_chain
 * @param target_key_type
 * @return list of matching signature providers
 */
std::vector<fc::crypto::signature_provider_ptr> query_signature_providers(
   const std::optional<fc::crypto::signature_provider_id_t>& id_opt = std::nullopt,
   std::optional<fc::crypto::chain_kind_t> target_chain = std::nullopt,
   std::optional<fc::crypto::chain_key_type_t> target_key_type = std::nullopt
   );




}
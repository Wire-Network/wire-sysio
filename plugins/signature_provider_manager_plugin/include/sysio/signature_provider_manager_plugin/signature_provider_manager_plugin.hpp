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
 * Result of building a signing provider from a `<provider-type>:<spec_data>` spec body.
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
 * Handler invoked when the plugin encounters an unrecognised `<provider-type>:` scheme in a signature-provider spec.
 * The handler owns parsing of its own `spec_data` and returns a built `provider_spec_result`.
 *
 * The packaged way to ship a scheme is a provider plugin (e.g. `signature_provider_ssm_plugin`) that registers its
 * handler from its CONSTRUCTOR via `sigprov::register_scheme_handler` -- the manager then creates the configured
 * providers at its own init, gated on the plugin being enabled via `--plugin`. Host applications that do not use the
 * plugins may instead register directly on the manager instance via
 * `signature_provider_manager_plugin::register_spec_handler(scheme, handler)` (ungated). The built-in `KEY:` and
 * `KIOD:` schemes are not registrable -- they are handled directly by the plugin.
 */
using spec_handler = std::function<provider_spec_result(
   fc::crypto::chain_key_type_t,
   const fc::crypto::public_key&,
   std::string_view /*spec_data*/)>;

/**
 * Redact any inline private key from a signature-provider spec so it can be safely logged.
 *
 * A spec has the form `<name>,<chain-kind>,<key-type>,<public-key>,<provider>`, where the final comma-separated field
 * is the provider. Only a `KEY:<private-key>` provider embeds secret key material; `KIOD:`/other providers reference
 * external material and malformed specs carry no inline secret, so both are returned unchanged. When the provider field
 * is `KEY:...`, everything after the `KEY:` marker is replaced with `<redacted>`. Only the final field is inspected, so
 * a `KEY:`-prefixed name never triggers redaction.
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
 * This plugin handles the creation, management and querying of signature providers which are used for signing
 * transactions and messages. It supports multiple chain types and key formats, allowing flexible key management across
 * different blockchain protocols.
 * 
 * Key features:
 * - Creation of signature providers from specifications
 * - Management of named and anonymous providers
 * - Query interface for finding providers by various criteria
 * - Support for multiple chain types and key formats
 *
 * Concurrency contract: the provider set is IMMUTABLE AFTER STARTUP. Every mutator (`create_provider`,
 * `register_spec_handler`, `register_default_signature_providers`) asserts it is called before this plugin has started;
 * all mutation happens on the main thread during the sequential initialize/startup phases. Runtime access from other
 * threads is read-only and therefore needs no synchronization.
 */
class signature_provider_manager_plugin : public appbase::plugin<signature_provider_manager_plugin> {
public:
   constexpr static auto option_name_provider = "signature-provider";
   signature_provider_manager_plugin();
   virtual ~signature_provider_manager_plugin();

   APPBASE_PLUGIN_REQUIRES((http_client_plugin))
   virtual void set_program_options(boost::program_options::options_description&,
                                    boost::program_options::options_description& cfg) override;

   /**
    * Plugin initialize hook. Creates every configured `--signature-provider` provider eagerly and synchronously:
    *
    *   - `KEY:` / `KIOD:` are built in and handled directly.
    *   - An extension scheme (`SSM:` / `KMS:`) is resolved through a handler
    *     the providing plugin registered at CONSTRUCTION via
    *     `sigprov::register_scheme_handler` -- available here because appbase
    *     constructs all plugins before initializing any. The scheme is honored
    *     only when its plugin is enabled via `--plugin`; a spec whose plugin
    *     is linked but not enabled aborts init with the exact `plugin =` line
    *     to add, and an unknown scheme aborts with a "no plugin provides this"
    *     error.
    *
    * Because every provider consumer `APPBASE_PLUGIN_REQUIRES` this plugin, this init runs before any of them -- so the
    * full provider set exists by the time chain_plugin / producer_plugin / the outposts look, independent of `--plugin`
    * ordering.
    */
   void plugin_initialize(const variables_map& options);

   /**
    * Plugin startup hook.
    *
    * The startup-probe pass: any provider whose `spec_handler` supplied a `startup_probe` in its `provider_spec_result`
    * (today this is only the KMS handler, which issues a free `GetPublicKey` to validate credentials, region, IAM, and
    * pinned key) has that probe invoked here. Aborts startup loudly on permanent misconfiguration; transient errors are
    * logged and deferred to the first sign, so an AWS blip at restart never blocks a boot. Attaching a probe IS the
    * opt-in -- there is deliberately no enable flag. A no-op when no probes were attached.
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
    * Check whether a provider name came from the explicit five-field
    * `--signature-provider` form.
    *
    * Anonymous four-field providers receive a process-local `key-N` name,
    * while programmatically created/default providers were not specified by
    * the operator. Neither category satisfies configuration references that
    * require a stable, explicitly shared provider name.
    *
    * @param key_name Provider name referenced by another plugin option.
    * @return true only when that exact name was explicitly configured.
    */
   bool is_explicitly_configured_provider(const std::string& key_name);

   /**
    * Get a provider by public key or key name
    *
    * @param key to lookup provider
    * @return provider if found, otherwise assert/throw
    */
   fc::crypto::signature_provider_ptr get_provider(const fc::crypto::signature_provider_id_t& key);

   /**
    * List all existing registered signature providers, with both as `pair<key_name,public_key>`
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
    * Register a handler for an unrecognised `<provider-type>:` scheme directly on this instance -- the explicit
    * host-application / test path.
    *
    * Built-in schemes (`KEY:`, `KIOD:`) are not registrable -- attempting to register them throws. Each non-built-in
    * scheme may be registered at most once. A handler registered here is ungated: matching specs create eagerly at
    * `plugin_initialize` with no `--plugin` check. Call it before `app().initialize(...)` so it is in place when the
    * manager parses the `--signature-provider` options:
    *
    *     auto& plugin = app()._register_plugin<signature_provider_manager_plugin>();
    *     plugin.register_spec_handler("KMS", &sysio::sigprov::kms::create_kms_provider);
    *
    * `register_plugin<>` (static, used elsewhere in main()) only enqueues the plugin name; use `_register_plugin<>`
    * (instance method, idempotent) to construct or fetch the instance first. Provider PLUGINS do not use this -- they
    * register in the process-wide `sigprov` registry from their constructor (`sigprov::register_scheme_handler`), which
    * the manager gates on `--plugin`.
    *
    * @param scheme  the `<provider-type>` token (e.g. "KMS"); case-sensitive
    * @param handler the parser/builder for `scheme:<spec_data>` specs
    * @throws fc::exception if `scheme` is empty, names a built-in, has
    *         already been registered, or `handler` is empty
    */
   void register_spec_handler(std::string scheme, sysio::spec_handler handler);

private:
   /**
    * Enforce the immutable-after-startup contract at the mutator boundary: throws `plugin_config_exception` unless this
    * plugin is still in the `registered` or `initialized` state. A future runtime spec-reload feature must remove this
    * (and add the synchronization the containers currently need none of) rather than work around it.
    *
    * @param operation the public mutator's name, for the error message
    */
   void assert_pre_startup(std::string_view operation) const;

   std::unique_ptr<class signature_provider_manager_plugin_impl> my;
};

namespace sigprov {

/**
 * A construction-registered handler for a `<provider-type>:` scheme, plus the plugin that provides it.
 */
struct scheme_handler_entry {
   spec_handler handler;     ///< builds a provider from `scheme:<spec_data>`
   std::string  plugin_name; ///< providing plugin's demangled name (`--plugin` gating + errors)
};

/**
 * Register a provider plugin's scheme handler with the manager, from the plugin's CONSTRUCTOR.
 *
 * This is what makes provider ordering a non-issue. appbase constructs every registered plugin before it initializes
 * any, so a handler registered here is present by the time the manager's `plugin_initialize` creates providers -- and
 * the manager is `APPBASE_PLUGIN_REQUIRES`-d by every consumer (chain_plugin, producer_plugin, the outposts), so its
 * init runs before theirs. The manager therefore creates every configured provider at its own init, before any
 * consumer, regardless of where the provider plugin's `--plugin` line sits in the config.
 *
 * The handler is a free function (no plugin instance needed). Registering is cheap and side-effect-free -- no AWS, no
 * network; whether the scheme is actually engaged is gated separately by the manager on the `--plugin` enable list.
 * Process-wide static storage (a fact about the binary, not any one application instance), idempotent via
 * insert-or-assign because test binaries construct plugins repeatedly across scoped_app instances. Deliberately
 * unsynchronized: writes (plugin construction) and reads (the manager's init) all happen on the main thread in
 * sequential appbase lifecycle phases.
 *
 * @param scheme      the `<provider-type>` token (e.g. "SSM"); case-sensitive
 * @param handler     builder for `scheme:<spec_data>` specs
 * @param plugin_name the providing plugin's demangled name, as `--plugin` and
 *                    the manager's enable check expect it (pass `name()`)
 */
void register_scheme_handler(std::string_view scheme, spec_handler handler, std::string_view plugin_name);

/**
 * The construction-registered entry for @p scheme, or nullptr if no linked provider plugin provides it. The returned
 * pointer is valid for the process lifetime (entries are never erased).
 *
 * @param scheme the `<provider-type>` token to look up
 */
const scheme_handler_entry* find_scheme_handler(std::string_view scheme);

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

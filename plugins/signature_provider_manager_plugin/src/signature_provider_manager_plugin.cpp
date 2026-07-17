
#include <boost/algorithm/string.hpp>
#include <fc/crypto/chain_types_reflect.hpp>
#include <fc/crypto/ethereum/ethereum_utils.hpp>
#include <fc/network/url.hpp>
#include <fc/time.hpp>
#include <fc/crypto/bls_private_key.hpp>
#include <fc/crypto/key_serdes.hpp>
#include <fc/crypto/signature_provider.hpp>
#include <fc/io/fstream.hpp>
#include <fc/io/json.hpp>
#include <fc-lite/algorithm.hpp>
#include <fc/io/secure_file.hpp>

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <set>

#include <sysio/chain/types.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/signature_provider_manager_plugin/signature_provider_manager_plugin.hpp>
#include <sysio/http_client_plugin/http_client_plugin.hpp>

namespace sysio {
namespace {
constexpr auto option_name_kiod_timeout = "signature-provider-kiod-timeout";

/// The two built-in `<provider-type>:` schemes, handled directly by this
/// plugin (never through `_spec_handlers`, never registrable).
constexpr std::string_view scheme_key  = "KEY";
constexpr std::string_view scheme_kiod = "KIOD";

/**
 * Best-effort extraction of a spec's non-built-in `<provider-type>` scheme.
 *
 * Splits the 4/5-field `--signature-provider` spec, takes the final
 * (provider) field, and returns the token before its first `:`. Returns
 * nullopt for the built-in `KEY`/`KIOD` schemes and for ANY malformation
 * (wrong field count, missing/empty scheme) -- malformed specs must fall
 * through to the eager `create_provider` path so they throw today's
 * canonical errors, not a retained-spec error.
 *
 * @param spec the full spec exactly as configured
 * @return the extension scheme token, or nullopt (built-in or malformed)
 */
std::optional<std::string> extract_extension_scheme(const std::string& spec) {
   constexpr std::size_t max_spec_fields = 5;
   auto parts = fc::split(spec, ',', max_spec_fields);
   if (parts.size() != 4 && parts.size() != max_spec_fields)
      return std::nullopt;
   const auto& provider = parts.back();
   const auto colon = provider.find(':');
   if (colon == std::string::npos || colon == 0)
      return std::nullopt;
   auto scheme = provider.substr(0, colon);
   if (scheme == scheme_key || scheme == scheme_kiod)
      return std::nullopt;
   return scheme;
}

std::filesystem::path default_signature_provider_spec_file() {
   return app().config_dir() / "default_signature_providers.json";
}

/**
 * Restrict a file to owner read/write only (0600).
 *
 * Used to bring pre-existing key files that predate the owner-only hardening (and were therefore created
 * group/world readable under the process umask) down to owner-only when they are loaded. New key files are
 * instead written through `fc::write_secure_file`, which creates them owner-only from the start. Best-effort:
 * a failure to set permissions is logged, not fatal, so loading still succeeds on platforms/filesystems that
 * do not support POSIX permission bits.
 *
 * @param file_path the file to restrict; it must already exist.
 */
void restrict_file_to_owner(const std::filesystem::path& file_path) {
   std::error_code ec;
   std::filesystem::permissions(file_path,
                                std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                std::filesystem::perm_options::replace, ec);
   if (ec) {
      wlog("could not restrict permissions on {}: {}", file_path.string(), ec.message());
   }
}
} // namespace

namespace sigprov {
namespace {
/**
 * Backing store for the scheme -> providing-plugin announcements. Meyers
 * statics rather than plugin-instance state: provider-plugin constructors run
 * while appbase is still constructing registered plugins -- possibly before
 * the manager instance exists -- and the mapping is a fact about the binary,
 * not about any one application instance (test binaries tear applications
 * down and reconstruct plugins; the announcement legitimately survives).
 */
std::map<std::string, std::string>& scheme_plugin_registry() {
   static std::map<std::string, std::string> registry;
   return registry;
}

std::mutex& scheme_plugin_registry_mutex() {
   static std::mutex m;
   return m;
}
} // namespace

void announce_scheme_plugin(const std::string& scheme, const std::string& plugin_name) {
   std::scoped_lock lock(scheme_plugin_registry_mutex());
   scheme_plugin_registry().insert_or_assign(scheme, plugin_name);
}

std::optional<std::string> announced_scheme_plugin(const std::string& scheme) {
   std::scoped_lock lock(scheme_plugin_registry_mutex());
   const auto& registry = scheme_plugin_registry();
   if (auto it = registry.find(scheme); it != registry.end())
      return it->second;
   return std::nullopt;
}
} // namespace sigprov


class signature_provider_manager_plugin_impl {

public:
   /**
    * `kiod` request timeout
    */
   fc::microseconds _kiod_provider_timeout_us;

   /**
    * When true, `plugin_startup()` runs the opt-in startup-probe pass. Set
    * via `enable_startup_probes()` by a probe-owning provider plugin during
    * its `plugin_initialize` (the kms plugin does so when its
    * `signature-provider-kms-startup-check` option is set) or by tests. The
    * mechanism is generic: any registered handler may attach a probe.
    *
    * Intentionally a plain `bool`, unlike the atomic members below: it is
    * written during the initialize phase and read once in `plugin_startup`,
    * which run sequentially in the appbase lifecycle.
    */
   bool _startup_probe_enabled{false};

   fc::crypto::sign_fn make_kiod_signature_provider(const string& url_str, const chain::public_key_type& pubkey) const {
      fc::url kiod_url;
      if (boost::algorithm::starts_with(url_str, "unix://"))
         // send the entire string after unix:// to http_plugin. It'll auto-detect which part
         //  is the unix socket path, and which part is the url to hit on the server
         kiod_url = fc::url("unix", url_str.substr(7), fc::ostring(), fc::ostring(), fc::ostring(), fc::ostring(),
                            fc::ovariant_object(), std::optional<uint16_t>());
      else
         kiod_url = fc::url(url_str);

      return [to = _kiod_provider_timeout_us, kiod_url, pubkey](const chain::digest_type& digest) {
         fc::variant params;
         fc::to_variant(std::make_pair(digest, pubkey), params);
         auto deadline = to.count() >= 0 ? fc::time_point::now() + to : fc::time_point::maximum();
         return app()
                .get_plugin<http_client_plugin>()
                .get_client()
                .post_sync(kiod_url, params, deadline)
                .as<chain::signature_type>();
      };
   }

   sysio::provider_spec_result create_provider_from_spec(
      fc::crypto::chain_key_type_t key_type,
      const fc::crypto::public_key& public_key,
      const std::string& spec) {
      using namespace fc::crypto;
      constexpr std::size_t max_split = 2;
      auto spec_parts = fc::split(spec, ':', max_split);
      FC_ASSERT(spec_parts.size() == max_split, "Provider spec '{}' is malformed. Format: '<spec type>:<spec data>'", spec);

      auto spec_type_str = spec_parts[0];
      auto spec_data = spec_parts[1];
      FC_ASSERT(!spec_data.empty(), "Provider spec '{}' is malformed. Format: '<spec type>:<spec data>' has empty <spec data>", spec);

      if (spec_type_str == scheme_key) {
         chain::private_key_type privkey;

         switch (key_type) {
         case chain_key_type_wire:
         case chain_key_type_wire_bls:
         case chain_key_type_ethereum:
         case chain_key_type_solana: {
            // Runtime dispatch over the per-type native parsers lives in libfc
            // (fc/crypto/signature_provider.cpp) so extension handlers that
            // also construct local-key providers (e.g. the ssm sub-library)
            // share it. The sui / unknown arms stay here: libfc cannot throw
            // the chain-level config exceptions this plugin's contract uses.
            privkey = from_native_string_to_private_key(key_type, spec_data);
            break;
         }
         case chain_key_type_sui: {
            // to_fc_string throughout these arms: unlike to_string, it is total -- an out-of-range
            // value formats as its number instead of throwing bad_enum_cast from inside the throw.
            FC_THROW_EXCEPTION(sysio::chain::pending_impl_exception, "Key type needs to be implemented: {}",
                               chain_key_type_reflector::to_fc_string(key_type));
         }
         default: {
            FC_THROW_EXCEPTION(sysio::chain::config_parse_error, "Unknown or Unsupported chain kind: {}",
                               chain_key_type_reflector::to_fc_string(key_type));
         }
         }

         FC_ASSERT(public_key == privkey.get_public_key(), "Private key does not match given public key for {}",
                   fc::json::to_log_string(public_key));
         return {.signer = fc::crypto::make_local_sign_fn(privkey), .private_key = privkey};
      }

      if (spec_type_str == scheme_kiod) {
         return {.signer = make_kiod_signature_provider(spec_data, public_key)};
      }

      // Any other scheme resolves through `_spec_handlers` -- populated by a
      // provider plugin's `plugin_initialize` (the packaged path) or by a
      // host application's `main()` before `app().initialize(...)`. The
      // handler owns parsing its own `spec_data`. If a handler returns a
      // `startup_probe`, the caller (`create_provider`) appends it to
      // `_startup_probes` only after `set_provider` succeeds, so a provider
      // rejected as a duplicate never leaves an orphan probe behind.
      sysio::spec_handler handler;
      {
         std::scoped_lock lock(_signing_providers_mutex);
         if (auto it = _spec_handlers.find(spec_type_str); it != _spec_handlers.end()) {
            handler = it->second;
         }
      }
      if (handler) {
         return handler(key_type, public_key, spec_data);
      }

      SYS_THROW(chain::plugin_config_exception,
                "Unknown provider type \"{}\". Built-in types are KEY and KIOD; "
                "additional types (e.g. KMS, SSM) are provided by optional "
                "signature-provider plugins (enable with `plugin = ...`) or "
                "registered by the host application via register_spec_handler() "
                "-- this binary has no registration for \"{}\".",
                spec_type_str, spec_type_str);
   }

   /**
    * Get the next sequence # used for anonymous keys (those without a name/alias)
    *
    * @return next sequence #
    */
   std::uint32_t next_anon_key_counter() { return _anon_key_counter++; }

   /**
    * Add the entry by both `key_name` & `public_key` to associative maps
    *
    * @param provider to set
    */
   fc::crypto::signature_provider_ptr set_provider(const fc::crypto::signature_provider_ptr& provider) {
      std::scoped_lock lock(_signing_providers_mutex);
      SYS_ASSERT(!_signing_providers_by_pubkey.contains(provider->public_key) &&
                 !_signing_providers_by_name.contains(provider->key_name),
                 chain::plugin_config_exception,
                 "A signature provider with key_name \"{}\" or public_key \"{}\" already exists",
                 provider->key_name, fc::json::to_log_string(provider->public_key));

      _signing_providers_by_pubkey.insert_or_assign(provider->public_key, provider);

      _signing_providers_by_name.insert({provider->key_name, provider});
      return provider;
   }


   bool has_provider(const fc::crypto::signature_provider_id_t& key) {
      std::scoped_lock lock(_signing_providers_mutex);
      if (holds_alternative<std::string>(key)) {
         return _signing_providers_by_name.contains(std::get<std::string>(key));
      }

      return _signing_providers_by_pubkey.contains(std::get<chain::public_key_type>(key));
   }

   fc::crypto::signature_provider_ptr get_provider(const fc::crypto::signature_provider_id_t& key) {
      std::scoped_lock lock(_signing_providers_mutex);
      if (holds_alternative<std::string>(key)) {
         auto& keyName = std::get<std::string>(key);
         SYS_ASSERT(_signing_providers_by_name.contains(keyName), chain::plugin_config_exception,
                    "No signature provider exists with name \"{}\"", keyName);

         return _signing_providers_by_name.at(keyName);
      }

      auto& pub_key = std::get<chain::public_key_type>(key);
      SYS_ASSERT(_signing_providers_by_pubkey.contains(pub_key), chain::plugin_config_exception,
                 "No signature provider exists with public key \"{}\"", fc::json::to_log_string(pub_key));

      return _signing_providers_by_pubkey.at(pub_key);
   }

   std::vector<fc::crypto::signature_provider_ptr> query_providers(
      const std::optional<fc::crypto::signature_provider_id_t>& id_opt,
      std::optional<fc::crypto::chain_kind_t> target_chain,
      std::optional<fc::crypto::chain_key_type_t> target_key_type) {
      std::scoped_lock lock(_signing_providers_mutex);
      return std::views::values(_signing_providers_by_pubkey) | std::views::filter([&](const auto& entry) {
                if (target_chain.has_value() && entry->target_chain != target_chain.value()) {
                   return false;
                }
                if (target_key_type.has_value() && entry->key_type != target_key_type.value()) {
                   return false;
                }
                if (!id_opt.has_value()) {
                   return true;
                }
                if (std::holds_alternative<std::string>(id_opt.value())) {
                   return entry->key_name.contains(std::get<std::string>(id_opt.value()));
                } else {
                   return entry->public_key == std::get<chain::public_key_type>(id_opt.value());
                }
             }) |
             std::ranges::to<std::vector>();
   }

   void save_default_signature_provider_specs() {
      // Ensure any on-disk specs were merged before snapshotting (idempotent,
      // takes its own leaf locks -- never called under the mutex).
      load_default_signature_provider_specs();

      std::map<fc::crypto::chain_key_type_t, std::string> specs_snapshot;
      {
         std::scoped_lock lock(_signing_providers_mutex);
         specs_snapshot = _default_signature_provider_specs;
      }

      auto def_sig_prov_file = default_signature_provider_spec_file();

      fc::mutable_variant_object vo;
      for (const auto& [key_type, spec] : specs_snapshot) {
         auto key_type_str = fc::crypto::chain_key_type_reflector::to_string(key_type);
         vo(key_type_str, spec);
      }

      auto file_content = fc::json::to_string(vo, fc::time_point::maximum());

      // This file persists private signing keys: publish it through the secure-file helper so the content
      // is written to an owner-only temporary file and atomically renamed into place (fsync'd), never
      // passing through a group/world-readable window under the process umask.
      fc::write_secure_file(def_sig_prov_file, file_content);
   }

   void load_default_signature_provider_specs() {
      // Idempotence guard only: this is an initialize-phase function (see
      // register_default_signature_providers), so the atomic exchange keeps
      // repeat calls cheap without holding any lock across the file I/O and
      // provider creation below.
      if (_default_signature_providers_loaded.exchange(true)) {
         return;
      }

      auto def_sig_prov_file = default_signature_provider_spec_file();
      if (!std::filesystem::exists(def_sig_prov_file)) {
         return;
      }

      // Key files that predate the owner-only hardening were written under the process umask and may be
      // group/world readable. Bring any such pre-existing file down to owner-only before its specs are
      // registered; a freshly generated file is already owner-only, so leave it untouched.
      std::error_code perm_ec;
      const auto perms = std::filesystem::status(def_sig_prov_file, perm_ec).permissions();
      if (perm_ec || (perms & (std::filesystem::perms::group_all | std::filesystem::perms::others_all)) !=
                        std::filesystem::perms::none) {
         restrict_file_to_owner(def_sig_prov_file);
      }

      std::string json_data;
      fc::read_file_contents(def_sig_prov_file.string(), json_data);
      auto vo = fc::json::from_string(json_data, fc::json::parse_type::relaxed_parser).as<fc::variant_object>();

      // Parse into a local map first, merge under a brief lock, then create
      // the providers unlocked: create_provider takes its own leaf locks and
      // may invoke a spec handler (network I/O for remote-backed schemes).
      std::map<fc::crypto::chain_key_type_t, std::string> loaded;
      for (const auto& item : vo) {
         auto key_type_str = item.key();
         auto spec = item.value().as_string();
         auto key_type = fc::crypto::chain_key_type_reflector::from_string(key_type_str.c_str());
         loaded[key_type] = spec;
      }

      {
         std::scoped_lock lock(_signing_providers_mutex);
         for (const auto& [key_type, spec] : loaded) {
            _default_signature_provider_specs[key_type] = spec;
         }
      }

      for (const auto& [key_type, spec] : loaded) {
         create_provider(spec);
      }
   }

   void register_default_signature_providers(const vector<fc::crypto::chain_key_type_t>& key_types) {
      static constexpr std::array supported_key_types = {fc::crypto::chain_key_type_wire,
                                                         fc::crypto::chain_key_type_wire_bls};

      // A retained spec means this boot is already doomed (throw_if_unclaimed_specs
      // fails plugin_startup); bail out BEFORE the default-generation below can
      // create -- and persist to default_signature_providers.json -- an anonymous
      // key the operator never asked for and later boots would silently re-load.
      throw_if_unclaimed_specs();

      // Initialize-phase API: called from chain_plugin's plugin_initialize, and
      // appbase runs plugin initializes sequentially on the main thread, so the
      // check-then-create sequence below needs no cross-thread atomicity. The
      // container mutex still guards each individual access against runtime
      // reader threads; no lock is held across create_provider or the file save.
      load_default_signature_provider_specs();
      bool changed = false;
      for (const auto& key_type : key_types) {
         FC_ASSERT(fc::contains(supported_key_types, key_type),
                   "Unsupported key type: {}", key_type);
         bool have_default_spec;
         {
            std::scoped_lock lock(_signing_providers_mutex);
            have_default_spec = _default_signature_provider_specs.contains(key_type);
         }
         // A stored default spec was already created by load_default_signature_provider_specs,
         // and a configured provider of this key type makes a default redundant.
         if (have_default_spec || query_providers(std::nullopt, std::nullopt, key_type).size())
            continue;

         // create anonymous key
         auto key_name = std::format("{}-default", fc::crypto::chain_key_type_reflector::to_string(key_type));
         fc::crypto::private_key privkey;
         switch (key_type) {
         case fc::crypto::chain_key_type_wire: {
            privkey = fc::crypto::private_key::generate();
            break;
         }
         case fc::crypto::chain_key_type_wire_bls: {
            privkey = fc::crypto::private_key::generate(fc::crypto::private_key::key_type::bls);
            break;
         }
         default: {
            FC_THROW_EXCEPTION(sysio::chain::config_parse_error, "Unknown or Unsupported chain kind: {}",
                               fc::crypto::chain_key_type_reflector::to_fc_string(key_type));
         }
         }
         auto pub_key_str = privkey.get_public_key().to_string({});
         auto spec = fc::crypto::to_signature_provider_spec(key_name,
                                                            fc::crypto::chain_kind_wire,
                                                            key_type,
                                                            pub_key_str,
                                                            std::format("KEY:{}", privkey.to_string({})));

         {
            std::scoped_lock lock(_signing_providers_mutex);
            _default_signature_provider_specs[key_type] = spec;
         }
         changed = true;

         dlog("Registering default signature provider spec (type={})",
              fc::crypto::chain_key_type_reflector::to_string(key_type));
         create_provider(spec);
      }

      // IF EVERYTHING HAS SUCCEEDED TO THIS POINT
      // THEN WRITE THE CHANGED DEFAULT KEYS
      if (changed) {
         save_default_signature_provider_specs();
      }
   }

   fc::crypto::signature_provider_ptr create_provider(const string& spec) {
      using namespace fc::crypto;
      //<name>,<chain-kind>,<key-type>,<public-key>,<private-key-provider-spec>
      auto spec_parts = fc::split(spec, ',', 5);
      auto num_parts = spec_parts.size();
      SYS_ASSERT(num_parts == 5 || num_parts == 4, chain::plugin_config_exception, "Invalid key spec: {}",
                 redact_signature_provider_spec(spec));
      std::string key_name;
      std::size_t target_chain_idx = 1;
      if (num_parts == 4) {
         target_chain_idx = 0;
      } else {
         key_name = spec_parts[0];
      }

      auto kind = chain_kind_reflector::from_string(spec_parts[target_chain_idx].c_str());
      auto key_type = chain_key_type_reflector::from_string(spec_parts[target_chain_idx + 1].c_str());
      auto public_key_text = spec_parts[target_chain_idx + 2];
      auto private_key_provider_spec = spec_parts[target_chain_idx + 3];

      if (key_name.empty()) {
         key_name = std::format("key-{}", next_anon_key_counter());
      }

      return create_provider(key_name, kind, key_type, public_key_text, private_key_provider_spec);
   }

   /**
    * `plugin_initialize`'s per-spec entry point: create the provider when the
    * spec's scheme is resolvable now (built-in, or registered pre-init by a
    * host `main()`), otherwise retain it for the scheme's provider plugin to
    * claim via `create_configured_providers()`. Malformed specs are NOT
    * retained -- `extract_extension_scheme` returns nullopt for them, so they
    * fall through to `create_provider` and throw its canonical errors.
    *
    * @param spec the full spec exactly as configured
    * @return the created provider, or null when the spec was retained
    */
   fc::crypto::signature_provider_ptr create_or_retain_provider(const std::string& spec) {
      if (auto scheme = extract_extension_scheme(spec)) {
         std::scoped_lock lock(_signing_providers_mutex);
         if (!_spec_handlers.contains(*scheme)) {
            dlog("Retaining signature provider spec until a \"{}\" handler is registered: {}",
                 *scheme, redact_signature_provider_spec(spec));
            _unclaimed_specs.push_back({.scheme = std::move(*scheme), .raw_spec = spec});
            return nullptr;
         }
      }
      return create_provider(spec);
   }

   std::vector<fc::crypto::signature_provider_ptr> create_configured_providers(const std::string& scheme) {
      // Consume the matching retained specs under a brief lock, then create
      // unlocked: creation may invoke the scheme's handler, which for
      // remote-backed schemes (e.g. SSM's one-time GetParameter fetch) does
      // network I/O that must never run under the providers mutex.
      std::vector<unclaimed_spec> to_create;
      {
         std::scoped_lock lock(_signing_providers_mutex);
         auto matches = [&](const unclaimed_spec& s) { return s.scheme == scheme; };
         std::ranges::copy_if(_unclaimed_specs, std::back_inserter(to_create), matches);
         std::erase_if(_unclaimed_specs, matches);
      }

      std::vector<fc::crypto::signature_provider_ptr> created;
      created.reserve(to_create.size());
      for (const auto& s : to_create) {
         dlog("Creating retained \"{}\" signature provider spec: {}",
              scheme, redact_signature_provider_spec(s.raw_spec));
         created.push_back(create_provider(s.raw_spec));
      }
      return created;
   }

   /**
    * Fail the boot if any retained spec was never claimed. Called from
    * `plugin_startup()` and -- earlier, to prevent the anonymous-default-key
    * side effect -- from `register_default_signature_providers()`. Three
    * diagnoses per spec: the scheme's handler is registered but its plugin
    * never claimed (plugin bug); the scheme was announced by a plugin that
    * is not enabled (names the exact `plugin =` remediation); or nothing in
    * this binary provides the scheme at all.
    */
   void throw_if_unclaimed_specs() {
      std::vector<unclaimed_spec> unclaimed;
      std::set<std::string> registered_schemes;
      {
         std::scoped_lock lock(_signing_providers_mutex);
         if (_unclaimed_specs.empty()) {
            return;
         }
         unclaimed = _unclaimed_specs;
         for (const auto& [scheme, handler] : _spec_handlers) {
            registered_schemes.insert(scheme);
         }
      }

      std::string detail;
      for (const auto& s : unclaimed) {
         const auto redacted = redact_signature_provider_spec(s.raw_spec);
         if (registered_schemes.contains(s.scheme)) {
            detail += std::format(
               "  - scheme \"{}\" (spec: {}): its handler is registered, but the providing plugin "
               "never created the configured providers (missing create_configured_providers(\"{}\") "
               "call).\n",
               s.scheme, redacted, s.scheme);
         } else if (auto owner = sigprov::announced_scheme_plugin(s.scheme)) {
            detail += std::format(
               "  - scheme \"{}\" (spec: {}) is provided by plugin \"{}\", which is built into this "
               "binary but not enabled. Add \"plugin = {}\" to the config file (or pass "
               "--plugin {}).\n",
               s.scheme, redacted, *owner, *owner, *owner);
         } else {
            detail += std::format(
               "  - scheme \"{}\" (spec: {}): no plugin in this binary provides this scheme "
               "(built-in schemes are KEY and KIOD).\n",
               s.scheme, redacted);
         }
      }
      SYS_THROW(chain::plugin_config_exception,
                "{} --signature-provider spec(s) name a provider scheme with no created provider:\n{}",
                unclaimed.size(), detail);
   }

   fc::crypto::signature_provider_ptr create_provider(const std::string& key_name,
                                                      fc::crypto::chain_kind_t target_chain,
                                                      fc::crypto::chain_key_type_t key_type,
                                                      const std::string& public_key_text,
                                                      const std::string& private_key_provider_spec) {
      using namespace fc::crypto;

      FC_ASSERT(!key_name.empty(), "Key name must not be empty");

      chain::public_key_type pubkey;

      switch (key_type) {
      case chain_key_type_wire:
      case chain_key_type_wire_bls:
      case chain_key_type_ethereum:
      case chain_key_type_solana: {
         // Runtime dispatch lives in libfc (fc/crypto/signature_provider.cpp);
         // the sui / unknown arms stay here for the chain-level exception
         // taxonomy, same as the KEY: private-key parse.
         pubkey = from_native_string_to_public_key(key_type, public_key_text);
         break;
      }
      case chain_key_type_sui: {
         FC_THROW_EXCEPTION(sysio::chain::pending_impl_exception, "Key type: {}",
                            chain_key_type_reflector::to_fc_string(key_type));
      }
      default: {
         FC_THROW_EXCEPTION(sysio::chain::config_parse_error, "Unknown or Unsupported chain kind: {}",
                            static_cast<uint8_t>(target_chain));
      }
      }

      auto [signer, privkey, startup_probe] =
         create_provider_from_spec(key_type, pubkey, private_key_provider_spec);
      auto provider = std::make_shared<signature_provider_t>(
         signature_provider_t{target_chain, key_type, key_name, pubkey, privkey, signer}
         );

      // Register first. set_provider() throws plugin_config_exception on a
      // duplicate key_name / public_key; reaching the statement after it means
      // the provider is in the maps. Only then is the startup probe (if any)
      // retained -- a rejected provider leaves nothing behind for
      // plugin_startup() to probe.
      set_provider(provider);

      if (startup_probe) {
         std::scoped_lock lock(_signing_providers_mutex);
         _startup_probes.push_back(std::move(startup_probe));
      }
      return provider;
   }

   /**
    * Register a `<provider-type>:` scheme handler.
    *
    * Built-in `KEY` and `KIOD` are not registrable. Each non-built-in scheme
    * may be registered at most once. A pure map insert with no side effects:
    * provider plugins pair it with `create_configured_providers()` from
    * their `plugin_initialize`; host applications may instead call it from
    * `main()` before `app().initialize(...)`, in which case matching specs
    * create eagerly at parse time and no claim step is involved.
    */
   void register_spec_handler(std::string scheme, sysio::spec_handler handler) {
      FC_ASSERT(!scheme.empty(), "spec handler scheme must not be empty");
      FC_ASSERT(scheme != scheme_key && scheme != scheme_kiod,
                "Cannot override built-in spec handler \"{}\"", scheme);
      FC_ASSERT(static_cast<bool>(handler),
                "spec handler for \"{}\" must not be empty", scheme);
      std::scoped_lock lock(_signing_providers_mutex);
      auto [it, inserted] = _spec_handlers.try_emplace(std::move(scheme), std::move(handler));
      FC_ASSERT(inserted, "spec handler \"{}\" already registered", it->first);
   }

   /**
    * Run the opt-in startup-probe pass.
    *
    * For every provider whose handler attached a `startup_probe`, invoke
    * that probe. Today only the KMS handler (the `kms` sub-library) does so
    * -- the probe issues a single `GetPublicKey` call that resolves AWS
    * credentials, warms the client, and verifies the KMS key matches the
    * pinned public key.
    *
    * A permanent misconfiguration thrown by any probe propagates out of
    * `plugin_startup()` and aborts startup loudly instead of failing on the
    * first production sign. A transient error (e.g. KMS throttle / timeout)
    * is logged and skipped -- the lazy first-sign check is left to retry it
    * rather than killing node startup.
    *
    * A no-op unless `_startup_probe_enabled` is set, and a no-op when no
    * probes were registered.
    *
    * The probe list is one-shot: this drains it (whether or not the check is
    * enabled) so it never lingers as a misleading registry of signers. A
    * provider registered after `plugin_startup()` -- e.g. via a future
    * runtime spec reload -- is not retroactively probed; its pinning check
    * (if any) still runs lazily on the first sign through whatever one-shot
    * guard the handler attaches.
    */
   void run_startup_probes() {
      // Drain the probe list under the lock. Moving it out both hands the
      // network-bound probes a private copy to run without holding the
      // providers mutex, and empties the member so the one-shot nature of
      // the startup check is structural rather than just documented.
      std::vector<std::function<void()>> probes;
      {
         std::scoped_lock lock(_signing_providers_mutex);
         probes = std::exchange(_startup_probes, {});
      }
      if (!_startup_probe_enabled || probes.empty()) {
         return;
      }

      ilog("Running signature-provider startup probes for {} signing key(s)", probes.size());
      std::size_t deferred = 0;
      for (auto& probe : probes) {
         try {
            probe();
         } catch (const chain::signing_transient_exception& e) {
            // A transient signing error (e.g. KMS throttle / KMSInternal /
            // timeout) at startup is not a misconfiguration -- don't abort the
            // node over it. Log and continue; the lazy first-sign path
            // re-runs the same probe.
            ++deferred;
            wlog("Signature-provider startup probe: transient error for one "
                 "key, deferring its check to the first sign: {}",
                 e.to_detail_string());
         }
         // A permanent misconfiguration throws chain::plugin_config_exception,
         // which propagates out of plugin_startup() and aborts startup loudly.
      }
      if (deferred == 0) {
         ilog("Signature-provider startup probes passed");
      } else {
         ilog("Signature-provider startup probes passed; {} key(s) hit a "
              "transient error and will be re-checked on the first sign",
              deferred);
      }
   }

private:
   std::atomic_uint32_t _anon_key_counter{0};

   /**
    * Guards ONLY the container members below (`_spec_handlers`, the two
    * provider maps, `_startup_probes`, `_unclaimed_specs`,
    * `_default_signature_provider_specs`).
    *
    * Lock discipline -- deliberately a plain (non-recursive) mutex: every
    * critical section is leaf-level. Snapshot/copy under the lock, then
    * invoke handlers/probes/signers or do file/network I/O unlocked; no
    * function that holds the lock may call another function that acquires
    * it. A would-be re-entrant path means restructuring the code
    * (gather-then-act, snapshot-then-run), never a recursive mutex. The
    * leaf-only rule also keeps a future reader/writer (`std::shared_mutex`,
    * equally non-recursive) swap mechanical, should contention ever warrant
    * one.
    */
   std::mutex _signing_providers_mutex{};

   std::atomic_bool _default_signature_providers_loaded{false};
   std::map<fc::crypto::chain_key_type_t, std::string> _default_signature_provider_specs{};

   /**
    * A `--signature-provider` spec whose `<provider-type>` scheme had no
    * registered handler when `plugin_initialize` parsed it. Retained instead
    * of thrown: the provider plugin that supplies the scheme initializes
    * after this plugin (it depends on it) and claims its specs via
    * `create_configured_providers()`. Anything still here after the
    * initialize phase fails the boot (see `throw_if_unclaimed_specs`).
    */
   struct unclaimed_spec {
      std::string scheme;   ///< extracted `<provider-type>`, e.g. "SSM"
      std::string raw_spec; ///< the full spec exactly as configured
   };

   /**
    * Retained specs awaiting their scheme's provider plugin, in config
    * order. Guarded by `_signing_providers_mutex`.
    */
   std::vector<unclaimed_spec> _unclaimed_specs{};
   /**
    * Internal map for storing signature providers
    */
   std::map<std::string, fc::crypto::signature_provider_ptr> _signing_providers_by_name{};
   std::map<chain::public_key_type, fc::crypto::signature_provider_ptr> _signing_providers_by_pubkey{};

   /**
    * One-shot startup probes, one per provider whose handler attached one
    * (today: KMS). Collected as providers are created, run by
    * `run_startup_probes()` when `_startup_probe_enabled` is set. Guarded by
    * `_signing_providers_mutex`.
    */
   std::vector<std::function<void()>> _startup_probes{};

   /**
    * Spec-handler registry for non-built-in `<provider-type>:` schemes.
    * Populated by the host application's `register_spec_handler()` calls
    * before `app().initialize(...)`. Looked up in `create_provider_from_spec`
    * when the scheme is neither `KEY` nor `KIOD`. Guarded by
    * `_signing_providers_mutex`.
    */
   std::map<std::string, sysio::spec_handler> _spec_handlers{};
};

signature_provider_manager_plugin::signature_provider_manager_plugin()
   : my(std::make_unique<signature_provider_manager_plugin_impl>()) {}

signature_provider_manager_plugin::~signature_provider_manager_plugin() {}

void signature_provider_manager_plugin::set_program_options(options_description&, options_description& cfg) {
   cfg.add_options()(
      option_name_kiod_timeout, boost::program_options::value<int32_t>()->default_value(5),
      "Limits the maximum time (in milliseconds) that is allowed for sending requests to a kiod provider for signing");
   cfg.add_options()(
      "signature-provider", boost::program_options::value<std::vector<std::string>>()->multitoken(),
      "Signature provider spec formatted as (check docs for details): "
      "`<name>,<chain-kind>,<key-type>,<public-key>,<private-key-provider-spec>`. "
      "Provider types KEY:<private-key> and KIOD:<url> are built in; additional "
      "schemes are provided by optional signature-provider plugins -- e.g. "
      "`plugin = sysio::signature_provider_ssm_plugin` for SSM:<region>:<parameter-name> "
      "(key fetched once from AWS SSM Parameter Store at startup) and "
      "`plugin = sysio::signature_provider_kms_plugin` for KMS:<key-ref> "
      "(remote signing via AWS KMS).");
}

const char* signature_provider_manager_plugin::signature_provider_help_text() const {
   return "Key=Value pairs in the form <public-key>=<provider-spec>\n"
      "Where:\n"
      "<name>,<chain-kind>,<key-type>,<public-key>,<private-key-provider-spec>\n"
      "   <name>                 name to use when referencing this provider, if empty then auto-assigned\n\n"
      "   <chain-kind>           chain where the key will be used\n\n"
      "   <key-type>             key format to parse\n\n"
      "   <public-key>           is a string form of a valid <key-type>\n\n"
      "   <provider-spec>        is a string in the form <provider-type>:<data>\n\n"
      "       <provider-type>    KEY and KIOD are built in. Additional types are provided\n"
      "                          by optional signature-provider plugins (enable with\n"
      "                          `plugin = ...`) or registered by the host application.\n\n"
      "       KEY:<private-key>  is a string containing a private key of the key-type specified\n\n"
      "       KIOD:<url>         is the URL where kiod is available and the appropriate wallet(s) are unlocked\n\n"
      "       SSM:<param-ref>    fetches the private key once at startup from AWS SSM Parameter\n"
      "                          Store (SecureString) and signs locally; <param-ref> is\n"
      "                          <region>:<parameter-name> or a full parameter ARN. Enable with\n"
      "                          `plugin = sysio::signature_provider_ssm_plugin`.\n\n"
      "       KMS:<key-ref>      signs remotely via AWS KMS (secp256k1/ethereum keys only; the\n"
      "                          key never leaves KMS); <key-ref> is <region>:<key-id-or-alias>\n"
      "                          or a full key/alias ARN. Enable with\n"
      "                          `plugin = sysio::signature_provider_kms_plugin`.\n\n";
}

void signature_provider_manager_plugin::plugin_initialize(const variables_map& options) {
   if (options.contains(option_name_kiod_timeout))
      my->_kiod_provider_timeout_us = fc::milliseconds(options.at(option_name_kiod_timeout).as<int32_t>());

   if (options.contains(option_name_provider)) {
      auto specs = options.at(option_name_provider).as<std::vector<std::string>>();
      for (const auto& spec : specs) {
         dlog("Registering signature provider from spec: {}", redact_signature_provider_spec(spec));
         if (auto provider = my->create_or_retain_provider(spec)) {
            dlog("Registered signature provider ({}): {}",
                 provider->key_name, provider->public_key.to_string({}));
         }
      }
   }
}

void signature_provider_manager_plugin::plugin_startup() {
   // Unclaimed-spec accounting first: every retained spec must have been
   // claimed by its scheme's provider plugin during the initialize phase;
   // anything left here is a misconfiguration (typically a missing
   // `plugin =` line, which the error names) and fails the boot.
   my->throw_if_unclaimed_specs();

   // Opt-in startup-probe pass for any provider whose handler attached a
   // probe (today: KMS). A no-op unless a probe-owning plugin called
   // enable_startup_probes(); when enabled, a permanent probe failure throws
   // here and aborts startup loudly.
   my->run_startup_probes();
}

fc::crypto::signature_provider_ptr signature_provider_manager_plugin::create_provider(const std::string& spec) {
   return my->create_provider(spec);
}


fc::crypto::signature_provider_ptr signature_provider_manager_plugin::create_provider(
   const std::string& key_name, fc::crypto::chain_kind_t target_chain, fc::crypto::chain_key_type_t key_type,
   const std::string& public_key_text, const std::string& private_key_provider_spec) {
   return my->create_provider(key_name, target_chain, key_type, public_key_text, private_key_provider_spec);
}

fc::crypto::sign_fn
signature_provider_manager_plugin::create_anonymous_provider_from_private_key(chain::private_key_type priv) const {
   return fc::crypto::make_local_sign_fn(priv);
}

bool signature_provider_manager_plugin::has_provider(const fc::crypto::signature_provider_id_t& key) {
   return my->has_provider(key);
}

fc::crypto::signature_provider_ptr signature_provider_manager_plugin::get_provider(
   const fc::crypto::signature_provider_id_t& key) {
   return my->get_provider(key);
}

std::vector<fc::crypto::signature_provider_ptr>
signature_provider_manager_plugin::query_providers(const std::optional<fc::crypto::signature_provider_id_t>& id_opt,
                                                   std::optional<fc::crypto::chain_kind_t> target_chain,
                                                   std::optional<fc::crypto::chain_key_type_t> target_key_type) {
   return my->query_providers(id_opt, target_chain, target_key_type);
}

void signature_provider_manager_plugin::register_default_signature_providers(
   const std::vector<fc::crypto::chain_key_type_t>& key_types) {
   my->register_default_signature_providers(key_types);
}

void signature_provider_manager_plugin::register_spec_handler(
   std::string scheme, sysio::spec_handler handler) {
   my->register_spec_handler(std::move(scheme), std::move(handler));
}

std::vector<fc::crypto::signature_provider_ptr>
signature_provider_manager_plugin::create_configured_providers(const std::string& scheme) {
   return my->create_configured_providers(scheme);
}

void signature_provider_manager_plugin::enable_startup_probes() {
   my->_startup_probe_enabled = true;
}

std::vector<fc::crypto::signature_provider_ptr> query_signature_providers(
   const std::optional<fc::crypto::signature_provider_id_t>& id_opt,
   std::optional<fc::crypto::chain_kind_t> target_chain,
   std::optional<fc::crypto::chain_key_type_t> target_key_type) {
   auto& plug = app().get_plugin<signature_provider_manager_plugin>();
   return plug.query_providers(id_opt, target_chain, target_key_type);
}


fc::crypto::signature_provider_ptr get_signature_provider(const fc::crypto::signature_provider_id_t& id,
                                                          std::optional<fc::crypto::chain_kind_t> target_chain) {
   auto& plug = app().get_plugin<signature_provider_manager_plugin>();
   auto providers = plug.query_providers(id, target_chain);

   FC_ASSERT(providers.size() == 1, "Expected exactly one provider for query \"{}\", found {}",
             fc::json::to_log_string(id), providers.size());
   return providers[0];
}

} // namespace sysio
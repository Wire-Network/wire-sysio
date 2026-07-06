
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

#include <filesystem>

#include <sysio/chain/types.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/signature_provider_manager_plugin/signature_provider_manager_plugin.hpp>
#include <sysio/http_client_plugin/http_client_plugin.hpp>

namespace sysio {
namespace {
constexpr auto option_name_kiod_timeout = "signature-provider-kiod-timeout";

/// Opt-in flag: when true, plugin_startup() invokes every registered
/// `startup_probe` -- today only the AWS KMS handler attaches one (a
/// `GetPublicKey` call that validates credentials / region / IAM / pinned
/// key). Default false, so nodes with no probe-registering providers and
/// offline test environments are unaffected. The option name retains "kms"
/// for backward compatibility with existing operator configs.
constexpr auto option_name_kms_startup_check = "signature-provider-kms-startup-check";

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


class signature_provider_manager_plugin_impl {

public:
   /**
    * `kiod` request timeout
    */
   fc::microseconds _kiod_provider_timeout_us;

   /**
    * When true, `plugin_startup()` runs the opt-in startup-probe pass. Set
    * from `option_name_kms_startup_check` in `plugin_initialize`. The option
    * name keeps "kms" for backward compatibility with operator configs; the
    * mechanism is generic and any registered handler may attach a probe.
    *
    * Intentionally a plain `bool`, unlike the atomic members below: it is
    * written once in `plugin_initialize` and read once in `plugin_startup`,
    * which run sequentially in the appbase lifecycle.
    */
   bool _startup_probe_enabled{false};

   fc::crypto::sign_fn make_key_signature_provider(const chain::private_key_type& key) const {
      return [key](const chain::digest_type& digest) { return key.sign(digest); };
   }

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

      if (spec_type_str == "KEY") {
         chain::private_key_type privkey;

         switch (key_type) {
         case chain_key_type_wire: {
            privkey = from_native_string_to_private_key<chain_key_type_wire>(spec_data);
            break;
         }
         case chain_key_type_wire_bls: {
            privkey = from_native_string_to_private_key<chain_key_type_wire_bls>(spec_data);
            break;
         }

         case chain_key_type_ethereum: {
            privkey = from_native_string_to_private_key<chain_key_type_ethereum>(spec_data);
            break;
         }
         case chain_key_type_solana: {
            privkey = from_native_string_to_private_key<chain_key_type_solana>(spec_data);
            break;
         }
         case chain_key_type_sui: {
            FC_THROW_EXCEPTION(sysio::chain::pending_impl_exception, "Key type needs to be implemented: {}",
                               chain_key_type_reflector::to_string(key_type));
         }
         default: {
            FC_THROW_EXCEPTION(sysio::chain::config_parse_error, "Unknown or Unsupported chain kind: {}",
                               chain_key_type_reflector::to_fc_string(key_type));
         }
         }

         FC_ASSERT(public_key == privkey.get_public_key(), "Private key does not match given public key for {}",
                   fc::json::to_log_string(public_key));
         return {.signer = make_key_signature_provider(privkey), .private_key = privkey};
      }

      if (spec_type_str == "KIOD") {
         return {.signer = make_kiod_signature_provider(spec_data, public_key)};
      }

      // Any other scheme must have been registered by the host application
      // (via `register_spec_handler`) before `app().initialize(...)`. The
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
                "additional types (e.g. KMS) must be registered by the host "
                "application via register_spec_handler() before app().initialize() "
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
      std::scoped_lock lock(_signing_providers_mutex);
      if (!_default_signature_providers_loaded)
         load_default_signature_provider_specs();

      auto def_sig_prov_file = default_signature_provider_spec_file();

      fc::mutable_variant_object vo;
      for (const auto& [key_type, spec] : _default_signature_provider_specs) {
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
      std::scoped_lock lock(_signing_providers_mutex);
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
      for (const auto& item : vo) {
         auto key_type_str = item.key();
         auto spec = item.value().as_string();
         auto key_type = fc::crypto::chain_key_type_reflector::from_string(key_type_str.c_str());
         _default_signature_provider_specs[key_type] = spec;
         create_provider(spec);
      }

   }

   void register_default_signature_providers(const vector<fc::crypto::chain_key_type_t>& key_types) {
      static constexpr std::array supported_key_types = {fc::crypto::chain_key_type_wire,
                                                         fc::crypto::chain_key_type_wire_bls};

      std::scoped_lock lock(_signing_providers_mutex);
      load_default_signature_provider_specs();
      bool changed = false;
      for (const auto& key_type : key_types) {
         FC_ASSERT(fc::contains(supported_key_types, key_type),
                   "Unsupported key type: {}", key_type);
         if (_default_signature_provider_specs.contains(key_type) || query_providers(std::nullopt, std::nullopt, key_type).size())
            continue;

         std::string spec;
         if (_default_signature_provider_specs.contains(key_type)) {
            spec = _default_signature_provider_specs.at(key_type);
         } else {
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
            spec = fc::crypto::to_signature_provider_spec(key_name,
                                                          fc::crypto::chain_kind_wire,
                                                          key_type,
                                                          pub_key_str,
                                                          std::format("KEY:{}", privkey.to_string({})));

            _default_signature_provider_specs[key_type] = spec;
            changed = true;
         }

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

   fc::crypto::signature_provider_ptr create_provider(const std::string& key_name,
                                                      fc::crypto::chain_kind_t target_chain,
                                                      fc::crypto::chain_key_type_t key_type,
                                                      const std::string& public_key_text,
                                                      const std::string& private_key_provider_spec) {
      using namespace fc::crypto;

      FC_ASSERT(!key_name.empty(), "Key name must not be empty");

      chain::public_key_type pubkey;

      switch (key_type) {
      case chain_key_type_wire: {
         pubkey = from_native_string_to_public_key<chain_key_type_wire>(public_key_text);
         break;
      }
      case chain_key_type_wire_bls: {
         pubkey = from_native_string_to_public_key<chain_key_type_wire_bls>(public_key_text);
         break;
      }
      case chain_key_type_ethereum: {
         pubkey = from_native_string_to_public_key<chain_key_type_ethereum>(public_key_text);
         break;
      }
      case chain_key_type_solana: {
         pubkey = from_native_string_to_public_key<chain_key_type_solana>(public_key_text);
         break;
      }
      case chain_key_type_sui: {
         FC_THROW_EXCEPTION(sysio::chain::pending_impl_exception, "Key type: {}",
                            chain_key_type_reflector::to_string(key_type));
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
    * may be registered at most once. Host applications register their
    * extensions (e.g. the `kms` sub-library's `create_kms_provider`) in
    * `main()` before `app().initialize(...)`, so registrations are in place
    * by the time `plugin_initialize` parses each `--signature-provider`
    * option.
    */
   void register_spec_handler(std::string scheme, sysio::spec_handler handler) {
      FC_ASSERT(!scheme.empty(), "spec handler scheme must not be empty");
      FC_ASSERT(scheme != "KEY" && scheme != "KIOD",
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
    * Recursive mutex used to protect access if multiple
    * plugins tried to load providers on multiple threads
    * (defaults or user-provided) simultaneously.
    *
    * TODO: @jglanz Swap for a std::shared_mutex R/W
    */
   std::recursive_mutex _signing_providers_mutex{};

   std::atomic_bool _default_signature_providers_loaded{false};
   std::map<fc::crypto::chain_key_type_t, std::string> _default_signature_provider_specs{};
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
      "`<name>,<chain-kind>,<key-type>,<public-key>,<private-key-provider-spec>`");;
   cfg.add_options()(
      option_name_kms_startup_check,
      boost::program_options::value<bool>()->default_value(false),
      "Run startup probes for any registered signing-provider extensions "
      "(today: AWS KMS, which probes every `KMS:` key with a GetPublicKey "
      "call so a credentials / region / IAM / pinned-key misconfiguration "
      "fails fast). Off by default; has no effect when no probe-registering "
      "providers are configured. The option name retains 'kms' for backward "
      "compatibility with existing operator configs.");
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
      "       <provider-type>    KEY and KIOD are built in. Additional types\n"
      "                          (e.g. KMS) are registered by the host application;\n"
      "                          this binary supports only the types its main() registers.\n\n"
      "       KEY:<private-key>  is a string containing a private key of the key-type specified\n\n"
      "       KIOD:<url>         is the URL where kiod is available and the appropriate wallet(s) are unlocked\n\n";
}

void signature_provider_manager_plugin::plugin_initialize(const variables_map& options) {
   if (options.contains(option_name_kiod_timeout))
      my->_kiod_provider_timeout_us = fc::milliseconds(options.at(option_name_kiod_timeout).as<int32_t>());

   if (options.contains(option_name_kms_startup_check))
      my->_startup_probe_enabled = options.at(option_name_kms_startup_check).as<bool>();

   if (options.contains(option_name_provider)) {
      auto specs = options.at(option_name_provider).as<std::vector<std::string>>();
      for (const auto& spec : specs) {
         dlog("Registering signature provider from spec: {}", redact_signature_provider_spec(spec));
         auto provider = create_provider(spec);
         dlog("Registered signature provider ({}): {}",
              provider->key_name, provider->public_key.to_string({}));
      }
   }
}

void signature_provider_manager_plugin::plugin_startup() {
   // Opt-in startup-probe pass for any provider whose handler attached a
   // probe (today: KMS). A no-op unless `signature-provider-kms-startup-check`
   // is enabled; when enabled, a permanent probe failure throws here and
   // aborts startup loudly.
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
   return [priv](const fc::sha256& d) { return priv.sign(d); };
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
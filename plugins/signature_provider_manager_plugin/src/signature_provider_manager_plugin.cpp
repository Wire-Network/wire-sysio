
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
#include <gsl-lite/gsl-lite.hpp>

#include <sysio/chain/types.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/signature_provider_manager_plugin/signature_provider_manager_plugin.hpp>
#include <sysio/http_client_plugin/http_client_plugin.hpp>

namespace sysio {
namespace {
constexpr auto option_name_provider = "signature-provider";
constexpr auto option_name_kiod_timeout_us = "signature-provider-kiod-timeout-us";
auto _signature_provider_manager_plugin = application::register_plugin<signature_provider_manager_plugin>();

std::filesystem::path default_signature_provider_spec_file() {
   return app().config_dir() / "default_signature_providers.json";
}
} // namespace


class signature_provider_manager_plugin_impl {

public:
   /**
    * `kiod` request timeout
    */
   fc::microseconds _kiod_provider_timeout_us;

   fc::crypto::signature_provider_sign_fn make_key_signature_provider(const chain::private_key_type& key) const {
      return [key](const chain::digest_type& digest) { return key.sign(digest); };
   }

   fc::crypto::signature_provider_sign_fn make_kiod_signature_provider(const string& url_str,
                                                                       const chain::public_key_type& pubkey) const {
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

   std::pair<fc::crypto::signature_provider_sign_fn, std::optional<fc::crypto::private_key>> create_provider_from_spec(
      fc::crypto::chain_key_type_t key_type,
      const fc::crypto::public_key& public_key,
      const std::string& spec) {
      using namespace fc::crypto;
      auto spec_parts = fc::split(spec, ':', 2);
      auto spec_type_str = spec_parts[0];
      auto spec_data = spec_parts[1];

      if (spec_type_str == "KEY") {
         auto privkey_str = spec_data;
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
         case chain_key_type_sui:
         case chain_key_type_solana: {
            FC_THROW_EXCEPTION(sysio::chain::pending_impl_exception, "Key type needs to be implemented: ${type}",
                               ("type", chain_key_type_reflector::to_string(key_type)));
         }
         default: {
            FC_THROW_EXCEPTION(sysio::chain::config_parse_error, "Unknown or Unsupported chain kind: ${kind}",
                               ("kind", chain_key_type_reflector::to_fc_string(key_type)));
         }
         }

         FC_ASSERT(public_key == privkey.get_public_key(), "Private key does not match given public key for ${pub}",
                   ("pub", public_key));
         return {make_key_signature_provider(privkey), privkey};
      }

      if (spec_type_str == "KIOD") {
         return {make_kiod_signature_provider(spec_data, public_key), std::nullopt};
      }

      SYS_THROW(chain::plugin_config_exception, "Unsupported key provider type \"${t}\"", ("t", spec_type_str));
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
                 "A signature provider with key_name \"${keyName}\" or public_key \"${pubKey}\" already exists",
                 ("keyName", provider->key_name)("pubKey", provider->public_key));

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
                    "No signature provider exists with name \"${keyName}\"", ("keyName", keyName));

         return _signing_providers_by_name.at(keyName);
      }

      auto& pub_key = std::get<chain::public_key_type>(key);
      SYS_ASSERT(_signing_providers_by_pubkey.contains(pub_key), chain::plugin_config_exception,
                 "No signature provider exists with public key \"${pubKey}\"", ("pubKey", pub_key));

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

      auto file_content = fc::json::to_string(vo, {});

      {
         fc::cfile file(def_sig_prov_file, fc::cfile::truncate_rw_mode);
         gsl_lite::final_action file_guard([&file]() { file.close(); });

         file.write(file_content.c_str(), file_content.size());
      }
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
                   "Unsupported key type: ${keyType}", ("keyType", key_type));
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
               privkey = fc::crypto::private_key::generate<fc::ecc::private_key_shim>();
               break;
            }
            case fc::crypto::chain_key_type_wire_bls: {
               privkey = fc::crypto::private_key::generate<fc::crypto::bls::private_key_shim>();
               break;
            }
            default: {
               FC_THROW_EXCEPTION(sysio::chain::config_parse_error, "Unknown or Unsupported chain kind: ${kind}",
                                  ("kind", fc::crypto::chain_key_type_reflector::to_fc_string(key_type)));
            }
            }
            auto pub_key_str = privkey.get_public_key().to_native_string({});
            spec = fc::crypto::to_signature_provider_spec(key_name,
                                                          fc::crypto::chain_kind_wire,
                                                          key_type,
                                                          pub_key_str,
                                                          std::format("KEY:{}", privkey.to_native_string({})));

            _default_signature_provider_specs[key_type] = spec;
            changed = true;
         }

         dlogf("Registering default signature provider spec (type={})",
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
      SYS_ASSERT(num_parts == 5 || num_parts == 4, chain::plugin_config_exception, "Invalid key spec: ${spec}",
                 ("spec", spec));
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
      case chain_key_type_sui:
      case chain_key_type_solana: {
         FC_THROW_EXCEPTION(sysio::chain::pending_impl_exception, "Key type: ${type}",
                            ("type", chain_key_type_reflector::to_string(key_type)));
      }
      default: {
         FC_THROW_EXCEPTION(sysio::chain::config_parse_error, "Unknown or Unsupported chain kind: ${kind}",
                            ("kind", static_cast<uint8_t>(target_chain)));
      }
      }

      auto [signer, privkey] = create_provider_from_spec(key_type, pubkey, private_key_provider_spec);
      auto provider = std::make_shared<signature_provider_t>(
         signature_provider_t{target_chain, key_type, key_name, pubkey, privkey, signer}
         );

      return set_provider(provider);
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
};

signature_provider_manager_plugin::signature_provider_manager_plugin()
   : my(std::make_unique<signature_provider_manager_plugin_impl>()) {}

signature_provider_manager_plugin::~signature_provider_manager_plugin() {}

void signature_provider_manager_plugin::set_program_options(options_description&, options_description& cfg) {
   cfg.add_options()(
      "signature-provider-kiod-timeout", boost::program_options::value<int32_t>()->default_value(5),
      "Limits the maximum time (in milliseconds) that is allowed for sending requests to a kiod provider for signing");
   cfg.add_options()(
      "signature-provider", boost::program_options::value<std::vector<std::string>>()->multitoken(),
      "Signature provider spec formatted as (check docs for details): "
      "`<name>,<chain-kind>,<key-type>,<public-key>,<private-key-provider-spec>`");;
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
      "       <provider-type>    is KEY, KIOD, or SE\n\n"
      "       KEY:<private-key>  is a string containing a private key of the key-type specified\n\n"
      "       KIOD:<url>         is the URL where kiod is available and the appropriate wallet(s) are unlocked\n\n";
}

void signature_provider_manager_plugin::plugin_initialize(const variables_map& options) {
   if (options.contains(option_name_kiod_timeout_us))
      my->_kiod_provider_timeout_us = fc::milliseconds(options.at(option_name_kiod_timeout_us).as<int32_t>());

   if (options.contains(option_name_provider)) {
      auto specs = options.at(option_name_provider).as<std::vector<std::string>>();
      for (const auto& spec : specs) {
         dlog("Registering signature provider from spec: ${spec}", ("spec", spec));
         auto provider = create_provider(spec);
         dlog("Registered signature provider (${name}): ${publicKey}",
              ("name", provider->key_name)("publicKey", provider->public_key.to_native_string({})));
      }
   }
}

fc::crypto::signature_provider_ptr signature_provider_manager_plugin::create_provider(const std::string& spec) {
   return my->create_provider(spec);
}


fc::crypto::signature_provider_ptr signature_provider_manager_plugin::create_provider(
   const std::string& key_name, fc::crypto::chain_kind_t target_chain, fc::crypto::chain_key_type_t key_type,
   const std::string& public_key_text, const std::string& private_key_provider_spec) {
   return my->create_provider(key_name, target_chain, key_type, public_key_text, private_key_provider_spec);
}

fc::crypto::signature_provider_sign_fn
signature_provider_manager_plugin::create_anonymous_provider_from_private_key(chain::private_key_type priv) const {
   return my->make_key_signature_provider(priv);
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

   FC_ASSERT(providers.size() == 1, "Expected exactly one provider for query \"${id}\", found ${found}",
             ("id", id)("found", providers.size()));
   return providers[0];
}

} // namespace sysio
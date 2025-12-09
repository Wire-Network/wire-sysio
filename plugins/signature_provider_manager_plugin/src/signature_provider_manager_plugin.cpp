// #include <sysio/wallet_plugin/wallet_api.hpp>

#include <boost/algorithm/string.hpp>
#include <fc/crypto/chain_types_reflect.hpp>
#include <fc/crypto/ethereum_utils.hpp>
#include <fc/network/url.hpp>
#include <fc/time.hpp>
#include <ranges>

#include <sysio/chain/types.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/signature_provider_manager_plugin/signature_provider_manager_plugin.hpp>
#include <sysio/http_client_plugin/http_client_plugin.hpp>


namespace sysio {
namespace {
constexpr auto option_name_provider     = "signature-provider";
constexpr auto option_name_kiod_timeout_us = "signature-provider-kiod-timeout-us";
static auto    _signature_provider_manager_plugin = application::register_plugin<signature_provider_manager_plugin>();
} // namespace


class signature_provider_manager_plugin_impl {

public:
   /**
    * `kiod` request timeout
    */
   fc::microseconds _kiod_provider_timeout_us;

   signature_provider_sign_fn make_key_signature_provider(const chain::private_key_type& key) const {
      return [key](const chain::digest_type& digest) { return key.sign(digest); };
   }

   signature_provider_sign_fn make_kiod_signature_provider(const string&                url_str,
                                                           const chain::public_key_type pubkey) const {
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

   signature_provider_sign_fn create_provider_from_spec(fc::crypto::chain_key_type_t key_type,
                                                        fc::crypto::public_key public_key, const std::string& spec) {
      using namespace fc::crypto;
      auto spec_parts    = fc::split(spec, ':', 2);
      auto spec_type_str = spec_parts[0];
      auto spec_data     = spec_parts[1];

      if (spec_type_str == "KEY") {
         auto                    privkey_str = spec_data;
         chain::private_key_type privkey;

         switch (key_type) {
         case chain_key_type_wire: {
            privkey = fc::crypto::private_key(spec_data);
            break;
         }
         case chain_key_type_ethereum: {
            auto em_privkey      = fc::crypto::ethereum::parse_private_key(spec_data);
            auto em_privkey_data = em_privkey.get_secret();
            auto em_privkey_shim = fc::em::private_key_shim(em_privkey_data);
            privkey              = fc::crypto::private_key(em_privkey_shim);
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
         return make_key_signature_provider(privkey);
      }

      if (spec_type_str == "KIOD") {
         return make_kiod_signature_provider(spec_data, public_key);
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
   signature_provider_ptr set_provider(const signature_provider_ptr& provider) {
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


   bool has_provider(const signature_provider_id_t& key) {
      std::scoped_lock lock(_signing_providers_mutex);
      if (holds_alternative<std::string>(key)) {
         return _signing_providers_by_name.contains(std::get<std::string>(key));
      }

      return _signing_providers_by_pubkey.contains(std::get<chain::public_key_type>(key));
   }

   signature_provider_ptr get_provider(const signature_provider_id_t& key) {
      std::scoped_lock lock(_signing_providers_mutex);
      if (holds_alternative<std::string>(key)) {
         auto keyName = std::get<std::string>(key);
         SYS_ASSERT(_signing_providers_by_name.contains(keyName), chain::plugin_config_exception,
                    "No signature provider exists with name \"${keyName}\"", ("keyName", keyName));

         return _signing_providers_by_name.at(keyName);
      }

      auto pub_key = std::get<chain::public_key_type>(key);
      SYS_ASSERT(_signing_providers_by_pubkey.contains(pub_key), chain::plugin_config_exception,
                 "No signature provider exists with public key \"${pubKey}\"", ("pubKey", pub_key));

      return _signing_providers_by_pubkey.at(pub_key);
   }

   std::vector<signature_provider_ptr> query_providers(const std::optional<signature_provider_id_t>& id_opt,
                                                       std::optional<fc::crypto::chain_kind_t>       target_chain) {
      std::scoped_lock lock(_signing_providers_mutex);
      return std::views::values(_signing_providers_by_pubkey) | std::views::filter([&](const auto& entry) {
                if (target_chain.has_value() && entry->target_chain != target_chain.value()) {
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

private:
   std::atomic_uint32_t _anon_key_counter{0};

   std::mutex _signing_providers_mutex{};
   /**
    * Internal map for storing signature providers
    */
   std::map<std::string, signature_provider_ptr>            _signing_providers_by_name{};
   std::map<chain::public_key_type, signature_provider_ptr> _signing_providers_by_pubkey{};
};


signature_provider_manager_plugin::signature_provider_manager_plugin()
   : my(std::make_unique<signature_provider_manager_plugin_impl>()) {}

signature_provider_manager_plugin::~signature_provider_manager_plugin() {}

void signature_provider_manager_plugin::set_program_options(options_description&, options_description& cfg) {
   cfg.add_options()(
      "signature-provider-kiod-timeout", boost::program_options::value<int32_t>()->default_value(5),
      "Limits the maximum time (in milliseconds) that is allowed for sending requests to a kiod provider for signing")(
      "signature-provider", boost::program_options::value<std::vector<std::string>>()->multitoken(),
      "Signature provider spec formatted as (check docs for details): "
      "`<name>,<chain-kind>,<key-type>,<public-key>,<private-key-provider-spec>`");
   ;
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
         ilog("Registered signature provider (${name}): ${publicKey}",
              ("name", provider->key_name)("publicKey", provider->public_key.to_string({})));
      }
   }
}

signature_provider_ptr signature_provider_manager_plugin::create_provider(const std::string& spec) {
   using namespace fc::crypto;
   //<name>,<chain-kind>,<key-type>,<public-key>,<private-key-provider-spec>
   auto spec_parts = fc::split(spec, ',', 5);
   auto num_parts  = spec_parts.size();
   SYS_ASSERT(num_parts == 5 || num_parts == 4, chain::plugin_config_exception, "Invalid key spec: ${spec}",
              ("spec", spec));
   std::string key_name;
   std::size_t target_chain_idx = 1;
   if (num_parts == 4) {
      target_chain_idx = 0;
   } else {
      key_name = spec_parts[0];
   }


   auto kind                      = chain_kind_reflector::from_string(spec_parts[target_chain_idx].c_str());
   auto key_type                  = chain_key_type_reflector::from_string(spec_parts[target_chain_idx + 1].c_str());
   auto public_key_text           = spec_parts[target_chain_idx + 2];
   auto private_key_provider_spec = spec_parts[target_chain_idx + 3];

   if (key_name.empty()) {
      key_name = std::format("key-{}", my->next_anon_key_counter());
   }

   return create_provider(key_name, kind, key_type, public_key_text, private_key_provider_spec);
}


signature_provider_ptr signature_provider_manager_plugin::create_provider(
   const std::string& key_name, fc::crypto::chain_kind_t target_chain, fc::crypto::chain_key_type_t key_type,
   const std::string& public_key_text, const std::string& private_key_provider_spec) {
   using namespace fc::crypto;

   FC_ASSERT(!key_name.empty(), "Key name must not be empty");

   chain::public_key_type pubkey;

   switch (key_type) {
   case chain_key_type_wire: {
      pubkey = public_key(public_key_text);
      break;
   }
   case chain_key_type_ethereum: {
      auto em_pubkey      = fc::crypto::ethereum::parse_public_key(public_key_text);
      auto em_pubkey_data = em_pubkey.serialize();
      auto em_pubkey_shim = fc::em::public_key_shim(em_pubkey_data);
      pubkey              = fc::crypto::public_key(em_pubkey_shim);
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

   auto provider = std::make_shared<signature_provider_t>(
      signature_provider_t{target_chain, key_type, key_name, pubkey,
                         my->create_provider_from_spec(key_type, pubkey, private_key_provider_spec)});

   return my->set_provider(provider);
}

signature_provider_ptr signature_provider_manager_plugin::create_kiod_provider(fc::crypto::chain_kind_t target_chain,
                                                                               fc::crypto::chain_key_type_t key_type,
                                                                               std::string        public_key_text,
                                                                               std::string        url,
                                                                               const std::string& key_name) {
   FC_ASSERT(false, "Not implemented yet");
}


signature_provider_sign_fn
signature_provider_manager_plugin::create_anonymous_provider_from_private_key(chain::private_key_type priv) const {
   return my->make_key_signature_provider(priv);
}

bool signature_provider_manager_plugin::has_provider(const signature_provider_id_t& key) {
   return my->has_provider(key);
}

signature_provider_ptr signature_provider_manager_plugin::get_provider(const signature_provider_id_t& key) {
   return my->get_provider(key);
}

std::vector<signature_provider_ptr>
signature_provider_manager_plugin::query_providers(const std::optional<signature_provider_id_t>& id_opt,
                                                   std::optional<fc::crypto::chain_kind_t> target_chain) {
   return my->query_providers(id_opt, target_chain);
}

std::vector<signature_provider_ptr> query_signature_providers(const std::optional<signature_provider_id_t>&          id_opt,
                                                              std::optional<fc::crypto::chain_kind_t> target_chain) {
   auto& plug = app().get_plugin<signature_provider_manager_plugin>();
   return plug.query_providers(id_opt, target_chain);
}


signature_provider_ptr get_signature_provider(const signature_provider_id_t&                      id,
                                              std::optional<fc::crypto::chain_kind_t> target_chain) {
   auto& plug      = app().get_plugin<signature_provider_manager_plugin>();
   auto  providers = plug.query_providers(id, target_chain);

   FC_ASSERT(providers.size() == 1, "Expected exactly one provider for query \"${id}\", found ${found}",
             ("id", id)("found", providers.size()));
   return providers[0];
}

} // namespace sysio
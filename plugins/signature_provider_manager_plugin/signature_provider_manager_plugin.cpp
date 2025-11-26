#include "sysio/wallet_plugin/wallet_api.hpp"

#include <ranges>
#include <sysio/signature_provider_manager_plugin/signature_provider_manager_plugin.hpp>
#include <sysio/chain/exceptions.hpp>

#include <fc/time.hpp>
#include <fc/network/url.hpp>

#include <boost/algorithm/string.hpp>

namespace sysio {
static auto _signature_provider_manager_plugin = application::register_plugin<signature_provider_manager_plugin>();

class signature_provider_manager_plugin_impl {

public:
   /**
    * `kiod` request timeout
    */
   fc::microseconds _kiod_provider_timeout_us;

   signature_provider_sign_fn
   make_key_signature_provider(const chain::private_key_type& key) const {
      return [key](const chain::digest_type& digest) {
         return key.sign(digest);
      };
   }

   signature_provider_sign_fn
   make_kiod_signature_provider(const string& url_str, const chain::public_key_type pubkey) const {
      fc::url kiod_url;
      if (boost::algorithm::starts_with(url_str, "unix://"))
         //send the entire string after unix:// to http_plugin. It'll auto-detect which part
         // is the unix socket path, and which part is the url to hit on the server
         kiod_url = fc::url("unix", url_str.substr(7), fc::ostring(), fc::ostring(), fc::ostring(), fc::ostring(),
                            fc::ovariant_object(), std::optional<uint16_t>());
      else
         kiod_url = fc::url(url_str);

      return [to=_kiod_provider_timeout_us, kiod_url, pubkey](const chain::digest_type& digest) {
         fc::variant params;
         fc::to_variant(std::make_pair(digest, pubkey), params);
         auto deadline = to.count() >= 0 ? fc::time_point::now() + to : fc::time_point::maximum();
         return app().get_plugin<http_client_plugin>().get_client().post_sync(kiod_url, params, deadline).as<
            chain::signature_type>();
      };
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
   signature_provider& set_provider(signature_provider&& provider) {
      scoped_lock lock(_signing_providers_mutex);
      SYS_ASSERT(!_signing_providers_by_pubkey.contains(provider.public_key) &&
                 !_signing_providers_by_name.contains(provider.key_name),
                 chain::plugin_config_exception,
                 "A signature provider with key_name \"${keyName}\" or public_key \"${pubKey}\" already exists",
                 ("keyName", provider.key_name)("pubKey",provider.public_key)
         );
      _signing_providers_by_pubkey[provider.public_key] = std::move(provider);
      auto& provider_ref                                = _signing_providers_by_pubkey[provider.public_key];
      _signing_providers_by_name.insert({
         provider.key_name,
         provider_ref
      });
      return provider_ref;
   }

   signature_provider& set_provider(const signature_provider& provider) {
      signature_provider owned_provider(provider);
      return set_provider(std::move(owned_provider));
   }

   bool has_provider(const signature_provider_key_type& key) {
      scoped_lock lock(_signing_providers_mutex);
      if (holds_alternative<std::string>(key)) {
         return _signing_providers_by_name.contains(std::get<std::string>(key));
      }

      return _signing_providers_by_pubkey.contains(std::get<chain::public_key_type>(key));
   }

   signature_provider& get_provider(const signature_provider_key_type& key) {
      scoped_lock lock(_signing_providers_mutex);
      if (holds_alternative<std::string>(key)) {
         auto keyName = std::get<std::string>(key);
         SYS_ASSERT(_signing_providers_by_name.contains(keyName),
                    chain::plugin_config_exception, "No signature provider exists with name \"${keyName}\"",
                    ("keyName", keyName));

         return _signing_providers_by_name.at(keyName);
      }

      auto pub_key = std::get<chain::public_key_type>(key);
      SYS_ASSERT(_signing_providers_by_pubkey.contains(pub_key),
                 chain::plugin_config_exception, "No signature provider exists with public key \"${pubKey}\"",
                 ("pubKey", pub_key));

      return _signing_providers_by_pubkey.at(pub_key);

   }

   std::vector<std::pair<std::string, chain::public_key_type>> list_providers() {
      return std::views::values(_signing_providers_by_pubkey) |
             std::views::transform([&](const auto& entry) {
                return std::make_pair(entry.key_name, entry.public_key);
             }) |
             ranges::to<std::vector>();
   }

private:
   std::atomic_uint32_t _anon_key_counter{0};

   std::mutex _signing_providers_mutex{};
   /**
    * Internal map for storing signature providers
    */
   std::map<std::string, std::reference_wrapper<signature_provider>> _signing_providers_by_name{};
   std::map<chain::public_key_type, signature_provider>              _signing_providers_by_pubkey{};
};

signature_provider_manager_plugin::signature_provider_manager_plugin() : my(
   std::make_unique<signature_provider_manager_plugin_impl>()) {}

signature_provider_manager_plugin::~signature_provider_manager_plugin() {}

void signature_provider_manager_plugin::set_program_options(options_description&, options_description& cfg) {
   cfg.add_options()
      ("kiod-provider-timeout", boost::program_options::value<int32_t>()->default_value(5),
       "Limits the maximum time (in milliseconds) that is allowed for sending requests to a kiod provider for signing");
}

const char* signature_provider_manager_plugin::signature_provider_help_text() const {
   return "Key=Value pairs in the form <public-key>=<provider-spec>\n"
      "Where:\n"
      "   <public-key>        \tis a string form of a vaild SYSIO public key\n\n"
      "   <provider-spec>     \tis a string in the form <provider-type>:<data>\n\n"
      "   <provider-type>     \tis KEY, KIOD, or SE\n\n"
      "   KEY:<data>          \tis a string form of a valid SYSIO private key which maps to the provided public key\n\n"
      "   KEY:<name>:<data>   \tsame as the previous with the exception of also having an associative name, for things like 'ethereum'\n\n"
      "   KIOD:<data>         \tis the URL where kiod is available and the appropriate wallet(s) are unlocked\n\n";
}

void signature_provider_manager_plugin::plugin_initialize(const variables_map& options) {
   my->_kiod_provider_timeout_us = fc::milliseconds(options.at("kiod-provider-timeout").as<int32_t>());
}

signature_provider&
signature_provider_manager_plugin::create_provider_for_spec(const std::string& spec) {
   auto delim = spec.find("=");
   SYS_ASSERT(delim != std::string::npos, chain::plugin_config_exception, "Missing \"=\" in the key spec pair");
   auto pub_key_str = spec.substr(0, delim);
   auto spec_str    = spec.substr(delim + 1);

   auto spec_parts = fc::split(spec_str, ':');

   SYS_ASSERT(spec_parts.size() > 1, chain::plugin_config_exception, "Missing \":\" in the key spec pair");
   auto& spec_type_str          = spec_parts.front();
   auto  [spec_data, spec_name] = spec_parts.size() == 2
                                    ? std::pair{spec_parts[1], std::format("key-{}", my->next_anon_key_counter())}
                                    : std::pair{spec_parts[2], spec_parts[1]};

   auto               pubkey = chain::public_key_type(pub_key_str);
   signature_provider entry{
      spec_name,
      pubkey,
      fc::crypto::get_public_key_type(pub_key_str)
   };

   if (spec_type_str == "KEY") {
      chain::private_key_type privkey(spec_data);
      SYS_ASSERT(pubkey == privkey.get_public_key(), chain::plugin_config_exception,
                 "Private key does not match given public key for ${pub}", ("pub", pubkey));
      entry.sign = my->make_key_signature_provider(privkey);
   } else if (spec_type_str == "KIOD") {
      entry.sign = my->make_kiod_signature_provider(spec_data, pubkey);
   } else {
      SYS_THROW(chain::plugin_config_exception, "Unsupported key provider type \"${t}\"", ("t", spec_type_str));
   }

   return my->set_provider(std::move(entry));
}

signature_provider& signature_provider_manager_plugin::create_provider_for_spec(const std::string& key_name,
   const std::string&                                                                              spec) {
   auto delim = spec.find("=");
   SYS_ASSERT(delim != std::string::npos, chain::plugin_config_exception, "Missing \"=\" in the key spec pair");
   auto pub_key_str = spec.substr(0, delim);
   auto spec_str    = spec.substr(delim + 1);

   auto spec_parts = fc::split(spec_str, ':');

   SYS_ASSERT(spec_parts.size() > 1, chain::plugin_config_exception, "Missing \":\" in the key spec pair");
   SYS_ASSERT(spec_parts.size() < 3 || key_name.empty(), chain::plugin_config_exception,
              "Your provided a key name (${keyName}),"
              " AND provided a spec which includes a keyName, these are mutually exclusive.",
              ("keyName", key_name));
   auto& spec_type_str          = spec_parts.front();
   auto  [spec_data, spec_name] = spec_parts.size() == 2
                                    ? std::pair{
                                       spec_parts[1],
                                       key_name.empty() ? std::format("key-{}", my->next_anon_key_counter()) : key_name
                                    }
                                    : std::pair{spec_parts[2], spec_parts[1]};

   auto               pubkey = chain::public_key_type(pub_key_str);
   signature_provider provider{
      spec_name,
      pubkey,
      fc::crypto::get_public_key_type(pub_key_str)
   };

   if (spec_type_str == "KEY") {
      chain::private_key_type privkey(spec_data);
      SYS_ASSERT(pubkey == privkey.get_public_key(), chain::plugin_config_exception,
                 "Private key does not match given public key for ${pub}", ("pub", pubkey));
      provider.sign = my->make_key_signature_provider(privkey);
   } else if (spec_type_str == "KIOD") {
      provider.sign = my->make_kiod_signature_provider(spec_data, pubkey);
   } else {
      SYS_THROW(chain::plugin_config_exception, "Unsupported key provider type \"${t}\"", ("t", spec_type_str));
   }

   return my->set_provider(std::move(provider));
}

signature_provider_sign_fn
signature_provider_manager_plugin::create_anonymous_provider_from_private_key(chain::private_key_type priv) const {
   return my->make_key_signature_provider(priv);
}

bool signature_provider_manager_plugin::has_provider(const signature_provider_key_type& key) {
   return my->has_provider(key);
}

signature_provider& signature_provider_manager_plugin::
get_provider(const signature_provider_key_type& key) {
   return my->get_provider(key);
}

std::vector<std::pair<std::string, chain::public_key_type>> signature_provider_manager_plugin::list_providers() {
   return my->list_providers();
}

}
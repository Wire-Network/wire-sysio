#include <sysio/signature_provider_plugin/signature_provider_plugin.hpp>
#include <sysio/chain/exceptions.hpp>

#include <fc/time.hpp>
#include <fc/network/url.hpp>

#include <boost/algorithm/string.hpp>

namespace sysio {
   static auto _signature_provider_plugin = application::register_plugin<signature_provider_plugin>();

class signature_provider_plugin_impl {
   public:
      fc::microseconds  _kiod_provider_timeout_us;

      signature_provider_plugin::signature_provider_type
      make_key_signature_provider(const chain::private_key_type& key) const {
         return [key]( const chain::digest_type& digest ) {
            return key.sign(digest);
         };
      }

      signature_provider_plugin::signature_provider_type
      make_kiod_signature_provider(const string& url_str, const chain::public_key_type pubkey) const {
         fc::url kiod_url;
         if(boost::algorithm::starts_with(url_str, "unix://"))
            //send the entire string after unix:// to http_plugin. It'll auto-detect which part
            // is the unix socket path, and which part is the url to hit on the server
            kiod_url = fc::url("unix", url_str.substr(7), fc::ostring(), fc::ostring(), fc::ostring(), fc::ostring(), fc::ovariant_object(), std::optional<uint16_t>());
         else
            kiod_url = fc::url(url_str);

         return [to=_kiod_provider_timeout_us, kiod_url, pubkey](const chain::digest_type& digest) {
            fc::variant params;
            fc::to_variant(std::make_pair(digest, pubkey), params);
            auto deadline = to.count() >= 0 ? fc::time_point::now() + to : fc::time_point::maximum();
            return app().get_plugin<http_client_plugin>().get_client().post_sync(kiod_url, params, deadline).as<chain::signature_type>();
         };
      }
};

signature_provider_plugin::signature_provider_plugin():my(new signature_provider_plugin_impl()){}
signature_provider_plugin::~signature_provider_plugin(){}

void signature_provider_plugin::set_program_options(options_description&, options_description& cfg) {
   cfg.add_options()
         ("kiod-provider-timeout", boost::program_options::value<int32_t>()->default_value(5),
          "Limits the maximum time (in milliseconds) that is allowed for sending requests to a kiod provider for signing")
         ;
}

const char* const signature_provider_plugin::signature_provider_help_text() const {
   return "Key=Value pairs in the form <public-key>=<provider-spec>\n"
          "Where:\n"
          "   <public-key>    \tis a string form of a vaild SYSIO public key\n\n"
          "   <provider-spec> \tis a string in the form <provider-type>:<data>\n\n"
          "   <provider-type> \tis KEY, KIOD, or SE\n\n"
          "   KEY:<data>      \tis a string form of a valid SYSIO private key which maps to the provided public key\n\n"
          "   KIOD:<data>    \tis the URL where kiod is available and the approptiate wallet(s) are unlocked\n\n"
          ;

}

void signature_provider_plugin::plugin_initialize(const variables_map& options) {
   my->_kiod_provider_timeout_us = fc::milliseconds( options.at("kiod-provider-timeout").as<int32_t>() );
}

std::pair<chain::public_key_type,signature_provider_plugin::signature_provider_type>
signature_provider_plugin::signature_provider_for_specification(const std::string& spec) const {
   auto delim = spec.find("=");
   SYS_ASSERT(delim != std::string::npos, chain::plugin_config_exception, "Missing \"=\" in the key spec pair");
   auto pub_key_str = spec.substr(0, delim);
   auto spec_str = spec.substr(delim + 1);

   auto spec_delim = spec_str.find(":");
   SYS_ASSERT(spec_delim != std::string::npos, chain::plugin_config_exception, "Missing \":\" in the key spec pair");
   auto spec_type_str = spec_str.substr(0, spec_delim);
   auto spec_data = spec_str.substr(spec_delim + 1);

   auto pubkey = chain::public_key_type(pub_key_str);

   if(spec_type_str == "KEY") {
      chain::private_key_type priv(spec_data);
      SYS_ASSERT(pubkey == priv.get_public_key(), chain::plugin_config_exception, "Private key does not match given public key for ${pub}", ("pub", pubkey));
      return std::make_pair(pubkey, my->make_key_signature_provider(priv));
   }
   else if(spec_type_str == "KIOD")
      return std::make_pair(pubkey, my->make_kiod_signature_provider(spec_data, pubkey));
   SYS_THROW(chain::plugin_config_exception, "Unsupported key provider type \"${t}\"", ("t", spec_type_str));
}

signature_provider_plugin::signature_provider_type
signature_provider_plugin::signature_provider_for_private_key(const chain::private_key_type priv) const {
   return my->make_key_signature_provider(priv);
}

}

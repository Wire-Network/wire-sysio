#pragma once
#include <sysio/chain/application.hpp>
#include <sysio/http_client_plugin/http_client_plugin.hpp>
#include <sysio/chain/types.hpp>

namespace sysio {

using namespace appbase;
struct signature_provider;
using signature_provider_key_type  = std::variant<std::string, chain::public_key_type>;
using signature_provider_sign_fn = std::function<chain::signature_type(chain::digest_type)>;

/**
 * Plugin responsible for managing signature providers.
 */
class signature_provider_manager_plugin : public appbase::plugin<signature_provider_manager_plugin> {
public:

   signature_provider_manager_plugin();
   virtual ~signature_provider_manager_plugin();

   APPBASE_PLUGIN_REQUIRES((http_client_plugin))
   virtual void set_program_options(options_description&, options_description& cfg) override;

   void plugin_initialize(const variables_map& options);
   void plugin_startup() {}
   void plugin_shutdown() {}

   const char* signature_provider_help_text() const;

   /**
    * Create a known signature provider
    *
    * @param spec string spec described in source
    * @return a signature provider
    */
   signature_provider& create_provider(const std::string& spec);

   /**
    * Create a key without parsing a spec
    *
    * @param target_chain
    * @param key_type
    * @param public_key_text
    * @param private_key_provider_spec
    * @param key_name
    * @return
    */
   signature_provider&
   create_provider(
      const std::string&         key_name,
      fc::crypto::chain_kind     target_chain,
      fc::crypto::chain_key_type key_type,
      const std::string&         public_key_text,
      const std::string&         private_key_provider_spec

      );

   signature_provider&
   create_kiod_provider(
      fc::crypto::chain_kind target_chain,
      fc::crypto::chain_key_type key_type,
      std::string public_key_text,
      std::string url,
      const std::string& key_name
   );
   /**
    * Create a signature provider for use by the caller and only the caller
    *
    * @param priv private key to use
    * @return an anonymous signature provider
    */
   signature_provider_sign_fn create_anonymous_provider_from_private_key(chain::private_key_type priv) const;

   /**
    * Check for the existence of a provider
    *
    * @param key public key or key name to check for existence of a signature provider
    * @return true if provider exists, false otherwise
    */
   bool has_provider(const signature_provider_key_type& key);

   /**
    * Get a provider by public key or key name
    *
    * @param key to lookup provider
    * @return provider if found, otherwise assert/throw
    */
   signature_provider& get_provider(const signature_provider_key_type& key);

   /**
    * List all existing registered signature providers,
    * with both as `pair<key_name,public_key>`
    *
    * @return list of available signature providers
    */
   std::vector<std::pair<std::string,chain::public_key_type>> list_providers();
private:
   std::unique_ptr<class signature_provider_manager_plugin_impl> my;
};

/**
 * `signature_provider_entry` constructed provider
 */
struct signature_provider  {

   /** The chain/key type */
   fc::crypto::chain_kind target_chain;

   /** The chain/key type */
   fc::crypto::chain_key_type key_type;


   /** The alias or name assigned to identify this key pair */
   string key_name;


   /** The public key component of this key pair */
   fc::crypto::public_key public_key;



   signature_provider_sign_fn sign;
};



}

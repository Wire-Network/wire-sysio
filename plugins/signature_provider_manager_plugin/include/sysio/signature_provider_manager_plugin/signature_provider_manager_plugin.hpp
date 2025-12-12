#pragma once
#include <sysio/chain/application.hpp>
#include <sysio/http_client_plugin/http_client_plugin.hpp>
#include <fc/crypto/chain_types_reflect.hpp>
#include <fc/crypto/private_key.hpp>
#include <fc/crypto/public_key.hpp>
#include <fc/crypto/signature.hpp>
#include <fc/crypto/signature_provider.hpp>

namespace sysio {

using namespace appbase;

// using namespace fc::crypto;


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
   fc::crypto::signature_provider_ptr create_provider(const std::string& spec);

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
   fc::crypto::signature_provider_ptr
   create_provider(
      const std::string&         key_name,
      fc::crypto::chain_kind_t     target_chain,
      fc::crypto::chain_key_type_t key_type,
      const std::string&         public_key_text,
      const std::string&         private_key_provider_spec

      );

   fc::crypto::signature_provider_ptr
   create_kiod_provider(
      fc::crypto::chain_kind_t target_chain,
      fc::crypto::chain_key_type_t key_type,
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
   fc::crypto::signature_provider_sign_fn create_anonymous_provider_from_private_key(fc::crypto::private_key priv) const;

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
      std::optional<fc::crypto::chain_kind_t> target_chain = std::nullopt
      );
private:
   std::unique_ptr<class signature_provider_manager_plugin_impl> my;
};

/**
 * @brief Get a signature provider
 *
 * if exactly 1 provider matches, it will be returned, otherwise throws
 *
 * @param query
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
 * @param query
 * @param target_chain
 * @return list of matching signature providers
 */
std::vector<fc::crypto::signature_provider_ptr> query_signature_providers(
   const std::optional<fc::crypto::signature_provider_id_t>& id_opt = std::nullopt,
   std::optional<fc::crypto::chain_kind_t> target_chain = std::nullopt
);

}

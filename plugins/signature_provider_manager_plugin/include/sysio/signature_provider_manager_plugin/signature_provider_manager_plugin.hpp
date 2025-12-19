#pragma once
#include <boost/program_options/options_description.hpp>
#include <sysio/chain/application.hpp>
#include <sysio/http_client_plugin/http_client_plugin.hpp>
#include <fc/crypto/chain_types_reflect.hpp>
#include <fc/crypto/private_key.hpp>
#include <fc/crypto/public_key.hpp>
#include <fc/crypto/signature.hpp>
#include <fc/crypto/signature_provider.hpp>

namespace sysio {

using namespace appbase;

// TODO: @jglanz retrofit/refactor BLS support
// //      ///               public_key   spec_type    spec_data
// /// Note: spec_data is private_key if spec_type is KEY
// static std::tuple<std::string, std::string, std::string> parse_signature_provider_spec(const std::string& spec);
//
//
// using signature_provider_type = std::function<chain::signature_type(chain::digest_type)>;
//
// // @return empty optional for BLS specs
// std::optional<std::pair<chain::public_key_type,signature_provider_type>> signature_provider_for_specification(const std::string& spec) const;
// signature_provider_type signature_provider_for_private_key(const chain::private_key_type& priv) const;
//
// // @return empty optional for non-BLS specs
// std::optional<std::pair<fc::crypto::bls::public_key, fc::crypto::bls::private_key>> bls_public_key_for_specification(const std::string& spec) const;

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
 */
class signature_provider_manager_plugin : public appbase::plugin<signature_provider_manager_plugin> {
public:
   signature_provider_manager_plugin();
   virtual ~signature_provider_manager_plugin();

   APPBASE_PLUGIN_REQUIRES((http_client_plugin))
   virtual void set_program_options(boost::program_options::options_description&,
                                    boost::program_options::options_description& cfg) override;

   void plugin_initialize(const variables_map& options);
   void plugin_startup() {}
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
   fc::crypto::signature_provider_sign_fn
   create_anonymous_provider_from_private_key(fc::crypto::private_key priv) const;

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

   // TODO: Implement using LUT
   // template <fc::crypto::chain_kind_t TargetChain>
   // void register_default_signature_providers() {
   //    auto key_types_tuple = std::get<TargetChain>(fc::crypto::chain_key_types);
   //    std::vector<fc::crypto::chain_key_type_t> key_types;
   //    std::apply([&](auto... args) {
   //       ([&](fc::crypto::chain_key_type_t key_type) {
   //          key_types.push_back(key_type);
   //       }(args), ...);
   //    }, key_types_tuple);
   //
   //    register_default_signature_providers(key_types);
   // }

private:
   std::unique_ptr<class signature_provider_manager_plugin_impl> my;
};

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
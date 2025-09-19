#pragma once

#include <sysio/chain_plugin/chain_plugin.hpp>
#include <sysio/signature_provider_plugin/signature_provider_plugin.hpp>
#include <sysio/http_plugin/http_plugin.hpp>
#include <sysio/chain/application.hpp>

namespace sysio {

using boost::signals2::signal;

/**
 * The batch operator plugin is configured with:
 * - An Outpost Client, which dictates the target chain (Ethereum, Solana, etc) 
 * - 2 signature providers, one for WIRE & one for the External Chain
 */
class batch_operator_plugin : public appbase::plugin<batch_operator_plugin> {
public:
   APPBASE_PLUGIN_REQUIRES((chain_plugin)(signature_provider_plugin)(http_plugin))   
   batch_operator_plugin();
   virtual ~batch_operator_plugin();
 

   virtual void set_program_options(options_description&, options_description& cfg) override;
 
   void plugin_initialize(const variables_map& options);
   void plugin_startup();
   void plugin_shutdown();

private:
   std::unique_ptr<class batch_operator_plugin_impl> my;
};

}

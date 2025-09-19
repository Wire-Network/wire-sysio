#pragma once

#include <sysio/batch_operator_plugin/batch_operator_plugin.hpp>
#include <sysio/http_plugin/http_plugin.hpp>
#include <sysio/chain/application.hpp>
#include <sysio/chain_plugin/chain_plugin.hpp>
#include <sysio/signature_provider_plugin/signature_provider_plugin.hpp>

namespace sysio {

using boost::signals2::signal;

/**
 * The batch operator plugin is configured with:
 * - An Outpost Client, which dictates the target chain (Ethereum, Solana, etc) 
 * - 2 signature providers, one for WIRE & one for the External Chain
 */
class challenger_plugin : public appbase::plugin<challenger_plugin> {
public:
   APPBASE_PLUGIN_REQUIRES((chain_plugin)(signature_provider_plugin)(http_plugin)(batch_operator_plugin))   
   challenger_plugin();
   virtual ~challenger_plugin();
 

   virtual void set_program_options(options_description&, options_description& cfg) override;
 
   void plugin_initialize(const variables_map& options);
   void plugin_startup();
   void plugin_shutdown();

private:
   std::unique_ptr<class challenger_plugin_impl> my;
};

}

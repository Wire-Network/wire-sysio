#pragma once

#include <sysio/outpost_ethereum_client_plugin.hpp>

namespace sysio {

class beacon_chain_update_plugin : public appbase::plugin<beacon_chain_update_plugin> {
public:
   APPBASE_PLUGIN_REQUIRES((outpost_ethereum_client_plugin)(signature_provider_manager_plugin))
   beacon_chain_update_plugin();
   virtual ~beacon_chain_update_plugin() = default;

   virtual void set_program_options(options_description& cli, options_description& cfg) override;

   virtual void plugin_initialize(const variables_map& options);

   virtual void plugin_startup();

   virtual void plugin_shutdown();

private:
   std::shared_ptr<class beacon_chain_update_plugin_impl> my;
};


} // namespace sysio

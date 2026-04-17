#pragma once

#include <sysio/outpost_ethereum_client_plugin.hpp>

namespace sysio {

class wire_eth_maintenance_plugin : public appbase::plugin<wire_eth_maintenance_plugin> {
public:
   APPBASE_PLUGIN_REQUIRES((outpost_ethereum_client_plugin)(signature_provider_manager_plugin)(cron_plugin))
   wire_eth_maintenance_plugin();
   virtual ~wire_eth_maintenance_plugin() = default;

   virtual void set_program_options(options_description& cli, options_description& cfg) override;

   virtual void plugin_initialize(const variables_map& options);

   virtual void plugin_startup();

   virtual void plugin_shutdown();

private:
   std::shared_ptr<class wire_eth_maintenance_plugin_impl> my;
};


} // namespace sysio

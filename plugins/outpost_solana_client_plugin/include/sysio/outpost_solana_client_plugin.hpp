#pragma once

#include <sysio/outpost_client_plugin.hpp>
#include <sysio/chain_plugin/chain_plugin.hpp>
#include <sysio/outpost_client/solana/solana_client.hpp>
#include <sysio/signature_provider_manager_plugin/signature_provider_manager_plugin.hpp>

namespace sysio {
using namespace sysio::outpost_client::solana;

class outpost_solana_client_plugin : public appbase::plugin<outpost_solana_client_plugin> {
public:
   APPBASE_PLUGIN_REQUIRES((outpost_client_plugin)(signature_provider_manager_plugin))
   outpost_solana_client_plugin()          = default;
   virtual ~outpost_solana_client_plugin() = default;

   virtual void set_program_options(options_description& cli, options_description& cfg) override;

   virtual void plugin_initialize(const variables_map& options);

   virtual void plugin_startup();

   virtual void plugin_shutdown();

private:
   // std::optional<boost::signals2::scoped_connection> _irreversible_block_connection{std::nullopt};
};


} // namespace sysio
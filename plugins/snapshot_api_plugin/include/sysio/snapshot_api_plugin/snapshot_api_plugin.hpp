#pragma once

#include <sysio/chain_plugin/chain_plugin.hpp>
#include <sysio/producer_plugin/producer_plugin.hpp>
#include <sysio/http_plugin/http_plugin.hpp>

#include <sysio/chain/application.hpp>

namespace sysio {

using namespace appbase;

/// See snapshot_api_plugin/README.md
class snapshot_api_plugin : public plugin<snapshot_api_plugin> {
public:
   APPBASE_PLUGIN_REQUIRES((chain_plugin)(producer_plugin)(http_plugin))

   snapshot_api_plugin() = default;
   snapshot_api_plugin(const snapshot_api_plugin&) = delete;
   snapshot_api_plugin(snapshot_api_plugin&&) = delete;
   snapshot_api_plugin& operator=(const snapshot_api_plugin&) = delete;
   snapshot_api_plugin& operator=(snapshot_api_plugin&&) = delete;
   ~snapshot_api_plugin() override = default;

   void set_program_options(options_description& cli, options_description& cfg) override {}
   void plugin_initialize(const variables_map& vm);
   void plugin_startup();
   void plugin_shutdown() {}
};

}

#pragma once

#include <sysio/chain_plugin/chain_plugin.hpp>
#include <sysio/operator_plugin/operator_plugin.hpp>
#include <sysio/signature_provider_plugin/signature_provider_plugin.hpp>


namespace sysio::batch_operator_plugin {

  class batch_operator_plugin : public appbase::plugin<batch_operator_plugin> {
    public:

      APPBASE_PLUGIN_REQUIRES((chain_plugin)(operator_plugin::operator_plugin)(signature_provider_plugin))

      virtual ~batch_operator_plugin() = default;

      virtual void set_program_options(options_description& cli, options_description& cfg);


      virtual void plugin_initialize(const variables_map& options);

      virtual void plugin_startup();

      virtual void plugin_shutdown();

  };



} // namespace sysio

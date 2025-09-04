#pragma once
// #include <appbase/application.hpp>
// #include <appbase/abstract_plugin.hpp>
// #include <boost/program_options/options_description.hpp>
// #include <boost/program_options/variables_map.hpp>
#include <sysio/chain/application.hpp>
#include <sysio/http_plugin/http_plugin.hpp>
#include <sysio/chain_plugin/chain_plugin.hpp>
#include <sysio/producer_plugin/producer_plugin.hpp>
#include <sysio/net_plugin/net_plugin.hpp>


namespace sysio {
  using namespace appbase;

  class ca_plugin : public appbase::plugin<ca_plugin> {
    public:

      APPBASE_PLUGIN_REQUIRES((http_plugin)(chain_plugin)(producer_plugin)(net_plugin))

      ca_plugin();

      virtual ~ca_plugin();

      virtual void set_program_options(
        boost::program_options::options_description& cli,
        boost::program_options::options_description& cfg
      ) override;

      void plugin_initialize(const boost::program_options::variables_map& options);

      void plugin_startup();

      void plugin_shutdown();
  };
}

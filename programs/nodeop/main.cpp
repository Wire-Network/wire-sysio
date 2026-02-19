#include <sysio/chain/app.hpp>

#include <sysio/db_size_api_plugin/db_size_api_plugin.hpp>
#include <sysio/net_api_plugin/net_api_plugin.hpp>
#include <sysio/producer_api_plugin/producer_api_plugin.hpp>
#include <sysio/prometheus_plugin/prometheus_plugin.hpp>
#include <sysio/state_history_plugin/state_history_plugin.hpp>
#include <sysio/test_control_api_plugin/test_control_api_plugin.hpp>
#include <sysio/test_control_plugin/test_control_plugin.hpp>
#include <sysio/trace_api/trace_api_plugin.hpp>

#include <sysio/chain_api_plugin/chain_api_plugin.hpp>
#include <sysio/chain_plugin/chain_plugin.hpp>
#include <sysio/net_plugin/net_plugin.hpp>
#include <sysio/producer_plugin/producer_plugin.hpp>

using namespace appbase;
using namespace sysio;

int main(int argc, char** argv)
{
   chain::application exe{application_config{}};
   application_base::register_plugin<trace_api_plugin>();
   application_base::register_plugin<db_size_api_plugin>();
   application_base::register_plugin<http_client_plugin>();
   application_base::register_plugin<http_plugin>();
   application_base::register_plugin<net_api_plugin>();
   application_base::register_plugin<producer_api_plugin>();
   application_base::register_plugin<test_control_plugin>();
   application_base::register_plugin<test_control_api_plugin>();
   application_base::register_plugin<state_history_plugin>();
   application_base::register_plugin<prometheus_plugin>();
   application_base::register_plugin<chain_api_plugin>();
   application_base::register_plugin<signature_provider_manager_plugin>();
   auto r = exe.init<
      resource_monitor_plugin,
      chain_plugin,
      net_plugin,
      producer_plugin
   >(argc, argv);
   if (r != exit_code::SUCCESS)
      return r == exit_code::NODE_MANAGEMENT_SUCCESS ? exit_code::SUCCESS : r;

   producer_plugin& prod_plug = app().get_plugin<producer_plugin>();
   exe.set_stop_executor_cb([&prod_plug]() {
      prod_plug.interrupt();
   });

   return exe.exec();
}

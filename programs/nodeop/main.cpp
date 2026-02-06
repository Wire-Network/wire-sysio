#include <sysio/chain/app.hpp>

#include <sysio/chain_plugin/chain_plugin.hpp>
#include <sysio/net_plugin/net_plugin.hpp>
#include <sysio/producer_plugin/producer_plugin.hpp>

using namespace appbase;
using namespace sysio;

int main(int argc, char** argv)
{
   chain::application exe{application_config{}};

   auto r = exe.init<chain_plugin, net_plugin, producer_plugin>(argc, argv);
   if (r != exit_code::SUCCESS)
      return r == exit_code::NODE_MANAGEMENT_SUCCESS ? exit_code::SUCCESS : r;

   producer_plugin& prod_plug = app().get_plugin<producer_plugin>();
   exe.set_stop_executor_cb([&prod_plug]() {
      prod_plug.interrupt();
   });

   return exe.exec();
}

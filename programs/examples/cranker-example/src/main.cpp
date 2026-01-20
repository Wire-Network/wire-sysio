
#include <sysio/chain/wire_application.hpp>
#include <sysio/cron_plugin.hpp>
#include <sysio/outpost_ethereum_client_plugin.hpp>

using namespace appbase;
using namespace sysio;
using namespace sysio::chain;

int main(int argc, char** argv) {

   wire_application exe{wire_application_config{}};

   auto r = exe.init<outpost_ethereum_client_plugin, cron_plugin>(argc, argv);
   if (r != SUCCESS)
      return r == NODE_MANAGEMENT_SUCCESS ? SUCCESS : r;

   auto& cron_plug = app().get_plugin<cron_plugin>();
   auto& eth_plug = app().get_plugin<outpost_ethereum_client_plugin>();
   auto eth_clients = eth_plug.get_clients();
   FC_ASSERT(!eth_clients.empty(), "At least 1 ethereum client must be configured");
   auto eth_client = eth_clients[0];

   cron_plug.add_job(
      {
         .milliseconds = {0},
         .seconds = {5, 15, 25, 35, 45, 55}
      },
      [&]() {
         auto now = std::chrono::utc_clock::now();
         auto now_str = std::format("{:%H:%M:%S}", now);
         ilogf("{}: Getting ethereum gas price", now_str);

         auto current_price = eth_client->client->get_gas_price();
         ilogf("{}: Current Price> {}WEI", now_str, current_price.str());
      },
      cron_service::job_metadata_t{
         .one_at_a_time = true, .tags = {"ethereum", "gas"}, .label = "cron_5s_heartbeat"});

   return exe.exec();
}

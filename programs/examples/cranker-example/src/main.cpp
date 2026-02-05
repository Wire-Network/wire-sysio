
#include <sysio/chain/app.hpp>
#include <sysio/cron_plugin.hpp>
#include <sysio/outpost_ethereum_client_plugin.hpp>

using namespace appbase;
using namespace sysio;
using namespace sysio::chain;

int main(int argc, char** argv) {

   chain::application exe{application_config{}};

   auto r = exe.init<outpost_ethereum_client_plugin, cron_plugin>(argc, argv);
   if (r != exit_code::SUCCESS)
      return r == exit_code::NODE_MANAGEMENT_SUCCESS ? exit_code::SUCCESS : r;

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
         ilog("{}: Getting ethereum gas price", now_str);

         auto current_price = eth_client->client->get_gas_price();
         ilog("{}: Current Price> {}WEI", now_str, current_price.str());
      },
      cron_service::job_metadata_t{
         .one_at_a_time = true, .tags = {"ethereum", "gas"}, .label = "cron_5s_heartbeat"});

   return exe.exec();
}

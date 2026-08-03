
#include <sysio/chain/app.hpp>
#include <sysio/cron_plugin.hpp>
#include <sysio/outpost_ethereum_client_plugin.hpp>

using namespace appbase;
using namespace sysio;
using namespace sysio::chain;

int main(int argc, char** argv) {

   chain::application exe{application_config{.enable_resource_monitor = false, .log_on_exit = false}};

   auto r = exe.init<outpost_ethereum_client_plugin, cron_plugin>(argc, argv);
   if (r != exit_code::SUCCESS)
      return r == exit_code::NODE_MANAGEMENT_SUCCESS ? exit_code::SUCCESS : r;

   try {
      auto& cron_plug = app().get_plugin<cron_plugin>();
      auto& eth_plug = app().get_plugin<outpost_ethereum_client_plugin>();
      // Ethereum clients are constructed at plugin_initialize (the signature_provider_manager_plugin creates every
      // provider at its own init, before this plugin initializes), so they are ready to read immediately after init<>.
      auto eth_clients = eth_plug.get_clients();
      FC_ASSERT(!eth_clients.empty(), "At least 1 ethereum client must be configured");
      auto eth_client = eth_clients[0];

      cron_plug.add_job(
         {
            .milliseconds = {cron_service::job_schedule::step_value{5000}}
         },
         [&]() {
            // The logging framework already timestamps log messages.
            ilog("Getting ethereum gas price");

            auto current_price = eth_client->client->get_gas_price();
            ilog("Current Price> {}WEI", current_price.str());
         },
         cron_service::job_metadata_t{
            .one_at_a_time = true, .tags = {"ethereum", "gas"}, .label = "cron_5s_heartbeat"});

      return exe.exec();
   } catch (const fc::exception& e) {
      elog("{}", e.to_detail_string());
   } catch (const boost::exception& e) {
      elog("{}", boost::diagnostic_information(e));
   } catch (const std::exception& e) {
      elog("{}", e.what());
   } catch (...) {
      elog("unknown exception");
   }

   return exit_code::OTHER_FAIL;

}

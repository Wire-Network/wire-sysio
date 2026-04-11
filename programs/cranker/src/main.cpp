
#include <sysio/chain/app.hpp>
#include <sysio/cron_plugin.hpp>
#include <sysio/outpost_ethereum_client_plugin.hpp>
#include <sysio/beacon_chain_update_plugin.hpp>

using namespace appbase;
using namespace sysio;
using namespace sysio::chain;

int main(int argc, char** argv) {

   chain::application exe{application_config{.enable_resource_monitor = false, .log_on_exit = false}};

   auto r = exe.init<signature_provider_manager_plugin, outpost_ethereum_client_plugin, cron_plugin, beacon_chain_update_plugin>(argc, argv);
   if (r != exit_code::SUCCESS)
      return r == exit_code::NODE_MANAGEMENT_SUCCESS ? exit_code::SUCCESS : r;

   try {
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

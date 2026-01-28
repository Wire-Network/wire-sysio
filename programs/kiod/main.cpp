#include <sysio/chain/app.hpp>

#include <sysio/http_plugin/http_plugin.hpp>
#include <sysio/wallet_plugin/wallet_plugin.hpp>
#include <sysio/wallet_api_plugin/wallet_api_plugin.hpp>
#include <sysio/version/version.hpp>

#include "config.hpp"

using namespace sysio;
using namespace sysio::chain;

int main(int argc, char** argv)
{
   chain::application exe{application_config{.enable_resource_monitor = false}};

   std::filesystem::path home = fc::home_path();
   app().set_default_data_dir(home / "sysio-wallet");
   app().set_default_config_dir(home / "sysio-wallet");
   http_plugin::set_defaults({
      .default_unix_socket_path = kiod::config::key_store_executable_name + ".sock",
      .default_http_port = 0,
      .server_header = kiod::config::key_store_executable_name + "/" + app().version_string(),
      .support_categories = false
   });

   auto r = exe.init<wallet_plugin, wallet_api_plugin, http_plugin>(argc, argv);
   if (r != exit_code::SUCCESS)
      return r == exit_code::NODE_MANAGEMENT_SUCCESS ? exit_code::SUCCESS : r;

   auto& http = app().get_plugin<http_plugin>();
   http.add_handler({"/v1/" + kiod::config::key_store_executable_name + "/stop",
                    api_category::node,
                    [](string, string, url_response_callback cb) {
      cb(200, fc::variant(fc::variant_object()));
      app().quit();
   }}, appbase::exec_queue::read_write );

   return exe.exec();
}

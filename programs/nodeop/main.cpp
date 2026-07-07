#include <sysio/batch_operator_plugin/batch_operator_plugin.hpp>
#include <sysio/chain/app.hpp>
#include <sysio/chain_api_plugin/chain_api_plugin.hpp>
#include <sysio/chain_plugin/chain_plugin.hpp>
#include <sysio/db_size_api_plugin/db_size_api_plugin.hpp>
#include <sysio/external_debugging_plugin/external_debugging_plugin.hpp>
#include <sysio/net_api_plugin/net_api_plugin.hpp>
#include <sysio/net_plugin/net_plugin.hpp>
#include <sysio/producer_api_plugin/producer_api_plugin.hpp>
#include <sysio/producer_plugin/producer_plugin.hpp>
#include <sysio/prometheus_plugin/prometheus_plugin.hpp>
#include <sysio/signature_provider_manager_plugin/signature_provider_manager_plugin.hpp>
#include <sysio/signature_provider_manager_plugin/ssm/ssm_signature_provider.hpp>
#include <sysio/snapshot_api_plugin/snapshot_api_plugin.hpp>
#include <sysio/state_history_plugin/state_history_plugin.hpp>
#include <sysio/test_control_api_plugin/test_control_api_plugin.hpp>
#include <sysio/test_control_plugin/test_control_plugin.hpp>
#include <sysio/trace_api/trace_api_plugin.hpp>
#include <sysio/underwriter_plugin/underwriter_plugin.hpp>

#include <string>
#include <string_view>

using namespace appbase;
using namespace sysio;

namespace {
/// Spec scheme nodeop registers for AWS SSM Parameter Store-backed keys.
constexpr std::string_view ssm_spec_scheme = "SSM";
} // namespace

int main(int argc, char** argv)
{
   chain::application exe{application_config{}};
   application_base::register_plugin<trace_api_plugin>();
   application_base::register_plugin<db_size_api_plugin>();
   application_base::register_plugin<http_client_plugin>();
   application_base::register_plugin<http_plugin>();
   application_base::register_plugin<net_api_plugin>();
   application_base::register_plugin<producer_api_plugin>();
   application_base::register_plugin<snapshot_api_plugin>();
   application_base::register_plugin<test_control_plugin>();
   application_base::register_plugin<test_control_api_plugin>();
   application_base::register_plugin<state_history_plugin>();
   application_base::register_plugin<prometheus_plugin>();
   application_base::register_plugin<chain_api_plugin>();
   application_base::register_plugin<signature_provider_manager_plugin>();
   application_base::register_plugin<batch_operator_plugin>();
   application_base::register_plugin<external_debugging_plugin>();
   application_base::register_plugin<underwriter_plugin>();
   // Opt nodeop into the `SSM:` signature-provider scheme: the private key is
   // fetched once from AWS SSM Parameter Store (SecureString) at startup and
   // signing is local thereafter, so it is suitable for every signing path
   // including block production. Registration must precede exe.init(), which
   // parses --signature-provider specs during plugin initialization. The
   // registration itself is inert -- a registry entry only, no AWS SDK init,
   // threads, or network -- unless a configured spec actually uses `SSM:`.
   app()._register_plugin<signature_provider_manager_plugin>().register_spec_handler(
      std::string{ssm_spec_scheme}, &sysio::sigprov::ssm::create_ssm_provider);
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

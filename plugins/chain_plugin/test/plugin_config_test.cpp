#include <array>
#include <boost/program_options.hpp>
#include <boost/test/unit_test.hpp>
#include <sysio/chain/app.hpp>
#include <sysio/chain_plugin/chain_plugin.hpp>
#include <stdint.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

/** Initialize chain_plugin with one snapshot response-size limit override. */
sysio::chain::exit_code::exit_code initialize_with_snapshot_size_limit(std::string_view option_name,
                                                                       std::string_view option_value) {
   fc::temp_directory tmp;
   sysio::chain::application exe({.enable_resource_monitor = false});

   const auto tmp_path = tmp.path().string();
   std::vector<std::string> arguments{
      "test_chain_plugin",
      "--snapshot-endpoint",
      "http://127.0.0.1:1",
      "--config-dir",
      tmp_path,
      "--data-dir",
      tmp_path,
      "--" + std::string(option_name),
      std::string(option_value),
   };
   std::vector<char*> argv;
   argv.reserve(arguments.size());
   for (auto& argument : arguments) {
      argv.push_back(argument.data());
   }

   return exe.init<sysio::chain_plugin>(static_cast<int>(argv.size()), argv.data());
}

} // anonymous namespace

BOOST_AUTO_TEST_CASE(chain_plugin_default_tests) {
   fc::temp_directory  tmp;
   sysio::chain::application exe({
      .enable_resource_monitor = false
   });

   auto tmp_path = tmp.path().string();
   std::array          args = {
       "test_chain_plugin", "--blocks-log-stride", "10", "--config-dir", tmp_path.c_str(), "--data-dir", tmp_path.c_str(),
   };

   BOOST_CHECK(exe.init<sysio::chain_plugin>(args.size(), const_cast<char**>(args.data())) == sysio::chain::exit_code::SUCCESS);
   auto& plugin = appbase::app().get_plugin<sysio::chain_plugin>();

   auto* config = std::get_if<sysio::chain::partitioned_blocklog_config>(&plugin.chain_config().blog);
   BOOST_REQUIRE(config);
   BOOST_CHECK_EQUAL(config->max_retained_files, UINT32_MAX);
   BOOST_CHECK_EQUAL(config->stride, 10);

}

/** Verify snapshot endpoint registration and removal of endpoint-specific resource knobs. */
BOOST_AUTO_TEST_CASE(chain_plugin_snapshot_endpoint_option_registration) {
   sysio::chain::application exe({.enable_resource_monitor = false});
   sysio::chain_plugin plugin;
   boost::program_options::options_description cli;
   boost::program_options::options_description cfg;
   boost::program_options::options_description options;
   plugin.set_program_options(cli, cfg);
   options.add(cli).add(cfg);

   std::array arguments{
      "test_chain_plugin",
      "--snapshot-endpoint", "http://127.0.0.1:1",
   };
   boost::program_options::variables_map variables;
   boost::program_options::store(
      boost::program_options::parse_command_line(arguments.size(), const_cast<char**>(arguments.data()), options),
      variables);
   boost::program_options::notify(variables);

   BOOST_CHECK_EQUAL(variables.at("snapshot-endpoint").as<std::string>(), "http://127.0.0.1:1");
   BOOST_CHECK(options.find_nothrow("snapshot-endpoint-additional-ca-file", false) != nullptr);
   BOOST_CHECK(options.find_nothrow("snapshot-endpoint-additional-ca-path", false) != nullptr);
   BOOST_CHECK(options.find_nothrow("snapshot-endpoint-proxy", false) != nullptr);

   constexpr std::array removed_options{
      "snapshot-endpoint-connect-timeout-ms",
      "snapshot-endpoint-header-timeout-ms",
      "snapshot-endpoint-idle-timeout-ms",
      "snapshot-endpoint-total-timeout-ms",
      "snapshot-endpoint-max-download-size-mb",
      "snapshot-endpoint-min-disk-free-mb",
   };
   for (const auto* option_name : removed_options) {
      BOOST_CHECK(options.find_nothrow(option_name, false) == nullptr);
   }
}

/** Verify that the snapshot response-size limit rejects zero and overflow before connecting. */
BOOST_AUTO_TEST_CASE(chain_plugin_snapshot_endpoint_option_validation) {
   constexpr std::array invalid_options{
      std::pair{"chain-state-db-size-mb", "0"},
      std::pair{"chain-state-db-size-mb", "17592186044416"},
   };

   for (const auto& [option_name, option_value] : invalid_options) {
      BOOST_TEST_CONTEXT(option_name << '=' << option_value) {
         BOOST_CHECK(initialize_with_snapshot_size_limit(option_name, option_value) != sysio::chain::exit_code::SUCCESS);
      }
   }
}

#ifdef SYSIO_SYS_VM_OC_RUNTIME_ENABLED
/** Verify the default SYS VM OC whitelist when the OC runtime is compiled into this build. */
BOOST_AUTO_TEST_CASE(chain_plugin_default_sys_vm_oc_whitelist) {
   fc::temp_directory  tmp;
   sysio::chain::application exe({
      .enable_resource_monitor = false
   });

   auto tmp_path = tmp.path().string();
   std::array          args = {
       "test_chain_plugin", "--config-dir", tmp_path.c_str(), "--data-dir", tmp_path.c_str(),
   };

   BOOST_CHECK(exe.init<sysio::chain_plugin>(args.size(), const_cast<char**>(args.data())) == sysio::chain::exit_code::SUCCESS);
   auto& plugin = appbase::app().get_plugin<sysio::chain_plugin>();

   BOOST_CHECK(plugin.chain().is_sys_vm_oc_whitelisted(sysio::chain::name{"wire"}));
   BOOST_CHECK(plugin.chain().is_sys_vm_oc_whitelisted(sysio::chain::name{"core.wire"}));
   BOOST_CHECK(plugin.chain().is_sys_vm_oc_whitelisted(sysio::chain::name{"xs.wire"}));
   BOOST_CHECK(!plugin.chain().is_sys_vm_oc_whitelisted(sysio::chain::name{"vault"}));
   BOOST_CHECK(!plugin.chain().is_sys_vm_oc_whitelisted(sysio::chain::name{"xs"}));
   BOOST_CHECK(!plugin.chain().is_sys_vm_oc_whitelisted(sysio::chain::name{""}));
}

/** Verify command-line overrides for the SYS VM OC whitelist on builds that include the OC runtime. */
BOOST_AUTO_TEST_CASE(chain_plugin_sys_vm_oc_whitelist) {
   fc::temp_directory  tmp;
   sysio::chain::application exe({
      .enable_resource_monitor = false
   });

   auto tmp_path = tmp.path().string();
   std::array          args = {
      "test_chain_plugin", "--sys-vm-oc-whitelist", "hello", "--config-dir", tmp_path.c_str(), "--data-dir", tmp_path.c_str(),
  };

   BOOST_CHECK(exe.init<sysio::chain_plugin>(args.size(), const_cast<char**>(args.data())) == sysio::chain::exit_code::SUCCESS);
   auto& plugin = appbase::app().get_plugin<sysio::chain_plugin>();
   BOOST_CHECK(plugin.chain().is_sys_vm_oc_whitelisted(sysio::chain::name{"hello"}));
   BOOST_CHECK(plugin.chain().is_sys_vm_oc_whitelisted(sysio::chain::name{"xs.hello"}));
   BOOST_CHECK(!plugin.chain().is_sys_vm_oc_whitelisted(sysio::chain::name{"wire"}));
}
#endif

#include <array>
#include <boost/test/unit_test.hpp>
#include <sysio/chain/app.hpp>
#include <sysio/chain_plugin/chain_plugin.hpp>
#include <stdint.h>

BOOST_AUTO_TEST_CASE(chain_plugin_default_tests) {
   fc::temp_directory  tmp;
   sysio::chain::application exe({});

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

   // test default sys-vm-oc-whitelist
   BOOST_CHECK(plugin.chain().is_sys_vm_oc_whitelisted(sysio::chain::name{"wire"}));
   BOOST_CHECK(plugin.chain().is_sys_vm_oc_whitelisted(sysio::chain::name{"core.wire"}));
   BOOST_CHECK(plugin.chain().is_sys_vm_oc_whitelisted(sysio::chain::name{"xs.wire"}));
   BOOST_CHECK(!plugin.chain().is_sys_vm_oc_whitelisted(sysio::chain::name{"vault"}));
   BOOST_CHECK(!plugin.chain().is_sys_vm_oc_whitelisted(sysio::chain::name{"xs"}));
   BOOST_CHECK(!plugin.chain().is_sys_vm_oc_whitelisted(sysio::chain::name{""}));
}

BOOST_AUTO_TEST_CASE(chain_plugin_sys_vm_oc_whitelist) {
   fc::temp_directory  tmp;
   sysio::chain::application exe({});

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

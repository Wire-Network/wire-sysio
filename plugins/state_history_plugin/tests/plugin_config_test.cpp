#include <array>
#include <boost/test/unit_test.hpp>
#include <sysio/state_history/log.hpp>
#include <sysio/state_history_plugin/state_history_plugin.hpp>
#include <fc/filesystem.hpp>
#include <stdint.h>

BOOST_AUTO_TEST_CASE(state_history_plugin_default_tests) {
   fc::temp_directory  tmp;
   appbase::scoped_app app;

   auto tmp_path = tmp.path().string();
   std::array args = {"test_state_history",    "--trace-history", "--state-history-stride", "10",
                      "--data-dir",      tmp_path.c_str()};

   BOOST_CHECK(app->initialize<sysio::state_history_plugin>(args.size(), const_cast<char**>(args.data())));
   auto& plugin = app->get_plugin<sysio::state_history_plugin>();

   BOOST_REQUIRE(plugin.trace_log());
   auto* config = std::get_if<sysio::state_history::partition_config>(&plugin.trace_log()->config());
   BOOST_REQUIRE(config);
   BOOST_CHECK_EQUAL(config->max_retained_files, UINT32_MAX);
}

BOOST_AUTO_TEST_CASE(state_history_plugin_retain_blocks_tests) {
   fc::temp_directory  tmp;
   appbase::scoped_app app;

   auto tmp_path = tmp.path().string();
   std::array args = {"test_state_history",    "--trace-history", "--state-history-log-retain-blocks", "4242",
                      "--data-dir",      tmp_path.c_str()};

   BOOST_CHECK(app->initialize<sysio::state_history_plugin>(args.size(), const_cast<char**>(args.data())));
   auto& plugin = app->get_plugin<sysio::state_history_plugin>();

   BOOST_REQUIRE(plugin.trace_log());
   auto* config = std::get_if<sysio::state_history::prune_config>(&plugin.trace_log()->config());
   BOOST_REQUIRE(config);
   BOOST_CHECK_EQUAL(config->prune_blocks, 4242u);
}

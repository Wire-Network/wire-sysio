#include <boost/test/unit_test.hpp>

#include <sysio/producer_plugin/producer_plugin.hpp>
#include <sysio/producer_plugin/snapshot_attestation_recovery.hpp>

#include <sysio/testing/tester.hpp>

#include <sysio/chain/genesis_state.hpp>
#include <sysio/chain/thread_utils.hpp>
#include <sysio/chain/trace.hpp>
#include <sysio/chain/name.hpp>

#include <sysio/chain/application.hpp>

#include "snapshot_attestation_test_utils.hpp"

#include <limits>

using namespace sysio;
using namespace sysio::chain;

BOOST_AUTO_TEST_SUITE(program_options)

BOOST_AUTO_TEST_CASE(state_dir) {
   fc::temp_directory temp;
   auto temp_dir = temp.path();
   auto state_dir = temp.path() / "state";
   auto custom_state_dir = temp.path() / "custom_state_dir";

   auto temp_dir_str = temp_dir.string();
   auto custom_state_dir_str = custom_state_dir.string();
      
   appbase::scoped_app app;

   std::promise<std::tuple<producer_plugin*, chain_plugin*>> plugin_promise;
   std::future<std::tuple<producer_plugin*, chain_plugin*>> plugin_fut = plugin_promise.get_future();
   std::thread app_thread( [&]() {
      try {
         fc::logger::default_logger().set_log_level(fc::log_level::debug);
         std::vector<const char*> argv =
            {"test",
             "--data-dir",   temp_dir_str.c_str(),
             "--state-dir",  custom_state_dir_str.c_str(),
             "--config-dir", temp_dir_str.c_str(),
             "-p", "sysio", "-e" };
         app->initialize<chain_plugin, producer_plugin>( argv.size(), (char**) &argv[0] );
         app->startup();
         // app was constructed on the outer thread; capture this thread as main_thread_id_
         // before releasing the promise so producer_plugin's main-thread asserts see the loop thread.
         app->executor().set_main_thread_id();
         plugin_promise.set_value( {app->find_plugin<producer_plugin>(), app->find_plugin<chain_plugin>()} );
         app->exec();
         return;
      } FC_LOG_AND_DROP()
      BOOST_CHECK(!"app threw exception see logged error");
   } );

   auto[prod_plug, chain_plug] = plugin_fut.get();
   [[maybe_unused]] auto chain_id = chain_plug->get_chain_id();

   // check that "--state-dir" option was taken into account
   BOOST_CHECK(  exists( custom_state_dir ));
   BOOST_CHECK( !exists( state_dir ));
      
   app->quit();
   app_thread.join();
}

/** Refuse snapshot-provider startup while the durable disagreement latch is set. */
BOOST_AUTO_TEST_CASE(quarantined_snapshot_provider_rejects_startup) {
   fc::temp_directory temp;
   const auto data_dir = temp.path() / "data";
   const auto state_dir = temp.path() / "state";
   const auto config_dir = temp.path() / "config";
   const auto snapshots_dir = temp.path() / "snapshots";
   std::filesystem::create_directories(data_dir);
   std::filesystem::create_directories(config_dir);
   std::filesystem::create_directories(snapshots_dir);

   const auto quarantined = snapshot_attestation_test::make_recovery_state(
      genesis_state{}.compute_chain_id(), true);
   const auto sidecar = snapshots_dir / snapshot_attestation_recovery_filename;
   save_snapshot_attestation_recovery_state(sidecar, quarantined);

   const auto data_dir_string = data_dir.string();
   const auto state_dir_string = state_dir.string();
   const auto config_dir_string = config_dir.string();
   const auto snapshots_dir_string = snapshots_dir.string();
   appbase::scoped_app app;
   std::vector<const char*> argv = {
      "test",
      "--data-dir", data_dir_string.c_str(),
      "--state-dir", state_dir_string.c_str(),
      "--config-dir", config_dir_string.c_str(),
      "--snapshots-dir", snapshots_dir_string.c_str(),
      "--snapshot-provider-account", snapshot_attestation_test::provider_account_name,
   };

   BOOST_CHECK_THROW(
      (app->initialize<chain_plugin, producer_plugin>(argv.size(), (char**)&argv[0])),
      plugin_config_exception);

   const auto retained = load_snapshot_attestation_recovery_state(sidecar);
   BOOST_CHECK(retained.disagreement_detected);
   BOOST_REQUIRE(retained.pending_vote);
   BOOST_CHECK(retained.pending_vote->head_block_id == quarantined.pending_vote->head_block_id);
}

/** Align the automatic provider schedule with exact snapshot interval multiples. */
BOOST_AUTO_TEST_CASE(snapshot_provider_auto_schedule_targets_exact_block_multiples) {
   const auto request = make_snapshot_provider_auto_schedule_request();
   BOOST_CHECK_EQUAL(snapshot_provider_block_spacing, request.block_spacing);
   // on_start_block(start + 1) snapshots the current head, so start itself is the exact
   // snapshotted height and each recurrence remains an exact interval multiple.
   BOOST_CHECK_EQUAL(snapshot_provider_block_spacing, request.start_block_num);
   BOOST_CHECK_EQUAL(std::numeric_limits<uint32_t>::max(), request.end_block_num);
}

BOOST_AUTO_TEST_SUITE_END()

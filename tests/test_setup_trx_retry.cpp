#include <boost/test/unit_test.hpp>

#include <sysio/producer_plugin/producer_plugin.hpp>
#include <sysio/chain/application.hpp>
#include <sysio/chain/config.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/trace.hpp>

#include <contracts.hpp>
#include "chain_test_utils.hpp"

#include <chrono>
#include <future>
#include <memory>
#include <thread>
#include <vector>

namespace {
using namespace sysio;
using namespace sysio::chain;
using namespace sysio::test_utils;

/// A drop the node attributes to an objective cpu limit, which a resubmission is expected to clear.
/// Returned as the concrete type so throwing it does not slice away what the test is asserting on.
tx_cpu_usage_exceeded resubmittable_drop() {
   return tx_cpu_usage_exceeded( FC_LOG_MESSAGE( error, "injected objective cpu drop" ) );
}

/// A failure that says something is wrong with the transaction itself, which a resubmission cannot clear.
sysio_assert_message_exception permanent_failure() {
   return sysio_assert_message_exception( FC_LOG_MESSAGE( error, "injected contract assert" ) );
}

/// Apply a producer runtime option from the app thread that owns the producer plugin, and wait for it to land.
/// @param app  running application whose thread owns the plugin
/// @param prod producer plugin to reconfigure
/// @param ms   new max-transaction-time in milliseconds, negative for no cap
void set_max_transaction_time( appbase::scoped_app& app, producer_plugin* prod, int32_t ms ) {
   auto applied = std::make_shared<std::promise<void>>();
   std::future<void> applied_future = applied->get_future();

   app->executor().post( priority::high, exec_queue::read_write, [prod, ms, applied]() {
      prod->update_runtime_options( producer_plugin::runtime_options{ .max_transaction_time = ms } );
      applied->set_value();
   });

   BOOST_REQUIRE( applied_future.wait_for( std::chrono::seconds(5) ) != std::future_status::timeout );
}

/// Build the setcode transaction that deploys the bios contract to the system account.
/// This is the transaction the CI failure dropped, so it is the one worth driving through the policy.
signed_transaction bios_setcode_trx() {
   const auto& wasm = testing::contracts::sysio_bios_wasm();

   signed_transaction trx;
   trx.actions.emplace_back( std::vector<permission_level>{{config::system_account_name, config::active_name}},
                             chain::setcode{
                                .account   = config::system_account_name,
                                .vmtype    = 0,
                                .vmversion = 0,
                                .code      = bytes( wasm.begin(), wasm.end() )
                             });
   return trx;
}
} // namespace

BOOST_AUTO_TEST_SUITE(setup_trx_retry)

// The policy keeps pushing while the node reports a drop a resubmission can clear, and returns the trace of the
// attempt the node finally accepted.
BOOST_AUTO_TEST_CASE(resubmits_until_the_node_accepts) {
   size_t pushes = 0;
   size_t waits  = 0;
   auto   accepted = std::make_shared<transaction_trace>();

   auto trace = resubmit_setup_trx(
      [&]() -> transaction_trace_ptr {
         if( ++pushes == 1 )
            throw resubmittable_drop();
         return accepted;
      },
      [&]() { ++waits; return true; } );

   BOOST_CHECK( trace == accepted );
   BOOST_CHECK_EQUAL( pushes, 2u );
   BOOST_CHECK_EQUAL( waits, 1u );  // the chain is waited on once, before the resubmission
}

// A node that keeps dropping the transaction is reported to the test rather than resubmitted at forever, and the
// failure the test sees is the one from the last attempt.
BOOST_AUTO_TEST_CASE(reports_the_drop_after_the_last_attempt) {
   size_t pushes = 0;
   size_t waits  = 0;

   BOOST_CHECK_THROW( resubmit_setup_trx(
                         [&]() -> transaction_trace_ptr { ++pushes; throw resubmittable_drop(); },
                         [&]() { ++waits; return true; } ),
                      tx_cpu_usage_exceeded );

   BOOST_CHECK_EQUAL( pushes, max_setup_trx_attempts );
   BOOST_CHECK_EQUAL( waits, max_setup_trx_attempts - 1 );  // no wait after the attempt that is reported
}

// A failure a resubmission cannot clear is a real test failure: it is reported from the first attempt, without
// spending the remaining attempts or waiting on the chain.
BOOST_AUTO_TEST_CASE(reports_a_failure_a_resubmission_cannot_clear) {
   size_t pushes = 0;
   size_t waits  = 0;

   BOOST_CHECK_THROW( resubmit_setup_trx(
                         [&]() -> transaction_trace_ptr { ++pushes; throw permanent_failure(); },
                         [&]() { ++waits; return true; } ),
                      sysio_assert_message_exception );

   BOOST_CHECK_EQUAL( pushes, 1u );
   BOOST_CHECK_EQUAL( waits, 0u );
}

// A chain that stops advancing is reported as the drop that got us there, not as a wait that timed out: resubmitting
// against the same head would only repeat the transaction that was just dropped.
BOOST_AUTO_TEST_CASE(reports_the_drop_when_the_chain_stops_advancing) {
   size_t pushes = 0;
   size_t waits  = 0;

   BOOST_CHECK_THROW( resubmit_setup_trx(
                         [&]() -> transaction_trace_ptr { ++pushes; throw resubmittable_drop(); },
                         [&]() { ++waits; return false; } ),
                      tx_cpu_usage_exceeded );

   BOOST_CHECK_EQUAL( pushes, 1u );
   BOOST_CHECK_EQUAL( waits, 1u );
}

// End to end against a producing node: a cap of 0ms guarantees the node drops the bios setcode on the first attempt,
// and lifting the cap before the second guarantees it is accepted. That covers what the helper has to get right for a
// resubmission to be a resubmission at all -- the chain advanced, and the transaction was restamped against the new
// head and re-signed, so the node sees a distinct transaction rather than the one it just dropped.
BOOST_AUTO_TEST_CASE(resubmits_a_setup_transaction_the_node_drops) {
   fc::temp_directory  temp;
   appbase::scoped_app app;
   auto                temp_dir_str = temp.path().string();

   std::promise<std::tuple<producer_plugin*, chain_plugin*>> plugin_promise;
   std::future<std::tuple<producer_plugin*, chain_plugin*>>  plugin_fut = plugin_promise.get_future();

   std::thread app_thread( [&]() {
      try {
         std::vector<const char*> argv = {
            "test",
            "-p", "sysio", "-e",
            "--data-dir", temp_dir_str.c_str(),
            "--config-dir", temp_dir_str.c_str(),
            // 0 == every transaction is out of time before it starts, so the first attempt is dropped for certain.
            "--max-transaction-time=0",
            "--abi-serializer-max-time-ms=999"
         };
         app->initialize<chain_plugin, producer_plugin>( argv.size(), (char**)&argv[0] );
         app->find_plugin<chain_plugin>()->chain();
         app->startup();
         app->executor().set_main_thread_id();
         plugin_promise.set_value( {app->find_plugin<producer_plugin>(), app->find_plugin<chain_plugin>()} );
         app->exec();
         return;
      } FC_LOG_AND_DROP()
      BOOST_CHECK(!"app threw exception see logged error");
   } );

   auto shutdown = fc::make_scoped_exit( [&]() {
      app->quit();
      if( app_thread.joinable() )
         app_thread.join();
   } );

   auto[prod_plug, chain_plug] = plugin_fut.get();
   auto& control = chain_plug->chain();

   const auto before = read_head( app, control );
   BOOST_REQUIRE( before.has_value() );

   size_t                     attempts = 0;
   std::optional<fc::exception> first_failure;
   auto                       trx      = bios_setcode_trx();
   auto                       trace    = resubmit_setup_trx(
      [&]() {
         // Lift the cap for the resubmission only, so the first attempt is dropped and the second is not.
         if( ++attempts == 2 )
            set_max_transaction_time( app, prod_plug, -1 );
         try {
            return push_input_trx_once( app, control, config::system_account_name, trx );
         } catch( const fc::exception& e ) {
            if( !first_failure )
               first_failure = e;
            throw;
         }
      },
      [&]() { return wait_for_head_block_advance( app, control ); } );

   BOOST_CHECK_EQUAL( attempts, 2u );
   BOOST_REQUIRE( first_failure.has_value() );  // the node really did drop the first attempt
   BOOST_CHECK_EQUAL( first_failure->code(), tx_cpu_usage_exceeded::code_value );  // for the reason CI hit
   BOOST_REQUIRE( !!trace );
   BOOST_CHECK( !!trace->receipt );        // the resubmission was accepted into a block
   BOOST_CHECK( !trace->except );

   const auto after = read_head( app, control );
   BOOST_REQUIRE( after.has_value() );
   BOOST_CHECK_GT( after->block_num, before->block_num );  // the resubmission waited for a block it could reference
}

BOOST_AUTO_TEST_SUITE_END()

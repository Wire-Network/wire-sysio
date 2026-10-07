#include <boost/test/unit_test.hpp>

#include <sysio/producer_plugin/producer_plugin.hpp>

#include <sysio/testing/tester.hpp>

#include <sysio/chain/application.hpp>
#include <sysio/chain/fork_database.hpp>
#include <sysio/chain/platform_timer.hpp>

#include <fc/scoped_exit.hpp>

#include <future>
#include <thread>

using namespace sysio;
using namespace sysio::chain;

namespace {

/// Starts a node in read_mode, holds its main thread with the transaction timer running, reports a received block the
/// way net_plugin does and returns the timer state. received_block() interrupts on the calling thread, so the state
/// is final once it returns.
platform_timer::state_t timer_state_after_received_block(const char* read_mode, fork_db_add_t fork_db_add_result) {
   using namespace std::chrono_literals;
   fc::temp_directory temp;
   appbase::scoped_app app;
   auto temp_dir_str = temp.path().string();
   // a node that is not a producer has no default genesis it can start from, so give it the tester's
   const std::string genesis_file = (temp.path() / "genesis.json").string();
   fc::json::save_to_file(sysio::testing::base_tester::default_genesis(), genesis_file);

   // declared before on_exit so they outlive the task that holds the main thread
   std::promise<void>       release_main;
   std::shared_future<void> released = release_main.get_future().share();
   std::promise<std::tuple<platform_timer*, block_num_type>> holding_promise;
   auto holding_fut = holding_promise.get_future();

   std::promise<std::tuple<producer_plugin*, chain_plugin*>> plugin_promise;
   std::future<std::tuple<producer_plugin*, chain_plugin*>>  plugin_fut = plugin_promise.get_future();
   std::thread app_thread( [&]() {
      try {
         std::vector<const char*> argv = {"test", "--data-dir", temp_dir_str.c_str(), "--config-dir",
                                          temp_dir_str.c_str(), "--genesis-json", genesis_file.c_str(), read_mode};
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
   auto on_exit = fc::make_scoped_exit([&]() {
      try { release_main.set_value(); } catch (const std::future_error&) {} // never leave the main thread held
      app->quit();
      if (app_thread.joinable())
         app_thread.join();
   });

   BOOST_REQUIRE(plugin_fut.wait_for(30s) == std::future_status::ready);
   auto [prod_plug, chain_plug] = plugin_fut.get();

   app->executor().post( priority::high, exec_queue::read_write, [&holding_promise, released, cp = chain_plug]() {
      controller& chain = cp->chain();
      // on the main thread this is the timer received_block() interrupts
      platform_timer& timer = chain.get_thread_local_timer();
      timer.start(fc::time_point::maximum());
      holding_promise.set_value({&timer, chain.head().block_num()});
      released.wait();
      timer.stop();
   } );
   BOOST_REQUIRE(holding_fut.wait_for(30s) == std::future_status::ready);
   auto [timer, head_block_num] = holding_fut.get();

   BOOST_REQUIRE(timer->timer_state() == platform_timer::state_t::running);
   prod_plug->received_block(head_block_num + 2, fork_db_add_result);
   return timer->timer_state();
}

} // namespace

BOOST_AUTO_TEST_SUITE(received_block_tests)

// Head mode applies a received block right away, so the block interrupts the running transaction. This also shows the
// harness watches the timer received_block() interrupts.
BOOST_AUTO_TEST_CASE(head_mode_interrupts) {
   BOOST_CHECK(timer_state_after_received_block("--read-mode=head", fork_db_add_t::appended_to_head) ==
               platform_timer::state_t::interrupted);
   BOOST_CHECK(timer_state_after_received_block("--read-mode=head", fork_db_add_t::fork_switch) ==
               platform_timer::state_t::interrupted);
}

// Irreversible mode applies a received block only once it becomes irreversible, so nothing is interrupted for it.
BOOST_AUTO_TEST_CASE(irreversible_mode_does_not_interrupt) {
   BOOST_CHECK(timer_state_after_received_block("--read-mode=irreversible", fork_db_add_t::appended_to_head) ==
               platform_timer::state_t::running);
   BOOST_CHECK(timer_state_after_received_block("--read-mode=irreversible", fork_db_add_t::fork_switch) ==
               platform_timer::state_t::running);
}

BOOST_AUTO_TEST_SUITE_END()

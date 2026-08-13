#include <boost/test/unit_test.hpp>

#include <sysio/producer_plugin/producer_plugin.hpp>

#include <sysio/chain/application.hpp>
#include <sysio/chain/controller.hpp>
#include <sysio/chain/genesis_state.hpp>

#include <fc/mock_time.hpp>
#include <fc/mockable_deadline_timer.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

namespace {

using namespace sysio;
using namespace sysio::chain;

/// How long to let real time pass while asserting that nothing happens without the virtual clock
/// moving. Short, because a false negative here only weakens the test, it cannot make it flaky.
constexpr std::chrono::milliseconds real_time_patience{200};

/// How long to wait, in real time, for the plugin to react to virtual time moving. Generous, since
/// this bounds a failure rather than the happy path: the reaction is normally within a few ms.
constexpr std::chrono::seconds reaction_timeout{10};

/// Engages fc's mock clock for the life of a test and releases it afterwards, so that a test using
/// virtual time does not leave the clock frozen for whatever runs next in the same binary.
struct scoped_mock_clock {
   explicit scoped_mock_clock(const fc::time_point& start) { fc::mock_time_traits::set_now(start); }
   ~scoped_mock_clock() { fc::mock_time_traits::unset(); }
   scoped_mock_clock(const scoped_mock_clock&) = delete;
   scoped_mock_clock& operator=(const scoped_mock_clock&) = delete;
};

/// Spin until pred() or reaction_timeout elapses. Returns whether pred() held.
template<typename Pred>
bool wait_for(Pred pred) {
   const auto deadline = std::chrono::steady_clock::now() + reaction_timeout;
   while (!pred()) {
      if (std::chrono::steady_clock::now() > deadline)
         return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
   }
   return true;
}

} // namespace

BOOST_AUTO_TEST_SUITE(mockable_production_timer)

/**
 * Block production is driven entirely by the produce timer, and nothing today covers the path from
 * arming that timer to a block actually being produced: block_timing_util tests only decide which
 * slot to wake for, and the integration tests race a real clock. A slot lost between the two, for
 * instance by the correlation id race schedule_delayed_production_loop documents, produces no log
 * line and no failure, it simply produces no block.
 *
 * With fc's mock clock engaged the production timer becomes virtual, so a whole production round can
 * be stepped slot by slot with no sleeping and no tolerance for a loaded machine: either the block
 * for a slot appears when the clock reaches it, or it does not.
 */
BOOST_AUTO_TEST_CASE(production_follows_the_virtual_clock) {
   fc::temp_directory temp;
   appbase::scoped_app app;
   const auto temp_dir_str = temp.path().string();

   // Engage the mock clock before the plugin starts, so the chain and the production timer share one
   // frame of reference from genesis onward. This has to be the timestamp genesis_state's
   // parameterized constructor stamps, which is the one chain_plugin builds a fresh chain with;
   // genesis_state's default constructor leaves initial_timestamp at epoch, so reading it from a
   // default constructed genesis_state would silently put the clock 55 years behind the chain. The
   // check after startup below turns any future divergence into a failure rather than a hang.
   const fc::time_point genesis = fc::time_point::from_iso_string("2025-01-01T12:00:00");
   const scoped_mock_clock mock_clock{genesis};
   BOOST_REQUIRE(fc::mock_time_traits::is_set());
   BOOST_REQUIRE_EQUAL(fc::time_point::now().time_since_epoch().count(), genesis.time_since_epoch().count());
   BOOST_REQUIRE_GT(genesis.time_since_epoch().count(), 0);

   std::promise<std::tuple<producer_plugin*, chain_plugin*>> plugin_promise;
   auto plugin_fut = plugin_promise.get_future();
   std::thread app_thread([&]() {
      try {
         std::vector<const char*> argv = {"test",
                                          "--data-dir", temp_dir_str.c_str(),
                                          "--config-dir", temp_dir_str.c_str(),
                                          "-p", "sysio", "-e"};
         app->initialize<signature_provider_manager_plugin, chain_plugin, producer_plugin>(argv.size(),
                                                                                          (char**)&argv[0]);
         app->startup();
         app->executor().set_main_thread_id();
         plugin_promise.set_value({app->find_plugin<producer_plugin>(), app->find_plugin<chain_plugin>()});
         app->exec();
         return;
      }
      FC_LOG_AND_DROP()
      BOOST_CHECK(!"app threw exception see logged error");
   });

   auto [prod_plug, chain_plug] = plugin_fut.get();

   // The clock and the chain must agree, or the producer simply has nothing to do and this test
   // would hang rather than report anything useful.
   const auto head_time = chain_plug->chain().head().block_time();
   BOOST_REQUIRE_MESSAGE(head_time >= genesis && head_time <= genesis + fc::seconds(5),
                         "mock clock and chain genesis disagree: chain head is at "
                            << head_time.time_since_epoch().count() << "us, clock is at "
                            << genesis.time_since_epoch().count() << "us");

   std::atomic<uint32_t> blocks_produced{0};
   auto ab = chain_plug->chain().accepted_block().connect([&](const chain::block_signal_params&) { ++blocks_produced; });

   // The node produces the block for the slot the clock already sits on, so let production settle
   // before sampling, otherwise the first step races that startup block.
   wait_for([&]() { return blocks_produced.load() > 0; });
   const uint32_t settled = blocks_produced.load();

   // 1. Real time alone must move nothing. This is the property that makes the test deterministic:
   //    a loaded machine cannot advance the chain behind the test's back.
   std::this_thread::sleep_for(real_time_patience);
   BOOST_CHECK_EQUAL(blocks_produced.load(), settled);

   // 2. Every slot the clock advances through must yield exactly one block. Stepping one slot at a
   //    time is what makes a silently skipped slot a failure rather than a timing artifact.
   constexpr uint32_t slots_to_step = 12;
   const auto          slot_time    = fc::milliseconds(config::block_interval_ms);
   fc::time_point      now          = genesis;
   for (uint32_t slot = 1; slot <= slots_to_step; ++slot) {
      const uint32_t before = blocks_produced.load();
      now += slot_time;
      fc::mock_time_traits::set_now(now);
      BOOST_REQUIRE_MESSAGE(wait_for([&]() { return blocks_produced.load() > before; }),
                            "no block produced for step " << slot << " of " << slots_to_step
                                                          << " after advancing the clock by one slot");
   }

   BOOST_CHECK_GE(blocks_produced.load(), settled + slots_to_step);

   // 3. Stopping the clock must stop production, confirming that step 2 measured the clock driving
   //    production rather than production simply running free.
   const uint32_t held = blocks_produced.load();
   std::this_thread::sleep_for(real_time_patience);
   BOOST_CHECK_EQUAL(blocks_produced.load(), held);

   app->quit();
   app_thread.join();
}

BOOST_AUTO_TEST_SUITE_END()

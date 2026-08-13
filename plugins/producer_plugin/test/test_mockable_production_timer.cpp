#include <boost/test/unit_test.hpp>

#include <sysio/producer_plugin/producer_plugin.hpp>

#include <sysio/chain/application.hpp>
#include <sysio/chain/controller.hpp>
#include <sysio/chain/exec_pri_queue.hpp>
#include <sysio/chain/genesis_state.hpp>

#include <fc/mock_time.hpp>
#include <fc/scoped_exit.hpp>
#include <fc/mockable_timer.hpp>

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

/// The timestamp genesis_state's parameterized constructor stamps, which is what chain_plugin builds
/// a fresh chain with. genesis_state's default constructor leaves initial_timestamp at epoch, so
/// this cannot be read from a default constructed genesis_state without silently putting the mock
/// clock 55 years behind the chain. running_node checks the two against each other at startup.
constexpr const char* genesis_timestamp = "2025-01-01T12:00:00";

/// Spin until pred() holds or reaction_timeout elapses. Returns whether pred() held.
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

/// A single producing node running under the mock clock, with the clock engaged before startup so
/// the chain and the production timer share one frame of reference from genesis onward.
class running_node {
public:
   explicit running_node(const std::vector<const char*>& extra_args = {})
      : _genesis(fc::time_point::from_iso_string(genesis_timestamp))
      , _temp_dir_str(_temp.path().string())
      , _mock_clock(_genesis) {
      BOOST_REQUIRE(fc::mock_time_traits::is_set());
      BOOST_REQUIRE_EQUAL(fc::time_point::now().time_since_epoch().count(), _genesis.time_since_epoch().count());
      BOOST_REQUIRE_GT(_genesis.time_since_epoch().count(), 0);

      std::promise<std::tuple<producer_plugin*, chain_plugin*>> plugin_promise;
      auto                                                      plugin_fut = plugin_promise.get_future();
      _app_thread                                                          = std::thread([&]() {
         try {
            std::vector<const char*> argv = {"test",
                                             "--data-dir", _temp_dir_str.c_str(),
                                             "--config-dir", _temp_dir_str.c_str(),
                                             "-p", "sysio", "-e"};
            argv.insert(argv.end(), extra_args.begin(), extra_args.end());
            _app->initialize<signature_provider_manager_plugin, chain_plugin, producer_plugin>(argv.size(),
                                                                                              (char**)&argv[0]);
            _app->startup();
            _app->executor().set_main_thread_id();
            plugin_promise.set_value({_app->find_plugin<producer_plugin>(), _app->find_plugin<chain_plugin>()});
            _app->exec();
            return;
         }
         FC_LOG_AND_DROP()
         BOOST_CHECK(!"app threw exception see logged error");
      });

      std::tie(_prod_plug, _chain_plug) = plugin_fut.get();

      // The clock and the chain must agree, or the producer has nothing to do and every test here
      // would hang rather than report anything useful.
      const auto head_time = _chain_plug->chain().head().block_time();
      BOOST_REQUIRE_MESSAGE(head_time >= _genesis && head_time <= _genesis + fc::seconds(5),
                            "mock clock and chain genesis disagree: chain head is at "
                               << head_time.time_since_epoch().count() << "us, clock is at "
                               << _genesis.time_since_epoch().count() << "us");

      _accepted_block = _chain_plug->chain().accepted_block().connect(
         [this](const chain::block_signal_params&) { ++_blocks_produced; });

      // The node produces the block for the slot the clock already sits on, so let production settle
      // before a test samples, otherwise its first step races that startup block.
      wait_for([this]() { return _blocks_produced.load() > 0; });
      _now = _genesis;
   }

   ~running_node() {
      _app->quit();
      if (_app_thread.joinable())
         _app_thread.join();
   }

   running_node(const running_node&)            = delete;
   running_node& operator=(const running_node&) = delete;

   uint32_t         blocks_produced() const { return _blocks_produced.load(); }
   producer_plugin* producer() { return _prod_plug; }

   /// Whether the node is in its write window. The read only window is the complement, so this is
   /// what says which side of the _ro_timer cycle the node is on.
   bool in_write_window() const { return _chain_plug->chain().is_write_window(); }

   /// Run fn on the node's main thread, where anything touching chain state has to run.
   template<typename F>
   void post_to_main_thread(F&& fn) {
      _app->post(appbase::priority::high, std::forward<F>(fn));
   }

   /// Enqueue read-only work. switch_to_read_window stays in the write window when this queue is
   /// empty, so a test that wants the read window to open has to give it something to do.
   template<typename F>
   void post_read_only(F&& fn) {
      _app->executor().post(appbase::priority::low, appbase::exec_queue::read_only, std::forward<F>(fn));
   }

   /// Advance the virtual clock by an arbitrary amount, for waits that are not a whole slot.
   void advance(fc::microseconds by) {
      _now += by;
      fc::mock_time_traits::set_now(_now);
   }

   /// Advance the virtual clock by one production slot and wait for the block it owes. Returns
   /// whether that block arrived.
   bool step_one_slot() {
      const uint32_t before = _blocks_produced.load();
      _now += fc::milliseconds(config::block_interval_ms);
      fc::mock_time_traits::set_now(_now);
      return wait_for([&]() { return _blocks_produced.load() > before; });
   }

private:
   fc::temp_directory                 _temp;
   const fc::time_point               _genesis;
   const std::string                  _temp_dir_str;
   const fc::scoped_mock_clock        _mock_clock;
   appbase::scoped_app                _app;
   std::thread                        _app_thread;
   producer_plugin*                   _prod_plug  = nullptr;
   chain_plugin*                      _chain_plug = nullptr;
   std::atomic<uint32_t>              _blocks_produced{0};
   fc::time_point                     _now;
   boost::signals2::scoped_connection _accepted_block;
};

constexpr uint32_t slots_to_step = 12;

} // namespace

BOOST_AUTO_TEST_SUITE(mockable_production_timer)

/**
 * Block production is driven entirely by the produce timer, and nothing else covers the path from
 * arming that timer to a block actually being produced: block_timing_util's tests decide which slot
 * to wake for and stop there, and the integration tests race a real clock. A slot lost between the
 * two emits no log line and no error, it simply produces no block.
 *
 * With fc's mock clock engaged the production timer becomes virtual, so a production round can be
 * stepped slot by slot with no sleeping and no tolerance for a loaded machine: either the block for
 * a slot appears when the clock reaches it, or it does not.
 */
BOOST_AUTO_TEST_CASE(production_follows_the_virtual_clock) {
   running_node   node;
   const uint32_t settled = node.blocks_produced();

   // 1. Real time alone must move nothing. This is the property that makes the test deterministic:
   //    a loaded machine cannot advance the chain behind the test's back.
   std::this_thread::sleep_for(real_time_patience);
   BOOST_CHECK_EQUAL(node.blocks_produced(), settled);

   // 2. Every slot the clock advances through must yield a block. Stepping one slot at a time is
   //    what makes a silently skipped slot a failure rather than a timing artifact.
   for (uint32_t slot = 1; slot <= slots_to_step; ++slot) {
      BOOST_REQUIRE_MESSAGE(node.step_one_slot(),
                            "no block produced for step " << slot << " of " << slots_to_step
                                                          << " after advancing the clock by one slot");
   }
   BOOST_CHECK_GE(node.blocks_produced(), settled + slots_to_step);

   // 3. Stopping the clock must stop production, confirming that step 2 measured the clock driving
   //    production rather than production simply running free.
   const uint32_t held = node.blocks_produced();
   std::this_thread::sleep_for(real_time_patience);
   BOOST_CHECK_EQUAL(node.blocks_produced(), held);
}

/**
 * schedule_delayed_production_loop rechecks its correlation id inside the posted lambda because a
 * competing schedule_* call can land between the timer firing and that lambda running, and running
 * the production loop unconditionally would bump the id again and, in its own words, "starve the
 * just-scheduled produce_block timer". A starved timer produces no block and reports nothing, so
 * that reasoning is currently held in place by the comment alone.
 *
 * get_integrity_hash is the disturbance, and it is reachable through the plugin's public interface:
 * while a block is being built it aborts the pending block and reschedules the production loop on
 * scope exit, which is exactly a competing schedule_* call arriving with a produce timer armed. The
 * invariant a starved timer breaks is that every slot still yields a block.
 */
BOOST_AUTO_TEST_CASE(production_survives_a_competing_reschedule) {
   running_node   node;
   const uint32_t settled = node.blocks_produced();

   for (uint32_t slot = 1; slot <= slots_to_step; ++slot) {
      // Land the abort and its reschedule while the produce timer for the block in flight is armed.
      std::promise<void> disturbed;
      auto               disturbed_fut = disturbed.get_future();
      node.post_to_main_thread([&]() {
         try {
            node.producer()->get_integrity_hash();
         }
         FC_LOG_AND_DROP()
         disturbed.set_value();
      });
      BOOST_REQUIRE_MESSAGE(disturbed_fut.wait_for(reaction_timeout) == std::future_status::ready,
                            "the node stopped servicing its main thread at step " << slot);

      BOOST_REQUIRE_MESSAGE(node.step_one_slot(),
                            "no block produced for step " << slot << " of " << slots_to_step
                                                          << ": the produce timer was starved by the reschedule "
                                                             "that get_integrity_hash triggers");
   }

   BOOST_CHECK_GE(node.blocks_produced(), settled + slots_to_step);
}

/**
 * The read only window is bounded by wall clock in exactly the way block production is: _ro_timer
 * arms for the write window, its handler switches to the read window, and that window closes on its
 * own deadline. Nothing covered that cycle, because observing it against a real clock means racing
 * it, and _ro_timer carries its own correlation id guard against a handler from a previous cycle
 * landing in the current one.
 *
 * With the clock owned by the test the cycle is observable directly: read only work cannot run
 * before the write window expires, and it must run once it has. switch_to_read_window stays in the
 * write window when the read only queue is empty, so the queue is primed first.
 */
BOOST_AUTO_TEST_CASE(read_window_opens_only_when_the_write_window_expires) {
   // Read only threads are rejected on a producing node outside test mode.
   producer_plugin::set_test_mode(true);
   auto restore_mode = fc::make_scoped_exit([]() { producer_plugin::set_test_mode(false); });

   constexpr uint32_t write_window_us = 200'000;
   running_node       node{{"--read-only-threads", "2",
                            "--read-only-write-window-time-us", "200000",
                            "--read-only-read-window-time-us", "60000"}};

   BOOST_REQUIRE_MESSAGE(node.in_write_window(), "a node should start in its write window");

   // switch_to_read_window stays put when the read only queue is empty, so give it work to find.
   // Read only tasks can also be drained on the main thread, so their running proves nothing about
   // the window; the window state itself is what this checks.
   // Real time alone must not close the write window, the property that makes this deterministic.
   std::this_thread::sleep_for(real_time_patience);
   BOOST_CHECK(node.in_write_window());

   // Prime the queue immediately before crossing the deadline: the main thread also drains read only
   // work during the write window, so a backlog has to still be there when the timer fires.
   std::atomic<uint32_t> read_only_ran{0};
   for (uint32_t i = 0; i < 2000; ++i)
      node.post_read_only([&]() { ++read_only_ran; });

   // Crossing the write window deadline must hand the read only threads their window.
   node.advance(fc::microseconds(write_window_us + 1000));
   BOOST_REQUIRE_MESSAGE(wait_for([&]() { return !node.in_write_window(); }),
                         "the write window did not close after its deadline passed in virtual time");
   BOOST_CHECK_MESSAGE(wait_for([&]() { return read_only_ran.load() > 0; }),
                       "the read window opened but its queued work did not run");

   // The read window must close on its own deadline, leaving the node producing again.
   BOOST_REQUIRE_MESSAGE(wait_for([&]() { return node.in_write_window(); }),
                         "the read window did not close and hand the node back to production");
   BOOST_REQUIRE_MESSAGE(node.step_one_slot(), "production did not resume after the read window closed");
}

BOOST_AUTO_TEST_SUITE_END()

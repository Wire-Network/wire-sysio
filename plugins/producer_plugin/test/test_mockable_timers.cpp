#include <boost/test/unit_test.hpp>

#include <sysio/producer_plugin/block_timing_util.hpp>
#include <sysio/producer_plugin/producer_plugin.hpp>

#include <sysio/chain/application.hpp>
#include <sysio/chain/controller.hpp>
#include <sysio/chain/genesis_state.hpp>

#include <sysio/testing/tester.hpp>

#include <fc/mock_time.hpp>
#include <fc/scoped_exit.hpp>
#include <fc/system_timer.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
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

/// How long to leave the timer thread alone with a moved clock. Under mock time the timer polls the
/// virtual clock rather than sleeping out its delay, so this is many poll intervals; it is used
/// where a test needs the timer to have fired and nothing on the test side can observe that it has.
constexpr std::chrono::milliseconds timer_thread_patience{50};

/// The timestamp genesis_state's parameterized constructor stamps, which is what chain_plugin builds
/// a fresh chain with. genesis_state's default constructor leaves initial_timestamp at epoch, so
/// this cannot be read from a default constructed genesis_state without silently putting the mock
/// clock 55 years behind the chain. running_node checks the two against each other at startup.
constexpr const char* genesis_timestamp = "2025-01-01T12:00:00";

/// Pinned rather than left to the default so the deadlines below are computable. cpu_effort mirrors
/// the plugin's own derivation from it.
constexpr uint32_t produce_block_offset_ms = 450;

/// The one producer running_node holds a key for, so the one whose windows it can commit in.
constexpr chain::account_name own_producer = config::system_account_name;

/// How long to give the node to do something with a slot the clock has just moved into, before
/// concluding it did nothing with it. Bounds a per-slot poll, so it is short.
constexpr std::chrono::milliseconds slot_settle_budget{200};

/// Spin until pred() holds or reaction_timeout elapses. Returns whether pred() held.
/// Spin until pred() holds or the given budget elapses. Returns whether pred() held.
template<typename Pred>
bool wait_up_to(std::chrono::milliseconds budget, Pred pred) {
   const auto deadline = std::chrono::steady_clock::now() + budget;
   while (!pred()) {
      if (std::chrono::steady_clock::now() > deadline)
         return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
   }
   return true;
}

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
   /// Starts a node on its own fresh chain, or on a data directory seeded beforehand. A seeded
   /// chain's head is wherever seeding left it, so the clock has to start there rather than at
   /// genesis, otherwise the node has nothing to do and every wait below simply times out.
   explicit running_node(const std::vector<const char*>& extra_args = {},
                         const std::string&              seededDataDir = {},
                         fc::time_point                  clockStart    = fc::time_point{})
      : _dataDir(seededDataDir.empty() ? _temp.path().string() : seededDataDir)
      , _clockStart(clockStart == fc::time_point{} ? fc::time_point::from_iso_string(genesis_timestamp) : clockStart)
      , _mock_clock(_clockStart) {
      BOOST_REQUIRE(fc::mock_time_traits::is_set());
      BOOST_REQUIRE_EQUAL(fc::time_point::now().time_since_epoch().count(), _clockStart.time_since_epoch().count());
      BOOST_REQUIRE_GT(_clockStart.time_since_epoch().count(), 0);

      // The promise is held by shared_ptr and captured by value because the app thread outlives this
      // constructor: a promise on the constructor's stack is destroyed the moment the startup value
      // releases it, and anything the thread reported through it afterwards would be writing to
      // freed storage.
      auto startup     = std::make_shared<std::promise<std::tuple<producer_plugin*, chain_plugin*>>>();
      auto startup_fut = startup->get_future();
      _app_thread      = std::thread([this, startup, args = extra_args]() {
         bool started = false;
         try {
            std::vector<const char*> argv = {"test",
                                             "--data-dir", _dataDir.c_str(),
                                             "--config-dir", _dataDir.c_str(),
                                             "-p", "sysio", "-e",
                                             "--produce-block-offset-ms", "450"};
            argv.insert(argv.end(), args.begin(), args.end());
            _app->initialize<signature_provider_manager_plugin, chain_plugin, producer_plugin>(argv.size(),
                                                                                              (char**)&argv[0]);
            _app->startup();
            _app->executor().set_main_thread_id();
            startup->set_value({_app->find_plugin<producer_plugin>(), _app->find_plugin<chain_plugin>()});
            started = true;
            _app->exec();
            return;
         }
         FC_LOG_AND_DROP()
         // Boost.Test is not safe to call from this thread, and doing so while the main thread is
         // unwinding a failed check turns a legible failure into an abort, so record it instead.
         _app_threw = true;
         // Only a failure during startup answers the promise, and leaving it unset there would
         // strand the constructor forever. Past that point nobody is waiting on it any more, and the
         // failure is reported through _app_threw alone.
         if (!started)
            startup->set_value({nullptr, nullptr});
      });

      // The thread is joinable from here on, and a constructor that fails never runs the destructor:
      // std::thread's own destructor would terminate the process, replacing whichever assertion
      // failed with a bare abort. Everything below therefore unwinds through this.
      auto stopNode = fc::make_scoped_exit([this]() {
         _app->quit();
         if (_app_thread.joinable())
            _app_thread.join();
      });

      std::tie(_prod_plug, _chain_plug) = startup_fut.get();
      if (_prod_plug == nullptr || _chain_plug == nullptr)
         BOOST_FAIL("the node failed to start, see the logged error");

      // Connect and seed together on the app thread. Doing both there is what makes the snapshot
      // whole: a block accepted between reading the head and connecting the signal would go
      // unrecorded, and reading the head from this thread is the race the snapshot exists to remove.
      // The promise is owned by both sides for the same reason the startup one is: if the wait below
      // gives up, this lambda may still be queued behind whatever is holding the app thread.
      auto observing     = std::make_shared<std::promise<void>>();
      auto observing_fut = observing->get_future();
      post_to_main_thread([this, observing]() {
         chain::controller& chain = _chain_plug->chain();
         _accepted_block          = chain.accepted_block().connect([this](const chain::block_signal_params& params) {
            const auto& [block, id] = params;
            record_head(block->block_num(), block->producer, block->timestamp);
            ++_blocks_produced;
         });
         const auto head = chain.head();
         record_head(head.block_num(), head.producer(), chain::block_timestamp_type(head.block_time()));
         observing->set_value();
      });
      BOOST_REQUIRE_MESSAGE(observing_fut.wait_for(reaction_timeout) == std::future_status::ready,
                            "the node never serviced its main thread after startup");

      // The clock and the chain must agree, or the producer has nothing to do and every test here
      // would hang rather than report anything useful.
      const auto head_time = head_block_timestamp().to_time_point();
      BOOST_REQUIRE_MESSAGE(head_time >= _clockStart - fc::seconds(1) && head_time <= _clockStart + fc::seconds(5),
                            "mock clock and chain head disagree: head is at "
                               << head_time.time_since_epoch().count() << "us, clock is at "
                               << _clockStart.time_since_epoch().count() << "us");

      // Nothing is owed yet, so there is nothing to settle before a test samples: the next block's
      // deadline falls inside the slot after the one the clock sits on, and virtual time does not
      // move until a test moves it.
      _now = _clockStart;
      stopNode.cancel();   // fully constructed, so the destructor owns the thread from here
   }

   ~running_node() {
      _app->quit();
      if (_app_thread.joinable())
         _app_thread.join();
   }

   running_node(const running_node&)            = delete;
   running_node& operator=(const running_node&) = delete;

   uint32_t         blocks_produced() const { return _blocks_produced.load(); }
   bool             app_threw() const { return _app_threw.load(); }
   producer_plugin* producer() { return _prod_plug; }

   uint32_t head_block_num() const { return head().block_num; }

   /// Timestamp of the head block, the anchor for working out when the next one is owed.
   chain::block_timestamp_type head_block_timestamp() const { return head().timestamp; }

   /// Whether the node is building a block for a slot it does not own, which is what speculating is.
   ///
   /// Asked of the pending block rather than the head, because the head cannot answer it: the other
   /// producers in the schedule have no key here, so nothing they own is ever committed and the head
   /// only ever shows this node's own work or whatever history the chain was seeded with.
   bool speculating() {
      const auto building = pending();
      return building.building && building.producer != own_producer;
   }

   /// What the node is building right now, as a value that changes once it moves on to a new slot.
   /// A test waiting for the node to react to the clock has to wait for this to differ from what it
   /// was: waiting on a condition that already held before the clock moved returns at once and
   /// leaves the sampling racing the timer thread.
   struct pending_block {
      bool                building  = false;
      chain::account_name producer;
      uint32_t            block_num = 0;

      bool operator==(const pending_block&) const = default;
   };

   pending_block pending() {
      return on_main_thread([this]() {
         chain::controller& chain = _chain_plug->chain();
         if (!chain.is_building_block())
            return pending_block{};
         return pending_block{true, chain.pending_block_producer(), chain.pending_block_num()};
      });
   }

   /// Where the virtual clock currently stands.
   fc::time_point now() const { return _now; }

   /// Move the virtual clock to an absolute point. Only valid going forwards.
   void advance_to(const fc::time_point& t) {
      BOOST_REQUIRE_GE(t.time_since_epoch().count(), _now.time_since_epoch().count());
      _now = t;
      fc::mock_time_traits::set_now(_now);
   }

   /// Run fn on the node's main thread, where anything touching chain state has to run.
   ///
   /// Posted with the priority and queue the plugin's own timer lambdas use, because the executor
   /// runs handlers of equal priority in the order they were posted and one case below depends on
   /// where its work lands relative to those.
   template<typename F>
   void post_to_main_thread(F&& fn) {
      _app->executor().post(appbase::priority::high, exec_queue::read_write, std::forward<F>(fn));
   }

   /// Run fn on the main thread and wait for it, handing back whatever it returned.
   ///
   /// Anything reading chain state has to go through here. controller::head() hands back a
   /// block_handle by value, so copying it while the app thread is replacing chain_head races on the
   /// shared pointer inside, and controller has no lock a test could take instead.
   ///
   /// The callable and the promise are owned by the posted lambda rather than borrowed from this
   /// frame. Giving up on the wait below does not withdraw the lambda: it stays queued behind
   /// whatever is holding the app thread and runs later, by which time anything left on this stack
   /// is gone.
   template<typename F>
   auto on_main_thread(F&& fn) -> decltype(fn()) {
      using result_t = decltype(fn());
      auto result    = std::make_shared<std::promise<result_t>>();
      auto fut       = result->get_future();
      post_to_main_thread([result, call = std::decay_t<F>(std::forward<F>(fn))]() mutable {
         result->set_value(call());
      });
      BOOST_REQUIRE_MESSAGE(fut.wait_for(reaction_timeout) == std::future_status::ready,
                            "the node stopped servicing its main thread");
      return fut.get();
   }

   /// Advance the virtual clock by an arbitrary amount, for waits that are not a whole slot.
   void advance(fc::microseconds by) {
      _now += by;
      fc::mock_time_traits::set_now(_now);
   }

   /// Step the clock a slot at a time until pred() holds, giving the node a chance to react to each
   /// slot before moving on. Returns whether it held within the bound.
   template<typename Pred>
   bool advance_slots_until(uint32_t max_slots, Pred pred) {
      for (uint32_t slot = 0; slot < max_slots; ++slot) {
         if (pred())
            return true;
         advance(fc::milliseconds(config::block_interval_ms));
         wait_up_to(slot_settle_budget, pred);
      }
      return pred();
   }

   /// The cpu effort the node was configured with, which is what spaces block deadlines.
   static fc::microseconds cpu_effort() {
      return fc::microseconds(config::block_interval_us -
                              (produce_block_offset_ms * 1000 / config::producer_repetitions));
   }

   /// When the block after the current head is due to ship, for a test that wants a single block.
   fc::time_point next_block_deadline() const {
      const auto next = chain::block_timestamp_type(head_block_timestamp().slot + 1);
      return block_timing_util::calculate_producing_block_deadline(cpu_effort(), next);
   }

   /// Advance the clock a slot at a time until the head moves, and report whether it did.
   ///
   /// Counting blocks against slots elapsed is not a safe invariant here. A block ships at its cpu
   /// effort deadline rather than at its slot, and those deadlines sit closer together than slots
   /// do, so the node runs progressively further ahead across a round and then waits out a gap of
   /// nearly two slots at the round boundary. How far ahead it is at any moment follows from where
   /// in that round it started, which is startup timing rather than anything under test. What does
   /// hold, and what a starved timer breaks, is that production keeps moving while the clock does.
   bool advance_until_head_moves() {
      const uint32_t before = head_block_num();
      // Three slots covers the round boundary gap, the longest legitimate pause in shipping.
      for (uint32_t slot = 0; slot < 3; ++slot) {
         _now += fc::milliseconds(config::block_interval_ms);
         fc::mock_time_traits::set_now(_now);
         if (wait_up_to(std::chrono::seconds(2), [&]() { return head_block_num() > before; }))
            return true;
      }
      // One last generous wait, so a merely loaded machine is not mistaken for a stalled one.
      return wait_for([&]() { return head_block_num() > before; });
   }

private:
   /// What the app thread last accepted. Kept here rather than read from the controller on demand so
   /// that no test thread ever touches chain state.
   struct head_state {
      uint32_t                    block_num = 0;
      chain::account_name         producer;
      chain::block_timestamp_type timestamp;
   };

   /// Called on the app thread only, from the accepted_block slot and from the seeding post.
   void record_head(uint32_t block_num, chain::account_name producer, chain::block_timestamp_type timestamp) {
      std::lock_guard g(_head_mtx);
      _head = head_state{block_num, producer, timestamp};
   }

   head_state head() const {
      std::lock_guard g(_head_mtx);
      return _head;
   }

   fc::temp_directory                 _temp;
   const std::string                  _dataDir;
   const fc::time_point               _clockStart;
   const fc::scoped_mock_clock        _mock_clock;
   appbase::scoped_app                _app;
   std::thread                        _app_thread;
   producer_plugin*                   _prod_plug  = nullptr;
   chain_plugin*                      _chain_plug = nullptr;
   std::atomic<uint32_t>              _blocks_produced{0};
   std::atomic<bool>                  _app_threw{false};
   fc::time_point                     _now;
   mutable std::mutex                 _head_mtx;
   head_state                         _head;
   boost::signals2::scoped_connection _accepted_block;
};

constexpr uint32_t slots_to_step = 12;   // one full production round

/// Build a chain in `dir` whose active schedule holds more producers than the node under test will
/// own, so that node has to speculate through the other windows rather than producing every slot.
/// Returns the head block time, which is where the mock clock has to stand when the node opens it.
///
/// The schedule is installed with a tester rather than by pushing transactions into the running
/// plugin, because setting producers goes through sysio.bios and needs protocol features and a
/// finalizer policy in place first; the tester already knows how to do all of that. The tester is
/// destroyed before the node starts, so only one controller ever has the directory open.
fc::time_point seed_chain_with_other_producers(const fc::temp_directory&              dir,
                                               const std::vector<chain::account_name>& others) {
   using namespace sysio::testing;

   tester t(dir, true);
   t.produce_block();
   t.set_bios_contract();
   t.preactivate_all_builtin_protocol_features();
   t.produce_block();
   t.init_roa();
   finalizer_keys fin_keys(t, 1u, 1u);
   fin_keys.activate_savanna(0u);

   t.create_accounts(others);
   std::vector<chain::account_name> schedule{config::system_account_name};
   schedule.insert(schedule.end(), others.begin(), others.end());
   t.set_producers(schedule);

   // A proposer policy takes effect a round after it is proposed, so run out two rounds of the new
   // schedule to be certain the node opens a chain that is already using it.
   t.produce_blocks(2 * schedule.size() * config::producer_repetitions);

   BOOST_REQUIRE_EQUAL(t.control->head_active_producers().producers.size(), schedule.size());
   return t.control->head().block_time();
}

/// A node whose schedule holds producers it has no key for, so it must speculate through their
/// windows rather than producing every slot. That is the only state in which the delayed production
/// loop is armed, so both of the cases that care about it start here.
///
/// The seeded directory and the signature provider string are held alongside the node because it
/// borrows both: the directory for as long as it runs, the string while it is starting up.
class speculating_node {
public:
   explicit speculating_node(const std::vector<chain::account_name>& others)
      : _others(others)
      , _head_time(seed_chain_with_other_producers(_seeded, _others))
      // <chain-kind>,<key-type>,<public-key>,<private-key-provider-spec>. Only sysio's key, so the
      // node produces its own window and speculates through the rest.
      , _provider("wire,wire,"
                  + sysio::testing::base_tester::get_public_key(config::system_account_name, "active").to_string({})
                  + ",KEY:"
                  + sysio::testing::base_tester::get_private_key(config::system_account_name, "active").to_string({}))
      , node({"--signature-provider", _provider.c_str(), "--production-pause-vote-timeout-ms", "0"},
             _seeded.path().string(), _head_time) {}

   /// Slots in one turn of the whole schedule, which bounds every walk over it.
   uint32_t rotation() const { return (_others.size() + 1) * config::producer_repetitions; }

private:
   const std::vector<chain::account_name> _others;
   fc::temp_directory                     _seeded;
   const fc::time_point                   _head_time;
   const std::string                      _provider;

public:
   /// Declared last so it is constructed last, after everything it borrows above exists.
   running_node node;
};

} // namespace

BOOST_AUTO_TEST_SUITE(mockable_timers)

/**
 * Block production is driven entirely by the produce timer, and nothing else covers the path from
 * arming that timer to a block actually being produced: block_timing_util's tests decide which slot
 * to wake for and stop there, and the integration tests race a real clock. A slot lost between the
 * two emits no log line and no error, it simply produces no block.
 *
 * With fc's mock clock engaged the production timer becomes virtual, so a round can be driven with
 * no sleeping and no tolerance for a loaded machine: the head advances only when the test moves the
 * clock, and it must keep advancing for as long as the test keeps moving it.
 *
 * What that establishes is BOUNDED LIVENESS, not a per-slot guarantee, and the difference is
 * deliberate rather than a shortcut. Blocks ship at their cpu-effort deadline -- 462.5ms at the
 * default offset -- rather than at the 500ms slot boundary, so a node runs progressively further
 * ahead across a round and then waits out a gap of roughly two slots. Requiring a block from every
 * slot would therefore fail on correct behaviour, at a step that moves with startup timing. This
 * case asserts instead that the head never stalls longer than that legitimate gap, twelve times
 * running, and that twelve advances yield at least twelve blocks.
 *
 * It follows that a regression losing a single deadline is NOT caught here: the head still moves
 * within the allowance, just later. Detecting that needs a slot budget over the whole round, and a
 * fixed budget is brittle for the same reason a per-slot assertion is -- how much drift has
 * accumulated depends on where in the round the node started. What this case does catch is a timer
 * that stops re-arming, which is the failure the seam under test can actually introduce.
 */
BOOST_AUTO_TEST_CASE(production_follows_the_virtual_clock) {
   running_node   node;
   const uint32_t settled = node.blocks_produced();

   // 1. Real time alone must move nothing. This is the property that makes the test deterministic:
   //    a loaded machine cannot advance the chain behind the test's back.
   std::this_thread::sleep_for(real_time_patience);
   BOOST_CHECK_EQUAL(node.blocks_produced(), settled);

   // 2. The head must keep advancing for as long as the clock does, and each advance must arrive
   //    inside the longest legitimate pause -- the round-boundary gap advance_until_head_moves
   //    allows for. A timer that stops re-arming fails here on the step where it stopped.
   for (uint32_t step = 1; step <= slots_to_step; ++step) {
      BOOST_REQUIRE_MESSAGE(node.advance_until_head_moves(),
                            "production stopped at step " << step << " of " << slots_to_step
                                                          << ": the head did not move across three slots of "
                                                             "virtual time");
   }

   // Each of those steps required the head to move at least once, and this node is the only
   // producer, so every advance is a block it produced.
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
 * just-scheduled produce_block timer". Nothing reached that interleaving, so the reasoning was held
 * in place by the comment alone.
 *
 * What this case establishes is that the interleaving happens and that production comes back from
 * it. It does not establish that the recheck is what brings production back: with the recheck
 * removed the node still recovers, in the same number of slots, because every route into
 * schedule_production_loop re-arms the timer anyway. The recheck is defence in depth, and this is
 * what would notice if the interleaving ever stopped being survivable.
 *
 * get_integrity_hash is the disturbance, and it is reachable through the plugin's public interface:
 * while a block is being built it aborts the pending block and reschedules the production loop on
 * scope exit, which is exactly a competing schedule_* call arriving with a timer already armed.
 *
 * Provoking it takes a barrier rather than timing. The timer runs on the plugin's own thread and only
 * posts to the app thread, so holding the app thread lets the timer fire and enqueue while the
 * disturbance sits in front of it in the queue; releasing then runs the disturbance, which bumps the
 * id, and only afterwards the lambda that was armed under the old one. The node has to be speculating
 * for any of it, since that is the only state in which the delayed loop is the armed timer.
 */
BOOST_AUTO_TEST_CASE(production_survives_a_competing_reschedule) {
   speculating_node seeded{{"defproducera"_n, "defproducerb"_n}};
   running_node&    node = seeded.node;

   // The wake up is only armed while the node is speculating, so get there first.
   BOOST_REQUIRE_MESSAGE(node.advance_slots_until(2 * seeded.rotation(), [&]() { return node.speculating(); }),
                         "the node never built a block for a producer it has no key for, so the delayed "
                         "production loop was never armed and there is no stale lambda to provoke");

   const uint32_t settled = node.blocks_produced();

   // The gate and the disturbance both outlive this frame if anything here gives up: they stay queued
   // behind whichever of them is holding the app thread, so they own their synchronization rather
   // than borrowing this stack. The guard is what stops a failed wait from unwinding past a gate that
   // is still holding the thread, which would leave the node unable to shut down.
   auto holding = std::make_shared<std::promise<void>>();
   auto release = std::make_shared<std::promise<void>>();
   auto holding_fut = holding->get_future();
   auto release_fut = std::make_shared<std::shared_future<void>>(release->get_future());
   auto released    = false;
   auto openTheGate = fc::make_scoped_exit([&]() {
      if (!released)
         release->set_value();
   });

   // Take the app thread and keep it. The timer runs on the plugin's own thread and only posts here,
   // so while this is held the timer can fire and enqueue with nothing on this side running.
   node.post_to_main_thread([holding, release_fut]() {
      holding->set_value();
      release_fut->wait();
   });
   BOOST_REQUIRE_MESSAGE(holding_fut.wait_for(reaction_timeout) == std::future_status::ready,
                         "the node stopped servicing its main thread");

   // Queue the disturbance behind the gate, before the clock moves. get_integrity_hash aborts the
   // pending block and reschedules the production loop on scope exit, which is a competing schedule_*
   // call bumping the correlation id. Queuing it now is what puts it ahead of whatever the timer
   // posts, since the executor runs handlers of equal priority in the order they were posted.
   auto disturbed     = std::make_shared<std::promise<void>>();
   auto disturbed_fut = disturbed->get_future();
   node.post_to_main_thread([producer = node.producer(), disturbed]() {
      try {
         producer->get_integrity_hash();
      }
      FC_LOG_AND_DROP()
      disturbed->set_value();
   });

   // Walk the clock past the armed wake up while the app thread is still held, so the timer fires
   // with the id it was armed with and lands its lambda behind the disturbance. Nothing here can
   // observe that post, and nothing on this side can be asked while the thread is held, so the walk
   // is blind and the pause is what gives the timer thread its chance to notice each step.
   for (uint32_t slot = 0; slot < seeded.rotation(); ++slot) {
      node.advance(fc::milliseconds(config::block_interval_ms));
      std::this_thread::sleep_for(timer_thread_patience);
   }

   // Release. The disturbance runs first and bumps the id, then the timer's lambda runs under an id
   // that is no longer current, which is the interleaving the recheck is written for.
   release->set_value();
   released = true;
   BOOST_REQUIRE_MESSAGE(disturbed_fut.wait_for(reaction_timeout) == std::future_status::ready,
                         "the node stopped servicing its main thread");

   BOOST_CHECK_MESSAGE(node.advance_slots_until(2 * seeded.rotation(),
                                               [&]() { return node.blocks_produced() > settled; }),
                       "the node never produced again after a wake up landed under a superseded "
                       "correlation id");
   BOOST_CHECK_MESSAGE(!node.app_threw(), "the node reported an exception while the stale lambda was in flight");
}

/**
 * A block is not shipped at its own slot time, it is shipped at the cpu effort deadline
 * calculate_producing_block_deadline works out, which falls earlier so the block has time to reach
 * the next producer. block_timing_util's tests check that arithmetic in isolation and nothing checks
 * that the plugin actually arms its timer on the result, which is the difference between a schedule
 * that keeps ahead of the wall clock and one that drifts a little later every round.
 *
 * Owning the clock makes the distinction observable: hold just short of the deadline and no block is
 * owed, cross it and the block appears, all while the block's own slot time is still in the future.
 */
BOOST_AUTO_TEST_CASE(a_block_ships_at_its_deadline_not_at_its_slot) {
   // running_node pins produce-block-offset-ms, which is what makes the deadline land strictly
   // inside the slot and so makes this case meaningful at all.
   const fc::microseconds cpu_effort = running_node::cpu_effort();
   running_node node;

   // The block the node owes next, and when it is due to ship it.
   const auto next_block_time = chain::block_timestamp_type(node.head_block_timestamp().slot + 1);
   const auto deadline        = block_timing_util::calculate_producing_block_deadline(cpu_effort, next_block_time);
   BOOST_REQUIRE_MESSAGE(deadline < next_block_time.to_time_point(),
                         "this case is only meaningful while the deadline precedes the slot it belongs to");
   BOOST_REQUIRE_MESSAGE(deadline > node.now(),
                         "the deadline for the next block has already passed, nothing to observe");

   const uint32_t settled = node.blocks_produced();

   // Just short of the deadline nothing is owed, even though real time has moved on.
   node.advance_to(deadline - fc::milliseconds(2));
   std::this_thread::sleep_for(real_time_patience);
   BOOST_CHECK_MESSAGE(node.blocks_produced() == settled,
                       "a block shipped before its deadline");

   // Crossing the deadline ships it, while its own slot time is still ahead of the clock.
   node.advance_to(deadline + fc::milliseconds(2));
   BOOST_REQUIRE_MESSAGE(wait_for([&]() { return node.blocks_produced() > settled; }),
                         "no block shipped once its deadline passed");
   BOOST_CHECK_MESSAGE(node.now() < next_block_time.to_time_point(),
                       "the block was only shipped after its slot time, so the deadline did nothing");
}

/**
 * The other half of schedule_production_loop is the speculating branch: when the slot belongs to
 * someone else the node builds a speculative block and arms schedule_delayed_production_loop to wake
 * for its own next window. Everything above drives a node that owns every slot, so that branch never
 * runs; only a schedule with producers this node has no key for reaches it.
 *
 * The property is that speculating is not a one way door. A node that hands off to other producers
 * has to come back and produce when its own window returns, and the wake up it arms while
 * speculating is the only thing that brings it back.
 */
BOOST_AUTO_TEST_CASE(production_resumes_after_speculating_through_other_windows) {
   speculating_node seeded{{"defproducera"_n, "defproducerb"_n}};
   running_node&    node = seeded.node;

   // Walk a whole rotation. The node must speculate through the windows it has no key for, and it
   // must produce again in its own, which is what the delayed wake up exists to do.
   const uint32_t rotation   = seeded.rotation();
   bool           speculated = false;
   bool           resumed    = false;

   for (uint32_t slot = 0; slot < 2 * rotation && !resumed; ++slot) {
      const uint32_t before        = node.blocks_produced();
      const auto     beforePending = node.pending();
      node.advance(fc::milliseconds(config::block_interval_ms));

      // Wait for the node to do something NEW with the slot, not for a condition that already held
      // before the clock moved. Once it is speculating it goes on speculating, so waiting on that
      // would return at once and leave the sampling below racing the timer thread.
      wait_up_to(slot_settle_budget,
                 [&]() { return node.blocks_produced() > before || node.pending() != beforePending; });

      // Then ask what it did with the slot: either it is building for a producer it has no key for,
      // or it committed one of its own.
      if (node.speculating())
         speculated = true;
      else if (speculated && node.blocks_produced() > before)
         resumed = true;
   }

   BOOST_REQUIRE_MESSAGE(speculated,
                         "the node never built a block for a producer it has no key for, so it never "
                         "speculated and this case did not exercise the delayed production loop");
   BOOST_CHECK_MESSAGE(resumed,
                       "the node speculated through another producer's window and never produced "
                       "again: the wake up armed while speculating did not bring it back");
}

BOOST_AUTO_TEST_SUITE_END()

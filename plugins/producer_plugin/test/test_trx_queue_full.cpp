#include <boost/test/unit_test.hpp>

#include <sysio/producer_plugin/producer_plugin.hpp>

#include <sysio/testing/bls_utils.hpp>
#include <sysio/testing/tester.hpp>

#include <sysio/chain/application.hpp>
#include <sysio/chain/config.hpp>
#include <sysio/chain/contract_types.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/genesis_state.hpp>
#include <sysio/chain/plugin_interface.hpp>
#include <sysio/chain/trace.hpp>
#include <sysio/chain/transaction_metadata.hpp>

#include <fc/mock_time.hpp>
#include <fc/scoped_exit.hpp>

#include <atomic>
#include <chrono>
#include <deque>
#include <future>
#include <optional>
#include <set>
#include <thread>

#include "incoming_trx_utils.hpp"

namespace {

using namespace sysio;
using namespace sysio::chain;
using namespace sysio::chain::literals;
using namespace std::chrono_literals;
using sysio::test::wait_timeout;
using sysio::test::wait_until;

/// incoming-transaction-queue-size-mb for the node: the smallest the option takes.
constexpr auto queue_size_mb = "1";
/// Block net limit the genesis sets: about 70 of the test transactions fit a block, so the executed ones spread over
/// several blocks.
constexpr uint32_t max_block_net_usage = 16 * 1024;
static_assert(max_block_net_usage > config::min_max_transaction_net_usage,
              "the chain requires a block net limit above the transaction net limit");
/// Lifetime of every pushed transaction, long enough that none expires during the test.
constexpr auto trx_lifetime = fc::minutes(5);
/// Genesis time, where the mock clock starts: past the first production round after the block timestamp epoch,
/// whose block deadlines the producer does not compute.
constexpr const char* genesis_timestamp = "2025-01-01T12:00:00";
/// Real time the node gets to produce a block once the mock clock has moved past its deadline.
constexpr auto block_patience = 2s;
/// Most moves of the mock clock one wait makes, enough blocks for every queued transaction to execute.
constexpr uint32_t max_clock_moves = 200;
/// Blocks produced before the test starts pushing.
constexpr uint32_t warmup_blocks = 3;
/// Empty blocks after the last answer, so an extra answer from a transaction executed late would show.
constexpr uint32_t trailing_empty_blocks = 3;

/// The answers one pushed transaction received.
struct trx_answers {
   transaction_id_type   id; ///< set before the push, read after the app thread has joined
   std::atomic<uint32_t> count{0};
   std::atomic<bool>     refused{false};  ///< answered with tx_resource_exhaustion
   std::atomic<bool>     executed{false}; ///< answered with a trace without an exception
   std::string           error;           ///< any other answer, written on the main thread, read after it has joined
};

/// A running producing node on the mock clock: pushes it p2p transactions signed by sysio@active, records their answers
/// and the blocks it produces, and moves the clock for it. While the clock stands still, the block being built stays
/// open.
class queue_full_node {
public:
   queue_full_node(appbase::scoped_app& app, producer_plugin* prod, chain_plugin* chain, const private_key_type& key,
                   fc::time_point clock_now)
      : app(app), prod(prod), chain(chain), key(key), chain_id(chain->get_chain_id()), clock_now(clock_now) {}

   appbase::scoped_app&    app;
   producer_plugin*        prod;
   chain_plugin*           chain;
   const private_key_type  key;
   const chain_id_type     chain_id;
   std::deque<trx_answers> answers;
   fc::time_point          clock_now; ///< where the mock clock stands, moved only by advance_clock_until

   // Written by the accepted_block slot on the app thread; the set and the count are read only after it has joined.
   std::set<transaction_id_type> trxs_in_blocks;
   uint32_t                      blocks_with_trxs = 0;
   std::atomic<uint32_t>         head_num{0};
   std::atomic<uint32_t>         empty_run{0};

   /// Records a block the node accepted. Called on the app thread.
   void on_accepted_block(const signed_block_ptr& block) {
      for (const transaction_receipt& receipt : block->transactions)
         trxs_in_blocks.insert(receipt.trx.id());
      blocks_with_trxs += block->transactions.empty() ? 0 : 1;
      empty_run = block->transactions.empty() ? empty_run + 1 : 0;
      head_num  = block->block_num();
   }

   /// Runs `f` on the main thread and returns its result. `f` can still run after a timeout has made this return, so
   /// it captures by value or only state that outlives the app thread.
   template <typename F>
   auto on_main_thread(F f) {
      using result_t = std::invoke_result_t<F&>;
      auto                  result     = std::make_shared<std::promise<result_t>>();
      std::future<result_t> result_fut = result->get_future();
      app->executor().post(priority::high, exec_queue::read_write, [result, f]() mutable { result->set_value(f()); });
      BOOST_REQUIRE(result_fut.wait_for(wait_timeout) == std::future_status::ready);
      return result_fut.get();
   }

   /// Moves the mock clock a block interval at a time until `pred` holds, letting the node finish its work at the
   /// current time before each move and produce the block that falls due after it. The clock moves between the main
   /// thread's tasks, never while one of them runs a transaction.
   /// @return whether `pred` held within max_clock_moves
   template <typename Pred>
   bool advance_clock_until(Pred&& pred) {
      for (uint32_t moves = 0; moves < max_clock_moves; ++moves) {
         // the node has handled the current time and armed its timer for the next block
         on_main_thread([]() { return true; });
         if (pred())
            return true;
         const uint32_t head_before = head_num;
         clock_now += fc::milliseconds(config::block_interval_ms);
         // moved by a main thread task, so between transactions: a move during one would bill it the whole step
         on_main_thread([now = clock_now]() {
            fc::mock_time_traits::set_now(now);
            return true;
         });
         wait_until([&]() { return head_num > head_before; }, block_patience);
      }
      return pred();
   }

   /// @return a transaction, authorized by sysio@active, of a native newaccount by sysio for a new account
   packed_transaction_ptr make_trx() const {
      signed_transaction trx = test::make_newaccount_trx(trx_lifetime);
      trx.sign(key, chain_id);
      return std::make_shared<packed_transaction>(std::move(trx));
   }

   /// @return transactions enough to fill the queue twice; the recovered key adds to the estimate, so this overshoots
   size_t twice_queue_capacity() const {
      const uint64_t estimate =
         transaction_metadata::create_no_recover_keys(make_trx(), transaction_metadata::trx_type::input)
            ->get_estimated_size();
      return 2 * config::bytes_per_mebibyte / estimate;
   }

   /// Pushes `n` new p2p transactions, recording each answer.
   void push(size_t n) {
      for (size_t i = 0; i < n; ++i) {
         trx_answers& a = answers.emplace_back();
         packed_transaction_ptr trx = make_trx();
         a.id = trx->id();
         app->post(priority::low, [this, trx, &a]() {
            app->get_method<plugin_interface::incoming::methods::transaction_async>()(
               trx, false, transaction_metadata::trx_type::input, false,
               [&a](const next_function_variant<transaction_trace_ptr>& result) {
                  ++a.count;
                  if (const auto* ex = std::get_if<fc::exception_ptr>(&result)) {
                     if ((*ex)->code() == tx_resource_exhaustion::code_value)
                        a.refused = true;
                     else
                        a.error = (*ex)->to_string();
                  } else if (const auto* trace = std::get_if<transaction_trace_ptr>(&result)) {
                     if (!(*trace)->except)
                        a.executed = true;
                     else
                        a.error = (*trace)->except->to_string();
                  }
               });
         });
      }
   }

   size_t count_refused() const {
      size_t n = 0;
      for (const auto& a : answers)
         n += a.refused ? 1 : 0;
      return n;
   }

   size_t count_answered() const {
      size_t n = 0;
      for (const auto& a : answers)
         n += a.count > 0 ? 1 : 0;
      return n;
   }

   size_t count_executed() const {
      size_t n = 0;
      for (const auto& a : answers)
         n += a.executed ? 1 : 0;
      return n;
   }

   /// Transactions in the incoming queue, and those waiting in the executor to be processed.
   std::pair<size_t, size_t> queue_sizes() {
      return on_main_thread([this]() {
         const producer_plugin::get_unapplied_transactions_result r =
            prod->get_unapplied_transactions({.limit = 0}, fc::time_point::maximum());
         return std::make_pair(r.unapplied_size, r.queued_size);
      });
   }

   /// Pauses production at the next block and returns once the node has stopped building blocks.
   void pause() {
      const block_num_type pause_block = on_main_thread([this]() {
         const block_num_type n = chain->chain().head().block_num() + 1;
         prod->pause_at_block({n});
         return n;
      });
      BOOST_REQUIRE(advance_clock_until([&]() {
         return on_main_thread([this, pause_block]() {
            return chain->chain().head().block_num() == pause_block && !chain->chain().is_building_block();
         });
      }));
   }

   void resume() {
      on_main_thread([this]() {
         prod->resume();
         return true;
      });
   }
};

/// Starts a producing node on the mock clock with a 1 MiB incoming queue and a small block net limit, runs
/// `push_phase` on it, moves the clock until every pushed transaction is answered and a few empty blocks follow, then
/// checks that each transaction got exactly one answer and was either refused and never executed, or executed and
/// included in a block.
template <typename PushPhase>
void run_queue_full_node(const std::vector<std::string>& extra_args, PushPhase&& push_phase) {
   fc::temp_directory temp;
   const std::string  temp_dir_str = temp.path().string();

   const private_key_type producer_key = test::sysio_key();
   const auto [bls_priv, bls_pub, bls_pop, bls_prov] = sysio::testing::get_bls_key("finalizeraa"_n);

   genesis_state genesis = sysio::testing::base_tester::default_genesis();
   genesis.initial_timestamp                               = fc::time_point::from_iso_string(genesis_timestamp);
   genesis.initial_configuration.max_block_net_usage       = max_block_net_usage;
   genesis.initial_configuration.max_transaction_net_usage = config::min_max_transaction_net_usage;
   const std::string genesis_file = (temp.path() / "genesis.json").string();
   fc::json::save_to_file(genesis, genesis_file);

   // Engaged before the app and its threads exist, and released after they are gone. It starts at genesis, so the
   // node's first blocks fall due as the test moves it.
   const fc::scoped_mock_clock mock_clock(genesis.initial_timestamp);
   appbase::scoped_app         app;

   // <chain-kind>,<key-type>,<public-key>,<private-key-provider-spec>
   const std::string producer_provider =
      "wire,wire," + producer_key.get_public_key().to_string({}) + ",KEY:" + producer_key.to_string({});
   const std::string finalizer_provider = "wire,wire_bls," + bls_pub.to_string() + ",KEY:" + bls_priv.to_string();

   std::vector<std::string> args = {"test",
                                    "--data-dir", temp_dir_str,
                                    "--config-dir", temp_dir_str,
                                    "--genesis-json", genesis_file,
                                    "-p", "sysio", "-e",
                                    "--signature-provider", producer_provider,
                                    "--signature-provider", finalizer_provider,
                                    "--production-pause-vote-timeout-ms", "0",
                                    "--incoming-transaction-queue-size-mb", queue_size_mb,
                                    "--disable-subjective-p2p-billing=true"};
   args.insert(args.end(), extra_args.begin(), extra_args.end());
   std::vector<const char*> argv;
   for (const std::string& a : args)
      argv.push_back(a.c_str());

   // State the app thread's callbacks write. Declared before the app thread and on_exit, so that on every exit path
   // the app thread has joined before this goes out of scope; read here only after it has joined.
   std::optional<queue_full_node>     node;
   boost::signals2::scoped_connection accepted;

   std::promise<std::tuple<producer_plugin*, chain_plugin*>> plugin_promise;
   std::future<std::tuple<producer_plugin*, chain_plugin*>>  plugin_fut = plugin_promise.get_future();
   std::thread app_thread([&]() {
      try {
         app->initialize<signature_provider_manager_plugin, chain_plugin, producer_plugin>(argv.size(),
                                                                                           (char**)argv.data());
         app->startup();
         // app was constructed on the outer thread; capture this thread as main_thread_id_
         // before releasing the promise so producer_plugin's main-thread asserts see the loop thread.
         app->executor().set_main_thread_id();
         plugin_promise.set_value({app->find_plugin<producer_plugin>(), app->find_plugin<chain_plugin>()});
         app->exec();
         return;
      }
      FC_LOG_AND_DROP()
      BOOST_CHECK(!"app threw exception see logged error");
   });
   auto on_exit = fc::make_scoped_exit([&]() {
      app->quit();
      if (app_thread.joinable())
         app_thread.join();
   });

   BOOST_REQUIRE(plugin_fut.wait_for(wait_timeout) == std::future_status::ready);
   auto [prod_plug, chain_plug] = plugin_fut.get();
   node.emplace(app, prod_plug, chain_plug, producer_key, genesis.initial_timestamp);
   accepted = chain_plug->chain().accepted_block().connect(
      [&node](const block_signal_params& t) { node->on_accepted_block(std::get<0>(t)); });
   BOOST_REQUIRE(node->advance_clock_until([&]() { return node->head_num >= warmup_blocks; }));

   push_phase(*node);

   BOOST_REQUIRE(node->advance_clock_until([&]() { return node->count_answered() == node->answers.size(); }));
   const uint32_t answered_at = node->head_num;
   BOOST_REQUIRE(node->advance_clock_until([&]() {
      return node->head_num > answered_at + trailing_empty_blocks && node->empty_run >= trailing_empty_blocks;
   }));
   BOOST_TEST(node->queue_sizes().first == 0u);

   app->quit();
   app_thread.join();

   BOOST_TEST_MESSAGE("pushed " << node->answers.size() << ", refused " << node->count_refused()
                                << ", blocks with transactions " << node->blocks_with_trxs);
   BOOST_TEST(node->count_refused() > 0u);
   BOOST_TEST(node->blocks_with_trxs > 1u); // the block net limit spread the executed transactions over several blocks
   for (size_t i = 0; i < node->answers.size(); ++i) {
      const trx_answers& a = node->answers[i];
      BOOST_TEST_CONTEXT("trx " << i << " " << a.error) {
         BOOST_TEST(a.count == 1u);
         BOOST_TEST(a.refused != a.executed);
         BOOST_TEST(node->trxs_in_blocks.count(a.id) == (a.executed ? 1u : 0u));
      }
   }
}

} // namespace

BOOST_AUTO_TEST_SUITE(incoming_trx_queue_full)

// No block is being built: the node has stopped at a block with its transaction queue still running, the state an
// aborted block leaves when the node then waits. Each transaction is queued until the queue is full and refused after.
BOOST_AUTO_TEST_CASE(refused_while_not_building_block) {
   run_queue_full_node({}, [](queue_full_node& node) {
      node.pause();
      node.on_main_thread([&node]() {
         node.app->executor().enable_trx_read_write_queue(true);
         return true;
      });

      const size_t batch = node.twice_queue_capacity();
      node.push(batch);
      BOOST_REQUIRE(wait_until([&]() { return node.count_answered() + node.queue_sizes().first >= batch; }));
      std::this_thread::sleep_for(200ms); // let any straggler land before the exact checks

      const size_t refused = node.count_refused();
      BOOST_TEST(refused > 0u);
      BOOST_TEST(node.count_answered() == refused);              // nothing executes while no block is being built
      BOOST_TEST(node.queue_sizes().first == batch - refused); // a refused transaction is not left in the queue

      node.resume();
   });
}

// A block is being built and is full: with the block-exhausted thresholds at 0 a full block does not end early, so
// every transaction after it is re-queued as exhausted until the queue is full and refused after. The clock stands
// still while the batch is pushed, so the one open block attempts every transaction.
BOOST_AUTO_TEST_CASE(refused_requeue_of_exhausted_trx) {
   run_queue_full_node({"--max-block-net-usage-threshold-bytes", "0", "--max-block-cpu-usage-threshold-us", "0"},
                       [](queue_full_node& node) {
      BOOST_REQUIRE(node.on_main_thread([&node]() { return node.chain->chain().is_building_block(); }));
      const uint32_t head_before = node.head_num;

      const size_t batch = node.twice_queue_capacity();
      node.push(batch);
      // each transaction is answered, executed or refused, or waits in the queue, never both
      BOOST_REQUIRE(wait_until([&]() { return node.count_answered() + node.queue_sizes().first == batch; }));

      BOOST_TEST(node.head_num == head_before); // all in the one block
      BOOST_TEST(node.count_executed() > 0u);   // the ones that fit it
      BOOST_TEST(node.count_refused() > 0u);    // the ones after the queue filled
   });
}

BOOST_AUTO_TEST_SUITE_END()

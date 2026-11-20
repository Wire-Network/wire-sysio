#include <boost/test/unit_test.hpp>

#include <sysio/producer_plugin/producer_plugin.hpp>

#include <sysio/testing/tester.hpp>

#include <sysio/chain/contract_types.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/transaction_metadata.hpp>
#include <sysio/chain/trace.hpp>
#include <sysio/chain/name.hpp>
#include <fc/thread_utils.hpp>

#include <sysio/chain/application.hpp>

#include <map>
#include <set>

#include "incoming_trx_utils.hpp"

namespace {

using namespace sysio;
using namespace sysio::chain;

/// Makes the test's transactions: native newaccounts by sysio, every few made to fail.
class trx_maker {
public:
   /// @param chain_id chain the transactions are signed for
   explicit trx_maker(const chain_id_type& chain_id)
      : chain_id(chain_id) {}

   /// @return the next transaction: a newaccount, a failing variant of one, or a copy of the previous transaction
   packed_transaction_ptr make() {
      ++count;
      if (count % 7 == 0) { // fail some as duplicates: a copy of the previous trx, the pair landing in a block once
         previous = std::make_shared<packed_transaction>(*previous);
         return previous;
      }
      signed_transaction trx = test::make_newaccount_trx();
      bool valid = true;
      if (count % 20 == 0) { // fail some via expired
         trx.expiration = fc::time_point_sec{fc::time_point::now()};
         valid = false;
      }
      if (count % 15 == 0) { // fail some for invalid unlinkauth
         const permission_level sysio_active{config::system_account_name, config::active_name};
         trx.actions.emplace(trx.actions.begin(), vector<permission_level>{sysio_active}, unlinkauth{});
         valid = false;
      }
      if (count % 13 == 0) { // fail some for invalid signature
         trx.sign(bad_key, chain_id);
         valid = false;
      } else {
         trx.sign(test::sysio_key(), chain_id);
      }
      previous = std::make_shared<packed_transaction>(std::move(trx));
      if (valid)
         valid_trx_ids.insert(previous->id());
      return previous;
   }

   /// @return ids of the transactions made so far that should execute; a copy shares its original's id
   const std::set<transaction_id_type>& valid_ids() const { return valid_trx_ids; }

private:
   const chain_id_type chain_id;
   const private_key_type bad_key =
      private_key_type::regenerate<fc::ecc::private_key_shim>(fc::sha256::hash(std::string("kevin")).to_uint64_array());
   uint64_t count = 0;
   std::set<transaction_id_type> valid_trx_ids;
   packed_transaction_ptr previous;
};

/// Outcomes of every run of a transaction, re-runs after a block abort included.
struct trx_runs {
   std::map<transaction_id_type, uint32_t> succeeded; ///< trx id -> runs that succeeded
   std::set<transaction_id_type> cpu_dropped;         ///< trxs with a run failed past a cpu limit

   /// Records the outcome of one run.
   void record(const transaction_trace_ptr& trace) {
      if (!trace->except)
         ++succeeded[trace->id];
      else if (trace->except->code() == tx_cpu_usage_exceeded::code_value)
         cpu_dropped.insert(trace->id);
   }
};

/// Checks that the transactions reported executed are the valid ones, that each is in exactly one block, or in none
/// if it is in `runs.cpu_dropped`, and that the blocks hold no other transaction.
void verify_executed(const std::deque<packed_transaction_ptr>& executed, const std::set<transaction_id_type>& valid,
                     const trx_runs& runs, const std::deque<signed_block_ptr>& blocks) {
   std::map<transaction_id_type, uint32_t> blocks_holding; // trx id -> blocks holding it
   for (const signed_block_ptr& block : blocks) {
      for (const transaction_receipt& receipt : block->transactions)
         ++blocks_holding[receipt.trx.id()];
   }

   // a trx and its copy can both be reported executed when a block abort falls between them
   std::set<transaction_id_type> executed_ids;
   for (const packed_transaction_ptr& trx : executed)
      executed_ids.insert(trx->id());

   BOOST_TEST(executed_ids.size() == valid.size());
   for (const transaction_id_type& id : valid) {
      BOOST_TEST_CONTEXT("valid trx " << id.str()) {
         BOOST_TEST(executed_ids.count(id) == 1u);
      }
   }
   size_t dropped = 0;        // executed trxs in no block
   size_t rerun_in_block = 0; // executed trxs in a block after more than one successful run
   for (const transaction_id_type& id : executed_ids) {
      const auto it = blocks_holding.find(id);
      const uint32_t holding = it == blocks_holding.end() ? 0u : it->second;
      const auto runs_it = runs.succeeded.find(id);
      const uint32_t succeeded = runs_it == runs.succeeded.end() ? 0u : runs_it->second;
      dropped += holding == 0u ? 1 : 0;
      rerun_in_block += holding == 1u && succeeded > 1u ? 1 : 0;
      BOOST_TEST_CONTEXT("executed trx " << id.str() << " in " << holding << " blocks") {
         BOOST_TEST(valid.count(id) == 1u);
         // re-run after a block abort, a trx past the producer's speculative-retry cpu cap is dropped; a client
         // resubmits it
         BOOST_TEST((holding == 1u || (holding == 0u && runs.cpu_dropped.count(id) == 1u)));
      }
   }
   for (const auto& [id, holding] : blocks_holding) {
      BOOST_TEST_CONTEXT("block trx " << id.str()) {
         BOOST_TEST(executed_ids.count(id) == 1u);
      }
   }
   // drops are rare even under load; most aborted trxs land in a block
   BOOST_TEST_MESSAGE("re-run trxs in a block " << rerun_in_block << ", dropped " << dropped);
   BOOST_TEST(rerun_in_block > dropped);
}

} // namespace

BOOST_AUTO_TEST_SUITE(ordered_trxs_full)

// Integration test of producer_plugin
// Test verifies that transactions are processed, reported to caller, and not lost
// even when blocks are aborted and some transactions fail.
BOOST_AUTO_TEST_CASE(producer) {
   fc::temp_directory temp;
   appbase::scoped_app app;

   auto temp_dir_str = temp.path().string();
   std::vector<std::string> args = {"test", "--data-dir", temp_dir_str, "--config-dir", temp_dir_str};
   const std::vector<std::string> node_args = test::producer_args();
   args.insert(args.end(), node_args.begin(), node_args.end());

   {
      std::promise<std::tuple<producer_plugin*, chain_plugin*>> plugin_promise;
      std::future<std::tuple<producer_plugin*, chain_plugin*>> plugin_fut = plugin_promise.get_future();
      std::thread app_thread( [&]() {
         try {
            fc::logger::default_logger().set_log_level(fc::log_level::debug);
            std::vector<const char*> argv;
            for (const std::string& arg : args)
               argv.push_back(arg.c_str());
            app->initialize<signature_provider_manager_plugin,chain_plugin, producer_plugin>(
               argv.size(), const_cast<char**>(argv.data()) );
            app->startup();
            // app was constructed on the outer thread; capture this thread as main_thread_id_
            // before releasing the promise so producer_plugin's main-thread asserts see the loop thread.
            app->executor().set_main_thread_id();
            plugin_promise.set_value(
                  {app->find_plugin<producer_plugin>(), app->find_plugin<chain_plugin>()} );
            app->exec();
            return;
         } FC_LOG_AND_DROP()
         BOOST_CHECK(!"app threw exception see logged error");
      } );

      auto[prod_plug, chain_plug] = plugin_fut.get();
      trx_maker maker( chain_plug->get_chain_id() );

      std::deque<signed_block_ptr> all_blocks;
      std::promise<void> empty_blocks_promise;
      std::future<void> empty_blocks_fut = empty_blocks_promise.get_future();
      int num_empty = std::numeric_limits<int>::max();
      auto ab = chain_plug->chain().accepted_block().connect( [&](const chain::block_signal_params& t) {
         const auto& [ block, id ] = t;
         all_blocks.push_back( block );
         if( block->transactions.empty() ) {
            if( --num_empty == 0 ) empty_blocks_promise.set_value();
         } else if( num_empty > 0 ) { // we want a few empty blocks after we have some non-empty blocks
            num_empty = 10;
         }
      } );
      trx_runs runs;
      auto at = chain_plug->chain().applied_transaction().connect(
         [&](std::tuple<const transaction_trace_ptr&, const packed_transaction_ptr&> t) {
            runs.record( std::get<0>( t ) );
         } );

      std::atomic<size_t> num_acked = 0;
      plugin_interface::compat::channels::transaction_ack::channel_type::handle incoming_transaction_ack_subscription =
            app->get_channel<plugin_interface::compat::channels::transaction_ack>().subscribe(
                  [&num_acked]( const std::pair<fc::exception_ptr, packed_transaction_ptr>& t){
                     ++num_acked;
                  } );

      std::deque<packed_transaction_ptr> trxs;
      std::atomic<size_t> next_calls = 0;
      std::atomic<size_t> num_posts = 0;
      std::atomic<size_t> trace_with_except = 0;
      std::atomic<bool> trx_match = true;
      const size_t num_pushes = 4242;
      for( size_t i = 1; i <= num_pushes; ++i ) {
         packed_transaction_ptr ptrx = maker.make();
         dlog( "posting {}", ptrx->id() );
         app->post( priority::low, [ptrx, &next_calls, &num_posts, &trace_with_except, &trx_match, &trxs, &app]() {
            ++num_posts;
            bool return_failure_traces = num_posts % 2;
            app->get_method<plugin_interface::incoming::methods::transaction_async>()(ptrx,
               false, // api_trx
               transaction_metadata::trx_type::input, // trx_type
               return_failure_traces, // return_failure_traces
               [ptrx, &next_calls, &trace_with_except, &trx_match, &trxs, return_failure_traces]
               (const next_function_variant<transaction_trace_ptr>& result) {
                  if( !std::holds_alternative<fc::exception_ptr>( result ) && !std::get<chain::transaction_trace_ptr>( result )->except ) {
                     if( std::get<chain::transaction_trace_ptr>( result )->id == ptrx->id() ) {
                        trxs.push_back( ptrx );
                     } else {
                        elog( "trace not for trx {}: {}",
                              ptrx->id(), fc::json::to_pretty_string(*std::get<chain::transaction_trace_ptr>(result)) );
                        trx_match = false;
                     }
                  } else if( !return_failure_traces && !std::holds_alternative<fc::exception_ptr>( result ) && std::get<chain::transaction_trace_ptr>( result )->except ) {
                     elog( "trace with except {}",
                           fc::json::to_pretty_string( *std::get<chain::transaction_trace_ptr>( result ) ) );
                     ++trace_with_except;
                  }
                  ++next_calls;
           });
         });
         if( i % (num_pushes/3) == 0 ) {
            // need to sleep or a fast machine might put all trxs in one block
            usleep( config::block_interval_us / 2 );
         }
         if( i % 200 == 0 ) {
            // get_integrity_hash aborts block and places aborted trxs into unapplied_transaction_queue
            // verifying that aborting block does not lose transactions
            app->post(priority::high, [&](){
               app->find_plugin<producer_plugin>()->get_integrity_hash();
            });
         }
      }

      BOOST_CHECK( empty_blocks_fut.wait_for(std::chrono::seconds(60)) == std::future_status::ready );

      BOOST_CHECK_EQUAL( trace_with_except, 0u ); // should not have any traces with except in it
      BOOST_CHECK_EQUAL( num_pushes, num_posts );
      BOOST_CHECK_EQUAL( num_pushes, next_calls );
      BOOST_CHECK_EQUAL( num_pushes, num_acked );
      BOOST_CHECK( trx_match.load() );

      app->quit();
      app_thread.join();

      BOOST_CHECK( all_blocks.size() > 3u ); // should have a few blocks otherwise test is running too fast
      BOOST_TEST_MESSAGE( "pushed " << num_pushes << ", valid " << maker.valid_ids().size() << ", reported executed "
                                    << trxs.size() << ", blocks " << all_blocks.size() );
      verify_executed( trxs, maker.valid_ids(), runs, all_blocks );
   }
}


BOOST_AUTO_TEST_SUITE_END()

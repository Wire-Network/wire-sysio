#include <boost/test/unit_test.hpp>

#include <sysio/producer_plugin/producer_plugin.hpp>

#include <sysio/testing/tester.hpp>

#include <sysio/chain/genesis_state.hpp>
#include <sysio/chain/thread_utils.hpp>
#include <sysio/chain/transaction_metadata.hpp>
#include <sysio/chain/trace.hpp>
#include <sysio/chain/name.hpp>

#include <sysio/chain/application.hpp>

namespace sysio::test::detail {
using namespace sysio::chain::literals;
struct testit {
   uint64_t      id;

   testit( uint64_t id = 0 )
         :id(id){}

   static account_name get_account() {
      return chain::config::system_account_name;
   }

   static action_name get_name() {
      return "testit"_n;
   }
};
}
FC_REFLECT( sysio::test::detail::testit, (id) )

namespace {

using namespace sysio;
using namespace sysio::chain;
using namespace sysio::test::detail;

auto make_unique_trx( const chain_id_type& chain_id ) {

   static uint64_t nextid = 0;
   ++nextid;

   account_name creator = config::system_account_name;

   signed_transaction trx;
   // if a transaction expires after it was aborted then it will not be included in a block
   trx.expiration = fc::time_point_sec{fc::time_point::now() + fc::seconds( nextid % 20 == 0 ? 0 : 60 )}; // fail some transactions via expired
   if( nextid % 15 == 0 ) { // fail some for invalid unlinkauth
      trx.actions.emplace_back( vector<permission_level>{{creator, config::active_name}},
                                unlinkauth{} );
      trx.actions.emplace_back( vector<permission_level>{{creator, config::active_name}},
                                testit{nextid} );
   } else {
      trx.actions.emplace_back( vector<permission_level>{{creator, config::active_name}},
                                testit{ nextid % 7 == 0 ? 0 : nextid} ); // fail some for duplicates
   }
   if( nextid % 13 == 0 ) { // fail some for invalid signature
      auto bad_priv_key = private_key_type::regenerate<fc::ecc::private_key_shim>(fc::sha256::hash(std::string("kevin")));
      trx.sign( bad_priv_key, chain_id );
   } else {
      auto priv_key = private_key_type::regenerate<fc::ecc::private_key_shim>(fc::sha256::hash(std::string("nathan")));
      trx.sign( priv_key, chain_id );
   }

   return std::make_shared<packed_transaction>( std::move(trx) );
}

// verify all trxs are in blocks only once
bool verify_equal( const std::deque<packed_transaction_ptr>& trxs, const std::deque<block_state_ptr>& all_blocks) {
   std::set<transaction_id_type> trxs_ids; // trx can appear more than once if they were aborted
   std::set<transaction_id_type> blk_trxs_ids;

   for( const auto& trx : trxs ) {
      trxs_ids.emplace( trx->id() );
   }
   for( const auto& bs : all_blocks ) {
      for( const auto& trx_receipt : bs->block->transactions ) {
         const auto& trx = std::get<packed_transaction>( trx_receipt.trx ).get_transaction();
         blk_trxs_ids.emplace( trx.id() );
      }
   }
   BOOST_CHECK_EQUAL( trxs_ids.size(), blk_trxs_ids.size() );

   for( const auto& trx : trxs ) {
      BOOST_CHECK( blk_trxs_ids.count( trx->id() ) == 1 );
   }

   return true;
}


}

BOOST_AUTO_TEST_SUITE(ordered_trxs_full)

// Integration test of producer_plugin
// Test verifies that transactions are processed, reported to caller, and not lost
// even when blocks are aborted and some transactions fail.
BOOST_AUTO_TEST_CASE(producer) {
   fc::temp_directory temp;
   appbase::scoped_app app;
   
   auto temp_dir_str = temp.path().string();
   
   {
      std::promise<std::tuple<producer_plugin*, chain_plugin*>> plugin_promise;
      std::future<std::tuple<producer_plugin*, chain_plugin*>> plugin_fut = plugin_promise.get_future();
      std::thread app_thread( [&]() {
         try {
            fc::logger::get(DEFAULT_LOGGER).set_log_level(fc::log_level::debug);
            std::vector<const char*> argv =
                  {"test", "--data-dir", temp_dir_str.c_str(), "--config-dir", temp_dir_str.c_str(),
                   "-p", "sysio", "-e", "--disable-subjective-p2p-billing=true" };
            app->initialize<chain_plugin, producer_plugin>( argv.size(), (char**) &argv[0] );
            app->startup();
            plugin_promise.set_value(
                  {app->find_plugin<producer_plugin>(), app->find_plugin<chain_plugin>()} );
            app->exec();
            return;
         } FC_LOG_AND_DROP()
         BOOST_CHECK(!"app threw exception see logged error");
      } );

      auto[prod_plug, chain_plug] = plugin_fut.get();
      auto chain_id = chain_plug->get_chain_id();

      std::deque<block_state_ptr> all_blocks;
      std::promise<void> empty_blocks_promise;
      std::future<void> empty_blocks_fut = empty_blocks_promise.get_future();
      auto ab = chain_plug->chain().accepted_block.connect( [&](const block_state_ptr& bsp) {
         static int num_empty = std::numeric_limits<int>::max();
         all_blocks.push_back( bsp );
         if( bsp->block->transactions.empty() ) {
            --num_empty;
            if( num_empty == 0 ) empty_blocks_promise.set_value();
         } else { // we want a few empty blocks after we have some non-empty blocks
            num_empty = 10;
         }
      } );
      auto bs = chain_plug->chain().block_start.connect( [&]( uint32_t bn ) {
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
         auto ptrx = make_unique_trx( chain_id );
         dlog( "posting ${id}", ("id", ptrx->id()) );
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
                        elog( "trace not for trx ${id}: ${t}",
                              ("id", ptrx->id())("t", fc::json::to_pretty_string(*std::get<chain::transaction_trace_ptr>(result))) );
                        trx_match = false;
                     }
                  } else if( !return_failure_traces && !std::holds_alternative<fc::exception_ptr>( result ) && std::get<chain::transaction_trace_ptr>( result )->except ) {
                     elog( "trace with except ${e}",
                           ("e", fc::json::to_pretty_string( *std::get<chain::transaction_trace_ptr>( result ) )) );
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

      empty_blocks_fut.wait_for(std::chrono::seconds(15));

      BOOST_CHECK_EQUAL( trace_with_except, 0u ); // should not have any traces with except in it
      BOOST_CHECK( all_blocks.size() > 3u ); // should have a few blocks otherwise test is running too fast
      BOOST_CHECK_EQUAL( num_pushes, num_posts );
      BOOST_CHECK_EQUAL( num_pushes, next_calls );
      BOOST_CHECK_EQUAL( num_pushes, num_acked );
      BOOST_CHECK( trx_match.load() );

      app->quit();
      app_thread.join();

      BOOST_REQUIRE( verify_equal(trxs, all_blocks ) );

   } 
}


BOOST_AUTO_TEST_SUITE_END()

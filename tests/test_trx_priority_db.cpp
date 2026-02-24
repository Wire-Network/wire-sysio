#include <boost/test/unit_test.hpp>

#include <sysio/chain/exceptions.hpp>
#include <sysio/producer_plugin/trx_priority_db.hpp>
#include <sysio_system_tester.hpp>
#include <appbase/application_base.hpp>

#include <fc/variant_object.hpp>

#include <fc/log/logger.hpp>
#include <sysio/chain/controller.hpp>

BOOST_AUTO_TEST_SUITE(trx_priority_db_tests)

using namespace sysio::testing;
using namespace sysio;
using namespace appbase;

// this test verifies trx_priority_db behavior when adding, deleting, and managing transaction priority entries via system actions
BOOST_AUTO_TEST_CASE( trx_priority_db_tests ) { try {
   sysio_system::sysio_system_tester chain;
   trx_priority_db priority_db;

   [[maybe_unused]] auto lib_connection = chain.control->irreversible_block().connect([&](const chain::block_signal_params& t) {
         const auto& [block, id] = t;
         assert(!!chain.control);
         priority_db.on_irreversible_block(block, id, *chain.control);
      });

   chain.produce_blocks(5); // verify empty system contract trx_priority table

   chain.push_action(config::system_account_name, "addtrxp"_n,
                     mvo()("receiver", "alice"_n)("action_name", "transfer"_n)("match_type", 0)("priority", 1));

   chain.push_action(config::system_account_name, "deltrxp"_n, mvo()("priority", 1)); // verify we can delete
   BOOST_TEST_REQUIRE( base_tester::error("assertion failure with message: Unable to find priority: 1") ==
                        chain.push_action(config::system_account_name, "deltrxp"_n, mvo()("priority", 1)) );

   chain.produce_block();

   // fill with more trx_priority entries
   chain.push_action(config::system_account_name, "addtrxp"_n,
                     mvo()("receiver", "alice"_n)("action_name", "transfer"_n)("match_type", 0)("priority", 1));
   chain.push_action(config::system_account_name, "addtrxp"_n,
                     mvo()("receiver", "alice"_n)("action_name", ""_n)("match_type", 1)("priority", 2));
   chain.push_action(config::system_account_name, "addtrxp"_n,
                     mvo()("receiver", "bob"_n)("action_name", ""_n)("match_type", 2)("priority", 3));
   chain.push_action(config::system_account_name, "addtrxp"_n,
                     mvo()("receiver", "bob"_n)("action_name", "transfer"_n)("match_type", 2)("priority", 4));
   chain.push_action(config::system_account_name, "addtrxp"_n,
                     mvo()("receiver", "bob"_n)("action_name", "other"_n)("match_type", 0)("priority", 5));
   chain.push_action(config::system_account_name, "addtrxp"_n,
                     mvo()("receiver", "bob"_n)("action_name", "another"_n)("match_type", 1)("priority", 6));

   chain.produce_blocks(trx_priority_db::trx_priority_refresh_interval+3);

   transaction trx;
   trx.actions.emplace_back( std::vector<permission_level>{}, "alice"_n, "transfer"_n, bytes{} );
   BOOST_TEST(priority::low + 1 == priority_db.get_trx_priority(trx) );

   trx.actions.clear();
   trx.actions.emplace_back( std::vector<permission_level>{}, "fred"_n, "dummy"_n, bytes{} );
   BOOST_TEST(priority::low == priority_db.get_trx_priority(trx) );

   trx.actions.clear();
   trx.actions.emplace_back( std::vector<permission_level>{}, "fred"_n, "dummy"_n, bytes{} );
   trx.actions.emplace_back( std::vector<permission_level>{}, "alice"_n, "transfer"_n, bytes{} );
   BOOST_TEST(priority::low == priority_db.get_trx_priority(trx) );

   trx.actions.clear();
   trx.actions.emplace_back( std::vector<permission_level>{}, "alice"_n, "dummy"_n, bytes{} );
   BOOST_TEST(priority::low + 2 == priority_db.get_trx_priority(trx) );

   trx.actions.clear();
   trx.actions.emplace_back( std::vector<permission_level>{}, "bob"_n, "dummy"_n, bytes{} );
   BOOST_TEST(priority::low + 3 == priority_db.get_trx_priority(trx) );

   trx.actions.clear();
   trx.actions.emplace_back( std::vector<permission_level>{}, "bob"_n, "transfer"_n, bytes{} );
   BOOST_TEST(priority::low + 4 == priority_db.get_trx_priority(trx) );

   trx.actions.clear();
   trx.actions.emplace_back( std::vector<permission_level>{}, "bob"_n, "other"_n, bytes{} );
   BOOST_TEST(priority::low + 5 == priority_db.get_trx_priority(trx) );

   trx.actions.clear();
   trx.actions.emplace_back( std::vector<permission_level>{}, "fred"_n, "dummy"_n, bytes{} );
   trx.actions.emplace_back( std::vector<permission_level>{}, "bob"_n, "other"_n, bytes{} );
   BOOST_TEST(priority::low + 3 == priority_db.get_trx_priority(trx) ); // other can only match if only action

   trx.actions.clear();
   trx.actions.emplace_back( std::vector<permission_level>{}, "fred"_n, "dummy"_n, bytes{} );
   trx.actions.emplace_back( std::vector<permission_level>{}, "bob"_n, "another"_n, bytes{} );
   BOOST_TEST(priority::low + 3 == priority_db.get_trx_priority(trx) ); // another can only match if first action

   trx.actions.clear();
   trx.actions.emplace_back( std::vector<permission_level>{}, "bob"_n, "another"_n, bytes{} );
   trx.actions.emplace_back( std::vector<permission_level>{}, "fred"_n, "dummy"_n, bytes{} );
   BOOST_TEST(priority::low + 6 == priority_db.get_trx_priority(trx) ); // another can only match if first action

} FC_LOG_AND_RETHROW() }

// negative tests - verify error handling and edge cases
BOOST_AUTO_TEST_CASE( trx_priority_db_negative_tests ) { try {
   sysio_system::sysio_system_tester chain;
   trx_priority_db priority_db;

   [[maybe_unused]] auto lib_connection = chain.control->irreversible_block().connect([&](const chain::block_signal_params& t) {
         const auto& [block, id] = t;
         assert(!!chain.control);
         priority_db.on_irreversible_block(block, id, *chain.control);
      });

   chain.produce_blocks(5);

   // test: delete non-existent priority
   BOOST_TEST_REQUIRE( base_tester::error("assertion failure with message: Unable to find priority: 999") ==
                        chain.push_action(config::system_account_name, "deltrxp"_n, mvo()("priority", 999)) );

   // test: add duplicate priority (same priority value)
   chain.push_action(config::system_account_name, "addtrxp"_n,
                     mvo()("receiver", "alice"_n)("action_name", "transfer"_n)("match_type", 0)("priority", 1));
   BOOST_TEST_REQUIRE( base_tester::error("assertion failure with message: Priority 1 already exists") ==
                        chain.push_action(config::system_account_name, "addtrxp"_n,
                                          mvo()("receiver", "bob"_n)("action_name", "deposit"_n)("match_type", 0)("priority", 1)) );

   chain.produce_blocks(trx_priority_db::trx_priority_refresh_interval+3);

   // test: transaction with no actions returns default priority
   transaction trx;
   BOOST_TEST(priority::low == priority_db.get_trx_priority(trx) );

   // test: transaction with non-matching receiver
   trx.actions.clear();
   trx.actions.emplace_back( std::vector<permission_level>{}, "nonexistent"_n, "transfer"_n, bytes{} );
   BOOST_TEST(priority::low == priority_db.get_trx_priority(trx) );

   // test: transaction with non-matching action
   trx.actions.clear();
   trx.actions.emplace_back( std::vector<permission_level>{}, "alice"_n, "nonexistent"_n, bytes{} );
   BOOST_TEST(priority::low == priority_db.get_trx_priority(trx) );

   // test: delete priority that was added, then verify no longer matches
   chain.push_action(config::system_account_name, "deltrxp"_n, mvo()("priority", 1));
   chain.produce_blocks(trx_priority_db::trx_priority_refresh_interval+3);

   trx.actions.clear();
   trx.actions.emplace_back( std::vector<permission_level>{}, "alice"_n, "transfer"_n, bytes{} );
   BOOST_TEST(priority::low == priority_db.get_trx_priority(trx) ); // should no longer match after delete

   // test: double delete fails
   BOOST_TEST_REQUIRE( base_tester::error("assertion failure with message: Unable to find priority: 1") ==
                        chain.push_action(config::system_account_name, "deltrxp"_n, mvo()("priority", 1)) );

   // test: negative priority value
   chain.push_action(config::system_account_name, "addtrxp"_n,
                     mvo()("receiver", "charlie"_n)("action_name", "withdraw"_n)("match_type", 0)("priority", -5));
   chain.produce_blocks(trx_priority_db::trx_priority_refresh_interval+3);

   trx.actions.clear();
   trx.actions.emplace_back( std::vector<permission_level>{}, "charlie"_n, "withdraw"_n, bytes{} );
   BOOST_TEST(priority::low - 5 == priority_db.get_trx_priority(trx) );

} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#include <boost/test/unit_test.hpp>
#pragma GCC diagnostic pop

#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/resource_limits.hpp>
#include <sysio/testing/tester.hpp>

#include <fc/exception/exception.hpp>
#include <fc/variant_object.hpp>

#include <contracts.hpp>
#include <test_contracts.hpp>

using namespace sysio;
using namespace sysio::chain;
using namespace sysio::testing;
using namespace fc;

BOOST_AUTO_TEST_SUITE(noop_tests)

// default CDT produces an apply() function that asserts on unknown action.
// This test verifies this behavior, which is useful for wire contracts that pay for resources.
// If unknown actions were allowed then a user could spam a contract that pays for resources with unknown actions.
BOOST_FIXTURE_TEST_CASE(noop, sysio::testing::validating_tester) { try {
    produce_block();

    create_account("noop"_n);
    create_account("noop2"_n);
    set_contract("noop"_n, test_contracts::noop_wasm(), test_contracts::noop_abi());
    produce_block();

    push_action("noop"_n, "anyaction"_n, "noop"_n, mutable_variant_object()("from", "noop"_n)("type", "")("data", "") );
    produce_block();

    // push a non-existing action, default CDT build produces an apply that asserts
    {
        signed_transaction trx;
        // action( vector<permission_level> auth, account_name account, action_name name, const bytes& data )
        trx.actions.emplace_back( vector<permission_level>{{"noop"_n, config::active_name}}, // auth
                                  "noop"_n,     // account_name
                                  "notexist"_n, // action_name notexist does not exist in the contract
                                  bytes{});     // data
        set_transaction_headers(trx);
        trx.sign( get_private_key( "noop"_n, "active" ), control->get_chain_id()  );
        BOOST_CHECK_THROW(push_transaction( trx ), sysio_assert_code_exception);
    }

    // push a non-existing action to an account without a contract
    {
        signed_transaction trx;
        trx.actions.emplace_back( vector<permission_level>{{"noop2"_n, config::active_name}}, // auth
                                  "noop2"_n,    // account_name
                                  "notexist"_n, // action_name notexist does not exist in the contract
                                  bytes{});     // data
        set_transaction_headers(trx);
        trx.sign( get_private_key( "noop2"_n, "active" ), control->get_chain_id()  );
        BOOST_CHECK_EXCEPTION(push_transaction( trx ), action_validate_exception,
                              fc_exception_message_is("No contract for action notexist on account noop2"));
    }

} FC_LOG_AND_RETHROW() }

// trx_generator's transfer load goes to the noop contract's transfer, which checks the sender's authority and changes
// nothing, so the load needs no token contract or balances.
BOOST_FIXTURE_TEST_CASE(transfer_checks_sender_authority, sysio::testing::validating_tester) { try {
   constexpr auto contract = "noop"_n;
   constexpr auto sender   = "alice"_n;
   constexpr auto receiver = "bob"_n;
   create_accounts({contract, sender, receiver});
   set_contract(contract, test_contracts::noop_wasm(), test_contracts::noop_abi());
   produce_block();

   const auto transfer = mutable_variant_object()("from", sender)("to", receiver)("quantity", "1.0000 CUR")
                                                 ("memo", "load");
   auto trace = push_action(contract, "transfer"_n, sender, transfer);
   BOOST_REQUIRE(trace->receipt);
   produce_block();

   // Signed by the receiver instead of the sender.
   BOOST_CHECK_THROW(push_action(contract, "transfer"_n, receiver, transfer), missing_auth_exception);
} FC_LOG_AND_RETHROW() }

// Deployed chains leave sysio.noop without a contract so nobody can push free no-op transactions, while contracts may
// notify it or send it inline actions as events. Those are billed to the originating action's payer, not sysio.noop.
BOOST_FIXTURE_TEST_CASE(inline_and_notify_to_account_without_contract, sysio::testing::validating_tester) { try {
   constexpr auto source = "alice"_n;
   constexpr auto noop   = "sysio.noop"_n;
   create_accounts({source, noop});
   set_code(source, test_contracts::action_order_test_wasm());
   set_abi(source, test_contracts::action_order_test_abi());
   produce_block();

   // A transaction sent straight to sysio.noop is rejected.
   {
      signed_transaction trx;
      trx.actions.emplace_back(vector<permission_level>{{noop, config::active_name}}, noop, "noop"_n, bytes{});
      set_transaction_headers(trx);
      trx.sign(get_private_key(noop, "active"), control->get_chain_id());
      BOOST_CHECK_EXCEPTION(push_transaction(trx), action_validate_exception,
                            fc_exception_message_is("No contract for action noop on account sysio.noop"));
   }

   // Leave sysio.noop no CPU or NET, so billing it anything would fail the transaction.
   auto& rlm = control->get_mutable_resource_limits_manager();
   int64_t ram_bytes = 0, net_weight = 0, cpu_weight = 0;
   rlm.get_account_limits(noop, ram_bytes, net_weight, cpu_weight);
   rlm.set_account_limits(noop, ram_bytes, 0, 0, false);
   produce_block();
   const auto& limits = control->get_resource_limits_manager();
   BOOST_REQUIRE_EQUAL(limits.get_account_cpu_limit_ex(noop).first.max, 0);
   BOOST_REQUIRE_EQUAL(limits.get_account_net_limit_ex(noop).first.max, 0);

   // inlnotify sends an inline action to inlt and notifies obs.
   auto trace = push_action(source, "inlnotify"_n, source, mutable_variant_object()("obs", noop)("inlt", noop));
   BOOST_REQUIRE(trace->receipt);

   const auto reached_noop = std::ranges::count_if(trace->action_traces, [&](const action_trace& at) {
      return at.receiver == noop && at.receipt.has_value();
   });
   BOOST_TEST(reached_noop == 2);

   BOOST_TEST(limits.get_account_cpu_limit_ex(noop).first.used == 0);
   BOOST_TEST(limits.get_account_net_limit_ex(noop).first.used == 0);
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()

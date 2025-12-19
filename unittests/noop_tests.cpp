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
        trx.actions.emplace_back( vector<permission_level>{{"noop"_n, sysio::chain::config::active_name}}, // auth
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
        trx.actions.emplace_back( vector<permission_level>{{"noop2"_n, sysio::chain::config::active_name}}, // auth
                                  "noop2"_n,    // account_name
                                  "notexist"_n, // action_name notexist does not exist in the contract
                                  bytes{});     // data
        set_transaction_headers(trx);
        trx.sign( get_private_key( "noop2"_n, "active" ), control->get_chain_id()  );
        BOOST_CHECK_EXCEPTION(push_transaction( trx ), action_validate_exception,
                              fc_exception_message_is("No contract for action notexist on account noop2"));
    }

} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()

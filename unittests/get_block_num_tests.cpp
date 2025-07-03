#include <sysio/chain/abi_serializer.hpp>
#include <sysio/chain/resource_limits.hpp>
#include <sysio/chain/generated_transaction_object.hpp>
#include <sysio/testing/tester.hpp>

#include <fc/variant_object.hpp>

#include <boost/test/unit_test.hpp>

#include <test_contracts.hpp>

#include "fork_test_utilities.hpp"

using namespace sysio::chain;
using namespace sysio::testing;
using namespace std::literals;

BOOST_AUTO_TEST_SUITE(get_block_num_tests)

BOOST_AUTO_TEST_CASE( get_block_num ) { try {
   tester c( setup_policy::full);

   const auto& tester1_account = account_name("tester1");
   c.create_accounts( {tester1_account} );
   c.produce_block();

   c.set_contract(tester1_account, test_contracts::get_block_num_test_wasm(),
                  test_contracts::get_block_num_test_abi());
   c.produce_block();

   c.push_action( tester1_account, "testblock"_n, tester1_account, mutable_variant_object()
      ("expected_result", c.control->head_block_num()+1)
   );

} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()

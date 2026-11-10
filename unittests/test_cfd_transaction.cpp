#include "test_cfd_transaction.hpp"
#include <test_contracts.hpp>

using namespace sysio::chain::literals;

std::vector<sysio::chain::signed_block_ptr> deploy_test_api(sysio::testing::tester& chain) {
   std::vector<sysio::chain::signed_block_ptr> result;
   chain.create_account("testapi"_n);
   chain.create_account("dummy"_n);
   result.push_back(chain.produce_block());
   chain.set_code("testapi"_n, sysio::testing::test_contracts::test_api_wasm());
   result.push_back(chain.produce_block());
   return result;
}

sysio::chain::transaction_trace_ptr push_test_dummy_transaction(sysio::testing::tester& chain) {
   sysio::chain::signed_transaction trx;
   dummy_action                     da = {DUMMY_ACTION_DEFAULT_A, DUMMY_ACTION_DEFAULT_B, DUMMY_ACTION_DEFAULT_C};
   trx.actions.emplace_back(
       std::vector<sysio::chain::permission_level>{{"testapi"_n, sysio::chain::config::active_name}}, da);
   chain.set_transaction_headers(trx);
   trx.sign(chain.get_private_key("testapi"_n, "active"), chain.control->get_chain_id());
   return chain.push_transaction(trx);
}

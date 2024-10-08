#include <sysio/chain/controller.hpp>
#include <sysio/chain/global_property_object.hpp>
#include <sysio/chain/permission_object.hpp>
#include <sysio/chain/resource_limits.hpp>
#include <sysio/chain/transaction.hpp>
#include <boost/test/unit_test.hpp>
#include <sysio/testing/tester.hpp>

#include "fork_test_utilities.hpp"
#include "test_cfd_transaction.hpp"

using namespace sysio;
using namespace sysio::chain;
using namespace sysio::testing;

BOOST_AUTO_TEST_SUITE(chain_tests)

BOOST_AUTO_TEST_CASE( replace_producer_keys ) try {
   validating_tester tester;

   const auto head_ptr = tester.control->head_block_state();
   BOOST_REQUIRE(head_ptr);

   const auto new_key = get_public_key(name("newkey"), config::active_name.to_string());

   // make sure new keys is not used
   for(const auto& prod : head_ptr->active_schedule.producers) {
      for(const auto& key : std::get<block_signing_authority_v0>(prod.authority).keys){  
         BOOST_REQUIRE(key.key != new_key);
      }
   }

   const auto old_version = head_ptr->pending_schedule.schedule.version;
   BOOST_REQUIRE_NO_THROW(tester.control->replace_producer_keys(new_key));
   const auto new_version = head_ptr->pending_schedule.schedule.version;
   // make sure version not been changed
   BOOST_REQUIRE(old_version == new_version);

   const auto& gpo = tester.control->db().get<global_property_object>();
   BOOST_REQUIRE(!gpo.proposed_schedule_block_num);
   BOOST_REQUIRE(gpo.proposed_schedule.version == 0);
   BOOST_REQUIRE(gpo.proposed_schedule.producers.empty());

   const uint32_t expected_threshold = 1;
   const weight_type expected_key_weight = 1;
   for(const auto& prod : head_ptr->active_schedule.producers) {
      BOOST_REQUIRE_EQUAL(std::get<block_signing_authority_v0>(prod.authority).threshold, expected_threshold);
      for(const auto& key : std::get<block_signing_authority_v0>(prod.authority).keys){
         BOOST_REQUIRE_EQUAL(key.key, new_key);
         BOOST_REQUIRE_EQUAL(key.weight, expected_key_weight);
       }
   }
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE( replace_account_keys ) try {
   validating_tester tester;
   const name usr = config::system_account_name;
   const name active_permission = config::active_name;
   const auto& rlm = tester.control->get_resource_limits_manager();
   const auto* perm = tester.control->db().find<permission_object, by_owner>(boost::make_tuple(usr, active_permission));
   BOOST_REQUIRE(perm != NULL);

   const int64_t old_size = (int64_t)(chain::config::billable_size_v<permission_object> + perm->auth.get_billable_size());
   const auto old_usr_auth = perm->auth;
   const auto new_key = get_public_key(name("newkey"), "active");
   const authority expected_authority(new_key);
   BOOST_REQUIRE(old_usr_auth != expected_authority);
   const auto old_ram_usg = rlm.get_account_ram_usage(usr);

   BOOST_REQUIRE_NO_THROW(tester.control->replace_account_keys(usr, active_permission, new_key));
   const int64_t new_size = (int64_t)(chain::config::billable_size_v<permission_object> + perm->auth.get_billable_size());
   const auto new_ram_usg = rlm.get_account_ram_usage(usr);
   BOOST_REQUIRE_EQUAL(old_ram_usg + (new_size - old_size), new_ram_usg);
   const auto new_usr_auth = perm->auth;
   BOOST_REQUIRE(new_usr_auth == expected_authority);

} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE( decompressed_size_over_limit ) try {
   tester chain;

   // build a transaction, add cf data, sign
   cf_action                        cfa;
   sysio::chain::signed_transaction trx;
   sysio::chain::action             act({}, cfa);
   trx.context_free_actions.push_back(act);
   // this is a over limit size (4+4)*129*1024 = 1032*1024 > 1M
   for(int i = 0; i < 129*1024; ++i){
      trx.context_free_data.emplace_back(fc::raw::pack<uint32_t>(100));
      trx.context_free_data.emplace_back(fc::raw::pack<uint32_t>(200));
   }
   // add a normal action along with cfa
   dummy_action         da = {DUMMY_ACTION_DEFAULT_A, DUMMY_ACTION_DEFAULT_B, DUMMY_ACTION_DEFAULT_C};
   sysio::chain::action act1(
       std::vector<sysio::chain::permission_level>{{"testapi"_n, sysio::chain::config::active_name}}, da);
   trx.actions.push_back(act1);
   chain.set_transaction_headers(trx);
   auto sig = trx.sign(chain.get_private_key("testapi"_n, "active"), chain.control->get_chain_id());

   // pack
   packed_transaction pt(trx, packed_transaction::compression_type::zlib);
   // try unpack and throw
   bytes packed_txn = pt.get_packed_transaction();
   bytes pcfd = pt.get_packed_context_free_data();
   vector<signature_type>  sigs;
   sigs.push_back(sig);
   BOOST_REQUIRE_EXCEPTION(packed_transaction copy( std::move(packed_txn), std::move(sigs), std::move(pcfd), packed_transaction::compression_type::zlib ),
                           tx_decompression_error,
                           [](const tx_decompression_error& e) {
                              return e.to_detail_string().find("Exceeded maximum decompressed transaction size") != std::string::npos;
                           });
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE( decompressed_size_under_limit ) try {
   tester chain;

   // build a transaction, add cf data, sign
   cf_action                        cfa;
   sysio::chain::signed_transaction trx;
   sysio::chain::action             act({}, cfa);
   trx.context_free_actions.push_back(act);
   // this is a under limit size  (4+4)*128*1024 = 1024*1024
   for(int i = 0; i < 100*1024; ++i){
      trx.context_free_data.emplace_back(fc::raw::pack<uint32_t>(100));
      trx.context_free_data.emplace_back(fc::raw::pack<uint32_t>(200));
   }
   // add a normal action along with cfa
   dummy_action         da = {DUMMY_ACTION_DEFAULT_A, DUMMY_ACTION_DEFAULT_B, DUMMY_ACTION_DEFAULT_C};
   sysio::chain::action act1(
       std::vector<sysio::chain::permission_level>{{"testapi"_n, sysio::chain::config::active_name}}, da);
   trx.actions.push_back(act1);
   chain.set_transaction_headers(trx);
   auto sig = trx.sign(chain.get_private_key("testapi"_n, "active"), chain.control->get_chain_id());

   // pack
   packed_transaction pt(trx, packed_transaction::compression_type::zlib);
   // try unpack
   bytes packed_txn = pt.get_packed_transaction();
   bytes pcfd = pt.get_packed_context_free_data();
   vector<signature_type>  sigs;
   sigs.push_back(sig);
   packed_transaction copy( std::move(packed_txn), std::move(sigs), std::move(pcfd), packed_transaction::compression_type::zlib );
   //passes if no exception is thrown

} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()

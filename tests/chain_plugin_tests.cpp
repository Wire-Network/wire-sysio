#include <boost/test/unit_test.hpp>
#include <boost/algorithm/string/predicate.hpp>

#include <sysio/testing/tester.hpp>
#include <sysio/chain/abi_serializer.hpp>
#include <sysio/chain/wasm_sysio_constraints.hpp>
#include <sysio/chain/resource_limits.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/wast_to_wasm.hpp>
#include <sysio/chain_plugin/chain_plugin.hpp>

#include <contracts.hpp>

#include <fc/io/fstream.hpp>

#include <Runtime/Runtime.h>

#include <fc/variant_object.hpp>
#include <fc/io/json.hpp>

#include <array>
#include <utility>

#ifdef NON_VALIDATING_TEST
#define TESTER tester
#else
#define TESTER validating_tester
#endif

using namespace sysio;
using namespace sysio::chain;
using namespace sysio::testing;
using namespace fc;

BOOST_AUTO_TEST_SUITE(chain_plugin_tests)

BOOST_FIXTURE_TEST_CASE( get_block_with_invalid_abi, TESTER ) try {
   produce_blocks(2);

   create_accounts( {"asserter"_n} );
   produce_block();

   // setup contract and abi
   set_code( "asserter"_n, contracts::asserter_wasm() );
   set_abi( "asserter"_n, contracts::asserter_abi().data() );
   produce_blocks(1);

   auto resolver = [&,this]( const account_name& name ) -> std::optional<abi_serializer> {
      try {
         const auto& accnt  = this->control->db().get<account_object,by_name>( name );
         abi_def abi;
         if (abi_serializer::to_abi(accnt.abi, abi)) {
            return abi_serializer(abi, abi_serializer::create_yield_function( abi_serializer_max_time ));
         }
         return std::optional<abi_serializer>();
      } FC_RETHROW_EXCEPTIONS(error, "resolver failed at chain_plugin_tests::abi_invalid_type");
   };

   // abi should be resolved
   BOOST_REQUIRE_EQUAL(true, resolver("asserter"_n).has_value());

   // make an action using the valid contract & abi
   fc::variant pretty_trx = mutable_variant_object()
      ("actions", variants({
         mutable_variant_object()
            ("account", "asserter")
            ("name", "procassert")
            ("authorization", variants({
               mutable_variant_object()
                  ("actor", "asserter")
                  ("permission", name(config::active_name).to_string())
            }))
            ("data", mutable_variant_object()
               ("condition", 1)
               ("message", "Should Not Assert!")
            )
         })
      );
   signed_transaction trx;
   abi_serializer::from_variant(pretty_trx, trx, resolver, abi_serializer::create_yield_function( abi_serializer_max_time ));
   set_transaction_headers(trx);
   trx.sign( get_private_key( "asserter"_n, "active" ), control->get_chain_id() );
   push_transaction( trx );
   produce_blocks(1);

   // retrieve block num
   uint32_t headnum = this->control->head_block_num();

   char headnumstr[20];
   sprintf(headnumstr, "%d", headnum);
   chain_apis::read_only::get_block_params param{headnumstr};
   chain_apis::read_only plugin(*(this->control), {}, fc::microseconds::maximum(), {}, {});

   // block should be decoded successfully
   std::string block_str = json::to_pretty_string(plugin.get_block(param));
   BOOST_TEST(block_str.find("procassert") != std::string::npos);
   BOOST_TEST(block_str.find("condition") != std::string::npos);
   BOOST_TEST(block_str.find("Should Not Assert!") != std::string::npos);
   BOOST_TEST(block_str.find("011253686f756c64204e6f742041737365727421") != std::string::npos); //action data

   // set an invalid abi (int8->xxxx)
   std::string abi2 = contracts::asserter_abi().data();
   auto pos = abi2.find("int8");
   BOOST_TEST(pos != std::string::npos);
   abi2.replace(pos, 4, "xxxx");
   set_abi("asserter"_n, abi2.c_str());
   produce_blocks(1);

   // resolving the invalid abi result in exception
   BOOST_CHECK_THROW(resolver("asserter"_n), invalid_type_inside_abi);

   // get the same block as string, results in decode failed(invalid abi) but not exception
   std::string block_str2 = json::to_pretty_string(plugin.get_block(param));
   BOOST_TEST(block_str2.find("procassert") != std::string::npos);
   BOOST_TEST(block_str2.find("condition") == std::string::npos); // decode failed
   BOOST_TEST(block_str2.find("Should Not Assert!") == std::string::npos); // decode failed
   BOOST_TEST(block_str2.find("011253686f756c64204e6f742041737365727421") != std::string::npos); //action data

} FC_LOG_AND_RETHROW() /// get_block_with_invalid_abi

BOOST_FIXTURE_TEST_CASE( get_account, TESTER ) try {
   produce_blocks(2);

   std::vector<account_name> accs{{ "alice"_n, "bob"_n, "cindy"_n}};
   create_accounts(accs, false, false);

   produce_block();

   chain_apis::read_only plugin(*(this->control), {}, fc::microseconds::maximum(), nullptr, nullptr);

   chain_apis::read_only::get_account_params p{"alice"_n};

   chain_apis::read_only::get_account_results result = plugin.read_only::get_account(p);

   auto check_result_basic = [](chain_apis::read_only::get_account_results result, sysio::name nm, bool isPriv) {
      BOOST_REQUIRE_EQUAL(nm, result.account_name);
      BOOST_REQUIRE_EQUAL(isPriv, result.privileged);

      BOOST_REQUIRE_EQUAL(2, result.permissions.size());
      if (result.permissions.size() > 1) {
         auto perm = result.permissions[0];
         BOOST_REQUIRE_EQUAL(name("active"_n), perm.perm_name); 
         BOOST_REQUIRE_EQUAL(name("owner"_n), perm.parent);
         auto auth = perm.required_auth;
         BOOST_REQUIRE_EQUAL(1, auth.threshold);
         BOOST_REQUIRE_EQUAL(1, auth.keys.size());
         BOOST_REQUIRE_EQUAL(0, auth.accounts.size());
         BOOST_REQUIRE_EQUAL(0, auth.waits.size());

         perm = result.permissions[1];
         BOOST_REQUIRE_EQUAL(name("owner"_n), perm.perm_name); 
         BOOST_REQUIRE_EQUAL(name(""_n), perm.parent); 
         auth = perm.required_auth;
         BOOST_REQUIRE_EQUAL(1, auth.threshold);
         BOOST_REQUIRE_EQUAL(1, auth.keys.size());
         BOOST_REQUIRE_EQUAL(0, auth.accounts.size());
         BOOST_REQUIRE_EQUAL(0, auth.waits.size());
      }
   };

   check_result_basic(result, name("alice"_n), false);

   for (auto perm : result.permissions) {
      BOOST_REQUIRE_EQUAL(true, perm.linked_actions.has_value());
      if (perm.linked_actions.has_value())
         BOOST_REQUIRE_EQUAL(0, perm.linked_actions->size());
   }
   BOOST_REQUIRE_EQUAL(0, result.sysio_any_linked_actions.size());

   // test link authority
   link_authority(name("alice"_n), name("bob"_n), name("active"_n), name("foo"_n));
   produce_block();
   result = plugin.read_only::get_account(p);

   check_result_basic(result, name("alice"_n), false);
   auto perm = result.permissions[0];
   BOOST_REQUIRE_EQUAL(1, perm.linked_actions->size());
   if (perm.linked_actions->size() >= 1) {
      auto la = (*perm.linked_actions)[0];
      BOOST_REQUIRE_EQUAL(name("bob"_n), la.account);
      BOOST_REQUIRE_EQUAL(true, la.action.has_value());
      if(la.action.has_value()) {
         BOOST_REQUIRE_EQUAL(name("foo"_n), la.action.value());
      }
   }
   BOOST_REQUIRE_EQUAL(0, result.sysio_any_linked_actions.size());

   // test link authority to sysio.any
   link_authority(name("alice"_n), name("bob"_n), name("sysio.any"_n), name("foo"_n));
   produce_block();
   result = plugin.read_only::get_account(p);
   check_result_basic(result, name("alice"_n), false);
   // active permission should no longer have linked auth, as sysio.any replaces it
   perm = result.permissions[0];
   BOOST_REQUIRE_EQUAL(0, perm.linked_actions->size());

   auto sysio_any_la = result.sysio_any_linked_actions;
   BOOST_REQUIRE_EQUAL(1, sysio_any_la.size());
   if (sysio_any_la.size() >= 1) {
      auto la = sysio_any_la[0];
      BOOST_REQUIRE_EQUAL(name("bob"_n), la.account);
      BOOST_REQUIRE_EQUAL(true, la.action.has_value());
      if(la.action.has_value()) {
         BOOST_REQUIRE_EQUAL(name("foo"_n), la.action.value());
      }
   }
} FC_LOG_AND_RETHROW() /// get_account

BOOST_AUTO_TEST_SUITE_END()

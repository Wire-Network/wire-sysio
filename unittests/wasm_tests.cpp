#include <array>
#include <utility>

#include <sysio/chain/abi_serializer.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/global_property_object.hpp>
#include <sysio/chain/resource_limits.hpp>
#include <sysio/chain/wasm_sysio_constraints.hpp>
#include <sysio/chain/wast_to_wasm.hpp>
#include <sysio/testing/tester.hpp>

#include <Inline/Serialization.h>
#include <IR/Module.h>
#include <WASM/WASM.h>

#include <boost/test/unit_test.hpp>
#include <boost/test/data/test_case.hpp>
#include <boost/test/data/monomorphic.hpp>
#include <boost/algorithm/string/predicate.hpp>

#include <fc/io/fstream.hpp>
#include <fc/io/incbin.h>
#include <fc/io/json.hpp>
#include <fc/variant_object.hpp>

#include "test_wasts.hpp"
#include "test_softfloat_wasts.hpp"

#include <test_contracts.hpp>

using namespace sysio;
using namespace sysio::chain;
using namespace sysio::testing;
using namespace fc;
namespace bdata = boost::unit_test::data;


struct assertdef {
   int8_t      condition;
   string      message;

   static account_name get_account() {
      return "asserter"_n;
   }

   static action_name get_name() {
      return "procassert"_n;
   }
};

FC_REFLECT(assertdef, (condition)(message));

struct provereset {
   static account_name get_account() {
      return "asserter"_n;
   }

   static action_name get_name() {
      return "provereset"_n;
   }
};

FC_REFLECT_EMPTY(provereset);

BOOST_AUTO_TEST_SUITE(wasm_tests)

/**
 * Prove that action reading and assertions are working
 */
BOOST_FIXTURE_TEST_CASE( basic_test, validating_tester ) try {
   produce_blocks(2);

   create_accounts( {"asserter"_n} );
   produce_block();

   set_code("asserter"_n, test_contracts::asserter_wasm());
   produce_blocks(1);

   transaction_id_type no_assert_id;
   {
      signed_transaction trx;
      trx.actions.emplace_back( vector<permission_level>{{"asserter"_n,config::active_name}},
                                assertdef {1, "Should Not Assert!"} );
      trx.actions[0].authorization = {{"asserter"_n,config::active_name}};

      set_transaction_headers(trx);
      trx.sign( get_private_key( "asserter"_n, "active" ), control->get_chain_id() );
      auto result = push_transaction( trx );
      BOOST_CHECK_EQUAL(result->receipt->status, transaction_receipt::executed);
      BOOST_CHECK_EQUAL(result->action_traces.size(), 1u);
      BOOST_CHECK_EQUAL(result->action_traces.at(0).receiver.to_string(),  name("asserter"_n).to_string() );
      BOOST_CHECK_EQUAL(result->action_traces.at(0).act.account.to_string(), name("asserter"_n).to_string() );
      BOOST_CHECK_EQUAL(result->action_traces.at(0).act.name.to_string(),  name("procassert"_n).to_string() );
      BOOST_CHECK_EQUAL(result->action_traces.at(0).act.authorization.size(),  1u );
      BOOST_CHECK_EQUAL(result->action_traces.at(0).act.authorization.at(0).actor.to_string(),  name("asserter"_n).to_string() );
      BOOST_CHECK_EQUAL(result->action_traces.at(0).act.authorization.at(0).permission.to_string(),  name(config::active_name).to_string() );
      no_assert_id = trx.id();
   }

   produce_blocks(1);

   BOOST_REQUIRE_EQUAL(true, chain_has_transaction(no_assert_id));
   const auto& receipt = get_transaction_receipt(no_assert_id);
   BOOST_CHECK_EQUAL(transaction_receipt::executed, receipt.status);

   transaction_id_type yes_assert_id;
   {
      signed_transaction trx;
      trx.actions.emplace_back( vector<permission_level>{{"asserter"_n,config::active_name}},
                                assertdef {0, "Should Assert!"} );

      set_transaction_headers(trx);
      trx.sign( get_private_key( "asserter"_n, "active" ), control->get_chain_id() );
      yes_assert_id = trx.id();

      BOOST_CHECK_THROW(push_transaction( trx ), sysio_assert_message_exception);
   }

   produce_blocks(1);

   auto has_tx = chain_has_transaction(yes_assert_id);
   BOOST_REQUIRE_EQUAL(false, has_tx);

} FC_LOG_AND_RETHROW() /// basic_test

/**
 * Prove the modifications to global variables are wiped between runs
 */
BOOST_FIXTURE_TEST_CASE( prove_mem_reset, validating_tester ) try {
   produce_blocks(2);

   create_accounts( {"asserter"_n} );
   produce_block();

   set_code("asserter"_n, test_contracts::asserter_wasm());
   produce_blocks(1);

   // repeat the action multiple times, each time the action handler checks for the expected
   // default value then modifies the value which should not survive until the next invoction
   for (int i = 0; i < 5; i++) {
      signed_transaction trx;
      trx.actions.emplace_back( vector<permission_level>{{"asserter"_n,config::active_name}},
                                provereset {} );

      set_transaction_headers(trx);
      trx.sign( get_private_key( "asserter"_n, "active" ), control->get_chain_id() );
      push_transaction( trx );
      produce_blocks(1);
      BOOST_REQUIRE_EQUAL(true, chain_has_transaction(trx.id()));
      const auto& receipt = get_transaction_receipt(trx.id());
      BOOST_CHECK_EQUAL(transaction_receipt::executed, receipt.status);
   }

} FC_LOG_AND_RETHROW() /// prove_mem_reset

/**
 * Prove the modifications to global variables are wiped between runs
 */
BOOST_FIXTURE_TEST_CASE( abi_from_variant, validating_tester ) try {
   produce_blocks(2);

   create_accounts( {"asserter"_n} );
   produce_block();

   set_code("asserter"_n, test_contracts::asserter_wasm());
   set_abi("asserter"_n, test_contracts::asserter_abi());
   produce_blocks(1);

   auto resolver = [&,this]( const account_name& name ) -> std::optional<abi_serializer> {
      try {
         const auto& accnt  = this->control->db().get<account_object,by_name>( name );
         if (abi_def abi; abi_serializer::to_abi(accnt.abi, abi)) {
            return abi_serializer(std::move(abi), abi_serializer::create_yield_function( abi_serializer_max_time ));
         }
         return std::optional<abi_serializer>();
      } FC_RETHROW_EXCEPTIONS(error, "Failed to find or parse ABI for ${name}", ("name", name))
   };

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
   BOOST_REQUIRE_EQUAL(true, chain_has_transaction(trx.id()));
   const auto& receipt = get_transaction_receipt(trx.id());
   BOOST_CHECK_EQUAL(transaction_receipt::executed, receipt.status);

} FC_LOG_AND_RETHROW() /// prove_mem_reset

// test softfloat 32 bit operations
BOOST_FIXTURE_TEST_CASE( f32_tests, validating_tester ) try {
   produce_blocks(2);
   produce_block();
   create_accounts( {"f32.tests"_n} );
   {
      set_code("f32.tests"_n, f32_test_wast);
      produce_blocks(10);

      signed_transaction trx;
      action act;
      act.account = "f32.tests"_n;
      act.name = ""_n;
      act.authorization = vector<permission_level>{{"f32.tests"_n,config::active_name}};
      trx.actions.push_back(act);

      set_transaction_headers(trx);
      trx.sign(get_private_key( "f32.tests"_n, "active" ), control->get_chain_id());
      push_transaction(trx);
      produce_blocks(1);
      BOOST_REQUIRE_EQUAL(true, chain_has_transaction(trx.id()));
      get_transaction_receipt(trx.id());
   }
} FC_LOG_AND_RETHROW()
BOOST_FIXTURE_TEST_CASE( f32_test_bitwise, validating_tester ) try {
   produce_blocks(2);
   create_accounts( {"f32.tests"_n} );
   produce_block();
   {
      set_code("f32.tests"_n, f32_bitwise_test_wast);
      produce_blocks(10);

      signed_transaction trx;
      action act;
      act.account = "f32.tests"_n;
      act.name = ""_n;
      act.authorization = vector<permission_level>{{"f32.tests"_n,config::active_name}};
      trx.actions.push_back(act);

      set_transaction_headers(trx);
      trx.sign(get_private_key( "f32.tests"_n, "active" ), control->get_chain_id());
      push_transaction(trx);
      produce_blocks(1);
      BOOST_REQUIRE_EQUAL(true, chain_has_transaction(trx.id()));
      get_transaction_receipt(trx.id());
   }
} FC_LOG_AND_RETHROW()
BOOST_FIXTURE_TEST_CASE( f32_test_cmp, validating_tester ) try {
   produce_blocks(2);
   create_accounts( {"f32.tests"_n} );
   produce_block();
   {
      set_code("f32.tests"_n, f32_cmp_test_wast);
      produce_blocks(10);

      signed_transaction trx;
      action act;
      act.account = "f32.tests"_n;
      act.name = ""_n;
      act.authorization = vector<permission_level>{{"f32.tests"_n,config::active_name}};
      trx.actions.push_back(act);

      set_transaction_headers(trx);
      trx.sign(get_private_key( "f32.tests"_n, "active" ), control->get_chain_id());
      push_transaction(trx);
      produce_blocks(1);
      BOOST_REQUIRE_EQUAL(true, chain_has_transaction(trx.id()));
      get_transaction_receipt(trx.id());
   }
} FC_LOG_AND_RETHROW()

// test softfloat 64 bit operations
BOOST_FIXTURE_TEST_CASE( f64_tests, validating_tester ) try {
   produce_blocks(2);
   create_accounts( {"f.tests"_n} );
   produce_block();
   {
      set_code("f.tests"_n, f64_test_wast);
      produce_blocks(10);

      signed_transaction trx;
      action act;
      act.account = "f.tests"_n;
      act.name = ""_n;
      act.authorization = vector<permission_level>{{"f.tests"_n,config::active_name}};
      trx.actions.push_back(act);

      set_transaction_headers(trx);
      trx.sign(get_private_key( "f.tests"_n, "active" ), control->get_chain_id());
      push_transaction(trx);
      produce_blocks(1);
      BOOST_REQUIRE_EQUAL(true, chain_has_transaction(trx.id()));
      get_transaction_receipt(trx.id());
   }
} FC_LOG_AND_RETHROW()
BOOST_FIXTURE_TEST_CASE( f64_test_bitwise, validating_tester ) try {
   produce_blocks(2);
   create_accounts( {"f.tests"_n} );
   produce_block();
   {
      set_code("f.tests"_n, f64_bitwise_test_wast);
      produce_blocks(10);

      signed_transaction trx;
      action act;
      act.account = "f.tests"_n;
      act.name = ""_n;
      act.authorization = vector<permission_level>{{"f.tests"_n,config::active_name}};
      trx.actions.push_back(act);

      set_transaction_headers(trx);
      trx.sign(get_private_key( "f.tests"_n, "active" ), control->get_chain_id());
      push_transaction(trx);
      produce_blocks(1);
      BOOST_REQUIRE_EQUAL(true, chain_has_transaction(trx.id()));
      get_transaction_receipt(trx.id());
   }
} FC_LOG_AND_RETHROW()
BOOST_FIXTURE_TEST_CASE( f64_test_cmp, validating_tester ) try {
   produce_blocks(2);
   create_accounts( {"f.tests"_n} );
   produce_block();
   {
      set_code("f.tests"_n, f64_cmp_test_wast);
      produce_blocks(10);

      signed_transaction trx;
      action act;
      act.account = "f.tests"_n;
      act.name = ""_n;
      act.authorization = vector<permission_level>{{"f.tests"_n,config::active_name}};
      trx.actions.push_back(act);

      set_transaction_headers(trx);
      trx.sign(get_private_key( "f.tests"_n, "active" ), control->get_chain_id());
      push_transaction(trx);
      produce_blocks(1);
      BOOST_REQUIRE_EQUAL(true, chain_has_transaction(trx.id()));
      get_transaction_receipt(trx.id());
   }
} FC_LOG_AND_RETHROW()

// test softfloat conversion operations
BOOST_FIXTURE_TEST_CASE( f32_f64_conversion_tests, validating_tester ) try {
   produce_blocks(2);

   create_accounts( {"ftests"_n} );
   produce_block();
   {
      set_code("ftests"_n, f32_f64_conv_wast);
      produce_blocks(10);

      signed_transaction trx;
      action act;
      act.account = "ftests"_n;
      act.name = ""_n;
      act.authorization = vector<permission_level>{{"ftests"_n,config::active_name}};
      trx.actions.push_back(act);

      set_transaction_headers(trx);
      trx.sign(get_private_key( "ftests"_n, "active" ), control->get_chain_id());
      push_transaction(trx);
      produce_blocks(1);
      BOOST_REQUIRE_EQUAL(true, chain_has_transaction(trx.id()));
      get_transaction_receipt(trx.id());
   }
} FC_LOG_AND_RETHROW()

// test softfloat conversion operations
BOOST_FIXTURE_TEST_CASE( f32_f64_overflow_tests, validating_tester ) try {
   int count = 0;
   auto check = [&](const char *wast_template, const char *op, const char *param) -> bool {
      count+=16;
      create_accounts( {name("ftests"_n.to_uint64_t()+count)} );
      produce_blocks(1);
      std::vector<char> wast;
      wast.resize(strlen(wast_template) + 128);
      sprintf(&(wast[0]), wast_template, op, param);
      set_code(name("ftests"_n.to_uint64_t()+count), &(wast[0]));
      produce_blocks(10);

      signed_transaction trx;
      action act;
      act.account = name("ftests"_n.to_uint64_t()+count);
      act.name = ""_n;
      act.authorization = vector<permission_level>{{name("ftests"_n.to_uint64_t()+count),config::active_name}};
      trx.actions.push_back(act);

      set_transaction_headers(trx);
      trx.sign(get_private_key( name("ftests"_n.to_uint64_t()+count), "active" ), control->get_chain_id());

      try {
         push_transaction(trx);
         produce_blocks(1);
         BOOST_REQUIRE_EQUAL(true, chain_has_transaction(trx.id()));
         get_transaction_receipt(trx.id());
         return true;
      } catch (sysio::chain::wasm_execution_error &) {
         return false;
      }
   };
   //
   //// float32 => int32
   // 2^31
   BOOST_REQUIRE_EQUAL(false, check(i32_overflow_wast, "i32_trunc_s_f32", "f32.const 2147483648"));
   // the maximum value below 2^31 representable in IEEE float32
   BOOST_REQUIRE_EQUAL(true, check(i32_overflow_wast, "i32_trunc_s_f32", "f32.const 2147483520"));
   // -2^31
   BOOST_REQUIRE_EQUAL(true, check(i32_overflow_wast, "i32_trunc_s_f32", "f32.const -2147483648"));
   // the maximum value below -2^31 in IEEE float32
   BOOST_REQUIRE_EQUAL(false, check(i32_overflow_wast, "i32_trunc_s_f32", "f32.const -2147483904"));

   //
   //// float32 => uint32
   BOOST_REQUIRE_EQUAL(true, check(i32_overflow_wast, "i32_trunc_u_f32", "f32.const 0"));
   BOOST_REQUIRE_EQUAL(false, check(i32_overflow_wast, "i32_trunc_u_f32", "f32.const -1"));
   // max value below 2^32 in IEEE float32
   BOOST_REQUIRE_EQUAL(true, check(i32_overflow_wast, "i32_trunc_u_f32", "f32.const 4294967040"));
   BOOST_REQUIRE_EQUAL(false, check(i32_overflow_wast, "i32_trunc_u_f32", "f32.const 4294967296"));

   //
   //// double => int32
   BOOST_REQUIRE_EQUAL(false, check(i32_overflow_wast, "i32_trunc_s_f64", "f64.const 2147483648"));
   BOOST_REQUIRE_EQUAL(true, check(i32_overflow_wast, "i32_trunc_s_f64", "f64.const 2147483647"));
   BOOST_REQUIRE_EQUAL(true, check(i32_overflow_wast, "i32_trunc_s_f64", "f64.const -2147483648"));
   BOOST_REQUIRE_EQUAL(false, check(i32_overflow_wast, "i32_trunc_s_f64", "f64.const -2147483649"));

   //
   //// double => uint32
   BOOST_REQUIRE_EQUAL(true, check(i32_overflow_wast, "i32_trunc_u_f64", "f64.const 0"));
   BOOST_REQUIRE_EQUAL(false, check(i32_overflow_wast, "i32_trunc_u_f64", "f64.const -1"));
   BOOST_REQUIRE_EQUAL(true, check(i32_overflow_wast, "i32_trunc_u_f64", "f64.const 4294967295"));
   BOOST_REQUIRE_EQUAL(false, check(i32_overflow_wast, "i32_trunc_u_f64", "f64.const 4294967296"));


   //// float32 => int64
   // 2^63
   BOOST_REQUIRE_EQUAL(false, check(i64_overflow_wast, "i64_trunc_s_f32", "f32.const 9223372036854775808"));
   // the maximum value below 2^63 representable in IEEE float32
   BOOST_REQUIRE_EQUAL(true, check(i64_overflow_wast, "i64_trunc_s_f32", "f32.const 9223371487098961920"));
   // -2^63
   BOOST_REQUIRE_EQUAL(true, check(i64_overflow_wast, "i64_trunc_s_f32", "f32.const -9223372036854775808"));
   // the maximum value below -2^63 in IEEE float32
   BOOST_REQUIRE_EQUAL(false, check(i64_overflow_wast, "i64_trunc_s_f32", "f32.const -9223373136366403584"));

   //// float32 => uint64
   BOOST_REQUIRE_EQUAL(false, check(i64_overflow_wast, "i64_trunc_u_f32", "f32.const -1"));
   BOOST_REQUIRE_EQUAL(true, check(i64_overflow_wast, "i64_trunc_u_f32", "f32.const 0"));
   // max value below 2^64 in IEEE float32
   BOOST_REQUIRE_EQUAL(true, check(i64_overflow_wast, "i64_trunc_u_f32", "f32.const 18446742974197923840"));
   BOOST_REQUIRE_EQUAL(false, check(i64_overflow_wast, "i64_trunc_u_f32", "f32.const 18446744073709551616"));

   //// double => int64
   // 2^63
   BOOST_REQUIRE_EQUAL(false, check(i64_overflow_wast, "i64_trunc_s_f64", "f64.const 9223372036854775808"));
   // the maximum value below 2^63 representable in IEEE float64
   BOOST_REQUIRE_EQUAL(true, check(i64_overflow_wast, "i64_trunc_s_f64", "f64.const 9223372036854774784"));
   // -2^63
   BOOST_REQUIRE_EQUAL(true, check(i64_overflow_wast, "i64_trunc_s_f64", "f64.const -9223372036854775808"));
   // the maximum value below -2^63 in IEEE float64
   BOOST_REQUIRE_EQUAL(false, check(i64_overflow_wast, "i64_trunc_s_f64", "f64.const -9223372036854777856"));

   //// double => uint64
   BOOST_REQUIRE_EQUAL(false, check(i64_overflow_wast, "i64_trunc_u_f64", "f64.const -1"));
   BOOST_REQUIRE_EQUAL(true, check(i64_overflow_wast, "i64_trunc_u_f64", "f64.const 0"));
   // max value below 2^64 in IEEE float64
   BOOST_REQUIRE_EQUAL(true, check(i64_overflow_wast, "i64_trunc_u_f64", "f64.const 18446744073709549568"));
   BOOST_REQUIRE_EQUAL(false, check(i64_overflow_wast, "i64_trunc_u_f64", "f64.const 18446744073709551616"));
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(misaligned_tests, validating_tester ) try {
   produce_blocks(2);
   create_accounts( {"aligncheck"_n} );
   produce_block();

   auto check_aligned = [&]( auto wast ) {
      set_code("aligncheck"_n, wast);
      produce_blocks(10);

      signed_transaction trx;
      action act;
      act.account = "aligncheck"_n;
      act.name = ""_n;
      act.authorization = vector<permission_level>{{"aligncheck"_n,config::active_name}};
      trx.actions.push_back(act);

      set_transaction_headers(trx);
      trx.sign(get_private_key( "aligncheck"_n, "active" ), control->get_chain_id());
      push_transaction(trx);
      produce_block();

      BOOST_REQUIRE_EQUAL(true, chain_has_transaction(trx.id()));
   };

   check_aligned(aligned_ref_wast);
   check_aligned(misaligned_ref_wast);
   check_aligned(aligned_const_ref_wast);
   check_aligned(misaligned_const_ref_wast);
} FC_LOG_AND_RETHROW()

/**
 * Make sure WASM "start" method is used correctly
 */
BOOST_FIXTURE_TEST_CASE( check_entry_behavior, validating_tester ) try {
   produce_blocks(2);
   create_accounts( {"entrycheck"_n} );
   produce_block();

   set_code("entrycheck"_n, entry_wast);
   produce_blocks(10);

   signed_transaction trx;
   action act;
   act.account = "entrycheck"_n;
   act.name = ""_n;
   act.authorization = vector<permission_level>{{"entrycheck"_n,config::active_name}};
   trx.actions.push_back(act);

   set_transaction_headers(trx);
   trx.sign(get_private_key( "entrycheck"_n, "active" ), control->get_chain_id());
   push_transaction(trx);
   produce_blocks(1);
   BOOST_REQUIRE_EQUAL(true, chain_has_transaction(trx.id()));
   const auto& receipt = get_transaction_receipt(trx.id());
   BOOST_CHECK_EQUAL(transaction_receipt::executed, receipt.status);
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( check_entry_behavior_2, validating_tester ) try {
   produce_blocks(2);
   create_accounts( {"entrycheck"_n} );
   produce_block();

   set_code("entrycheck"_n, entry_wast_2);
   produce_blocks(10);

   signed_transaction trx;
   action act;
   act.account = "entrycheck"_n;
   act.name = ""_n;
   act.authorization = vector<permission_level>{{"entrycheck"_n,config::active_name}};
   trx.actions.push_back(act);

   set_transaction_headers(trx);
   trx.sign(get_private_key( "entrycheck"_n, "active" ), control->get_chain_id());
   push_transaction(trx);
   produce_blocks(1);
   BOOST_REQUIRE_EQUAL(true, chain_has_transaction(trx.id()));
   const auto& receipt = get_transaction_receipt(trx.id());
   BOOST_CHECK_EQUAL(transaction_receipt::executed, receipt.status);
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( entry_import, validating_tester ) try {
   create_accounts( {"enterimport"_n} );
   produce_block();

   set_code("enterimport"_n, entry_import_wast);

   signed_transaction trx;
   action act;
   act.account = "enterimport"_n;
   act.name = ""_n;
   act.authorization = vector<permission_level>{{"enterimport"_n,config::active_name}};
   trx.actions.push_back(act);

   set_transaction_headers(trx);
   trx.sign(get_private_key( "enterimport"_n, "active" ), control->get_chain_id());
   BOOST_CHECK_THROW(push_transaction(trx), abort_called);
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( entry_db, validating_tester ) try {
   SKIP_TEST
   create_accounts( {"entrydb"_n} );
   produce_block();

   set_code("entrydb"_n, entry_db_wast);

   signed_transaction trx;
   action act;
   act.account = "entrydb"_n;
   act.name = ""_n;
   act.authorization = vector<permission_level>{{"entrydb"_n,config::active_name}};
   trx.actions.push_back(act);

   set_transaction_headers(trx);
   trx.sign(get_private_key( "entrydb"_n, "active" ), control->get_chain_id());
   push_transaction(trx);
} FC_LOG_AND_RETHROW()

/**
 * Ensure we can load a wasm w/o memory
 */
BOOST_FIXTURE_TEST_CASE( simple_no_memory_check, validating_tester ) try {
   produce_blocks(2);

   create_accounts( {"nomem"_n} );
   produce_block();

   set_code("nomem"_n, simple_no_memory_wast);
   produce_blocks(1);

   //the apply func of simple_no_memory_wast tries to call a native func with linear memory pointer
   signed_transaction trx;
   action act;
   act.account = "nomem"_n;
   act.name = ""_n;
   act.authorization = vector<permission_level>{{"nomem"_n,config::active_name}};
   trx.actions.push_back(act);
   trx.expiration = fc::time_point_sec{control->head_block_time()};
   set_transaction_headers(trx);
   trx.sign(get_private_key( "nomem"_n, "active" ), control->get_chain_id());
   BOOST_CHECK_THROW(push_transaction( trx ), wasm_execution_error);
} FC_LOG_AND_RETHROW()

//Make sure globals are all reset to their inital values
BOOST_FIXTURE_TEST_CASE( check_global_reset, validating_tester ) try {
   produce_blocks(2);

   create_accounts( {"globalreset"_n} );
   produce_block();

   set_code("globalreset"_n, mutable_global_wast);
   produce_blocks(1);

   signed_transaction trx;
   {
   action act;
   act.account = "globalreset"_n;
   act.name = name(0ULL);
   act.authorization = vector<permission_level>{{"globalreset"_n,config::active_name}};
   trx.actions.push_back(act);
   }
   {
   action act;
   act.account = "globalreset"_n;
   act.name = name(1ULL);
   act.authorization = vector<permission_level>{{"globalreset"_n,config::active_name}};
   trx.actions.push_back(act);
   }

   set_transaction_headers(trx);
   trx.sign(get_private_key( "globalreset"_n, "active" ), control->get_chain_id());
   push_transaction(trx);
   produce_blocks(1);
   BOOST_REQUIRE_EQUAL(true, chain_has_transaction(trx.id()));
   const auto& receipt = get_transaction_receipt(trx.id());
   BOOST_CHECK_EQUAL(transaction_receipt::executed, receipt.status);
} FC_LOG_AND_RETHROW()

//Make sure we can create a wasm with maximum pages, but not grow it any
BOOST_DATA_TEST_CASE( big_memory, bdata::make({setup_policy::old_wasm_parser, setup_policy::full}), policy ) try {
   validating_tester t(flat_set<account_name>{}, {}, policy);
   if(policy != setup_policy::full)
      t.preactivate_builtin_protocol_features({builtin_protocol_feature_t::configurable_wasm_limits});

   t.produce_blocks(2);


   t.create_accounts( {"bigmem"_n} );
   t.produce_block();

   string biggest_memory_wast_f = fc::format_string(biggest_memory_wast, fc::mutable_variant_object(
                                          "MAX_WASM_PAGES", sysio::chain::wasm_constraints::maximum_linear_memory/(64*1024)));

   t.set_code("bigmem"_n, biggest_memory_wast_f.c_str());
   t.produce_blocks(1);

   signed_transaction trx;
   action act;
   act.account = "bigmem"_n;
   act.name = ""_n;
   act.authorization = vector<permission_level>{{"bigmem"_n,config::active_name}};
   trx.actions.push_back(act);

   t.set_transaction_headers(trx);
   trx.sign(validating_tester::get_private_key( "bigmem"_n, "active" ), t.control->get_chain_id());
   //but should not be able to grow beyond largest page
   t.push_transaction(trx);

   t.produce_blocks(1);

   string too_big_memory_wast_f = fc::format_string(too_big_memory_wast, fc::mutable_variant_object(
                                          "MAX_WASM_PAGES_PLUS_ONE", sysio::chain::wasm_constraints::maximum_linear_memory/(64*1024)+1));
   BOOST_CHECK_THROW(t.set_code("bigmem"_n, too_big_memory_wast_f.c_str()), sysio::chain::wasm_exception);

} FC_LOG_AND_RETHROW()

BOOST_DATA_TEST_CASE( table_init_tests, bdata::make({setup_policy::old_wasm_parser, setup_policy::full}), policy ) try {
   validating_tester t(flat_set<account_name>{}, {}, policy);
   if(policy != setup_policy::full)
      t.preactivate_builtin_protocol_features({builtin_protocol_feature_t::configurable_wasm_limits});
   t.produce_blocks(2);

   t.create_accounts( {"tableinit"_n} );
   t.produce_block();

   t.set_code("tableinit"_n, valid_sparse_table);
   t.produce_blocks(1);

   BOOST_CHECK_THROW(t.set_code("tableinit"_n, too_big_table), sysio::chain::wasm_exception);

} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( table_init_oob, validating_tester ) try {
   create_accounts( {"tableinitoob"_n} );
   produce_block();

   signed_transaction trx;
   trx.actions.emplace_back(vector<permission_level>{{"tableinitoob"_n,config::active_name}}, "tableinitoob"_n, ""_n, bytes{});
   trx.actions[0].authorization = vector<permission_level>{{"tableinitoob"_n,config::active_name}};

    auto pushit_and_expect_fail = [&]() {
      produce_block();
      trx.signatures.clear();
      set_transaction_headers(trx);
      trx.sign(get_private_key("tableinitoob"_n, "active"), control->get_chain_id());

      //the unspecified_exception_code comes from WAVM, which manages to throw a WAVM specific exception
      // up to where exec_one captures it and doesn't understand it
      BOOST_CHECK_THROW(push_transaction(trx), sysio::chain::wasm_exception);
   };

   set_code("tableinitoob"_n, table_init_oob_wast);
   produce_block();

   pushit_and_expect_fail();
   //make sure doing it again didn't lodge something funky in to a cache
   pushit_and_expect_fail();

   set_code("tableinitoob"_n, table_init_oob_smaller_wast);
   produce_block();
   pushit_and_expect_fail();
   pushit_and_expect_fail();

   //an elem w/o a table is a setcode fail though
   BOOST_CHECK_THROW(set_code("tableinitoob"_n, table_init_oob_no_table_wast), sysio::chain::wasm_exception);

   set_code("tableinitoob"_n, table_init_oob_empty_wast);
   produce_block();
   pushit_and_expect_fail();
   pushit_and_expect_fail();

} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( memory_init_border, validating_tester ) try {
   produce_blocks(2);

   create_accounts( {"memoryborder"_n} );
   produce_block();

   set_code("memoryborder"_n, memory_init_borderline);
   produce_blocks(1);

   BOOST_CHECK_THROW(set_code("memoryborder"_n, memory_init_toolong), sysio::chain::wasm_exception);
   BOOST_CHECK_THROW(set_code("memoryborder"_n, memory_init_negative), sysio::chain::wasm_exception);

} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( imports, validating_tester ) try {
   try {
      produce_blocks(2);

      create_accounts( {"imports"_n} );
      produce_block();

      //this will fail to link but that's okay; mainly looking to make sure that the constraint
      // system doesn't choke when memories and tables exist only as imports
      BOOST_CHECK_THROW(set_code("imports"_n, memory_table_import), fc::exception);
   } catch ( const fc::exception& e ) {

        edump((e.to_detail_string()));
        throw;
   }

} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( nested_limit_test, validating_tester ) try {
   produce_blocks(2);

   create_accounts( {"nested"_n} );
   produce_block();

   // nested loops
   {
      std::stringstream ss;
      ss << "(module (export \"apply\" (func $apply)) (func $apply (param $0 i64) (param $1 i64) (param $2 i64)";
      for(unsigned int i = 0; i < 1023; ++i)
         ss << "(loop (drop (i32.const " <<  i << "))";
      for(unsigned int i = 0; i < 1023; ++i)
         ss << ")";
      ss << "))";
      set_code("nested"_n, ss.str().c_str());
   }
   {
      std::stringstream ss;
      ss << "(module (export \"apply\" (func $apply)) (func $apply (param $0 i64) (param $1 i64) (param $2 i64)";
      for(unsigned int i = 0; i < 1024; ++i)
         ss << "(loop (drop (i32.const " <<  i << "))";
      for(unsigned int i = 0; i < 1024; ++i)
         ss << ")";
      ss << "))";
      BOOST_CHECK_THROW(set_code("nested"_n, ss.str().c_str()), sysio::chain::wasm_exception);
   }

   // nested blocks
   {
      std::stringstream ss;
      ss << "(module (export \"apply\" (func $apply)) (func $apply (param $0 i64) (param $1 i64) (param $2 i64)";
      for(unsigned int i = 0; i < 1023; ++i)
         ss << "(block (drop (i32.const " <<  i << "))";
      for(unsigned int i = 0; i < 1023; ++i)
         ss << ")";
      ss << "))";
      set_code("nested"_n, ss.str().c_str());
   }
   {
      std::stringstream ss;
      ss << "(module (export \"apply\" (func $apply)) (func $apply (param $0 i64) (param $1 i64) (param $2 i64)";
      for(unsigned int i = 0; i < 1024; ++i)
         ss << "(block (drop (i32.const " <<  i << "))";
      for(unsigned int i = 0; i < 1024; ++i)
         ss << ")";
      ss << "))";
      BOOST_CHECK_THROW(set_code("nested"_n, ss.str().c_str()), sysio::chain::wasm_exception);
   }
   // nested ifs
   {
      std::stringstream ss;
      ss << "(module (export \"apply\" (func $apply)) (func $apply (param $0 i64) (param $1 i64) (param $2 i64)";
      for(unsigned int i = 0; i < 1023; ++i)
         ss << "(if (i32.wrap/i64 (get_local $0)) (then (drop (i32.const " <<  i << "))";
      for(unsigned int i = 0; i < 1023; ++i)
         ss << "))";
      ss << "))";
      set_code("nested"_n, ss.str().c_str());
   }
   {
      std::stringstream ss;
      ss << "(module (export \"apply\" (func $apply)) (func $apply (param $0 i64) (param $1 i64) (param $2 i64)";
      for(unsigned int i = 0; i < 1024; ++i)
         ss << "(if (i32.wrap/i64 (get_local $0)) (then (drop (i32.const " <<  i << "))";
      for(unsigned int i = 0; i < 1024; ++i)
         ss << "))";
      ss << "))";
      BOOST_CHECK_THROW(set_code("nested"_n, ss.str().c_str()), sysio::chain::wasm_exception);
   }
   // mixed nested
   {
      std::stringstream ss;
      ss << "(module (export \"apply\" (func $apply)) (func $apply (param $0 i64) (param $1 i64) (param $2 i64)";
      for(unsigned int i = 0; i < 223; ++i)
         ss << "(if (i32.wrap/i64 (get_local $0)) (then (drop (i32.const " <<  i << "))";
      for(unsigned int i = 0; i < 400; ++i)
         ss << "(block (drop (i32.const " <<  i << "))";
      for(unsigned int i = 0; i < 400; ++i)
         ss << "(loop (drop (i32.const " <<  i << "))";
      for(unsigned int i = 0; i < 800; ++i)
         ss << ")";
      for(unsigned int i = 0; i < 223; ++i)
         ss << "))";
      ss << "))";
      set_code("nested"_n, ss.str().c_str());
   }
   {
      std::stringstream ss;
      ss << "(module (export \"apply\" (func $apply)) (func $apply (param $0 i64) (param $1 i64) (param $2 i64)";
      for(unsigned int i = 0; i < 224; ++i)
         ss << "(if (i32.wrap/i64 (get_local $0)) (then (drop (i32.const " <<  i << "))";
      for(unsigned int i = 0; i < 400; ++i)
         ss << "(block (drop (i32.const " <<  i << "))";
      for(unsigned int i = 0; i < 400; ++i)
         ss << "(loop (drop (i32.const " <<  i << "))";
      for(unsigned int i = 0; i < 800; ++i)
         ss << ")";
      for(unsigned int i = 0; i < 224; ++i)
         ss << "))";
      ss << "))";
      BOOST_CHECK_THROW(set_code("nested"_n, ss.str().c_str()), sysio::chain::wasm_exception);
   }

} FC_LOG_AND_RETHROW()


BOOST_DATA_TEST_CASE( lotso_globals, bdata::make({setup_policy::old_wasm_parser, setup_policy::full}), policy ) try {
   validating_tester t(flat_set<account_name>{}, {}, policy);
   if(policy != setup_policy::full)
      t.preactivate_builtin_protocol_features({builtin_protocol_feature_t::configurable_wasm_limits});

   t.produce_blocks(2);

   t.create_accounts( {"globals"_n} );
   t.produce_block();

   std::stringstream ss;
   ss << "(module (export \"apply\" (func $apply)) (func $apply (param $0 i64) (param $1 i64) (param $2 i64))";
   for(unsigned int i = 0; i < 85; ++i)
      ss << "(global $g" << i << " (mut i32) (i32.const 0))" << "(global $g" << i+100 << " (mut i64) (i64.const 0))";
   //that gives us 1020 bytes of mutable globals
   //add a few immutable ones for good measure
   for(unsigned int i = 0; i < 10; ++i)
      ss << "(global $g" << i+200 << " i32 (i32.const 0))";

   t.set_code("globals"_n, (ss.str() + ")").c_str());
   //1024 should pass
   t.set_code("globals"_n, (ss.str() + "(global $z (mut i32) (i32.const -12)))").c_str());
   //1028 should fail
   BOOST_CHECK_THROW(t.set_code("globals"_n, (ss.str() + "(global $z (mut i64) (i64.const -12)))").c_str()), sysio::chain::wasm_exception);
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE( offset_check_old ) try {
   validating_tester t(flat_set<account_name>{}, {}, setup_policy::old_wasm_parser);
   t.produce_blocks(2);

   t.create_accounts( {"offsets"_n} );
   t.produce_block();

   vector<string> loadops = {
      "i32.load", "i64.load", "f32.load", "f64.load", "i32.load8_s", "i32.load8_u",
      "i32.load16_s", "i32.load16_u", "i64.load8_s", "i64.load8_u", "i64.load16_s",
      "i64.load16_u", "i64.load32_s", "i64.load32_u"
   };
   vector<vector<string>> storeops = {
      {"i32.store",   "i32"},
      {"i64.store",   "i64"},
      {"f32.store",   "f32"},
      {"f64.store",   "f64"},
      {"i32.store8",  "i32"},
      {"i32.store16", "i32"},
      {"i64.store8",  "i64"},
      {"i64.store16", "i64"},
      {"i64.store32", "i64"},
   };

   for(const string& s : loadops) {
      std::stringstream ss;
      ss << "(module (memory $0 " << sysio::chain::wasm_constraints::maximum_linear_memory/(64*1024) << ") (func $apply (param $0 i64) (param $1 i64) (param $2 i64)";
      ss << "(drop (" << s << " offset=" << sysio::chain::wasm_constraints::maximum_linear_memory-2 << " (i32.const 0)))";
      ss << ") (export \"apply\" (func $apply)) )";

      t.set_code("offsets"_n, ss.str().c_str());
      t.produce_block();
   }
   for(const vector<string>& o : storeops) {
      std::stringstream ss;
      ss << "(module (memory $0 " << sysio::chain::wasm_constraints::maximum_linear_memory/(64*1024) << ") (func $apply (param $0 i64) (param $1 i64) (param $2 i64)";
      ss << "(" << o[0] << " offset=" << sysio::chain::wasm_constraints::maximum_linear_memory-2 << " (i32.const 0) (" << o[1] << ".const 0))";
      ss << ") (export \"apply\" (func $apply)) )";

      t.set_code("offsets"_n, ss.str().c_str());
      t.produce_block();
   }

   for(const string& s : loadops) {
      std::stringstream ss;
      ss << "(module (memory $0 " << sysio::chain::wasm_constraints::maximum_linear_memory/(64*1024) << ") (func $apply (param $0 i64) (param $1 i64) (param $2 i64)";
      ss << "(drop (" << s << " offset=" << sysio::chain::wasm_constraints::maximum_linear_memory+4 << " (i32.const 0)))";
      ss << ") (export \"apply\" (func $apply)) )";

      BOOST_CHECK_THROW(t.set_code("offsets"_n, ss.str().c_str()), sysio::chain::wasm_exception);
      t.produce_block();
   }
   for(const vector<string>& o : storeops) {
      std::stringstream ss;
      ss << "(module (memory $0 " << sysio::chain::wasm_constraints::maximum_linear_memory/(64*1024) << ") (func $apply (param $0 i64) (param $1 i64) (param $2 i64)";
      ss << "(" << o[0] << " offset=" << sysio::chain::wasm_constraints::maximum_linear_memory+4 << " (i32.const 0) (" << o[1] << ".const 0))";
      ss << ") (export \"apply\" (func $apply)) )";

      BOOST_CHECK_THROW(t.set_code("offsets"_n, ss.str().c_str()), sysio::chain::wasm_exception);
      t.produce_block();
   }

} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( offset_check, validating_tester ) try {
   produce_blocks(2);

   create_accounts( {"offsets"_n} );
   produce_block();

   vector<string> loadops = {
      "i32.load", "i64.load", "f32.load", "f64.load", "i32.load8_s", "i32.load8_u",
      "i32.load16_s", "i32.load16_u", "i64.load8_s", "i64.load8_u", "i64.load16_s",
      "i64.load16_u", "i64.load32_s", "i64.load32_u"
   };
   vector<vector<string>> storeops = {
      {"i32.store",   "i32"},
      {"i64.store",   "i64"},
      {"f32.store",   "f32"},
      {"f64.store",   "f64"},
      {"i32.store8",  "i32"},
      {"i32.store16", "i32"},
      {"i64.store8",  "i64"},
      {"i64.store16", "i64"},
      {"i64.store32", "i64"},
   };

   for(const string& s : loadops) {
      std::stringstream ss;
      ss << "(module (memory $0 " << sysio::chain::wasm_constraints::maximum_linear_memory/(64*1024) << ") (func $apply (param $0 i64) (param $1 i64) (param $2 i64)";
      ss << "(drop (" << s << " offset=" << 0xFFFFFFFFu << " (i32.const 0)))";
      ss << ") (export \"apply\" (func $apply)) )";

      set_code("offsets"_n, ss.str().c_str());
      produce_block();
   }
   for(const vector<string>& o : storeops) {
      std::stringstream ss;
      ss << "(module (memory $0 " << sysio::chain::wasm_constraints::maximum_linear_memory/(64*1024) << ") (func $apply (param $0 i64) (param $1 i64) (param $2 i64)";
      ss << "(" << o[0] << " offset=" << 0xFFFFFFFFu << " (i32.const 0) (" << o[1] << ".const 0))";
      ss << ") (export \"apply\" (func $apply)) )";

      set_code("offsets"_n, ss.str().c_str());
      produce_block();
   }

} FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE(noop, validating_tester) try {
   produce_blocks(2);
   create_accounts( {"noop"_n, "alice"_n} );
   produce_block();

   set_code("noop"_n, test_contracts::noop_wasm());

   set_abi("noop"_n, test_contracts::noop_abi());
   const auto& accnt  = control->db().get<account_object,by_name>("noop"_n);
   abi_def abi;
   BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(accnt.abi, abi), true);
   abi_serializer abi_ser(std::move(abi), abi_serializer::create_yield_function( abi_serializer_max_time ));

   {
      produce_blocks(5);
      signed_transaction trx;
      action act;
      act.account = "noop"_n;
      act.name = "anyaction"_n;
      act.authorization = vector<permission_level>{{"noop"_n, config::active_name}};

      act.data = abi_ser.variant_to_binary("anyaction", mutable_variant_object()
                                           ("from", "noop")
                                           ("type", "some type")
                                           ("data", "some data goes here"),
                                           abi_serializer::create_yield_function( abi_serializer_max_time )
                                           );

      trx.actions.emplace_back(std::move(act));

      set_transaction_headers(trx);
      trx.sign(get_private_key("noop"_n, "active"), control->get_chain_id());
      push_transaction(trx);
      produce_block();

      BOOST_REQUIRE_EQUAL(true, chain_has_transaction(trx.id()));
   }

   {
      produce_blocks(5);
      signed_transaction trx;
      action act;
      act.account = "noop"_n;
      act.name = "anyaction"_n;
      act.authorization = vector<permission_level>{{"alice"_n, config::active_name}};

      act.data = abi_ser.variant_to_binary("anyaction", mutable_variant_object()
                                           ("from", "alice")
                                           ("type", "some type")
                                           ("data", "some data goes here"),
                                           abi_serializer::create_yield_function( abi_serializer_max_time )
                                           );

      trx.actions.emplace_back(std::move(act));

      set_transaction_headers(trx);
      trx.sign(get_private_key("alice"_n, "active"), control->get_chain_id());
      push_transaction(trx);
      produce_block();

      BOOST_REQUIRE_EQUAL(true, chain_has_transaction(trx.id()));
   }

 } FC_LOG_AND_RETHROW()

// abi_serializer::to_variant failed because sysio_system_abi modified via set_abi.
// This test also verifies that chain_initializer::eos_contract_abi() does not conflict
// with sysio_system_abi as they are not allowed to contain duplicates.
BOOST_FIXTURE_TEST_CASE(sysio_abi, validating_tester) try {
   produce_blocks(2);

   const auto& accnt  = control->db().get<account_object,by_name>(config::system_account_name);
   abi_def abi;
   BOOST_REQUIRE_EQUAL(abi_serializer::to_abi(accnt.abi, abi), true);
   abi_serializer abi_ser(std::move(abi), abi_serializer::create_yield_function( abi_serializer_max_time ));

   signed_transaction trx;
   name a = "alice"_n;
   authority owner_auth =  authority( get_public_key( a, "owner" ) );
   trx.actions.emplace_back( vector<permission_level>{{config::system_account_name,config::active_name}},
                             newaccount{
                                   .creator  = config::system_account_name,
                                   .name     = a,
                                   .owner    = owner_auth,
                                   .active   = authority( get_public_key( a, "active" ) )
                             });
   set_transaction_headers(trx);
   trx.sign( get_private_key( config::system_account_name, "active" ), control->get_chain_id()  );
   auto result = push_transaction( trx );

   fc::variant pretty_output;
   // verify to_variant works on eos native contract type: newaccount
   // see abi_serializer::to_abi()
   abi_serializer::to_variant(*result, pretty_output, get_resolver(), abi_serializer::create_yield_function( abi_serializer_max_time ));
   BOOST_TEST(fc::json::to_string(pretty_output, fc::time_point::now() + abi_serializer_max_time).find("newaccount") != std::string::npos);

   pretty_output.clear();
   abi_serializer::to_variant(*result, pretty_output, get_resolver(), abi_serializer_max_time);
   BOOST_TEST(fc::json::to_string(pretty_output, fc::time_point::now() + abi_serializer_max_time).find("newaccount") != std::string::npos);

   produce_block();
} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE( check_big_deserialization ) try {
   validating_tester t(flat_set<account_name>{}, {}, setup_policy::old_wasm_parser);
   t.produce_blocks(2);
   t.create_accounts( {"cbd"_n} );
   t.produce_block();

   std::stringstream ss;
   ss << "(module ";
   ss << "(export \"apply\" (func $apply))";
   ss << "  (func $apply  (param $0 i64)(param $1 i64)(param $2 i64))";
   for(unsigned int i = 0; i < wasm_constraints::maximum_section_elements-2; i++)
      ss << "  (func " << "$AA_" << i << ")";
   ss << ")";

   t.set_code("cbd"_n, ss.str().c_str());
   t.produce_blocks(1);

   t.produce_blocks(1);

   ss.str("");
   ss << "(module ";
   ss << "(export \"apply\" (func $apply))";
   ss << "  (func $apply  (param $0 i64)(param $1 i64)(param $2 i64))";
   for(unsigned int i = 0; i < wasm_constraints::maximum_section_elements; i++)
      ss << "  (func " << "$AA_" << i << ")";
   ss << ")";

   BOOST_CHECK_THROW(t.set_code("cbd"_n, ss.str().c_str()), wasm_serialization_error);
   t.produce_blocks(1);

   ss.str("");
   ss << "(module ";
   ss << "(export \"apply\" (func $apply))";
   ss << "  (func $apply  (param $0 i64)(param $1 i64)(param $2 i64))";
   ss << "  (func $aa ";
   for(unsigned int i = 0; i < wasm_constraints::maximum_code_size; i++)
      ss << "  (drop (i32.const 3))";
   ss << "))";

   BOOST_CHECK_THROW(t.set_code("cbd"_n, ss.str().c_str()), fc::assert_exception); // this is caught first by MAX_SIZE_OF_ARRAYS check
   t.produce_blocks(1);

   ss.str("");
   ss << "(module ";
   ss << "(memory $0 1)";
   ss << "(data (i32.const 20) \"";
   for(unsigned int i = 0; i < wasm_constraints::maximum_func_local_bytes-1; i++)
      ss << 'a';
   ss << "\")";
   ss << "(export \"apply\" (func $apply))";
   ss << "  (func $apply  (param $0 i64)(param $1 i64)(param $2 i64))";
   ss << "  (func $aa ";
      ss << "  (drop (i32.const 3))";
   ss << "))";

   t.set_code("cbd"_n, ss.str().c_str());
   t.produce_blocks(1);

   ss.str("");
   ss << "(module ";
   ss << "(memory $0 1)";
   ss << "(data (i32.const 20) \"";
   for(unsigned int i = 0; i < wasm_constraints::maximum_func_local_bytes; i++)
      ss << 'a';
   ss << "\")";
   ss << "(export \"apply\" (func $apply))";
   ss << "  (func $apply  (param $0 i64)(param $1 i64)(param $2 i64))";
   ss << "  (func $aa ";
      ss << "  (drop (i32.const 3))";
   ss << "))";

   BOOST_CHECK_THROW(t.set_code("cbd"_n, ss.str().c_str()), wasm_serialization_error);
   t.produce_blocks(1);

} FC_LOG_AND_RETHROW()


BOOST_FIXTURE_TEST_CASE( check_table_maximum, validating_tester ) try {
   produce_blocks(2);
   create_accounts( {"tbl"_n} );
   produce_block();

   set_code("tbl"_n, table_checker_wast);
   produce_blocks(1);
   {
   signed_transaction trx;
   action act;
   act.name = name(555ULL<<32 | 0ULL);       //top 32 is what we assert against, bottom 32 is indirect call index
   act.account = "tbl"_n;
   act.authorization = vector<permission_level>{{"tbl"_n,config::active_name}};
   trx.actions.push_back(act);
      set_transaction_headers(trx);
   trx.sign(get_private_key( "tbl"_n, "active" ), control->get_chain_id());
   push_transaction(trx);
   }

   produce_blocks(1);

   {
   signed_transaction trx;
   action act;
   act.name = name(555ULL<<32 | 1022ULL);       //top 32 is what we assert against, bottom 32 is indirect call index
   act.account = "tbl"_n;
   act.authorization = vector<permission_level>{{"tbl"_n,config::active_name}};
   trx.actions.push_back(act);
      set_transaction_headers(trx);
   trx.sign(get_private_key( "tbl"_n, "active" ), control->get_chain_id());
   push_transaction(trx);
   }

   produce_blocks(1);

   {
   signed_transaction trx;
   action act;
   act.name = name(7777ULL<<32 | 1023ULL);       //top 32 is what we assert against, bottom 32 is indirect call index
   act.account = "tbl"_n;
   act.authorization = vector<permission_level>{{"tbl"_n,config::active_name}};
   trx.actions.push_back(act);
      set_transaction_headers(trx);
   trx.sign(get_private_key( "tbl"_n, "active" ), control->get_chain_id());
   push_transaction(trx);
   }

   produce_blocks(1);

   {
   signed_transaction trx;
   action act;
   act.name = name(7778ULL<<32 | 1023ULL);       //top 32 is what we assert against, bottom 32 is indirect call index
   act.account = "tbl"_n;
   act.authorization = vector<permission_level>{{"tbl"_n,config::active_name}};
   trx.actions.push_back(act);
      set_transaction_headers(trx);
   trx.sign(get_private_key( "tbl"_n, "active" ), control->get_chain_id());

   //should fail, a check to make sure assert() in wasm is being evaluated correctly
   BOOST_CHECK_THROW(push_transaction(trx), sysio_assert_message_exception);
   }

   produce_blocks(1);

   {
   signed_transaction trx;
   action act;
   act.name = name(133ULL<<32 | 5ULL);       //top 32 is what we assert against, bottom 32 is indirect call index
   act.account = "tbl"_n;
   act.authorization = vector<permission_level>{{"tbl"_n,config::active_name}};
   trx.actions.push_back(act);
      set_transaction_headers(trx);
   trx.sign(get_private_key( "tbl"_n, "active" ), control->get_chain_id());

   //should fail, this element index (5) does not exist
   BOOST_CHECK_THROW(push_transaction(trx), sysio::chain::wasm_execution_error);
   }

   produce_blocks(1);

   {
   signed_transaction trx;
   action act;
   act.name = name(sysio::chain::wasm_constraints::maximum_table_elements+54334);
   act.account = "tbl"_n;
   act.authorization = vector<permission_level>{{"tbl"_n,config::active_name}};
   trx.actions.push_back(act);
      set_transaction_headers(trx);
   trx.sign(get_private_key( "tbl"_n, "active" ), control->get_chain_id());

   //should fail, this element index is out of range
   BOOST_CHECK_THROW(push_transaction(trx), sysio::chain::wasm_execution_error);
   }

   produce_blocks(1);

   //run a few tests with new, proper syntax, call_indirect
   set_code("tbl"_n, table_checker_proper_syntax_wast);
   produce_blocks(1);

   {
   signed_transaction trx;
   action act;
   act.name = name(555ULL<<32 | 1022ULL);       //top 32 is what we assert against, bottom 32 is indirect call index
   act.account = "tbl"_n;
   act.authorization = vector<permission_level>{{"tbl"_n,config::active_name}};
   trx.actions.push_back(act);
      set_transaction_headers(trx);
   trx.sign(get_private_key( "tbl"_n, "active" ), control->get_chain_id());
   push_transaction(trx);
   }

   produce_blocks(1);
   {
   signed_transaction trx;
   action act;
   act.name = name(7777ULL<<32 | 1023ULL);       //top 32 is what we assert against, bottom 32 is indirect call index
   act.account = "tbl"_n;
   act.authorization = vector<permission_level>{{"tbl"_n,config::active_name}};
   trx.actions.push_back(act);
   set_transaction_headers(trx);
   trx.sign(get_private_key( "tbl"_n, "active" ), control->get_chain_id());
   push_transaction(trx);
   }
   set_code("tbl"_n, table_checker_small_wast);
   produce_blocks(1);

   {
   signed_transaction trx;
   action act;
   act.name = name(888ULL);
   act.account = "tbl"_n;
   act.authorization = vector<permission_level>{{"tbl"_n,config::active_name}};
   trx.actions.push_back(act);
   set_transaction_headers(trx);
   trx.sign(get_private_key( "tbl"_n, "active" ), control->get_chain_id());

   //an element that is out of range and has no mmap access permission either (should be a trapped segv)
   BOOST_CHECK_EXCEPTION(push_transaction(trx), sysio::chain::wasm_execution_error, [](const sysio::chain::wasm_execution_error &e) {return true;});
   }
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( protected_globals, validating_tester ) try {
   produce_blocks(2);

   create_accounts( {"gob"_n} );
   produce_block();

   BOOST_CHECK_THROW(set_code("gob"_n, global_protection_none_get_wast), fc::exception);
   produce_blocks(1);

   BOOST_CHECK_THROW(set_code("gob"_n, global_protection_some_get_wast), fc::exception);
   produce_blocks(1);

   BOOST_CHECK_THROW(set_code("gob"_n, global_protection_none_set_wast), fc::exception);
   produce_blocks(1);

   BOOST_CHECK_THROW(set_code("gob"_n, global_protection_some_set_wast), fc::exception);
   produce_blocks(1);

   //sanity to make sure I got general binary construction okay
   set_code("gob"_n, global_protection_okay_get_wasm);
   produce_blocks(1);

   BOOST_CHECK_THROW(set_code("gob"_n, global_protection_none_get_wasm), fc::exception);
   produce_blocks(1);

   BOOST_CHECK_THROW(set_code("gob"_n, global_protection_some_get_wasm), fc::exception);
   produce_blocks(1);

   set_code("gob"_n, global_protection_okay_set_wasm);
   produce_blocks(1);

   BOOST_CHECK_THROW(set_code("gob"_n, global_protection_some_set_wasm), fc::exception);
   produce_blocks(1);
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( apply_export_and_signature, validating_tester ) try {
   produce_blocks(2);
   create_accounts( {"bbb"_n} );
   produce_block();

   BOOST_CHECK_THROW(set_code("bbb"_n, no_apply_wast), fc::exception);
   produce_blocks(1);

   BOOST_CHECK_THROW(set_code("bbb"_n, no_apply_2_wast), fc::exception);
   produce_blocks(1);

   BOOST_CHECK_THROW(set_code("bbb"_n, no_apply_3_wast), fc::exception);
   produce_blocks(1);

   BOOST_CHECK_THROW(set_code("bbb"_n, apply_wrong_signature_wast), fc::exception);
   produce_blocks(1);
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( trigger_serialization_errors, validating_tester) try {
   produce_blocks(2);
   const vector<uint8_t> proper_wasm = { 0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x0d, 0x02, 0x60, 0x03, 0x7f, 0x7f, 0x7f,
                                         0x00, 0x60, 0x03, 0x7e, 0x7e, 0x7e, 0x00, 0x02, 0x0e, 0x01, 0x03, 0x65, 0x6e, 0x76, 0x06, 0x73,
                                         0x68, 0x61, 0x32, 0x35, 0x36, 0x00, 0x00, 0x03, 0x02, 0x01, 0x01, 0x04, 0x04, 0x01, 0x70, 0x00,
                                         0x00, 0x05, 0x03, 0x01, 0x00, 0x20, 0x07, 0x09, 0x01, 0x05, 0x61, 0x70, 0x70, 0x6c, 0x79, 0x00,
                                         0x01, 0x0a, 0x0c, 0x01, 0x0a, 0x00, 0x41, 0x04, 0x41, 0x05, 0x41, 0x10, 0x10, 0x00, 0x0b, 0x0b,
                                         0x0b, 0x01, 0x00, 0x41, 0x04, 0x0b, 0x05, 0x68, 0x65, 0x6c, 0x6c, 0x6f };

   const vector<uint8_t> malformed_wasm = { 0x00, 0x61, 0x03, 0x0d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x0d, 0x02, 0x60, 0x03, 0x7f, 0x7f, 0x7f,
                                            0x00, 0x60, 0x03, 0x7e, 0x7e, 0x7e, 0x00, 0x02, 0x0e, 0x01, 0x03, 0x65, 0x6e, 0x76, 0x06, 0x73,
                                            0x68, 0x61, 0x32, 0x38, 0x36, 0x00, 0x00, 0x03, 0x03, 0x01, 0x01, 0x04, 0x04, 0x01, 0x70, 0x00,
                                            0x00, 0x05, 0x03, 0x01, 0x00, 0x20, 0x07, 0x09, 0x01, 0x05, 0x61, 0x70, 0x70, 0x6c, 0x79, 0x00,
                                            0x01, 0x0a, 0x0c, 0x01, 0x0a, 0x00, 0x41, 0x04, 0x41, 0x05, 0x41, 0x10, 0x10, 0x00, 0x0b, 0x0b,
                                            0x0b, 0x01, 0x00, 0x41, 0x04, 0x0b, 0x05, 0x68, 0x65, 0x6c, 0x6c, 0x6f };

   create_accounts( {"bbb"_n} );
   produce_block();

   set_code("bbb"_n, proper_wasm);
   BOOST_CHECK_THROW(set_code("bbb"_n, malformed_wasm), wasm_serialization_error);
   produce_blocks(1);
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( protect_injected, validating_tester ) try {
   produce_blocks(2);

   create_accounts( {"inj"_n} );
   produce_block();

   BOOST_CHECK_THROW(set_code("inj"_n, import_injected_wast), fc::exception);
   produce_blocks(1);
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( import_signature, validating_tester ) try {
   produce_blocks(2);

   create_accounts( {"imp"_n} );
   produce_block();

   BOOST_CHECK_THROW(set_code("imp"_n, import_wrong_signature_wast), wasm_exception);
   produce_blocks(1);
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( mem_growth_memset, validating_tester ) try {
   produce_blocks(2);

   create_accounts( {"grower"_n} );
   produce_block();

   action act;
   act.account = "grower"_n;
   act.name = ""_n;
   act.authorization = vector<permission_level>{{"grower"_n,config::active_name}};

   set_code("grower"_n, memory_growth_memset_store);
   {
      signed_transaction trx;
      trx.actions.push_back(act);
      set_transaction_headers(trx);
      trx.sign(get_private_key( "grower"_n, "active" ), control->get_chain_id());
      push_transaction(trx);
   }

   produce_blocks(1);
   set_code("grower"_n, memory_growth_memset_test);
   {
      signed_transaction trx;
      trx.actions.push_back(act);
      set_transaction_headers(trx);
      trx.sign(get_private_key( "grower"_n, "active" ), control->get_chain_id());
      push_transaction(trx);
   }
} FC_LOG_AND_RETHROW()

INCBIN(fuzz1, "fuzz1.wasm");
INCBIN(fuzz2, "fuzz2.wasm");
INCBIN(fuzz3, "fuzz3.wasm");
INCBIN(fuzz4, "fuzz4.wasm");
INCBIN(fuzz5, "fuzz5.wasm");
INCBIN(fuzz6, "fuzz6.wasm");
INCBIN(fuzz7, "fuzz7.wasm");
INCBIN(fuzz8, "fuzz8.wasm");
INCBIN(fuzz9, "fuzz9.wasm");
INCBIN(fuzz10, "fuzz10.wasm");
INCBIN(fuzz11, "fuzz11.wasm");
INCBIN(fuzz12, "fuzz12.wasm");
INCBIN(fuzz13, "fuzz13.wasm");
INCBIN(fuzz14, "fuzz14.wasm");
INCBIN(fuzz15, "fuzz15.wasm");
//INCBIN(fuzz13, "fuzz13.wasm");
INCBIN(big_allocation, "big_allocation.wasm");
INCBIN(crash_section_size_too_big, "crash_section_size_too_big.wasm");
INCBIN(leak_no_destructor, "leak_no_destructor.wasm");
INCBIN(leak_readExports, "leak_readExports.wasm");
INCBIN(leak_readFunctions, "leak_readFunctions.wasm");
INCBIN(leak_readFunctions_2, "leak_readFunctions_2.wasm");
INCBIN(leak_readFunctions_3, "leak_readFunctions_3.wasm");
INCBIN(leak_readGlobals, "leak_readGlobals.wasm");
INCBIN(leak_readImports, "leak_readImports.wasm");
INCBIN(leak_wasm_binary_cpp_L1249, "leak_wasm_binary_cpp_L1249.wasm");
INCBIN(readFunctions_slowness_out_of_memory, "readFunctions_slowness_out_of_memory.wasm");
INCBIN(locals_yc, "locals-yc.wasm");
INCBIN(locals_s, "locals-s.wasm");
INCBIN(slowwasm_localsets, "slowwasm_localsets.wasm");
INCBIN(getcode_deepindent, "getcode_deepindent.wasm");
INCBIN(indent_mismatch, "indent-mismatch.wasm");
INCBIN(deep_loops_ext_report, "deep_loops_ext_report.wasm");
INCBIN(80k_deep_loop_with_ret, "80k_deep_loop_with_ret.wasm");
INCBIN(80k_deep_loop_with_void, "80k_deep_loop_with_void.wasm");

BOOST_FIXTURE_TEST_CASE( fuzz, validating_tester ) try {
   produce_blocks(2);

   create_accounts( {"fuzzy"_n} );
   produce_block();

   {
      vector<uint8_t> wasm(gfuzz1Data, gfuzz1Data + gfuzz1Size);
      BOOST_CHECK_THROW(set_code("fuzzy"_n, wasm), fc::exception);
   }
   {
      vector<uint8_t> wasm(gfuzz2Data, gfuzz2Data + gfuzz2Size);
      BOOST_CHECK_THROW(set_code("fuzzy"_n, wasm), fc::exception);
   }
   {
      vector<uint8_t> wasm(gfuzz3Data, gfuzz3Data + gfuzz3Size);
      BOOST_CHECK_THROW(set_code("fuzzy"_n, wasm), fc::exception);
   }
   {
      vector<uint8_t> wasm(gfuzz4Data, gfuzz4Data + gfuzz4Size);
      BOOST_CHECK_THROW(set_code("fuzzy"_n, wasm), fc::exception);
   }
   {
      vector<uint8_t> wasm(gfuzz5Data, gfuzz5Data + gfuzz5Size);
      BOOST_CHECK_THROW(set_code("fuzzy"_n, wasm), fc::exception);
   }
   {
      vector<uint8_t> wasm(gfuzz6Data, gfuzz6Data + gfuzz6Size);
      BOOST_CHECK_THROW(set_code("fuzzy"_n, wasm), fc::exception);
   }
   {
      vector<uint8_t> wasm(gfuzz7Data, gfuzz7Data + gfuzz7Size);
      BOOST_CHECK_THROW(set_code("fuzzy"_n, wasm), fc::exception);
   }
   {
      vector<uint8_t> wasm(gfuzz8Data, gfuzz8Data + gfuzz8Size);
      BOOST_CHECK_THROW(set_code("fuzzy"_n, wasm), fc::exception);
   }
   {
      vector<uint8_t> wasm(gfuzz9Data, gfuzz9Data + gfuzz9Size);
      BOOST_CHECK_THROW(set_code("fuzzy"_n, wasm), fc::exception);
   }
   {
      vector<uint8_t> wasm(gfuzz10Data, gfuzz10Data + gfuzz10Size);
      BOOST_CHECK_THROW(set_code("fuzzy"_n, wasm), fc::exception);
   }
   {
      vector<uint8_t> wasm(gfuzz11Data, gfuzz11Data + gfuzz11Size);
      BOOST_CHECK_THROW(set_code("fuzzy"_n, wasm), fc::exception);
   }
   {
      vector<uint8_t> wasm(gfuzz12Data, gfuzz12Data + gfuzz12Size);
      BOOST_CHECK_THROW(set_code("fuzzy"_n, wasm), fc::exception);
   }
   {
      vector<uint8_t> wasm(gfuzz13Data, gfuzz13Data + gfuzz13Size);
      BOOST_CHECK_THROW(set_code("fuzzy"_n, wasm), fc::exception);
   }
   {
      vector<uint8_t> wasm(gfuzz14Data, gfuzz14Data + gfuzz14Size);
      BOOST_CHECK_THROW(set_code("fuzzy"_n, wasm), fc::exception);
   }
      {
      vector<uint8_t> wasm(gfuzz15Data, gfuzz15Data + gfuzz15Size);
      BOOST_CHECK_THROW(set_code("fuzzy"_n, wasm), fc::exception);
   }
   /*  TODO: update wasm to have apply(...) then call, claim is that this
    *  takes 1.6 seconds under wavm...
   {
      auto start = fc::time_point::now();
      vector<uint8_t> wasm(gfuzz13Data, gfuzz13Data + gfuzz13Size);
      set_code("fuzzy"_n, wasm);
      BOOST_CHECK_THROW(set_code("fuzzy"_n, wasm), fc::exception);
      auto end = fc::time_point::now();
      edump((end-start));
   }
   */

   {
      vector<uint8_t> wasm(gbig_allocationData, gbig_allocationData + gbig_allocationSize);
      BOOST_CHECK_THROW(set_code("fuzzy"_n, wasm), wasm_serialization_error);
   }
   {
      vector<uint8_t> wasm(gcrash_section_size_too_bigData, gcrash_section_size_too_bigData + gcrash_section_size_too_bigSize);
      BOOST_CHECK_THROW(set_code("fuzzy"_n, wasm), wasm_serialization_error);
   }
   {
      vector<uint8_t> wasm(gleak_no_destructorData, gleak_no_destructorData + gleak_no_destructorSize);
      BOOST_CHECK_THROW(set_code("fuzzy"_n, wasm), wasm_serialization_error);
   }
   {
      vector<uint8_t> wasm(gleak_readExportsData, gleak_readExportsData + gleak_readExportsSize);
      BOOST_CHECK_THROW(set_code("fuzzy"_n, wasm), wasm_serialization_error);
   }
   {
      vector<uint8_t> wasm(gleak_readFunctionsData, gleak_readFunctionsData + gleak_readFunctionsSize);
      BOOST_CHECK_THROW(set_code("fuzzy"_n, wasm), wasm_serialization_error);
   }
   {
      vector<uint8_t> wasm(gleak_readFunctions_2Data, gleak_readFunctions_2Data + gleak_readFunctions_2Size);
      BOOST_CHECK_THROW(set_code("fuzzy"_n, wasm), wasm_serialization_error);
   }
   {
      vector<uint8_t> wasm(gleak_readFunctions_3Data, gleak_readFunctions_3Data + gleak_readFunctions_3Size);
      BOOST_CHECK_THROW(set_code("fuzzy"_n, wasm), wasm_serialization_error);
   }
   {
      vector<uint8_t> wasm(gleak_readGlobalsData, gleak_readGlobalsData + gleak_readGlobalsSize);
      BOOST_CHECK_THROW(set_code("fuzzy"_n, wasm), wasm_serialization_error);
   }
   {
      vector<uint8_t> wasm(gleak_readImportsData, gleak_readImportsData + gleak_readImportsSize);
      BOOST_CHECK_THROW(set_code("fuzzy"_n, wasm), wasm_serialization_error);
   }
   {
      vector<uint8_t> wasm(gleak_wasm_binary_cpp_L1249Data, gleak_wasm_binary_cpp_L1249Data + gleak_wasm_binary_cpp_L1249Size);
      BOOST_CHECK_THROW(set_code("fuzzy"_n, wasm), wasm_serialization_error);
   }
   {
      vector<uint8_t> wasm(greadFunctions_slowness_out_of_memoryData, greadFunctions_slowness_out_of_memoryData + greadFunctions_slowness_out_of_memorySize);
      BOOST_CHECK_THROW(set_code("fuzzy"_n, wasm), wasm_serialization_error);
   }
   {
      vector<uint8_t> wasm(glocals_ycData, glocals_ycData + glocals_ycSize);
      BOOST_CHECK_THROW(set_code("fuzzy"_n, wasm), wasm_serialization_error);
   }
   {
      vector<uint8_t> wasm(glocals_sData, glocals_sData + glocals_sSize);
      BOOST_CHECK_THROW(set_code("fuzzy"_n, wasm), wasm_serialization_error);
   }
   {
      vector<uint8_t> wasm(gslowwasm_localsetsData, gslowwasm_localsetsData + gslowwasm_localsetsSize);
      BOOST_CHECK_THROW(set_code("fuzzy"_n, wasm), wasm_serialization_error);
   }
   {
      vector<uint8_t> wasm(gdeep_loops_ext_reportData, gdeep_loops_ext_reportData + gdeep_loops_ext_reportSize);
      BOOST_CHECK_THROW(set_code("fuzzy"_n, wasm), wasm_exception);
   }
   {
      vector<uint8_t> wasm(g80k_deep_loop_with_retData, g80k_deep_loop_with_retData + g80k_deep_loop_with_retSize);
      BOOST_CHECK_THROW(set_code("fuzzy"_n, wasm), wasm_exception);
   }
   {
      vector<uint8_t> wasm(g80k_deep_loop_with_voidData, g80k_deep_loop_with_voidData + g80k_deep_loop_with_voidSize);
      BOOST_CHECK_THROW(set_code("fuzzy"_n, wasm), wasm_exception);
   }
   {
      vector<uint8_t> wasm(ggetcode_deepindentData, ggetcode_deepindentData + ggetcode_deepindentSize);
      set_code( "fuzzy"_n, wasm );
   }
   {
      vector<uint8_t> wasm(gindent_mismatchData, gindent_mismatchData + gindent_mismatchSize);
      set_code( "fuzzy"_n, wasm );
   }

   produce_blocks(1);
} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( big_maligned_host_ptr, validating_tester ) try {
   produce_blocks(2);
   create_accounts( {"bigmaligned"_n} );
   produce_block();

   string large_maligned_host_ptr_wast_f = fc::format_string(large_maligned_host_ptr, fc::mutable_variant_object()
                                              ("MAX_WASM_PAGES", sysio::chain::wasm_constraints::maximum_linear_memory/(64*1024))
                                              ("MAX_NAME_ARRAY", (sysio::chain::wasm_constraints::maximum_linear_memory-1)/sizeof(chain::account_name)));

   set_code("bigmaligned"_n, large_maligned_host_ptr_wast_f.c_str());
   produce_blocks(1);

   signed_transaction trx;
   action act;
   act.account = "bigmaligned"_n;
   act.name = ""_n;
   act.authorization = vector<permission_level>{{"bigmaligned"_n,config::active_name}};
   trx.actions.push_back(act);
   set_transaction_headers(trx);
   trx.sign(get_private_key( "bigmaligned"_n, "active" ), control->get_chain_id());
   push_transaction(trx);
   produce_blocks(1);
} FC_LOG_AND_RETHROW()

BOOST_DATA_TEST_CASE( depth_tests, bdata::make({setup_policy::old_wasm_parser, setup_policy::full}), policy ) try {
   validating_tester t(flat_set<account_name>{}, {}, policy);
   if(policy != setup_policy::full)
      t.preactivate_builtin_protocol_features({builtin_protocol_feature_t::configurable_wasm_limits});

   t.produce_block();
   t.create_accounts( {"depth"_n} );
   t.produce_block();

   signed_transaction trx;
   trx.actions.emplace_back(vector<permission_level>{{"depth"_n,config::active_name}}, "depth"_n, ""_n, bytes{});
   trx.actions[0].authorization = vector<permission_level>{{"depth"_n,config::active_name}};

    auto pushit = [&]() {
      t.produce_block();
      trx.signatures.clear();
      t.set_transaction_headers(trx);
      trx.sign(validating_tester::get_private_key("depth"_n, "active"), t.control->get_chain_id());
      t.push_transaction(trx);
   };

   //strictly wasm recursion to maximum_call_depth & maximum_call_depth+1
   string wasm_depth_okay = fc::format_string(depth_assert_wasm, fc::mutable_variant_object()
                                              ("MAX_DEPTH", sysio::chain::wasm_constraints::maximum_call_depth));
   t.set_code("depth"_n, wasm_depth_okay.c_str());
   pushit();

   string wasm_depth_one_over = fc::format_string(depth_assert_wasm, fc::mutable_variant_object()
                                              ("MAX_DEPTH", sysio::chain::wasm_constraints::maximum_call_depth+1));
   t.set_code("depth"_n, wasm_depth_one_over.c_str());
   BOOST_CHECK_THROW(pushit(), wasm_execution_error);

   //wasm recursion but call an intrinsic as the last function instead
   string intrinsic_depth_okay = fc::format_string(depth_assert_intrinsic, fc::mutable_variant_object()
                                              ("MAX_DEPTH", sysio::chain::wasm_constraints::maximum_call_depth));
   t.set_code("depth"_n, intrinsic_depth_okay.c_str());
   pushit();

   string intrinsic_depth_one_over = fc::format_string(depth_assert_intrinsic, fc::mutable_variant_object()
                                              ("MAX_DEPTH", sysio::chain::wasm_constraints::maximum_call_depth+1));
   t.set_code("depth"_n, intrinsic_depth_one_over.c_str());
   BOOST_CHECK_THROW(pushit(), wasm_execution_error);

   //add a float operation in the mix to ensure any injected softfloat call doesn't count against limit
   string wasm_float_depth_okay = fc::format_string(depth_assert_wasm_float, fc::mutable_variant_object()
                                              ("MAX_DEPTH", sysio::chain::wasm_constraints::maximum_call_depth));
   t.set_code("depth"_n, wasm_float_depth_okay.c_str());
   pushit();

   string wasm_float_depth_one_over = fc::format_string(depth_assert_wasm_float, fc::mutable_variant_object()
                                              ("MAX_DEPTH", sysio::chain::wasm_constraints::maximum_call_depth+1));
   t.set_code("depth"_n, wasm_float_depth_one_over.c_str());
   BOOST_CHECK_THROW(pushit(), wasm_execution_error);

} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE( varuint_memory_flags_tests ) try {
   validating_tester t(flat_set<account_name>{}, {}, setup_policy::preactivate_feature_and_new_bios);

   t.create_accounts( {"memflags"_n} );
   t.produce_block();

   t.set_code("memflags"_n, varuint_memory_flags);
   t.produce_block();

   {
   signed_transaction trx;
   action act;
   act.account = "memflags"_n;
   act.name = ""_n;
   act.authorization = vector<permission_level>{{"memflags"_n,config::active_name}};
   trx.actions.push_back(act);
   t.set_transaction_headers(trx);
   trx.sign(validating_tester::get_private_key( "memflags"_n, "active" ), t.control->get_chain_id());
   t.push_transaction(trx);
   t.produce_block();
   }

   // Activate new parser
   t.preactivate_builtin_protocol_features({builtin_protocol_feature_t::configurable_wasm_limits});
   t.produce_block();

   // We should still be able to execute the old code
   {
   signed_transaction trx;
   action act;
   act.account = "memflags"_n;
   act.name = ""_n;
   act.authorization = vector<permission_level>{{"memflags"_n,config::active_name}};
   trx.actions.push_back(act);
   t.set_transaction_headers(trx);
   trx.sign(validating_tester::get_private_key( "memflags"_n, "active" ), t.control->get_chain_id());
   t.push_transaction(trx);
   t.produce_block();
   }

   t.set_code("memflags"_n, std::vector<uint8_t>{});
   BOOST_REQUIRE_THROW(t.set_code("memflags"_n, varuint_memory_flags), wasm_exception);
} FC_LOG_AND_RETHROW()

static char reset_memory_fail1_wast[] = R"======(
(module
 (memory 2)
 (func (export "apply") (param i64 i64 i64))
)
)======";

// In a previous version of sys-vm, this would leave
// memory incorrectly accessible to the next action.
static char reset_memory_fail2_wast[] = R"======(
(module
 (memory 2)
 (table 1 anyfunc)
 (func $apply (export "apply") (param i64 i64 i64))
 (elem (i32.const 1) $apply)
)
)======";

static char reset_memory_fail3_wast[] = R"======(
(module
 (memory 1)
 (func (export "apply") (param i64 i64 i64)
  (i64.store (i32.const 65536) (i64.const 0))
 )
)
)======";

BOOST_FIXTURE_TEST_CASE( reset_memory_fail, validating_tester ) try {
   produce_block();
   create_accounts( {"usemem"_n, "resetmem"_n, "accessmem"_n} );
   produce_block();

   set_code("usemem"_n, reset_memory_fail1_wast);
   set_code("resetmem"_n, reset_memory_fail2_wast);
   set_code("accessmem"_n, reset_memory_fail3_wast);
   produce_block();

   auto pushit = [&](name acct) {
      signed_transaction trx;
      trx.actions.push_back({ { { acct, config::active_name } }, acct, ""_n, bytes() });
      set_transaction_headers(trx);
      trx.sign(get_private_key( acct, "active" ), control->get_chain_id());
      push_transaction(trx);
   };
   pushit("usemem"_n);
   BOOST_CHECK_THROW(pushit("resetmem"_n), wasm_execution_error);
   BOOST_CHECK_THROW(pushit("accessmem"_n), wasm_execution_error);
   produce_block();
} FC_LOG_AND_RETHROW()

// TODO: Update to use sys-vm once merged
BOOST_AUTO_TEST_CASE( code_size )  try {
   using namespace IR;
   using namespace Serialization;
   std::vector<U8> code_start = {
      0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x07, 0x01, 0x60,
      0x03, 0x7e, 0x7e, 0x7e, 0x00, 0x03, 0x02, 0x01, 0x00, 0x07, 0x09, 0x01,
      0x05, 0x61, 0x70, 0x70, 0x6c, 0x79, 0x00, 0x00, 0x0a, 0x8b, 0x80, 0x80,
      0x0a, 0x01, 0x86, 0x80, 0x80, 0x0a, 0x00
   };

   std::vector<U8> code_end = { 0x0b };

   std::vector<U8> code_function_body;
   code_function_body.insert(code_function_body.end(), wasm_constraints::maximum_code_size + 4, 0x01);

   std::vector<U8> code;
   code.insert(code.end(), code_start.begin(), code_start.end());
   code.insert(code.end(), code_function_body.begin(), code_function_body.end());
   code.insert(code.end(), code_end.begin(), code_end.end());

   Module module;
   Serialization::MemoryInputStream stream((const U8*)code.data(), code.size());
   BOOST_CHECK_THROW(WASM::serialize(stream, module), FatalSerializationException);

} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE( billed_cpu_test ) try {
   SKIP_TEST

   fc::temp_directory tempdir;
   tester chain( tempdir, true );
   chain.execute_setup_policy( setup_policy::full );

   const resource_limits_manager& mgr = chain.control->get_resource_limits_manager();

   account_name acc = "asserter"_n;
   account_name user = "user"_n;
   chain.create_accounts( {acc, user} );
   chain.produce_block();

   auto create_trx = [&](auto trx_max_ms) {
      signed_transaction trx;
      trx.actions.emplace_back( vector<permission_level>{{acc, config::active_name}},
                                assertdef {1, "Should Not Assert!"} );
      static int num_secs = 1;
      chain.set_transaction_headers( trx, ++num_secs ); // num_secs provides nonce
      trx.max_cpu_usage_ms = trx_max_ms;
      trx.sign( chain.get_private_key( acc, "active" ), chain.control->get_chain_id() );
      auto ptrx = std::make_shared<packed_transaction>(trx);
      auto fut = transaction_metadata::start_recover_keys( ptrx, chain.control->get_thread_pool(), chain.control->get_chain_id(), fc::microseconds::maximum(), transaction_metadata::trx_type::input );
      return fut.get();
   };

   auto push_trx = [&]( const transaction_metadata_ptr& trx, fc::time_point deadline,
                     uint32_t billed_cpu_time_us, bool explicit_billed_cpu_time, uint32_t subjective_cpu_bill_us ) {
      auto r = chain.control->push_transaction( trx, deadline, fc::microseconds::maximum(), billed_cpu_time_us, explicit_billed_cpu_time, subjective_cpu_bill_us );
      if( r->except_ptr ) std::rethrow_exception( r->except_ptr );
      if( r->except ) throw *r->except;
      return r;
   };

   auto ptrx = create_trx(0);
   // no limits, just verifying trx works
   push_trx( ptrx, fc::time_point::maximum(), 0, false, 0 ); // non-explicit billing

   // setup account acc with large limits
   chain.push_action( config::system_account_name, "setalimits"_n, config::system_account_name, fc::mutable_variant_object()
         ("account", user)
         ("ram_bytes", -1)
         ("net_weight", 19'999'999)
         ("cpu_weight", 19'999'999)
   );
   chain.push_action( config::system_account_name, "setalimits"_n, config::system_account_name, fc::mutable_variant_object()
         ("account", acc)
         ("ram_bytes", -1)
         ("net_weight", 9'999)
         ("cpu_weight", 9'999)
   );

   chain.produce_block();

   auto max_cpu_time_us = chain.control->get_global_properties().configuration.max_transaction_cpu_usage;
   auto min_cpu_time_us = chain.control->get_global_properties().configuration.min_transaction_cpu_usage;

   auto cpu_limit = mgr.get_account_cpu_limit(acc).first; // huge limit ~17s

   ptrx = create_trx(0);
   BOOST_CHECK_LT( max_cpu_time_us, cpu_limit ); // max_cpu_time_us has to be less than cpu_limit to actually test max and not account
   // indicate explicit billing at transaction max, max_cpu_time_us has to be greater than account cpu time
   push_trx( ptrx, fc::time_point::maximum(), max_cpu_time_us, true, 0 );
   chain.produce_block();

   cpu_limit = mgr.get_account_cpu_limit(acc).first;

   // do not allow to bill greater than chain configured max, objective failure even with explicit billing for over max
   ptrx = create_trx(0);
   BOOST_CHECK_LT( max_cpu_time_us + 1, cpu_limit ); // max_cpu_time_us+1 has to be less than cpu_limit to actually test max and not account
   // indicate explicit billing at max + 1
   BOOST_CHECK_EXCEPTION( push_trx( ptrx, fc::time_point::maximum(), max_cpu_time_us + 1, true, 0 ), tx_cpu_usage_exceeded,
                          [](const tx_cpu_usage_exceeded& e){ fc_exception_message_starts_with starts("billed");
                                                              fc_exception_message_contains contains("reached on chain max_transaction_cpu_usage");
                                                              return starts(e) && contains(e); } );

   // allow to bill at trx configured max
   ptrx = create_trx(5); // set trx max at 5ms
   BOOST_CHECK_LT( 5 * 1000, cpu_limit ); // 5ms has to be less than cpu_limit to actually test trx max and not account
   // indicate explicit billing at max
   push_trx( ptrx, fc::time_point::maximum(), 5 * 1000, true, 0 );
   chain.produce_block();

   cpu_limit = mgr.get_account_cpu_limit(acc).first; // update after last trx

   // do not allow to bill greater than trx configured max, objective failure even with explicit billing for over max
   ptrx = create_trx(5); // set trx max at 5ms
   BOOST_CHECK_LT( 5 * 1000 + 1, cpu_limit ); // 5ms has to be less than cpu_limit to actually test trx max and not account
   // indicate explicit billing at max + 1
   BOOST_CHECK_EXCEPTION( push_trx( ptrx, fc::time_point::maximum(), 5 * 1000 + 1, true, 0 ), tx_cpu_usage_exceeded,
                          [](const tx_cpu_usage_exceeded& e){ fc_exception_message_starts_with starts("billed");
                                                              fc_exception_message_contains contains("reached trx specified max_cpu_usage_ms");
                                                              return starts(e) && contains(e); } );

   // bill at minimum
   ptrx = create_trx(0);
   // indicate explicit billing at transaction minimum
   push_trx( ptrx, fc::time_point::maximum(), min_cpu_time_us, true, 0 );
   chain.produce_block();

   // do not allow to bill less than minimum
   ptrx = create_trx(0);
   // indicate explicit billing at minimum-1, objective failure even with explicit billing for under min
   BOOST_CHECK_EXCEPTION( push_trx( ptrx, fc::time_point::maximum(), min_cpu_time_us - 1, true, 0 ), transaction_exception,
                          fc_exception_message_starts_with("cannot bill CPU time less than the minimum") );

   chain.push_action( config::system_account_name, "setalimits"_n, config::system_account_name, fc::mutable_variant_object()
         ("account", acc)
         ("ram_bytes", -1)
         ("net_weight", 75)
         ("cpu_weight", 75) // ~130ms
   );

   chain.produce_block();
   chain.produce_block( fc::days(1) ); // produce for one day to reset account cpu

   cpu_limit = mgr.get_account_cpu_limit_ex(acc).first.max;
   cpu_limit -= SYS_PERCENT( cpu_limit, 10 * config::percent_1 ); // transaction_context verifies within 10%, so subtract 10% out

   ptrx = create_trx(0);
   BOOST_CHECK_LT( cpu_limit, max_cpu_time_us );
   // indicate non-explicit billing at one less than our account cpu limit, will allow this trx to run, but only bills for actual use
   auto r = push_trx( ptrx, fc::time_point::maximum(), cpu_limit-1, false, 0 );
   BOOST_CHECK_LT( r->receipt->cpu_usage_us, cpu_limit-1 ); // verify not billed at provided bill amount when explicit_billed_cpu_time=false

   chain.produce_block();
   chain.produce_block( fc::days(1) ); // produce for one day to reset account cpu

   ptrx = create_trx(0);
   BOOST_CHECK_LT( cpu_limit+1, max_cpu_time_us ); // needs to be less or this just tests the same thing as max_cpu_time_us test above
   // indicate explicit billing at over our account cpu limit, not allowed
   cpu_limit = mgr.get_account_cpu_limit_ex(acc).first.max;
   BOOST_CHECK_EXCEPTION( push_trx( ptrx, fc::time_point::maximum(), cpu_limit+1, true, 0 ), tx_cpu_usage_exceeded,
                          [](const tx_cpu_usage_exceeded& e){ fc_exception_message_starts_with starts("billed");
                                                              fc_exception_message_contains contains("reached account cpu limit");
                                                              return starts(e) && contains(e); } );

   // leeway and subjective billing interaction tests
   auto leeway = fc::microseconds(config::default_subjective_cpu_leeway_us);
   chain.control->set_subjective_cpu_leeway(leeway);

   // Allow transaction with billed cpu less than 90% of (account cpu limit + leeway - subjective bill)
   chain.produce_block();
   chain.produce_block( fc::days(1) ); // produce for one day to reset account cpu
   ptrx = create_trx(0);
   uint32_t combined_cpu_limit = mgr.get_account_cpu_limit_ex(acc).first.max + leeway.count();
   uint32_t subjective_cpu_bill_us = leeway.count();
   uint32_t billed_cpu_time_us = SYS_PERCENT( (combined_cpu_limit - subjective_cpu_bill_us), 89 *config::percent_1 );
   push_trx( ptrx, fc::time_point::maximum(), billed_cpu_time_us, false, subjective_cpu_bill_us );

   // Allow transaction with billed cpu less than 90% of (account cpu limit + leeway) if subject bill is 0
   chain.control->set_subjective_cpu_leeway(leeway);
   chain.produce_block();
   chain.produce_block( fc::days(1) ); // produce for one day to reset account cpu
   ptrx = create_trx(0);
   combined_cpu_limit = mgr.get_account_cpu_limit_ex(acc).first.max + leeway.count();
   subjective_cpu_bill_us = 0;
   billed_cpu_time_us = SYS_PERCENT( combined_cpu_limit - subjective_cpu_bill_us, 89 *config::percent_1 );
   push_trx( ptrx, fc::time_point::maximum(), billed_cpu_time_us, false, subjective_cpu_bill_us );

   // Disallow transaction with billed cpu equal to 90% of (account cpu limit + leeway - subjective bill)
   chain.produce_block();
   chain.produce_block( fc::days(1) ); // produce for one day to reset account cpu
   ptrx = create_trx(0);
   cpu_limit = mgr.get_account_cpu_limit_ex(acc).first.max;
   combined_cpu_limit = cpu_limit + leeway.count();
   subjective_cpu_bill_us = cpu_limit;
   billed_cpu_time_us = SYS_PERCENT( combined_cpu_limit - subjective_cpu_bill_us, 90 * config::percent_1 );
   BOOST_CHECK_EXCEPTION(push_trx( ptrx, fc::time_point::maximum(), billed_cpu_time_us, false, subjective_cpu_bill_us ), tx_cpu_usage_exceeded,
                         [](const tx_cpu_usage_exceeded& e){ fc_exception_message_starts_with starts("estimated");
                                                             fc_exception_message_contains contains_reached("reached account cpu limit");
                                                             fc_exception_message_contains contains_subjective("with a subjective cpu of");
                                                             return starts(e) && contains_reached(e) && contains_subjective(e); } );

   // Disallow transaction with billed cpu greater 90% of (account cpu limit + leeway - subjective bill)
   subjective_cpu_bill_us = 0;
   billed_cpu_time_us = SYS_PERCENT( combined_cpu_limit - subjective_cpu_bill_us, 91 * config::percent_1 );
   BOOST_CHECK_EXCEPTION(push_trx( ptrx, fc::time_point::maximum(), billed_cpu_time_us, false, subjective_cpu_bill_us ), tx_cpu_usage_exceeded,
                         [](const tx_cpu_usage_exceeded& e){ fc_exception_message_starts_with starts("estimated");
                                                             fc_exception_message_contains contains_reached("reached account cpu limit");
                                                             fc_exception_message_contains contains_subjective("with a subjective cpu of");
                                                             return starts(e) && contains_reached(e) && !contains_subjective(e); } );

   // Test when cpu limit is 0
   chain.push_action( config::system_account_name, "setalimits"_n, config::system_account_name, fc::mutable_variant_object()
           ("account", acc)
           ("ram_bytes", -1)
           ("net_weight", 75)
           ("cpu_weight", 0)
   );

   chain.produce_block();
   chain.produce_block( fc::days(1) ); // produce for one day to reset account cpu

   // Allow transaction with billed cpu less than 90% of leeway subjective bill being 0 to run but fail it if no cpu is staked afterwards
   ptrx = create_trx(0);
   subjective_cpu_bill_us = 0;
   billed_cpu_time_us = SYS_PERCENT( leeway.count(), 89 *config::percent_1 );
   BOOST_CHECK_EXCEPTION(push_trx( ptrx, fc::time_point::maximum(), billed_cpu_time_us, false, subjective_cpu_bill_us ), tx_cpu_usage_exceeded,
                         [](const tx_cpu_usage_exceeded& e){ fc_exception_message_starts_with starts("billed");
                                                             fc_exception_message_contains contains("reached account cpu limit");
                                                             return starts(e) && contains(e); } );

} FC_LOG_AND_RETHROW()

/**
 * various tests with wasm & 0 pages worth of memory
 */
BOOST_FIXTURE_TEST_CASE( zero_memory_pages, validating_tester ) try {
   produce_blocks(2);

   create_accounts( {"zero"_n} );
   produce_block();

   signed_transaction trx;
   trx.actions.emplace_back(vector<permission_level>{{"zero"_n,config::active_name}}, "zero"_n, name(), bytes{});
   trx.actions[0].authorization = vector<permission_level>{{"zero"_n,config::active_name}};

   auto pushit = [&]() {
      produce_block();
      trx.signatures.clear();
      set_transaction_headers(trx);
      trx.sign(get_private_key("zero"_n, "active"), control->get_chain_id());
      push_transaction(trx);
   };

   //first, let's run another large memory contract just to prime the pump so to catch any
   //memory reinit faults.
   set_code("zero"_n, misaligned_ref_wast);
   pushit();

   //contract w/ 0 pages that does nothing
   set_code("zero"_n, zero_memory_do_nothing);
   pushit();

   //memory load with 0 pages of memory
   set_code("zero"_n, zero_memory_load);
   BOOST_CHECK_THROW(pushit(), wasm_execution_error);

   //do an intrinsic with 0 pages of memory
   set_code("zero"_n, zero_memory_intrinsic);
   BOOST_CHECK_THROW(pushit(), wasm_execution_error);

   //grow memory from 0 -> 1, should be able to access byte 0 now
   set_code("zero"_n, zero_memory_grow);
   pushit();

   //grow memory from 0 -> 1, should be unable to access byte 70K
   set_code("zero"_n, zero_memory_grow_hi);
   BOOST_CHECK_THROW(pushit(), wasm_execution_error);

} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( sysio_exit_in_start, validating_tester ) try {
   produce_blocks(2);
   create_accounts( {"startexit"_n} );
   produce_block();

   set_code("startexit"_n, exit_in_start_wast);
   produce_blocks(1);

   signed_transaction trx;
   action act;
   act.account = "startexit"_n;
   act.name = name();
   act.authorization = vector<permission_level>{{"startexit"_n,config::active_name}};
   trx.actions.push_back(act);
   set_transaction_headers(trx);
   trx.sign(get_private_key( "startexit"_n, "active" ), control->get_chain_id());
   push_transaction(trx);
   produce_blocks(1);
} FC_LOG_AND_RETHROW()

// memory.grow with a negative argument can shrink the available memory.
BOOST_FIXTURE_TEST_CASE( negative_memory_grow, validating_tester ) try {
   produce_blocks(2);


   create_accounts( {"negmemgrow"_n} );
   produce_block();

   set_code("negmemgrow"_n, negative_memory_grow_wast);
   produce_blocks(1);

   {
   signed_transaction trx;
   action act;
   act.account = "negmemgrow"_n;
   act.name = name();
   act.authorization = vector<permission_level>{{"negmemgrow"_n,config::active_name}};
   trx.actions.push_back(act);

   set_transaction_headers(trx);
   trx.sign(get_private_key( "negmemgrow"_n, "active" ), control->get_chain_id());
   push_transaction(trx);
   produce_blocks(1);
   }

   set_code("negmemgrow"_n, negative_memory_grow_trap_wast);
   produce_block();
   {
   signed_transaction trx;
   action act;
   act.account = "negmemgrow"_n;
   act.name = name();
   act.authorization = vector<permission_level>{{"negmemgrow"_n,config::active_name}};
   trx.actions.push_back(act);

   set_transaction_headers(trx);
   trx.sign(get_private_key( "negmemgrow"_n, "active" ), control->get_chain_id());
   BOOST_CHECK_THROW(push_transaction(trx), sysio::chain::wasm_execution_error);
   }

} FC_LOG_AND_RETHROW()

// This test is only applicable to Linux
#if defined(__linux__)
// Returns the number of memory mappings of the current process. Only works on Linux
static uint32_t get_num_memory_mappings() {
   std::string maps_file = "/proc/" + std::to_string(getpid()) + "/maps";
   std::ifstream maps_ifs(maps_file);
   if (!maps_ifs.is_open()) {
      return 0;
   }

   uint32_t num_mappings = 0;
   std::string line;
   while (std::getline(maps_ifs, line)) {
      num_mappings++;
   }
   return num_mappings;
}

BOOST_FIXTURE_TEST_CASE( memory_mapping_test, validating_tester ) try {
   static const std::string mem_map_wast_start = R"=====(
   (module
      (export "apply" (func $apply))
      (global i32 (i32.const
   )=====";
   static const std::string mem_map_wast_end = R"=====(
                               ))
      (func $apply (param i64) (param i64) (param i64)
      )
   )
   )=====";
   static const char* mem_map_abi = R"=====(
   {
      "version": "sysio::abi/1.2",
      "types": [],
      "structs": [{ "name": "dothedew", "base": "", "fields": [] }],
      "actions": [{ "name": "dothedew", "type": "dothedew", "ricardian_contract": ""}],
      "tables": [],
      "ricardian_clauses": []
   }
   )=====";

   produce_block();

   auto num_mappings_before = get_num_memory_mappings();
   BOOST_CHECK_GT(num_mappings_before, 0U); // must be able to get number of memory mappings

   // number of contracts to deploy
   constexpr uint32_t num_contracts = 5000;

   for (uint32_t i = 1; i < num_contracts; ++i) {
      std::stringstream ss;
      ss << "0x" << std::hex << i << "00000000";
      uint64_t name_value;
      ss >> name_value;
      auto acct = name(name_value);
      create_accounts({acct}, false, true, false);
      add_roa_policy(NODE_DADDY, acct, "0.0001 SYS", "0.0001 SYS", "0.0200 SYS", 0, 0);

      std::string contract_wast = mem_map_wast_start + " " + std::to_string(i) + mem_map_wast_end;
      set_code(acct, contract_wast.c_str());
      set_abi(acct, mem_map_abi);

      push_action(acct, "dothedew"_n, "sysio"_n, {});

      // do not put too many transactions in a single block
      if (i % 20 == 0 ) {
         produce_block();
      }
   }

   constexpr uint32_t margin_of_changes = 50;
   auto num_mappings_now = get_num_memory_mappings();
   if (num_mappings_now > num_mappings_before) {
      BOOST_CHECK_LT(num_mappings_now - num_mappings_before, margin_of_changes);
   }
} FC_LOG_AND_RETHROW()
#endif // defined(__linux__)

BOOST_FIXTURE_TEST_CASE(net_usage_tests, tester ) try {
   SKIP_TEST
   int count = 0;
   auto check = [&](int coderepeat, int max_net_usage)-> bool {
      account_name account{"f_tests"_n.to_uint64_t() + (count++) * 16};
      create_accounts({account});

      std::string code = R"=====(
   (module
   (import "env" "require_auth" (func $require_auth (param i64)))
   (import "env" "sysio_assert" (func $sysio_assert (param i32 i32)))
      (table 0 anyfunc)
      (memory $0 1)
      (export "apply" (func $apply))
      (func $i64_trunc_u_f64 (param $0 f64) (result i64) (i64.trunc_u/f64 (get_local $0)))
      (func $test (param $0 i64))
      (func $apply (param $0 i64)(param $1 i64)(param $2 i64)
      )=====";
      for (int i = 0; i < coderepeat; ++i) {
         code += "(call $test (call $i64_trunc_u_f64 (f64.const 1)))\n";
      }
      code += "))";
      produce_blocks(1);
      signed_transaction trx;
      auto wasm = ::sysio::chain::wast_to_wasm(code);
      trx.actions.emplace_back( vector<permission_level>{{account,config::active_name}},
                              setcode{
                                 .account    = account,
                                 .vmtype     = 0,
                                 .vmversion  = 0,
                                 .code       = bytes(wasm.begin(), wasm.end())
                              });
      set_transaction_headers(trx);
      if (max_net_usage) trx.max_net_usage_words = max_net_usage;
      trx.sign( get_private_key( account, "active" ), control->get_chain_id()  );
      try {
         packed_transaction ptrx(trx);
         push_transaction(ptrx);
         produce_blocks(1);
         return true;
      } catch (tx_net_usage_exceeded &) {
         return false;
      } catch (transaction_exception &) {
         return false;
      }
   };
   BOOST_REQUIRE_EQUAL(true, check(1024, 0)); // default behavior
   BOOST_REQUIRE_EQUAL(false, check(1024, 100)); // transaction max_net_usage too small
   BOOST_REQUIRE_EQUAL(false, check(config::default_max_block_net_usage+1, 0)); // larger than global maximum

} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE(weighted_net_usage_tests, tester ) try {
   SKIP_TEST
   account_name account = "f_tests"_n;
   account_name acc2 = "acc2"_n;
   create_accounts({account, acc2});
   int ver = 0;
   auto check = [&](int coderepeat)-> bool {
      std::string code = R"=====(
   (module
   (import "env" "require_auth" (func $require_auth (param i64)))
   (import "env" "sysio_assert" (func $sysio_assert (param i32 i32)))
      (table 0 anyfunc)
      (memory $0 1)
      (export "apply" (func $apply))
      (func $i64_trunc_u_f64 (param $0 f64) (result i64) (i64.trunc_u/f64 (get_local $0)))
      (func $test (param $0 i64))
      (func $apply (param $0 i64)(param $1 i64)(param $2 i64)
      )=====";
      for (int i = 0; i < coderepeat; ++i) {
         code += "(call $test (call $i64_trunc_u_f64 (f64.const ";
         code += (char)('0' + ver);
         code += ")))\n";
      }
      code += "))"; ver++;
      produce_blocks(1);
      signed_transaction trx;
      auto wasm = ::sysio::chain::wast_to_wasm(code);
      trx.actions.emplace_back( vector<permission_level>{{account,config::active_name}},
                              setcode{
                                 .account    = account,
                                 .vmtype     = 0,
                                 .vmversion  = 0,
                                 .code       = bytes(wasm.begin(), wasm.end())
                              });
      set_transaction_headers(trx);
      trx.sign( get_private_key( account, "active" ), control->get_chain_id()  );
      try {
         packed_transaction ptrx(trx);
         push_transaction(ptrx );
         produce_blocks(1);
         return true;
      } catch (tx_net_usage_exceeded &) {
         return false;
      }
   };
   BOOST_REQUIRE_EQUAL(true, check(128)); // no limits, should pass

   resource_limits_manager mgr = control->get_mutable_resource_limits_manager();
   mgr.set_account_limits(account, -1, 1, -1, false); // set weight = 1 for account

   BOOST_REQUIRE_EQUAL(true, check(128));

   mgr.set_account_limits(acc2, -1, 100000000, -1, false); // set a big weight for other account
   BOOST_REQUIRE_EQUAL(false, check(128));

} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()

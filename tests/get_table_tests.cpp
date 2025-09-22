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
#include <test_contracts.hpp>

#include <fc/io/fstream.hpp>

#include <fc/variant_object.hpp>
#include <fc/io/json.hpp>

#include <array>
#include <utility>

using namespace sysio;
using namespace sysio::chain;
using namespace sysio::testing;
using namespace fc;

static auto get_table_rows_full = [](chain_apis::read_only& plugin,
                                     chain_apis::read_only::get_table_rows_params& params,
                                     const fc::time_point& deadline) -> chain_apis::read_only::get_table_rows_result {
   auto res_nm_v =  plugin.get_table_rows(params, deadline)();
   BOOST_REQUIRE(!std::holds_alternative<fc::exception_ptr>(res_nm_v));
   return std::get<chain_apis::read_only::get_table_rows_result>(std::move(res_nm_v));
};

BOOST_AUTO_TEST_SUITE(get_table_tests)

transaction_trace_ptr
issue_tokens( validating_tester& t, account_name issuer, account_name to, const asset& amount,
              std::string memo = "", account_name token_contract = "sysio.token"_n )
{
   signed_transaction trx;

   trx.actions.emplace_back( t.get_action( token_contract, "issue"_n,
                                           vector<permission_level>{{issuer, config::active_name}},
                                           mutable_variant_object()
                                             ("to", issuer.to_string())
                                             ("quantity", amount)
                                             ("memo", memo)
   ) );

   trx.actions.emplace_back( t.get_action( token_contract, "transfer"_n,
                                           vector<permission_level>{{issuer, config::active_name}},
                                           mutable_variant_object()
                                             ("from", issuer.to_string())
                                             ("to", to.to_string())
                                             ("quantity", amount)
                                             ("memo", memo)
   ) );

   t.set_transaction_headers(trx);
   trx.sign( t.get_private_key( issuer, "active" ), t.control->get_chain_id()  );
   return t.push_transaction( trx );
}

BOOST_FIXTURE_TEST_CASE( get_scope_test, validating_tester ) try {
   produce_blocks(2);

   create_accounts({ "sysio.token"_n, "sysio.ram"_n, "sysio.ramfee"_n, "sysio.stake"_n,
      "sysio.bpay"_n, "sysio.vpay"_n, "sysio.saving"_n, "sysio.names"_n });

   std::vector<account_name> accs{"inita"_n, "initb"_n, "initc"_n, "initd"_n};
   create_accounts(accs);
   produce_block();

   set_code( "sysio.token"_n, test_contracts::sysio_token_wasm() );
   set_abi( "sysio.token"_n, test_contracts::sysio_token_abi() );
   set_privileged("sysio.token"_n);
   produce_blocks(1);

   // create currency
   auto act = mutable_variant_object()
         ("issuer",       "sysio")
         ("maximum_supply", sysio::chain::asset::from_string("1000000000.0000 SYS"));
   push_action("sysio.token"_n, "create"_n, "sysio.token"_n, act );

   // issue
   for (account_name a: accs) {
      issue_tokens( *this, config::system_account_name, a, sysio::chain::asset::from_string("999.0000 SYS") );
   }
   produce_blocks(1);

   // iterate over scope
   sysio::chain_apis::read_only plugin(*(this->control), {}, fc::microseconds::maximum(), fc::microseconds::maximum(), {});
   sysio::chain_apis::read_only::get_table_by_scope_params param{"sysio.token"_n, "accounts"_n, "inita", "", 10};
   sysio::chain_apis::read_only::get_table_by_scope_result result = plugin.read_only::get_table_by_scope(param, fc::time_point::maximum());

   BOOST_REQUIRE_EQUAL(5u, result.rows.size());
   BOOST_REQUIRE_EQUAL("", result.more);
   if (result.rows.size() >= 5u) {
      BOOST_REQUIRE_EQUAL(name("sysio.token"_n), result.rows[0].code);
      BOOST_REQUIRE_EQUAL(name("inita"_n), result.rows[0].scope);
      BOOST_REQUIRE_EQUAL(name("accounts"_n), result.rows[0].table);
      BOOST_REQUIRE_EQUAL(name("sysio"_n), result.rows[0].payer);
      BOOST_REQUIRE_EQUAL(1u, result.rows[0].count);

      BOOST_REQUIRE_EQUAL(name("initb"_n), result.rows[1].scope);
      BOOST_REQUIRE_EQUAL(name("initc"_n), result.rows[2].scope);
      BOOST_REQUIRE_EQUAL(name("initd"_n), result.rows[3].scope);
      BOOST_REQUIRE_EQUAL(name("sysio"_n), result.rows[4].scope);
   }

   param.lower_bound = "initb";
   param.upper_bound = "initc";
   result = plugin.read_only::get_table_by_scope(param, fc::time_point::maximum());
   BOOST_REQUIRE_EQUAL(2u, result.rows.size());
   BOOST_REQUIRE_EQUAL("", result.more);
   if (result.rows.size() >= 2u) {
      BOOST_REQUIRE_EQUAL(name("initb"_n), result.rows[0].scope);
      BOOST_REQUIRE_EQUAL(name("initc"_n), result.rows[1].scope);
   }

   param.limit = 1;
   result = plugin.read_only::get_table_by_scope(param, fc::time_point::maximum());
   BOOST_REQUIRE_EQUAL(1u, result.rows.size());
   BOOST_REQUIRE_EQUAL("initc", result.more);

   param.table = name(0);
   result = plugin.read_only::get_table_by_scope(param, fc::time_point::maximum());
   BOOST_REQUIRE_EQUAL(1u, result.rows.size());
   BOOST_REQUIRE_EQUAL("initc", result.more);

   param.table = "invalid"_n;
   result = plugin.read_only::get_table_by_scope(param, fc::time_point::maximum());
   BOOST_REQUIRE_EQUAL(0u, result.rows.size());
   BOOST_REQUIRE_EQUAL("", result.more);

} FC_LOG_AND_RETHROW() /// get_scope_test

BOOST_FIXTURE_TEST_CASE( get_table_test, validating_tester ) try {
   produce_blocks(2);

   create_accounts({ "sysio.token"_n, "sysio.ram"_n, "sysio.ramfee"_n, "sysio.stake"_n,
      "sysio.bpay"_n, "sysio.vpay"_n, "sysio.saving"_n, "sysio.names"_n });

   std::vector<account_name> accs{"inita"_n, "initb"_n};
   create_accounts(accs);
   produce_block();

   set_code( "sysio.token"_n, test_contracts::sysio_token_wasm() );
   set_abi( "sysio.token"_n, test_contracts::sysio_token_abi() );
   set_privileged("sysio.token"_n);
   produce_blocks(1);

   // create currency
   auto act = mutable_variant_object()
         ("issuer",       "sysio")
         ("maximum_supply", sysio::chain::asset::from_string("1000000000.0000 SYS"));
   push_action("sysio.token"_n, "create"_n, "sysio.token"_n, act );

   // issue
   for (account_name a: accs) {
      issue_tokens( *this, config::system_account_name, a, sysio::chain::asset::from_string("10000.0000 SYS") );
   }
   produce_blocks(1);

   // create currency 2
   act = mutable_variant_object()
         ("issuer",       "sysio")
         ("maximum_supply", sysio::chain::asset::from_string("1000000000.0000 AAA"));
   push_action("sysio.token"_n, "create"_n, "sysio.token"_n, act );
   // issue
   for (account_name a: accs) {
      issue_tokens( *this, config::system_account_name, a, sysio::chain::asset::from_string("9999.0000 AAA") );
   }
   produce_blocks(1);

   // create currency 3
   act = mutable_variant_object()
         ("issuer",       "sysio")
         ("maximum_supply", sysio::chain::asset::from_string("1000000000.0000 CCC"));
   push_action("sysio.token"_n, "create"_n, "sysio.token"_n, act );
   // issue
   for (account_name a: accs) {
      issue_tokens( *this, config::system_account_name, a, sysio::chain::asset::from_string("7777.0000 CCC") );
   }
   produce_blocks(1);

   // create currency 3
   act = mutable_variant_object()
         ("issuer",       "sysio")
         ("maximum_supply", sysio::chain::asset::from_string("1000000000.0000 BBB"));
   push_action("sysio.token"_n, "create"_n, "sysio.token"_n, act );
   // issue
   for (account_name a: accs) {
      issue_tokens( *this, config::system_account_name, a, sysio::chain::asset::from_string("8888.0000 BBB") );
   }
   produce_blocks(1);

   // get table: normal case
   sysio::chain_apis::read_only plugin(*(this->control), {}, fc::microseconds::maximum(), fc::microseconds::maximum(), {});
   sysio::chain_apis::read_only::get_table_rows_params p;
   p.code = "sysio.token"_n;
   p.scope = "inita";
   p.table = "accounts"_n;
   p.json = true;
   p.index_position = "primary";
   auto result = get_table_rows_full(plugin, p, fc::time_point::maximum());

   BOOST_REQUIRE_EQUAL(4u, result.rows.size());
   BOOST_REQUIRE_EQUAL(false, result.more);
   if (result.rows.size() >= 4u) {
      BOOST_REQUIRE_EQUAL("9999.0000 AAA", result.rows[0]["balance"].as_string());
      BOOST_REQUIRE_EQUAL("8888.0000 BBB", result.rows[1]["balance"].as_string());
      BOOST_REQUIRE_EQUAL("7777.0000 CCC", result.rows[2]["balance"].as_string());
      BOOST_REQUIRE_EQUAL("10000.0000 SYS", result.rows[3]["balance"].as_string());
   }

   // get table: reverse ordered
   p.reverse = true;
   result = get_table_rows_full(plugin, p, fc::time_point::maximum());
   BOOST_REQUIRE_EQUAL(4u, result.rows.size());
   BOOST_REQUIRE_EQUAL(false, result.more);
   if (result.rows.size() >= 4) {
      BOOST_REQUIRE_EQUAL("9999.0000 AAA", result.rows[3]["balance"].as_string());
      BOOST_REQUIRE_EQUAL("8888.0000 BBB", result.rows[2]["balance"].as_string());
      BOOST_REQUIRE_EQUAL("7777.0000 CCC", result.rows[1]["balance"].as_string());
      BOOST_REQUIRE_EQUAL("10000.0000 SYS", result.rows[0]["balance"].as_string());
   }

   // get table: reverse ordered, with ram payer
   p.reverse = true;
   p.show_payer = true;
   result = get_table_rows_full(plugin, p, fc::time_point::maximum());
   BOOST_REQUIRE_EQUAL(4u, result.rows.size());
   BOOST_REQUIRE_EQUAL(false, result.more);
   if (result.rows.size() >= 4u) {
      BOOST_REQUIRE_EQUAL("9999.0000 AAA", result.rows[3]["data"]["balance"].as_string());
      BOOST_REQUIRE_EQUAL("8888.0000 BBB", result.rows[2]["data"]["balance"].as_string());
      BOOST_REQUIRE_EQUAL("7777.0000 CCC", result.rows[1]["data"]["balance"].as_string());
      BOOST_REQUIRE_EQUAL("10000.0000 SYS", result.rows[0]["data"]["balance"].as_string());
      BOOST_REQUIRE_EQUAL("sysio", result.rows[0]["payer"].as_string());
      BOOST_REQUIRE_EQUAL("sysio", result.rows[1]["payer"].as_string());
      BOOST_REQUIRE_EQUAL("sysio", result.rows[2]["payer"].as_string());
      BOOST_REQUIRE_EQUAL("sysio", result.rows[3]["payer"].as_string());
   }
   p.show_payer = false;

   // get table: normal case, with bound
   p.lower_bound = "BBB";
   p.upper_bound = "CCC";
   p.reverse = false;
   result = get_table_rows_full(plugin, p, fc::time_point::maximum());
   BOOST_REQUIRE_EQUAL(2u, result.rows.size());
   BOOST_REQUIRE_EQUAL(false, result.more);
   if (result.rows.size() >= 2u) {
      BOOST_REQUIRE_EQUAL("8888.0000 BBB", result.rows[0]["balance"].as_string());
      BOOST_REQUIRE_EQUAL("7777.0000 CCC", result.rows[1]["balance"].as_string());
   }

   // get table: reverse case, with bound
   p.lower_bound = "BBB";
   p.upper_bound = "CCC";
   p.reverse = true;
   result = get_table_rows_full(plugin, p, fc::time_point::maximum());
   BOOST_REQUIRE_EQUAL(2u, result.rows.size());
   BOOST_REQUIRE_EQUAL(false, result.more);
   if (result.rows.size() >= 2u) {
      BOOST_REQUIRE_EQUAL("8888.0000 BBB", result.rows[1]["balance"].as_string());
      BOOST_REQUIRE_EQUAL("7777.0000 CCC", result.rows[0]["balance"].as_string());
   }

   // get table: normal case, with limit
   p.lower_bound = p.upper_bound = "";
   p.limit = 1;
   p.reverse = false;
   result = get_table_rows_full(plugin, p, fc::time_point::maximum());
   BOOST_REQUIRE_EQUAL(1u, result.rows.size());
   BOOST_REQUIRE_EQUAL(true, result.more);
   if (result.rows.size() >= 1u) {
      BOOST_REQUIRE_EQUAL("9999.0000 AAA", result.rows[0]["balance"].as_string());
   }

   // get table: reverse case, with limit
   p.lower_bound = p.upper_bound = "";
   p.limit = 1;
   p.reverse = true;
   result = get_table_rows_full(plugin, p, fc::time_point::maximum());
   BOOST_REQUIRE_EQUAL(1u, result.rows.size());
   BOOST_REQUIRE_EQUAL(true, result.more);
   if (result.rows.size() >= 1u) {
      BOOST_REQUIRE_EQUAL("10000.0000 SYS", result.rows[0]["balance"].as_string());
   }

   // get table: normal case, with bound & limit
   p.lower_bound = "BBB";
   p.upper_bound = "CCC";
   p.limit = 1;
   p.reverse = false;
   result = get_table_rows_full(plugin, p, fc::time_point::maximum());
   BOOST_REQUIRE_EQUAL(1u, result.rows.size());
   BOOST_REQUIRE_EQUAL(true, result.more);
   if (result.rows.size() >= 1u) {
      BOOST_REQUIRE_EQUAL("8888.0000 BBB", result.rows[0]["balance"].as_string());
   }

   // get table: reverse case, with bound & limit
   p.lower_bound = "BBB";
   p.upper_bound = "CCC";
   p.limit = 1;
   p.reverse = true;
   result = get_table_rows_full(plugin, p, fc::time_point::maximum());
   BOOST_REQUIRE_EQUAL(1u, result.rows.size());
   BOOST_REQUIRE_EQUAL(true, result.more);
   if (result.rows.size() >= 1u) {
      BOOST_REQUIRE_EQUAL("7777.0000 CCC", result.rows[0]["balance"].as_string());
   }

} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( get_table_next_key_test, validating_tester ) try {
   SKIP_TEST
   create_account("test"_n);

   // setup contract and abi
   set_code( "test"_n, test_contracts::get_table_test_wasm() );
   set_abi( "test"_n, test_contracts::get_table_test_abi() );
   produce_block();

   // Init some data
   push_action("test"_n, "addnumobj"_n, "test"_n, mutable_variant_object()("input", 2));
   push_action("test"_n, "addnumobj"_n, "test"_n, mutable_variant_object()("input", 5));
   push_action("test"_n, "addnumobj"_n, "test"_n, mutable_variant_object()("input", 7));
   push_action("test"_n, "addhashobj"_n, "test"_n, mutable_variant_object()("hashinput", "firstinput"));
   push_action("test"_n, "addhashobj"_n, "test"_n, mutable_variant_object()("hashinput", "secondinput"));
   push_action("test"_n, "addhashobj"_n, "test"_n, mutable_variant_object()("hashinput", "thirdinput"));
   produce_block();

   // The result of the init will populate
   // For numobjs table (secondary index is on sec64, sec128, secdouble, secldouble)
   // {
   //   "rows": [{
   //       "key": 0,
   //       "sec64": 2,
   //       "sec128": "0x02000000000000000000000000000000",
   //       "secdouble": "2.00000000000000000",
   //       "secldouble": "0x00000000000000000000000000000040"
   //     },{
   //       "key": 1,
   //       "sec64": 5,
   //       "sec128": "0x05000000000000000000000000000000",
   //       "secdouble": "5.00000000000000000",
   //       "secldouble": "0x00000000000000000000000000400140"
   //     },{
   //       "key": 2,
   //       "sec64": 7,
   //       "sec128": "0x07000000000000000000000000000000",
   //       "secdouble": "7.00000000000000000",
   //       "secldouble": "0x00000000000000000000000000c00140"
   //     }
   //   "more": false,
   //   "next_key": ""
   // }
   // For hashobjs table (secondary index is on sec256 and sec160):
   // {
   //   "rows": [{
   //       "key": 0,
   //       "hash_input": "firstinput",
   //       "sec256": "05f5aa6b6c5568c53e886591daa9d9f636fa8e77873581ba67ca46a0f96c226e",
   //       "sec160": "2a9baa59f1e376eda2e963c140d13c7e77c2f1fb"
   //     },{
   //       "key": 1,
   //       "hash_input": "secondinput",
   //       "sec256": "3cb93a80b47b9d70c5296be3817d34b48568893b31468e3a76337bb7d3d0c264",
   //       "sec160": "fb9d03d3012dc2a6c7b319f914542e3423550c2a"
   //     },{
   //       "key": 2,
   //       "hash_input": "thirdinput",
   //       "sec256": "2652d68fbbf6000c703b35fdc607b09cd8218cbeea1d108b5c9e84842cdd5ea5",
   //       "sec160": "ab4314638b573fdc39e5a7b107938ad1b5a16414"
   //     }
   //   ],
   //   "more": false,
   //   "next_key": ""
   // }


   chain_apis::read_only plugin(*(this->control), {}, fc::microseconds::maximum(), fc::microseconds::maximum(), {});
   chain_apis::read_only::get_table_rows_params params = []{
      chain_apis::read_only::get_table_rows_params params{};
      params.json=true;
      params.code="test"_n;
      params.scope="test";
      params.limit=1;
      return params;
   }();

   params.table = "numobjs"_n;

   // i64 primary key type
   params.key_type = "i64";
   params.index_position = "1";
   params.lower_bound = "0";

   auto res_1 = get_table_rows_full(plugin, params, fc::time_point::maximum());
   BOOST_REQUIRE(res_1.rows.size() > 0u);
   BOOST_TEST(res_1.rows[0].get_object()["key"].as<uint64_t>() == 0u);
   BOOST_TEST(res_1.next_key == "1");
   params.lower_bound = res_1.next_key;
   auto more2_res_1 = get_table_rows_full(plugin, params, fc::time_point::maximum());
   BOOST_REQUIRE(more2_res_1.rows.size() > 0u);
   BOOST_TEST(more2_res_1.rows[0].get_object()["key"].as<uint64_t>() == 1u);


   // i64 secondary key type
   params.key_type = "i64";
   params.index_position = "2";
   params.lower_bound = "5";

   auto res_2 = get_table_rows_full(plugin, params, fc::time_point::maximum());
   BOOST_REQUIRE(res_2.rows.size() > 0u);
   BOOST_TEST(res_2.rows[0].get_object()["sec64"].as<uint64_t>() == 5u);
   BOOST_TEST(res_2.next_key == "7");
   params.lower_bound = res_2.next_key;
   auto more2_res_2 = get_table_rows_full(plugin, params, fc::time_point::maximum());
   BOOST_REQUIRE(more2_res_2.rows.size() > 0u);
   BOOST_TEST(more2_res_2.rows[0].get_object()["sec64"].as<uint64_t>() == 7u);

   // i128 secondary key type
   params.key_type = "i128";
   params.index_position = "3";
   params.lower_bound = "5";

   auto res_3 = get_table_rows_full(plugin, params, fc::time_point::maximum());
   chain::uint128_t sec128_expected_value = 5;
   BOOST_REQUIRE(res_3.rows.size() > 0u);
   BOOST_CHECK(res_3.rows[0].get_object()["sec128"].as<chain::uint128_t>() == sec128_expected_value);
   BOOST_TEST(res_3.next_key == "7");
   params.lower_bound = res_3.next_key;
   auto more2_res_3 = get_table_rows_full(plugin, params, fc::time_point::maximum());
   chain::uint128_t more2_sec128_expected_value = 7;
   BOOST_REQUIRE(more2_res_3.rows.size() > 0u);
   BOOST_CHECK(more2_res_3.rows[0].get_object()["sec128"].as<chain::uint128_t>() == more2_sec128_expected_value);

   // float64 secondary key type
   params.key_type = "float64";
   params.index_position = "4";
   params.lower_bound = "5.0";

   auto res_4 = get_table_rows_full(plugin, params, fc::time_point::maximum());
   float64_t secdouble_expected_value = ui64_to_f64(5);
   BOOST_REQUIRE(res_4.rows.size() > 0u);
   float64_t secdouble_res_value = res_4.rows[0].get_object()["secdouble"].as<float64_t>();
   BOOST_CHECK(secdouble_res_value == secdouble_expected_value);
   BOOST_TEST(res_4.next_key == "7.00000000000000000");
   params.lower_bound = res_4.next_key;
   auto more2_res_4 = get_table_rows_full(plugin, params, fc::time_point::maximum());
   float64_t more2_secdouble_expected_value = ui64_to_f64(7);
   BOOST_REQUIRE(more2_res_4.rows.size() > 0u);
   float64_t more2_secdouble_res_value = more2_res_4.rows[0].get_object()["secdouble"].as<float64_t>();
   BOOST_CHECK(more2_secdouble_res_value == more2_secdouble_expected_value);

   // float128 secondary key type
   params.key_type = "float128";
   params.index_position = "5";
   params.lower_bound = "5.0";

   auto res_5 = get_table_rows_full(plugin, params, fc::time_point::maximum());
   float128_t secldouble_expected_value = ui64_to_f128(5);
   BOOST_REQUIRE(res_5.rows.size() > 0u);
   float128_t secldouble_res_value =  res_5.rows[0].get_object()["secldouble"].as<float128_t>();
   BOOST_TEST(secldouble_res_value == secldouble_expected_value);
   BOOST_TEST(res_5.next_key == "7.00000000000000000");
   params.lower_bound = res_5.next_key;
   auto more2_res_5 = get_table_rows_full(plugin, params, fc::time_point::maximum());
   float128_t more2_secldouble_expected_value = ui64_to_f128(7);
   BOOST_REQUIRE(more2_res_5.rows.size() > 0u);
   float128_t more2_secldouble_res_value =  more2_res_5.rows[0].get_object()["secldouble"].as<float128_t>();
   BOOST_TEST(more2_secldouble_res_value == more2_secldouble_expected_value);

   params.table = "hashobjs"_n;

   // sha256 secondary key type
   params.key_type = "sha256";
   params.index_position = "2";
   params.lower_bound = "2652d68fbbf6000c703b35fdc607b09cd8218cbeea1d108b5c9e84842cdd5ea5"; // This is hash of "thirdinput"

   auto res_6 = get_table_rows_full(plugin, params, fc::time_point::maximum());
   checksum256_type sec256_expected_value = checksum256_type::hash(std::string("thirdinput"));
   BOOST_REQUIRE(res_6.rows.size() > 0u);
   checksum256_type sec256_res_value = res_6.rows[0].get_object()["sec256"].as<checksum256_type>();
   BOOST_TEST(sec256_res_value == sec256_expected_value);
   BOOST_TEST(res_6.rows[0].get_object()["hash_input"].as<string>() == std::string("thirdinput"));
   BOOST_TEST(res_6.next_key == "3cb93a80b47b9d70c5296be3817d34b48568893b31468e3a76337bb7d3d0c264");
   params.lower_bound = res_6.next_key;
   auto more2_res_6 = get_table_rows_full(plugin, params, fc::time_point::maximum());
   checksum256_type more2_sec256_expected_value = checksum256_type::hash(std::string("secondinput"));
   BOOST_REQUIRE(more2_res_6.rows.size() > 0u);
   checksum256_type more2_sec256_res_value = more2_res_6.rows[0].get_object()["sec256"].as<checksum256_type>();
   BOOST_TEST(more2_sec256_res_value == more2_sec256_expected_value);
   BOOST_TEST(more2_res_6.rows[0].get_object()["hash_input"].as<string>() == std::string("secondinput"));

   // i256 secondary key type
   params.key_type = "i256";
   params.index_position = "2";
   params.lower_bound = "0x2652d68fbbf6000c703b35fdc607b09cd8218cbeea1d108b5c9e84842cdd5ea5"; // This is sha256 hash of "thirdinput" as number

   auto res_7 = get_table_rows_full(plugin, params, fc::time_point::maximum());
   checksum256_type i256_expected_value = checksum256_type::hash(std::string("thirdinput"));
   BOOST_REQUIRE(res_7.rows.size() > 0u);
   checksum256_type i256_res_value = res_7.rows[0].get_object()["sec256"].as<checksum256_type>();
   BOOST_TEST(i256_res_value == i256_expected_value);
   BOOST_TEST(res_7.rows[0].get_object()["hash_input"].as<string>() == "thirdinput");
   BOOST_TEST(res_7.next_key == "0x3cb93a80b47b9d70c5296be3817d34b48568893b31468e3a76337bb7d3d0c264");
   params.lower_bound = res_7.next_key;
   auto more2_res_7 = get_table_rows_full(plugin, params, fc::time_point::maximum());
   checksum256_type more2_i256_expected_value = checksum256_type::hash(std::string("secondinput"));
   BOOST_REQUIRE(more2_res_7.rows.size() > 0u);
   checksum256_type more2_i256_res_value = more2_res_7.rows[0].get_object()["sec256"].as<checksum256_type>();
   BOOST_TEST(more2_i256_res_value == more2_i256_expected_value);
   BOOST_TEST(more2_res_7.rows[0].get_object()["hash_input"].as<string>() == "secondinput");

   // ripemd160 secondary key type
   params.key_type = "ripemd160";
   params.index_position = "3";
   params.lower_bound = "ab4314638b573fdc39e5a7b107938ad1b5a16414"; // This is ripemd160 hash of "thirdinput"

   auto res_8 = get_table_rows_full(plugin, params, fc::time_point::maximum());
   ripemd160 sec160_expected_value = ripemd160::hash(std::string("thirdinput"));
   BOOST_REQUIRE(res_8.rows.size() > 0u);
   ripemd160 sec160_res_value = res_8.rows[0].get_object()["sec160"].as<ripemd160>();
   BOOST_TEST(sec160_res_value == sec160_expected_value);
   BOOST_TEST(res_8.rows[0].get_object()["hash_input"].as<string>() == "thirdinput");
   BOOST_TEST(res_8.next_key == "fb9d03d3012dc2a6c7b319f914542e3423550c2a");
   params.lower_bound = res_8.next_key;
   auto more2_res_8 = get_table_rows_full(plugin, params, fc::time_point::maximum());
   ripemd160 more2_sec160_expected_value = ripemd160::hash(std::string("secondinput"));
   BOOST_REQUIRE(more2_res_8.rows.size() > 0u);
   ripemd160 more2_sec160_res_value = more2_res_8.rows[0].get_object()["sec160"].as<ripemd160>();
   BOOST_TEST(more2_sec160_res_value == more2_sec160_expected_value);
   BOOST_TEST(more2_res_8.rows[0].get_object()["hash_input"].as<string>() == "secondinput");

} FC_LOG_AND_RETHROW() /// get_table_next_key_test

BOOST_AUTO_TEST_SUITE_END()

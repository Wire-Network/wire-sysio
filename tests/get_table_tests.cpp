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
   produce_block();

   create_accounts({ "sysio.token"_n, "sysio.ram"_n, "sysio.ramfee"_n, "sysio.stake"_n,
      "sysio.bpay"_n, "sysio.vpay"_n, "sysio.saving"_n, "sysio.names"_n });

   std::vector<account_name> accs{"inita"_n, "initb"_n, "initc"_n, "initd"_n};
   create_accounts(accs);
   produce_block();

   set_code( "sysio.token"_n, test_contracts::sysio_token_wasm() );
   set_abi( "sysio.token"_n, test_contracts::sysio_token_abi() );
   set_privileged("sysio.token"_n);
   produce_block();

   // create currency
   auto act = mutable_variant_object()
         ("issuer",       "sysio")
         ("maximum_supply", sysio::chain::asset::from_string("1000000000.0000 SYS"));
   push_action("sysio.token"_n, "create"_n, "sysio.token"_n, act );

   // issue
   for (account_name a: accs) {
      issue_tokens( *this, config::system_account_name, a, sysio::chain::asset::from_string("999.0000 SYS") );
   }
   produce_block();

   // iterate over scope
   std::optional<sysio::chain_apis::tracked_votes> _tracked_votes;
   sysio::chain_apis::read_only plugin(*(this->control), {}, {}, _tracked_votes, fc::microseconds::maximum(), fc::microseconds::maximum(), {});
   sysio::chain_apis::read_only::get_table_by_scope_params param{"sysio.token"_n, "accounts"_n, "inita", "", 10};
   sysio::chain_apis::read_only::get_table_by_scope_result result = plugin.read_only::get_table_by_scope(param, fc::time_point::maximum());

   // Results are ordered by (table, scope). With table filter "accounts", we see
   // all scopes for that table in scope order.
   BOOST_REQUIRE_EQUAL(5u, result.rows.size());
   BOOST_REQUIRE_EQUAL("", result.more);
   if (result.rows.size() >= 5u) {
      BOOST_REQUIRE_EQUAL(name("sysio.token"_n), result.rows[0].code);
      BOOST_REQUIRE_EQUAL(name("accounts"_n), result.rows[0].table);
      // Scope order within the "accounts" table
      BOOST_REQUIRE_EQUAL(name("inita"_n), result.rows[0].scope);
      BOOST_REQUIRE_EQUAL(name("initb"_n), result.rows[1].scope);
      BOOST_REQUIRE_EQUAL(name("initc"_n), result.rows[2].scope);
      BOOST_REQUIRE_EQUAL(name("initd"_n), result.rows[3].scope);
      BOOST_REQUIRE_EQUAL(name("sysio"_n), result.rows[4].scope);
   }

   // Scope bounds filtering
   param.lower_bound = "initb";
   param.upper_bound = "initc";
   result = plugin.read_only::get_table_by_scope(param, fc::time_point::maximum());
   BOOST_REQUIRE_EQUAL(2u, result.rows.size());
   BOOST_REQUIRE_EQUAL("", result.more);
   if (result.rows.size() >= 2u) {
      BOOST_REQUIRE_EQUAL(name("initb"_n), result.rows[0].scope);
      BOOST_REQUIRE_EQUAL(name("initc"_n), result.rows[1].scope);
   }

   // Pagination with limit=1 — more returns "table:scope" token
   param.limit = 1;
   result = plugin.read_only::get_table_by_scope(param, fc::time_point::maximum());
   BOOST_REQUIRE_EQUAL(1u, result.rows.size());
   BOOST_REQUIRE_EQUAL(name("initb"_n), result.rows[0].scope);
   BOOST_REQUIRE(!result.more.empty());
   // Follow the pagination token
   param.lower_bound = result.more;
   param.upper_bound = "initc";
   param.limit = 10;
   result = plugin.read_only::get_table_by_scope(param, fc::time_point::maximum());
   BOOST_REQUIRE_EQUAL(1u, result.rows.size());
   BOOST_REQUIRE_EQUAL(name("initc"_n), result.rows[0].scope);
   BOOST_REQUIRE_EQUAL("", result.more);

   // No table filter — returns all tables for this code
   param.table = name(0);
   param.lower_bound = "";
   param.upper_bound = "";
   param.limit = 100;
   result = plugin.read_only::get_table_by_scope(param, fc::time_point::maximum());
   // sysio.token has "accounts" table and "stat" table
   // Should have entries for both, ordered by (table, scope)
   BOOST_REQUIRE(result.rows.size() >= 5u);

   // Invalid table filter returns empty
   param.table = "invalid"_n;
   result = plugin.read_only::get_table_by_scope(param, fc::time_point::maximum());
   BOOST_REQUIRE_EQUAL(0u, result.rows.size());
   BOOST_REQUIRE_EQUAL("", result.more);

   // Reverse iteration with table filter
   param.table = "accounts"_n;
   param.lower_bound = "";
   param.upper_bound = "";
   param.limit = 10;
   param.reverse = true;
   result = plugin.read_only::get_table_by_scope(param, fc::time_point::maximum());
   BOOST_REQUIRE_EQUAL(5u, result.rows.size());
   // Reverse: scopes in descending order
   BOOST_REQUIRE_EQUAL(name("sysio"_n), result.rows[0].scope);
   BOOST_REQUIRE_EQUAL(name("initd"_n), result.rows[1].scope);
   BOOST_REQUIRE_EQUAL(name("initc"_n), result.rows[2].scope);
   BOOST_REQUIRE_EQUAL(name("initb"_n), result.rows[3].scope);
   BOOST_REQUIRE_EQUAL(name("inita"_n), result.rows[4].scope);

   // Reverse with scope bounds
   param.lower_bound = "initb";
   param.upper_bound = "initd";
   param.limit = 10;
   result = plugin.read_only::get_table_by_scope(param, fc::time_point::maximum());
   BOOST_REQUIRE_EQUAL(3u, result.rows.size());
   BOOST_REQUIRE_EQUAL(name("initd"_n), result.rows[0].scope);
   BOOST_REQUIRE_EQUAL(name("initc"_n), result.rows[1].scope);
   BOOST_REQUIRE_EQUAL(name("initb"_n), result.rows[2].scope);

   // Reverse pagination
   param.lower_bound = "";
   param.upper_bound = "";
   param.limit = 2;
   result = plugin.read_only::get_table_by_scope(param, fc::time_point::maximum());
   BOOST_REQUIRE_EQUAL(2u, result.rows.size());
   BOOST_REQUIRE_EQUAL(name("sysio"_n), result.rows[0].scope);
   BOOST_REQUIRE_EQUAL(name("initd"_n), result.rows[1].scope);
   BOOST_REQUIRE(!result.more.empty());
   // Follow reverse pagination token
   param.upper_bound = result.more;
   param.lower_bound = "";
   param.limit = 10;
   result = plugin.read_only::get_table_by_scope(param, fc::time_point::maximum());
   BOOST_REQUIRE_EQUAL(3u, result.rows.size());
   BOOST_REQUIRE_EQUAL(name("initc"_n), result.rows[0].scope);
   BOOST_REQUIRE_EQUAL(name("initb"_n), result.rows[1].scope);
   BOOST_REQUIRE_EQUAL(name("inita"_n), result.rows[2].scope);

} FC_LOG_AND_RETHROW() /// get_scope_test

BOOST_FIXTURE_TEST_CASE( get_table_test, validating_tester ) try {
   produce_block();

   create_accounts({ "sysio.token"_n, "sysio.ram"_n, "sysio.ramfee"_n, "sysio.stake"_n,
      "sysio.bpay"_n, "sysio.vpay"_n, "sysio.saving"_n, "sysio.names"_n });

   std::vector<account_name> accs{"inita"_n, "initb"_n};
   create_accounts(accs);
   produce_block();

   set_code( "sysio.token"_n, test_contracts::sysio_token_wasm() );
   set_abi( "sysio.token"_n, test_contracts::sysio_token_abi() );
   set_privileged("sysio.token"_n);
   produce_block();

   // create currency
   auto act = mutable_variant_object()
         ("issuer",       "sysio")
         ("maximum_supply", sysio::chain::asset::from_string("1000000000.0000 SYS"));
   push_action("sysio.token"_n, "create"_n, "sysio.token"_n, act );

   // issue
   for (account_name a: accs) {
      issue_tokens( *this, config::system_account_name, a, sysio::chain::asset::from_string("10000.0000 SYS") );
   }
   produce_block();

   // create currency 2
   act = mutable_variant_object()
         ("issuer",       "sysio")
         ("maximum_supply", sysio::chain::asset::from_string("1000000000.0000 AAA"));
   push_action("sysio.token"_n, "create"_n, "sysio.token"_n, act );
   // issue
   for (account_name a: accs) {
      issue_tokens( *this, config::system_account_name, a, sysio::chain::asset::from_string("9999.0000 AAA") );
   }
   produce_block();

   // create currency 3
   act = mutable_variant_object()
         ("issuer",       "sysio")
         ("maximum_supply", sysio::chain::asset::from_string("1000000000.0000 CCC"));
   push_action("sysio.token"_n, "create"_n, "sysio.token"_n, act );
   // issue
   for (account_name a: accs) {
      issue_tokens( *this, config::system_account_name, a, sysio::chain::asset::from_string("7777.0000 CCC") );
   }
   produce_block();

   // create currency 3
   act = mutable_variant_object()
         ("issuer",       "sysio")
         ("maximum_supply", sysio::chain::asset::from_string("1000000000.0000 BBB"));
   push_action("sysio.token"_n, "create"_n, "sysio.token"_n, act );
   // issue
   for (account_name a: accs) {
      issue_tokens( *this, config::system_account_name, a, sysio::chain::asset::from_string("8888.0000 BBB") );
   }
   produce_block();

   // get table: normal case
   std::optional<sysio::chain_apis::tracked_votes> _tracked_votes;
   sysio::chain_apis::read_only plugin(*(this->control), {}, {}, _tracked_votes, fc::microseconds::maximum(), fc::microseconds::maximum(), {});
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
      // KV payer is the account specified in emplace(), not the contract.
      // sysio.token issues from system_account_name, so payer is "sysio".
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


   std::optional<sysio::chain_apis::tracked_votes> _tracked_votes;
   chain_apis::read_only plugin(*(this->control), {}, {}, _tracked_votes, fc::microseconds::maximum(), fc::microseconds::maximum(), {});
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

   // ── Secondary index: idx64 (index_position=2, key_type=i64) ──────────
   {
      params.table = "numobjs"_n;
      params.key_type = "i64";
      params.index_position = "2";
      params.lower_bound = "0";
      params.upper_bound = "";
      params.limit = 10;

      auto res = get_table_rows_full(plugin, params, fc::time_point::maximum());
      BOOST_REQUIRE_EQUAL(res.rows.size(), 3u);
      // Secondary values are 2, 5, 7 — rows should be ordered by sec64
      BOOST_TEST(res.rows[0].get_object()["sec64"].as<uint64_t>() == 2u);
      BOOST_TEST(res.rows[1].get_object()["sec64"].as<uint64_t>() == 5u);
      BOOST_TEST(res.rows[2].get_object()["sec64"].as<uint64_t>() == 7u);
      BOOST_TEST(res.more == false);
   }

   // ── Secondary index: idx64 with pagination ────────────────────────────
   {
      params.table = "numobjs"_n;
      params.key_type = "i64";
      params.index_position = "2";
      params.lower_bound = "0";
      params.upper_bound = "";
      params.limit = 1;

      auto res = get_table_rows_full(plugin, params, fc::time_point::maximum());
      BOOST_REQUIRE_EQUAL(res.rows.size(), 1u);
      BOOST_TEST(res.rows[0].get_object()["sec64"].as<uint64_t>() == 2u);
      BOOST_TEST(res.more == true);
      BOOST_TEST(!res.next_key.empty());

      // Page 2
      params.lower_bound = res.next_key;
      auto res2 = get_table_rows_full(plugin, params, fc::time_point::maximum());
      BOOST_REQUIRE_EQUAL(res2.rows.size(), 1u);
      BOOST_TEST(res2.rows[0].get_object()["sec64"].as<uint64_t>() == 5u);
   }

   // ── Secondary index: idx64 with upper_bound ───────────────────────────
   {
      params.table = "numobjs"_n;
      params.key_type = "i64";
      params.index_position = "2";
      params.lower_bound = "3";
      params.upper_bound = "6";
      params.limit = 10;

      auto res = get_table_rows_full(plugin, params, fc::time_point::maximum());
      BOOST_REQUIRE_EQUAL(res.rows.size(), 1u);
      BOOST_TEST(res.rows[0].get_object()["sec64"].as<uint64_t>() == 5u);
   }

   // ── Secondary index: float64 (index_position=4, key_type=float64) ────
   {
      params.table = "numobjs"_n;
      params.key_type = "float64";
      params.index_position = "4";
      params.lower_bound = "0";
      params.upper_bound = "";
      params.limit = 10;

      auto res = get_table_rows_full(plugin, params, fc::time_point::maximum());
      BOOST_REQUIRE_EQUAL(res.rows.size(), 3u);
      // secdouble values: 2.0, 5.0, 7.0
      BOOST_TEST(res.rows[0].get_object()["secdouble"].as<double>() == 2.0);
      BOOST_TEST(res.rows[1].get_object()["secdouble"].as<double>() == 5.0);
      BOOST_TEST(res.rows[2].get_object()["secdouble"].as<double>() == 7.0);
   }

   // ── Secondary index: i128 (index_position=3, key_type=i128) ─────────
   {
      params.table = "numobjs"_n;
      params.key_type = "i128";
      params.index_position = "3";
      params.lower_bound = "0x00000000000000000000000000000000";
      params.upper_bound = "";
      params.limit = 10;

      auto res = get_table_rows_full(plugin, params, fc::time_point::maximum());
      BOOST_REQUIRE_EQUAL(res.rows.size(), 3u);
      // sec128 values are 2, 5, 7 (ABI serializer renders uint128 as decimal string)
      BOOST_TEST(res.rows[0].get_object()["sec128"].as_string() == "2");
      BOOST_TEST(res.rows[1].get_object()["sec128"].as_string() == "5");
      BOOST_TEST(res.rows[2].get_object()["sec128"].as_string() == "7");
   }

   // ── Secondary index: float128 (index_position=5, key_type=float128) ──
   {
      params.table = "numobjs"_n;
      params.key_type = "float128";
      params.index_position = "5";
      // float128 bounds: 0.0 encoded as LE hex (16 zero bytes)
      params.lower_bound = "0x00000000000000000000000000000000";
      params.upper_bound = "";
      params.limit = 10;

      auto res = get_table_rows_full(plugin, params, fc::time_point::maximum());
      BOOST_REQUIRE_EQUAL(res.rows.size(), 3u);
      // secldouble values 2.0, 5.0, 7.0 should be in order
   }

   // ── Secondary index: sha256 (hashobjs table, index_position=2) ───────
   {
      params.table = "hashobjs"_n;
      params.key_type = "sha256";
      params.index_position = "2";
      params.lower_bound = "0000000000000000000000000000000000000000000000000000000000000000";
      params.upper_bound = "";
      params.limit = 10;

      auto res = get_table_rows_full(plugin, params, fc::time_point::maximum());
      BOOST_REQUIRE_EQUAL(res.rows.size(), 3u);
      // Rows should be ordered by sec256 (natural byte order, no word swap)
   }

   // ── Secondary index: reverse iteration (idx64) ───────────────────────
   {
      params.table = "numobjs"_n;
      params.key_type = "i64";
      params.index_position = "2";
      params.lower_bound = "";
      params.upper_bound = "";
      params.limit = 10;
      params.reverse = true;

      auto res = get_table_rows_full(plugin, params, fc::time_point::maximum());
      BOOST_REQUIRE_EQUAL(res.rows.size(), 3u);
      // Reverse: should be 7, 5, 2
      BOOST_TEST(res.rows[0].get_object()["sec64"].as<uint64_t>() == 7u);
      BOOST_TEST(res.rows[1].get_object()["sec64"].as<uint64_t>() == 5u);
      BOOST_TEST(res.rows[2].get_object()["sec64"].as<uint64_t>() == 2u);
      params.reverse = std::nullopt;
   }

   // ── Secondary index: reverse with pagination ─────────────────────────
   {
      params.table = "numobjs"_n;
      params.key_type = "i64";
      params.index_position = "2";
      params.lower_bound = "";
      params.upper_bound = "";
      params.limit = 1;
      params.reverse = true;

      auto page1 = get_table_rows_full(plugin, params, fc::time_point::maximum());
      BOOST_REQUIRE_EQUAL(page1.rows.size(), 1u);
      BOOST_TEST(page1.rows[0].get_object()["sec64"].as<uint64_t>() == 7u);
      BOOST_TEST(page1.more == true);

      params.upper_bound = page1.next_key;
      auto page2 = get_table_rows_full(plugin, params, fc::time_point::maximum());
      BOOST_REQUIRE_EQUAL(page2.rows.size(), 1u);
      BOOST_TEST(page2.rows[0].get_object()["sec64"].as<uint64_t>() == 5u);
      params.reverse = std::nullopt;
   }

   // ── Secondary index: empty result (bounds that match nothing) ────────
   {
      params.table = "numobjs"_n;
      params.key_type = "i64";
      params.index_position = "2";
      params.lower_bound = "100";
      params.upper_bound = "200";
      params.limit = 10;

      auto res = get_table_rows_full(plugin, params, fc::time_point::maximum());
      BOOST_REQUIRE_EQUAL(res.rows.size(), 0u);
      BOOST_TEST(res.more == false);
   }

} FC_LOG_AND_RETHROW() /// get_table_next_key_test

// ─────────────────────────────────────────────────────────────────────────────
// get_kv_rows tests — exercise the /v1/chain/get_kv_rows API endpoint
// using the test_kv_map contract (kv::raw_table format=0).
// ─────────────────────────────────────────────────────────────────────────────

static auto get_kv_rows_full = [](chain_apis::read_only& plugin,
                                  chain_apis::read_only::get_kv_rows_params& params,
                                  const fc::time_point& deadline) -> chain_apis::read_only::get_kv_rows_result {
   auto res = plugin.get_kv_rows(params, deadline)();
   BOOST_REQUIRE(!std::holds_alternative<fc::exception_ptr>(res));
   return std::get<chain_apis::read_only::get_kv_rows_result>(std::move(res));
};

// Helper: push a "put" action on the test_kv_map contract
static transaction_trace_ptr
kv_put(validating_tester& t, account_name contract,
       const std::string& region, uint64_t id,
       const std::string& payload, uint64_t amount) {
   return t.push_action(contract, "put"_n, contract,
                        mutable_variant_object()
                           ("region", region)("id", id)
                           ("payload", payload)("amount", amount));
}

BOOST_FIXTURE_TEST_CASE( get_kv_rows_basic_test, validating_tester ) try {
   produce_block();
   create_accounts({"kvtest"_n});
   produce_block();

   set_code("kvtest"_n, test_contracts::test_kv_map_wasm());
   set_abi("kvtest"_n, test_contracts::test_kv_map_abi());
   produce_block();

   // Insert 5 rows with distinct (region, id) keys.
   // BE key ordering: region string first (length-prefixed), then id (big-endian uint64).
   // Lexicographic sort: "asia" < "europe" < "us" (by string comparison),
   // within same region, lower id first.
   kv_put(*this, "kvtest"_n, "us",     1, "payload_us1",     100);
   kv_put(*this, "kvtest"_n, "europe", 1, "payload_europe1", 200);
   kv_put(*this, "kvtest"_n, "asia",   1, "payload_asia1",   300);
   kv_put(*this, "kvtest"_n, "asia",   2, "payload_asia2",   400);
   kv_put(*this, "kvtest"_n, "europe", 2, "payload_europe2", 500);
   produce_block();

   std::optional<sysio::chain_apis::tracked_votes> _tracked_votes;
   chain_apis::read_only plugin(*(this->control), {}, {}, _tracked_votes,
                                fc::microseconds::maximum(), fc::microseconds::maximum(), {});

   // ── (a) Basic query: get all rows, json=true ──
   {
      chain_apis::read_only::get_kv_rows_params p;
      p.json = true;
      p.code = "kvtest"_n;
      p.table = "geodata"_n;
      p.limit = 100;

      auto result = get_kv_rows_full(plugin, p, fc::time_point::maximum());
      BOOST_REQUIRE_EQUAL(5u, result.rows.size());
      BOOST_REQUIRE_EQUAL(false, result.more);

      // Verify ordering: asia/1, asia/2, europe/1, europe/2, us/1
      // Each row has "key" and "value" fields.
      auto check_row = [](const fc::variant& row,
                          const std::string& exp_region, uint64_t exp_id,
                          const std::string& exp_payload, uint64_t exp_amount) {
         auto key = row["key"].get_object();
         BOOST_REQUIRE_EQUAL(exp_region, key["region"].as_string());
         BOOST_REQUIRE_EQUAL(exp_id, key["id"].as_uint64());
         auto val = row["value"].get_object();
         BOOST_REQUIRE_EQUAL(exp_payload, val["payload"].as_string());
         BOOST_REQUIRE_EQUAL(exp_amount, val["amount"].as_uint64());
      };

      check_row(result.rows[0], "asia",   1, "payload_asia1",   300);
      check_row(result.rows[1], "asia",   2, "payload_asia2",   400);
      check_row(result.rows[2], "europe", 1, "payload_europe1", 200);
      check_row(result.rows[3], "europe", 2, "payload_europe2", 500);
      check_row(result.rows[4], "us",     1, "payload_us1",     100);
   }

   // ── (b) Pagination: limit=2 ──
   {
      chain_apis::read_only::get_kv_rows_params p;
      p.json = true;
      p.code = "kvtest"_n;
      p.table = "geodata"_n;
      p.limit = 2;

      // Page 1: first 2 rows
      auto page1 = get_kv_rows_full(plugin, p, fc::time_point::maximum());
      BOOST_REQUIRE_EQUAL(2u, page1.rows.size());
      BOOST_REQUIRE_EQUAL(true, page1.more);
      BOOST_REQUIRE(!page1.next_key.empty());
      BOOST_REQUIRE_EQUAL("asia", page1.rows[0]["key"].get_object()["region"].as_string());
      BOOST_REQUIRE_EQUAL(1u, page1.rows[0]["key"].get_object()["id"].as_uint64());
      BOOST_REQUIRE_EQUAL("asia", page1.rows[1]["key"].get_object()["region"].as_string());
      BOOST_REQUIRE_EQUAL(2u, page1.rows[1]["key"].get_object()["id"].as_uint64());

      // Page 2: use next_key as lower_bound
      p.lower_bound = page1.next_key;
      auto page2 = get_kv_rows_full(plugin, p, fc::time_point::maximum());
      BOOST_REQUIRE_EQUAL(2u, page2.rows.size());
      BOOST_REQUIRE_EQUAL(true, page2.more);
      BOOST_REQUIRE(!page2.next_key.empty());
      BOOST_REQUIRE_EQUAL("europe", page2.rows[0]["key"].get_object()["region"].as_string());
      BOOST_REQUIRE_EQUAL(1u, page2.rows[0]["key"].get_object()["id"].as_uint64());
      BOOST_REQUIRE_EQUAL("europe", page2.rows[1]["key"].get_object()["region"].as_string());
      BOOST_REQUIRE_EQUAL(2u, page2.rows[1]["key"].get_object()["id"].as_uint64());

      // Page 3: last row
      p.lower_bound = page2.next_key;
      auto page3 = get_kv_rows_full(plugin, p, fc::time_point::maximum());
      BOOST_REQUIRE_EQUAL(1u, page3.rows.size());
      BOOST_REQUIRE_EQUAL(false, page3.more);
      BOOST_REQUIRE_EQUAL("us", page3.rows[0]["key"].get_object()["region"].as_string());
      BOOST_REQUIRE_EQUAL(1u, page3.rows[0]["key"].get_object()["id"].as_uint64());
   }

   // ── (c) Lower/upper bound: get only "europe" rows ──
   {
      chain_apis::read_only::get_kv_rows_params p;
      p.json = true;
      p.code = "kvtest"_n;
      p.table = "geodata"_n;
      p.limit = 100;
      // lower_bound is inclusive, upper_bound is exclusive.
      // Set lower = europe/0 (before any europe entry), upper = europe+1 char.
      // Since bounds are JSON key objects when json=true, we use the key struct format.
      p.lower_bound = R"({"region":"europe","id":0})";
      // Use a region that sorts just after "europe" to capture all europe/* keys.
      // "europf" > "europe" lexicographically, id=0.
      p.upper_bound = R"({"region":"europf","id":0})";

      auto result = get_kv_rows_full(plugin, p, fc::time_point::maximum());
      BOOST_REQUIRE_EQUAL(2u, result.rows.size());
      BOOST_REQUIRE_EQUAL(false, result.more);
      BOOST_REQUIRE_EQUAL("europe", result.rows[0]["key"].get_object()["region"].as_string());
      BOOST_REQUIRE_EQUAL(1u, result.rows[0]["key"].get_object()["id"].as_uint64());
      BOOST_REQUIRE_EQUAL("europe", result.rows[1]["key"].get_object()["region"].as_string());
      BOOST_REQUIRE_EQUAL(2u, result.rows[1]["key"].get_object()["id"].as_uint64());
   }

   // ── (d) Reverse iteration ──
   {
      chain_apis::read_only::get_kv_rows_params p;
      p.json = true;
      p.code = "kvtest"_n;
      p.table = "geodata"_n;
      p.limit = 100;
      p.reverse = true;

      auto result = get_kv_rows_full(plugin, p, fc::time_point::maximum());
      BOOST_REQUIRE_EQUAL(5u, result.rows.size());
      BOOST_REQUIRE_EQUAL(false, result.more);

      // Reverse order: us/1, europe/2, europe/1, asia/2, asia/1
      BOOST_REQUIRE_EQUAL("us",     result.rows[0]["key"].get_object()["region"].as_string());
      BOOST_REQUIRE_EQUAL(1u,       result.rows[0]["key"].get_object()["id"].as_uint64());
      BOOST_REQUIRE_EQUAL("europe", result.rows[1]["key"].get_object()["region"].as_string());
      BOOST_REQUIRE_EQUAL(2u,       result.rows[1]["key"].get_object()["id"].as_uint64());
      BOOST_REQUIRE_EQUAL("europe", result.rows[2]["key"].get_object()["region"].as_string());
      BOOST_REQUIRE_EQUAL(1u,       result.rows[2]["key"].get_object()["id"].as_uint64());
      BOOST_REQUIRE_EQUAL("asia",   result.rows[3]["key"].get_object()["region"].as_string());
      BOOST_REQUIRE_EQUAL(2u,       result.rows[3]["key"].get_object()["id"].as_uint64());
      BOOST_REQUIRE_EQUAL("asia",   result.rows[4]["key"].get_object()["region"].as_string());
      BOOST_REQUIRE_EQUAL(1u,       result.rows[4]["key"].get_object()["id"].as_uint64());
   }

   // ── (e) Hex mode: json=false ──
   {
      chain_apis::read_only::get_kv_rows_params p;
      p.json = false;
      p.code = "kvtest"_n;
      p.table = "geodata"_n;
      p.limit = 100;

      auto result = get_kv_rows_full(plugin, p, fc::time_point::maximum());
      BOOST_REQUIRE_EQUAL(5u, result.rows.size());
      BOOST_REQUIRE_EQUAL(false, result.more);

      // In hex mode, key and value are hex-encoded strings (no JSON decode).
      for (const auto& row : result.rows) {
         // key and value should be strings (hex)
         BOOST_REQUIRE(row["key"].is_string());
         BOOST_REQUIRE(row["value"].is_string());
         // Hex strings contain only [0-9a-f]
         auto key_hex = row["key"].as_string();
         auto val_hex = row["value"].as_string();
         BOOST_REQUIRE(!key_hex.empty());
         BOOST_REQUIRE(!val_hex.empty());
         for (char c : key_hex) {
            BOOST_REQUIRE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
         }
      }
   }

   // ── (f) Empty table: query account with no KV data ──
   {
      create_accounts({"emptyacc"_n});
      produce_block();
      // Deploy the contract but don't push any data
      set_code("emptyacc"_n, test_contracts::test_kv_map_wasm());
      set_abi("emptyacc"_n, test_contracts::test_kv_map_abi());
      produce_block();

      chain_apis::read_only::get_kv_rows_params p;
      p.json = true;
      p.code = "emptyacc"_n;
      p.table = "geodata"_n;
      p.limit = 100;

      auto result = get_kv_rows_full(plugin, p, fc::time_point::maximum());
      BOOST_REQUIRE_EQUAL(0u, result.rows.size());
      BOOST_REQUIRE_EQUAL(false, result.more);
      BOOST_REQUIRE_EQUAL("", result.next_key);
   }

   // ── (g) Multi-member key ordering: same region, different ids ──
   {
      // The "asia" entries inserted above should sort by id within the region.
      // We also add more entries to test id ordering explicitly.
      create_accounts({"kvorder"_n});
      produce_block();
      set_code("kvorder"_n, test_contracts::test_kv_map_wasm());
      set_abi("kvorder"_n, test_contracts::test_kv_map_abi());
      produce_block();

      // Insert entries with same region, various ids (out of order)
      kv_put(*this, "kvorder"_n, "region1", 100, "p100", 1);
      kv_put(*this, "kvorder"_n, "region1",   5, "p5",   2);
      kv_put(*this, "kvorder"_n, "region1",  50, "p50",  3);
      kv_put(*this, "kvorder"_n, "region1",   1, "p1",   4);
      kv_put(*this, "kvorder"_n, "region2",   1, "p2_1", 5);
      produce_block();

      chain_apis::read_only::get_kv_rows_params p;
      p.json = true;
      p.code = "kvorder"_n;
      p.table = "geodata"_n;
      p.limit = 100;

      auto result = get_kv_rows_full(plugin, p, fc::time_point::maximum());
      BOOST_REQUIRE_EQUAL(5u, result.rows.size());

      // Expected order: region1/1, region1/5, region1/50, region1/100, region2/1
      BOOST_REQUIRE_EQUAL("region1", result.rows[0]["key"].get_object()["region"].as_string());
      BOOST_REQUIRE_EQUAL(1u,        result.rows[0]["key"].get_object()["id"].as_uint64());

      BOOST_REQUIRE_EQUAL("region1", result.rows[1]["key"].get_object()["region"].as_string());
      BOOST_REQUIRE_EQUAL(5u,        result.rows[1]["key"].get_object()["id"].as_uint64());

      BOOST_REQUIRE_EQUAL("region1", result.rows[2]["key"].get_object()["region"].as_string());
      BOOST_REQUIRE_EQUAL(50u,       result.rows[2]["key"].get_object()["id"].as_uint64());

      BOOST_REQUIRE_EQUAL("region1", result.rows[3]["key"].get_object()["region"].as_string());
      BOOST_REQUIRE_EQUAL(100u,      result.rows[3]["key"].get_object()["id"].as_uint64());

      BOOST_REQUIRE_EQUAL("region2", result.rows[4]["key"].get_object()["region"].as_string());
      BOOST_REQUIRE_EQUAL(1u,        result.rows[4]["key"].get_object()["id"].as_uint64());
   }

} FC_LOG_AND_RETHROW()

BOOST_FIXTURE_TEST_CASE( get_kv_rows_reverse_pagination_test, validating_tester ) try {
   produce_block();
   create_accounts({"kvrev"_n});
   produce_block();

   set_code("kvrev"_n, test_contracts::test_kv_map_wasm());
   set_abi("kvrev"_n, test_contracts::test_kv_map_abi());
   produce_block();

   kv_put(*this, "kvrev"_n, "a", 1, "val1", 10);
   kv_put(*this, "kvrev"_n, "b", 1, "val2", 20);
   kv_put(*this, "kvrev"_n, "c", 1, "val3", 30);
   kv_put(*this, "kvrev"_n, "d", 1, "val4", 40);
   kv_put(*this, "kvrev"_n, "e", 1, "val5", 50);
   produce_block();

   std::optional<sysio::chain_apis::tracked_votes> _tracked_votes;
   chain_apis::read_only plugin(*(this->control), {}, {}, _tracked_votes,
                                fc::microseconds::maximum(), fc::microseconds::maximum(), {});

   // Reverse pagination with limit=2
   chain_apis::read_only::get_kv_rows_params p;
   p.json = true;
   p.code = "kvrev"_n;
   p.table = "geodata"_n;
   p.limit = 2;
   p.reverse = true;

   // Page 1 (reverse): e/1, d/1
   auto page1 = get_kv_rows_full(plugin, p, fc::time_point::maximum());
   BOOST_REQUIRE_EQUAL(2u, page1.rows.size());
   BOOST_REQUIRE_EQUAL(true, page1.more);
   BOOST_REQUIRE(!page1.next_key.empty());
   BOOST_REQUIRE_EQUAL("e", page1.rows[0]["key"].get_object()["region"].as_string());
   BOOST_REQUIRE_EQUAL("d", page1.rows[1]["key"].get_object()["region"].as_string());

   // Page 2 (reverse): use next_key as upper_bound
   // next_key from page1 points to "c" (exclusive), so page2 gets b, a
   p.upper_bound = page1.next_key;
   auto page2 = get_kv_rows_full(plugin, p, fc::time_point::maximum());
   BOOST_REQUIRE_EQUAL(2u, page2.rows.size());
   BOOST_REQUIRE_EQUAL(false, page2.more); // only b, a remain
   BOOST_REQUIRE_EQUAL("b", page2.rows[0]["key"].get_object()["region"].as_string());
   BOOST_REQUIRE_EQUAL("a", page2.rows[1]["key"].get_object()["region"].as_string());

} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()

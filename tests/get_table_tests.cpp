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
      BOOST_REQUIRE_EQUAL(string("accounts"), result.rows[0].table);
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

   // No table filter — returns all tables for this code with resolved names
   param.table = name(0);
   param.lower_bound = "";
   param.upper_bound = "";
   param.limit = 100;
   result = plugin.read_only::get_table_by_scope(param, fc::time_point::maximum());
   // sysio.token has "accounts" table and "stat" table
   // Should have entries for both, ordered by (table_id, scope)
   BOOST_REQUIRE(result.rows.size() >= 5u);
   // Verify that table names are resolved from ABI — now that the response
   // uses string, all table names (including "currency_stats") should resolve.
   for (const auto& row : result.rows) {
      BOOST_REQUIRE(!row.table.empty());
      BOOST_REQUIRE(row.table == "accounts" || row.table == "stat" ||
                    row.table == "account" || row.table == "currency_stats");
   }

   // Unfiltered pagination — verify "more" token has colon format and
   // following the token returns remaining rows
   param.limit = 1;
   result = plugin.read_only::get_table_by_scope(param, fc::time_point::maximum());
   BOOST_REQUIRE_EQUAL(1u, result.rows.size());
   BOOST_REQUIRE(!result.more.empty());
   auto colon_pos = result.more.find(':');
   BOOST_REQUIRE(colon_pos != std::string::npos);
   // Follow the pagination token — should get remaining rows
   auto total_first = result.rows.size();
   param.lower_bound = result.more;
   param.limit = 100;
   result = plugin.read_only::get_table_by_scope(param, fc::time_point::maximum());
   BOOST_REQUIRE(result.rows.size() >= 1u);
   // Total across pages should cover at least the 5 "accounts" scopes
   BOOST_REQUIRE(total_first + result.rows.size() >= 5u);

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

// Regression test for AntelopeIO/spring#615: get_table_by_scope pagination must
// not loop infinitely when a scope has multiple tables and limit < total pairs.
// The fix: `more` returns "table:scope" tokens so pagination resumes at the
// correct (table, scope) pair, not back at the start of the scope.
BOOST_FIXTURE_TEST_CASE( get_scope_pagination_no_infinite_loop_test, validating_tester ) try {
   produce_block();

   create_accounts({ "sysio.token"_n, "sysio.ram"_n, "sysio.ramfee"_n, "sysio.stake"_n,
      "sysio.bpay"_n, "sysio.vpay"_n, "sysio.saving"_n, "sysio.names"_n });

   std::vector<account_name> accs{"inita"_n, "initb"_n, "initc"_n};
   create_accounts(accs);
   produce_block();

   set_code("sysio.token"_n, test_contracts::sysio_token_wasm());
   set_abi("sysio.token"_n, test_contracts::sysio_token_abi());
   set_privileged("sysio.token"_n);
   produce_block();

   // Create 2 currencies so "stat" table has 2 scopes, plus "accounts" table
   // has 4 scopes (3 user accounts + sysio). Total: 6+ (table, scope) pairs.
   push_action("sysio.token"_n, "create"_n, "sysio.token"_n, mutable_variant_object()
      ("issuer", "sysio")("maximum_supply", "1000000000.0000 SYS"));
   push_action("sysio.token"_n, "create"_n, "sysio.token"_n, mutable_variant_object()
      ("issuer", "sysio")("maximum_supply", "1000000000.0000 AAA"));
   for (account_name a: accs) {
      issue_tokens(*this, config::system_account_name, a, chain::asset::from_string("100.0000 SYS"));
      issue_tokens(*this, config::system_account_name, a, chain::asset::from_string("100.0000 AAA"));
   }
   produce_block();

   std::optional<sysio::chain_apis::tracked_votes> _tracked_votes;
   chain_apis::read_only plugin(*(this->control), {}, {}, _tracked_votes,
                                fc::microseconds::maximum(), fc::microseconds::maximum(), {});

   // Paginate through ALL (table, scope) pairs with limit=1, no table filter.
   // Collect all results. Verify:
   // 1. No duplicates
   // 2. Pagination terminates (doesn't loop)
   // 3. Total covers all expected pairs

   std::vector<std::pair<string, string>> all_pairs;  // (table, scope)
   string lower_bound;
   int iterations = 0;
   const int max_iterations = 50; // safety valve

   while (iterations < max_iterations) {
      sysio::chain_apis::read_only::get_table_by_scope_params param;
      param.code = "sysio.token"_n;
      param.lower_bound = lower_bound;
      param.limit = 1;
      auto result = plugin.get_table_by_scope(param, fc::time_point::maximum());

      for (const auto& row : result.rows) {
         all_pairs.emplace_back(row.table, row.scope.to_string());
      }

      if (result.more.empty()) break;

      // The pagination token must differ from what we sent, otherwise we loop forever
      BOOST_REQUIRE(result.more != lower_bound);
      lower_bound = result.more;
      ++iterations;
   }

   // Must have terminated, not hit the safety valve
   BOOST_REQUIRE(iterations < max_iterations);

   // Should have found at least: accounts×(inita, initb, initc, sysio) + stat×(SYS, AAA)
   BOOST_REQUIRE(all_pairs.size() >= 6u);

   // No duplicates
   auto sorted = all_pairs;
   std::sort(sorted.begin(), sorted.end());
   auto it = std::unique(sorted.begin(), sorted.end());
   BOOST_REQUIRE_EQUAL(std::distance(sorted.begin(), it), static_cast<ptrdiff_t>(all_pairs.size()));

} FC_LOG_AND_RETHROW()

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
   p.table = "accounts";
   p.json = true;
   auto result = get_table_rows_full(plugin, p, fc::time_point::maximum());

   BOOST_REQUIRE_EQUAL(4u, result.rows.size());
   BOOST_REQUIRE_EQUAL(false, result.more);
   if (result.rows.size() >= 4u) {
      BOOST_REQUIRE_EQUAL("9999.0000 AAA", result.rows[0]["value"]["balance"].as_string());
      BOOST_REQUIRE_EQUAL("8888.0000 BBB", result.rows[1]["value"]["balance"].as_string());
      BOOST_REQUIRE_EQUAL("7777.0000 CCC", result.rows[2]["value"]["balance"].as_string());
      BOOST_REQUIRE_EQUAL("10000.0000 SYS", result.rows[3]["value"]["balance"].as_string());
   }

   // get table: reverse ordered
   p.reverse = true;
   result = get_table_rows_full(plugin, p, fc::time_point::maximum());
   BOOST_REQUIRE_EQUAL(4u, result.rows.size());
   BOOST_REQUIRE_EQUAL(false, result.more);
   if (result.rows.size() >= 4) {
      BOOST_REQUIRE_EQUAL("9999.0000 AAA", result.rows[3]["value"]["balance"].as_string());
      BOOST_REQUIRE_EQUAL("8888.0000 BBB", result.rows[2]["value"]["balance"].as_string());
      BOOST_REQUIRE_EQUAL("7777.0000 CCC", result.rows[1]["value"]["balance"].as_string());
      BOOST_REQUIRE_EQUAL("10000.0000 SYS", result.rows[0]["value"]["balance"].as_string());
   }

   // get table: reverse ordered, with ram payer
   p.reverse = true;
   p.show_payer = true;
   result = get_table_rows_full(plugin, p, fc::time_point::maximum());
   BOOST_REQUIRE_EQUAL(4u, result.rows.size());
   BOOST_REQUIRE_EQUAL(false, result.more);
   if (result.rows.size() >= 4u) {
      BOOST_REQUIRE_EQUAL("9999.0000 AAA", result.rows[3]["value"]["balance"].as_string());
      BOOST_REQUIRE_EQUAL("8888.0000 BBB", result.rows[2]["value"]["balance"].as_string());
      BOOST_REQUIRE_EQUAL("7777.0000 CCC", result.rows[1]["value"]["balance"].as_string());
      BOOST_REQUIRE_EQUAL("10000.0000 SYS", result.rows[0]["value"]["balance"].as_string());
      // KV payer is the account specified in emplace(), not the contract.
      // sysio.token issues from system_account_name, so payer is "sysio".
      BOOST_REQUIRE_EQUAL("sysio", result.rows[0]["payer"].as_string());
      BOOST_REQUIRE_EQUAL("sysio", result.rows[1]["payer"].as_string());
      BOOST_REQUIRE_EQUAL("sysio", result.rows[2]["payer"].as_string());
      BOOST_REQUIRE_EQUAL("sysio", result.rows[3]["payer"].as_string());
   }
   p.show_payer = false;

   // get table: normal case, with limit
   p.lower_bound = p.upper_bound = "";
   p.limit = 1;
   p.reverse = false;
   result = get_table_rows_full(plugin, p, fc::time_point::maximum());
   BOOST_REQUIRE_EQUAL(1u, result.rows.size());
   BOOST_REQUIRE_EQUAL(true, result.more);
   if (result.rows.size() >= 1u) {
      BOOST_REQUIRE_EQUAL("9999.0000 AAA", result.rows[0]["value"]["balance"].as_string());
   }

   // get table: reverse case, with limit
   p.lower_bound = p.upper_bound = "";
   p.limit = 1;
   p.reverse = true;
   result = get_table_rows_full(plugin, p, fc::time_point::maximum());
   BOOST_REQUIRE_EQUAL(1u, result.rows.size());
   BOOST_REQUIRE_EQUAL(true, result.more);
   if (result.rows.size() >= 1u) {
      BOOST_REQUIRE_EQUAL("10000.0000 SYS", result.rows[0]["value"]["balance"].as_string());
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

   params.table = "numobjs";

   // Primary key query with pagination (key_names=["scope","primary_key"])
   // Scope is "test" (set above), so bounds are for the primary_key field only.
   params.lower_bound = R"({"primary_key":0})";

   auto res_1 = get_table_rows_full(plugin, params, fc::time_point::maximum());
   BOOST_REQUIRE(res_1.rows.size() > 0u);
   BOOST_TEST(res_1.rows[0].get_object()["value"].get_object()["key"].as<uint64_t>() == 0u);
   BOOST_TEST(!res_1.next_key.empty());
   params.lower_bound = res_1.next_key;
   auto more2_res_1 = get_table_rows_full(plugin, params, fc::time_point::maximum());
   BOOST_REQUIRE(more2_res_1.rows.size() > 0u);
   BOOST_TEST(more2_res_1.rows[0].get_object()["value"].get_object()["key"].as<uint64_t>() == 1u);

   // NOTE: Secondary index tests for get_table_test contract are disabled because
   // the contract's ABI does not declare secondary_indexes in the new KV format.
   // Secondary index queries through the unified get_table_rows API require
   // secondary_indexes to be declared in the ABI table definition.
   // See get_kv_rows_index_name_test for secondary index query tests using
   // the test_kv_map contract which has proper ABI secondary_indexes.

} FC_LOG_AND_RETHROW() /// get_table_next_key_test

// ─────────────────────────────────────────────────────────────────────────────
// get_table_rows KV tests — exercise the /v1/chain/get_table_rows API endpoint
// using the test_kv_map contract (kv::raw_table format=0).
// ─────────────────────────────────────────────────────────────────────────────

static auto get_table_rows_kv = [](chain_apis::read_only& plugin,
                                  chain_apis::read_only::get_table_rows_params& params,
                                  const fc::time_point& deadline) -> chain_apis::read_only::get_table_rows_result {
   auto res = plugin.get_table_rows(params, deadline)();
   BOOST_REQUIRE(!std::holds_alternative<fc::exception_ptr>(res));
   return std::get<chain_apis::read_only::get_table_rows_result>(std::move(res));
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
      chain_apis::read_only::get_table_rows_params p;
      p.json = true;
      p.code = "kvtest"_n;
      p.table = "geodata";
      p.limit = 100;

      auto result = get_table_rows_kv(plugin, p, fc::time_point::maximum());
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
      chain_apis::read_only::get_table_rows_params p;
      p.json = true;
      p.code = "kvtest"_n;
      p.table = "geodata";
      p.limit = 2;

      // Page 1: first 2 rows
      auto page1 = get_table_rows_kv(plugin, p, fc::time_point::maximum());
      BOOST_REQUIRE_EQUAL(2u, page1.rows.size());
      BOOST_REQUIRE_EQUAL(true, page1.more);
      BOOST_REQUIRE(!page1.next_key.empty());
      BOOST_REQUIRE_EQUAL("asia", page1.rows[0]["key"].get_object()["region"].as_string());
      BOOST_REQUIRE_EQUAL(1u, page1.rows[0]["key"].get_object()["id"].as_uint64());
      BOOST_REQUIRE_EQUAL("asia", page1.rows[1]["key"].get_object()["region"].as_string());
      BOOST_REQUIRE_EQUAL(2u, page1.rows[1]["key"].get_object()["id"].as_uint64());

      // Page 2: use next_key as lower_bound
      p.lower_bound = page1.next_key;
      auto page2 = get_table_rows_kv(plugin, p, fc::time_point::maximum());
      BOOST_REQUIRE_EQUAL(2u, page2.rows.size());
      BOOST_REQUIRE_EQUAL(true, page2.more);
      BOOST_REQUIRE(!page2.next_key.empty());
      BOOST_REQUIRE_EQUAL("europe", page2.rows[0]["key"].get_object()["region"].as_string());
      BOOST_REQUIRE_EQUAL(1u, page2.rows[0]["key"].get_object()["id"].as_uint64());
      BOOST_REQUIRE_EQUAL("europe", page2.rows[1]["key"].get_object()["region"].as_string());
      BOOST_REQUIRE_EQUAL(2u, page2.rows[1]["key"].get_object()["id"].as_uint64());

      // Page 3: last row
      p.lower_bound = page2.next_key;
      auto page3 = get_table_rows_kv(plugin, p, fc::time_point::maximum());
      BOOST_REQUIRE_EQUAL(1u, page3.rows.size());
      BOOST_REQUIRE_EQUAL(false, page3.more);
      BOOST_REQUIRE_EQUAL("us", page3.rows[0]["key"].get_object()["region"].as_string());
      BOOST_REQUIRE_EQUAL(1u, page3.rows[0]["key"].get_object()["id"].as_uint64());
   }

   // ── (c) Lower/upper bound: get only "europe" rows ──
   {
      chain_apis::read_only::get_table_rows_params p;
      p.json = true;
      p.code = "kvtest"_n;
      p.table = "geodata";
      p.limit = 100;
      // lower_bound is inclusive, upper_bound is exclusive.
      // Set lower = europe/0 (before any europe entry), upper = europe+1 char.
      // Since bounds are JSON key objects when json=true, we use the key struct format.
      p.lower_bound = R"({"region":"europe","id":0})";
      // Use a region that sorts just after "europe" to capture all europe/* keys.
      // "europf" > "europe" lexicographically, id=0.
      p.upper_bound = R"({"region":"europf","id":0})";

      auto result = get_table_rows_kv(plugin, p, fc::time_point::maximum());
      BOOST_REQUIRE_EQUAL(2u, result.rows.size());
      BOOST_REQUIRE_EQUAL(false, result.more);
      BOOST_REQUIRE_EQUAL("europe", result.rows[0]["key"].get_object()["region"].as_string());
      BOOST_REQUIRE_EQUAL(1u, result.rows[0]["key"].get_object()["id"].as_uint64());
      BOOST_REQUIRE_EQUAL("europe", result.rows[1]["key"].get_object()["region"].as_string());
      BOOST_REQUIRE_EQUAL(2u, result.rows[1]["key"].get_object()["id"].as_uint64());
   }

   // ── (d) Reverse iteration ──
   {
      chain_apis::read_only::get_table_rows_params p;
      p.json = true;
      p.code = "kvtest"_n;
      p.table = "geodata";
      p.limit = 100;
      p.reverse = true;

      auto result = get_table_rows_kv(plugin, p, fc::time_point::maximum());
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
      chain_apis::read_only::get_table_rows_params p;
      p.json = false;
      p.code = "kvtest"_n;
      p.table = "geodata";
      p.limit = 100;

      auto result = get_table_rows_kv(plugin, p, fc::time_point::maximum());
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

      chain_apis::read_only::get_table_rows_params p;
      p.json = true;
      p.code = "emptyacc"_n;
      p.table = "geodata";
      p.limit = 100;

      auto result = get_table_rows_kv(plugin, p, fc::time_point::maximum());
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

      chain_apis::read_only::get_table_rows_params p;
      p.json = true;
      p.code = "kvorder"_n;
      p.table = "geodata";
      p.limit = 100;

      auto result = get_table_rows_kv(plugin, p, fc::time_point::maximum());
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
   chain_apis::read_only::get_table_rows_params p;
   p.json = true;
   p.code = "kvrev"_n;
   p.table = "geodata";
   p.limit = 2;
   p.reverse = true;

   // Page 1 (reverse): e/1, d/1
   auto page1 = get_table_rows_kv(plugin, p, fc::time_point::maximum());
   BOOST_REQUIRE_EQUAL(2u, page1.rows.size());
   BOOST_REQUIRE_EQUAL(true, page1.more);
   BOOST_REQUIRE(!page1.next_key.empty());
   BOOST_REQUIRE_EQUAL("e", page1.rows[0]["key"].get_object()["region"].as_string());
   BOOST_REQUIRE_EQUAL("d", page1.rows[1]["key"].get_object()["region"].as_string());

   // Page 2 (reverse): use next_key as upper_bound
   // next_key from page1 points to "c" (exclusive), so page2 gets b, a
   p.upper_bound = page1.next_key;
   auto page2 = get_table_rows_kv(plugin, p, fc::time_point::maximum());
   BOOST_REQUIRE_EQUAL(2u, page2.rows.size());
   BOOST_REQUIRE_EQUAL(false, page2.more); // only b, a remain
   BOOST_REQUIRE_EQUAL("b", page2.rows[0]["key"].get_object()["region"].as_string());
   BOOST_REQUIRE_EQUAL("a", page2.rows[1]["key"].get_object()["region"].as_string());

} FC_LOG_AND_RETHROW()

// Test get_table_rows with index_name parameter — full secondary index query
BOOST_FIXTURE_TEST_CASE( get_kv_rows_index_name_test, validating_tester ) try {
   produce_block();
   create_accounts({"sectest"_n});
   produce_block();
   set_code("sectest"_n, test_contracts::test_kv_sec_query_wasm());
   set_abi("sectest"_n, test_contracts::test_kv_sec_query_abi());
   produce_block();

   // Add users: id=1 owner=alice bal=100, id=2 owner=bob bal=200, id=3 owner=alice bal=300
   push_action("sectest"_n, "adduser"_n, "sectest"_n, mutable_variant_object()("id", 1)("owner", "alice")("balance", 100));
   push_action("sectest"_n, "adduser"_n, "sectest"_n, mutable_variant_object()("id", 2)("owner", "bob")("balance", 200));
   push_action("sectest"_n, "adduser"_n, "sectest"_n, mutable_variant_object()("id", 3)("owner", "alice")("balance", 300));
   produce_block();

   std::optional<sysio::chain_apis::tracked_votes> _tracked_votes;
   chain_apis::read_only plugin(*(this->control), {}, {}, _tracked_votes,
                                fc::microseconds::maximum(), fc::microseconds::maximum(), {});

   // (a) Primary query — all 3 rows
   {
      chain_apis::read_only::get_table_rows_params p;
      p.json = false;
      p.code = "sectest"_n;
      p.table = "users";
      auto result = get_table_rows_kv(plugin, p, fc::time_point::maximum());
      BOOST_CHECK_EQUAL(result.rows.size(), 3u);
   }

   // (b) Secondary index query by owner — should return rows for matching owner
   {
      chain_apis::read_only::get_table_rows_params p;
      p.json = false;
      p.code = "sectest"_n;
      p.table = "users";
      p.index_name = "byowner";
      auto result = get_table_rows_kv(plugin, p, fc::time_point::maximum());
      // All 3 rows should be returned (alice, alice, bob — sorted by sec key)
      BOOST_CHECK_EQUAL(result.rows.size(), 3u);
   }

   // (c) Non-existent index name — should throw
   {
      chain_apis::read_only::get_table_rows_params p;
      p.json = false;
      p.code = "sectest"_n;
      p.table = "users";
      p.index_name = "nonexistent";
      BOOST_CHECK_THROW(
         get_table_rows_kv(plugin, p, fc::time_point::maximum()),
         chain::contract_table_query_exception
      );
   }

   // (d) Secondary query with bounds — filter to specific owner (hex mode)
   {
      chain_apis::read_only::get_table_rows_params p;
      p.json = false;
      p.code = "sectest"_n;
      p.table = "users";
      p.index_name = "byowner";
      char alice_be[8]; chain::kv_encode_be64(alice_be, name("alice").to_uint64_t());
      p.lower_bound = fc::to_hex(alice_be, 8);
      uint64_t alice_plus_one = name("alice").to_uint64_t() + 1;
      char alice_ub[8]; chain::kv_encode_be64(alice_ub, alice_plus_one);
      p.upper_bound = fc::to_hex(alice_ub, 8);
      auto result = get_table_rows_kv(plugin, p, fc::time_point::maximum());
      BOOST_CHECK_EQUAL(result.rows.size(), 2u);
   }

   // (e) json=true primary query — verify ABI-decoded output
   {
      chain_apis::read_only::get_table_rows_params p;
      p.json = true;
      p.code = "sectest"_n;
      p.table = "users";
      auto result = get_table_rows_kv(plugin, p, fc::time_point::maximum());
      BOOST_REQUIRE_EQUAL(result.rows.size(), 3u);
      // Rows should have "key" and "value" fields
      // value should be ABI-decoded with owner and balance fields
      auto& v0 = result.rows[0].get_object();
      BOOST_CHECK(v0.contains("value"));
      auto& val = v0["value"].get_object();
      BOOST_CHECK(val.contains("owner"));
      BOOST_CHECK(val.contains("balance"));
   }

   // (f) json=true secondary query — verify results
   {
      chain_apis::read_only::get_table_rows_params p;
      p.json = true;
      p.code = "sectest"_n;
      p.table = "users";
      p.index_name = "byowner";
      auto result = get_table_rows_kv(plugin, p, fc::time_point::maximum());
      BOOST_REQUIRE_EQUAL(result.rows.size(), 3u);
      // Verify ABI-decoded values are present
      for (auto& row : result.rows) {
         auto& obj = row.get_object();
         BOOST_CHECK(obj.contains("value"));
         if (obj["value"].is_object()) {
            auto& val = obj["value"].get_object();
            BOOST_CHECK(val.contains("owner"));
            BOOST_CHECK(val.contains("balance"));
         }
      }
   }

   // (g) json=true secondary query with JSON bounds — filter to bob
   {
      chain_apis::read_only::get_table_rows_params p;
      p.json = true;
      p.code = "sectest"_n;
      p.table = "users";
      p.index_name = "byowner";
      // JSON bounds: key object with the index field name and value
      p.lower_bound = R"({"byowner": "bob"})";
      p.upper_bound = R"({"byowner": "boc"})";
      auto result = get_table_rows_kv(plugin, p, fc::time_point::maximum());
      // Bob has exactly 1 row (id=2, balance=200)
      BOOST_REQUIRE_EQUAL(result.rows.size(), 1u);
      auto& val = result.rows[0].get_object()["value"];
      if (val.is_object()) {
         BOOST_CHECK_EQUAL(val.get_object()["owner"].as_string(), "bob");
         BOOST_CHECK_EQUAL(val.get_object()["balance"].as_uint64(), 200u);
      }
   }

   // (h) json=true secondary query with JSON bounds — filter to alice (2 results)
   {
      chain_apis::read_only::get_table_rows_params p;
      p.json = true;
      p.code = "sectest"_n;
      p.table = "users";
      p.index_name = "byowner";
      p.lower_bound = R"({"byowner": "alice"})";
      p.upper_bound = R"({"byowner": "alicf"})";
      auto result = get_table_rows_kv(plugin, p, fc::time_point::maximum());
      BOOST_REQUIRE_EQUAL(result.rows.size(), 2u);
   }

   // (i) reverse=true secondary query — all rows in reverse order
   {
      chain_apis::read_only::get_table_rows_params p;
      p.json = true;
      p.code = "sectest"_n;
      p.table = "users";
      p.index_name = "byowner";
      p.reverse = true;
      auto result = get_table_rows_kv(plugin, p, fc::time_point::maximum());
      BOOST_REQUIRE_EQUAL(result.rows.size(), 3u);
      // Reverse order: bob first (higher name value), then alice entries
      auto& first_val = result.rows[0].get_object()["value"];
      if (first_val.is_object()) {
         BOOST_CHECK_EQUAL(first_val.get_object()["owner"].as_string(), "bob");
      }
   }

   // (j) reverse=true secondary with bounds — alice only, reversed
   {
      chain_apis::read_only::get_table_rows_params p;
      p.json = true;
      p.code = "sectest"_n;
      p.table = "users";
      p.index_name = "byowner";
      p.reverse = true;
      p.lower_bound = R"({"byowner": "alice"})";
      p.upper_bound = R"({"byowner": "alicf"})";
      auto result = get_table_rows_kv(plugin, p, fc::time_point::maximum());
      BOOST_REQUIRE_EQUAL(result.rows.size(), 2u);
   }

} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// New unified get_table_rows feature tests
// ---------------------------------------------------------------------------

// Test `find` exact lookup on scoped token table
BOOST_FIXTURE_TEST_CASE( get_table_find_test, validating_tester ) try {
   produce_block();
   create_accounts({ "sysio.token"_n, "sysio.ram"_n, "sysio.ramfee"_n, "sysio.stake"_n,
      "sysio.bpay"_n, "sysio.vpay"_n, "sysio.saving"_n, "sysio.names"_n });
   create_accounts({"inita"_n});
   produce_block();

   set_code("sysio.token"_n, test_contracts::sysio_token_wasm());
   set_abi("sysio.token"_n, test_contracts::sysio_token_abi());
   set_privileged("sysio.token"_n);
   produce_block();

   push_action("sysio.token"_n, "create"_n, "sysio.token"_n, mutable_variant_object()
      ("issuer", "sysio")("maximum_supply", "1000000000.0000 SYS"));
   push_action("sysio.token"_n, "create"_n, "sysio.token"_n, mutable_variant_object()
      ("issuer", "sysio")("maximum_supply", "1000000000.0000 AAA"));
   issue_tokens(*this, config::system_account_name, "inita"_n, chain::asset::from_string("100.0000 SYS"));
   issue_tokens(*this, config::system_account_name, "inita"_n, chain::asset::from_string("200.0000 AAA"));
   produce_block();

   std::optional<sysio::chain_apis::tracked_votes> _tracked_votes;
   chain_apis::read_only plugin(*(this->control), {}, {}, _tracked_votes,
                                fc::microseconds::maximum(), fc::microseconds::maximum(), {});

   // (a) find by exact primary key within scope — first verify what keys exist
   {
      // First, list all rows to see the key format
      chain_apis::read_only::get_table_rows_params p;
      p.json = true;
      p.code = "sysio.token"_n;
      p.table = "accounts";
      p.scope = "inita";
      p.limit = 10;
      auto all = get_table_rows_full(plugin, p, fc::time_point::maximum());
      BOOST_REQUIRE(all.rows.size() >= 1u);
      // Get the key of the first row and use it as the find value
      auto first_key = fc::json::to_string(all.rows[0]["key"], fc::time_point::maximum());

      // Now find that exact key
      p.lower_bound = p.upper_bound = "";
      p.limit = 50;
      p.find = first_key;
      auto result = get_table_rows_full(plugin, p, fc::time_point::maximum());
      BOOST_REQUIRE_EQUAL(1u, result.rows.size());
   }

   // (b) find with nonexistent key — should return 0 rows
   {
      chain_apis::read_only::get_table_rows_params p;
      p.json = true;
      p.code = "sysio.token"_n;
      p.table = "accounts";
      p.scope = "inita";
      p.find = R"({"primary_key": 9999999})";
      auto result = get_table_rows_full(plugin, p, fc::time_point::maximum());
      BOOST_REQUIRE_EQUAL(0u, result.rows.size());
   }

   // (c) find + lower_bound should error
   {
      chain_apis::read_only::get_table_rows_params p;
      p.json = true;
      p.code = "sysio.token"_n;
      p.table = "accounts";
      p.scope = "inita";
      p.find = R"({"primary_key": 1397703940})";
      p.lower_bound = R"({"primary_key": 0})";
      BOOST_CHECK_THROW(
         get_table_rows_full(plugin, p, fc::time_point::maximum()),
         chain::contract_table_query_exception
      );
   }

} FC_LOG_AND_RETHROW()

// Test scope isolation: same table, different scopes return different data
BOOST_FIXTURE_TEST_CASE( get_table_scope_isolation_test, validating_tester ) try {
   produce_block();
   create_accounts({ "sysio.token"_n, "sysio.ram"_n, "sysio.ramfee"_n, "sysio.stake"_n,
      "sysio.bpay"_n, "sysio.vpay"_n, "sysio.saving"_n, "sysio.names"_n });
   create_accounts({"inita"_n, "initb"_n});
   produce_block();

   set_code("sysio.token"_n, test_contracts::sysio_token_wasm());
   set_abi("sysio.token"_n, test_contracts::sysio_token_abi());
   set_privileged("sysio.token"_n);
   produce_block();

   push_action("sysio.token"_n, "create"_n, "sysio.token"_n, mutable_variant_object()
      ("issuer", "sysio")("maximum_supply", "1000000000.0000 SYS"));
   issue_tokens(*this, config::system_account_name, "inita"_n, chain::asset::from_string("100.0000 SYS"));
   issue_tokens(*this, config::system_account_name, "initb"_n, chain::asset::from_string("200.0000 SYS"));
   produce_block();

   std::optional<sysio::chain_apis::tracked_votes> _tracked_votes;
   chain_apis::read_only plugin(*(this->control), {}, {}, _tracked_votes,
                                fc::microseconds::maximum(), fc::microseconds::maximum(), {});

   // (a) scope=inita should see only inita's balance
   {
      chain_apis::read_only::get_table_rows_params p;
      p.json = true;
      p.code = "sysio.token"_n;
      p.table = "accounts";
      p.scope = "inita";
      auto result = get_table_rows_full(plugin, p, fc::time_point::maximum());
      BOOST_REQUIRE_EQUAL(1u, result.rows.size());
      BOOST_REQUIRE_EQUAL("100.0000 SYS", result.rows[0]["value"]["balance"].as_string());
   }

   // (b) scope=initb should see only initb's balance
   {
      chain_apis::read_only::get_table_rows_params p;
      p.json = true;
      p.code = "sysio.token"_n;
      p.table = "accounts";
      p.scope = "initb";
      auto result = get_table_rows_full(plugin, p, fc::time_point::maximum());
      BOOST_REQUIRE_EQUAL(1u, result.rows.size());
      BOOST_REQUIRE_EQUAL("200.0000 SYS", result.rows[0]["value"]["balance"].as_string());
   }

   // (c) empty scope on unscoped table (kv::table) should work
   {
      chain_apis::read_only::get_table_rows_params p;
      p.json = true;
      p.code = "sysio.token"_n;
      p.table = "stat";
      // stat table is scoped by symbol code in kv_multi_index, but let's test
      // that omitting scope queries ALL scopes
      p.scope = "";
      auto result = get_table_rows_full(plugin, p, fc::time_point::maximum());
      // Should find the SYS stat row (scope = SYS symbol code)
      BOOST_REQUIRE(result.rows.size() >= 1u);
   }

} FC_LOG_AND_RETHROW()

// Test `find` on unscoped kv::table
BOOST_FIXTURE_TEST_CASE( get_table_find_unscoped_test, validating_tester ) try {
   produce_block();
   create_accounts({"kvtest"_n});
   produce_block();

   set_code("kvtest"_n, test_contracts::test_kv_map_wasm());
   set_abi("kvtest"_n, test_contracts::test_kv_map_abi());
   produce_block();

   kv_put(*this, "kvtest"_n, "us", 1, "payload_us1", 100);
   kv_put(*this, "kvtest"_n, "europe", 1, "payload_europe1", 200);
   produce_block();

   std::optional<sysio::chain_apis::tracked_votes> _tracked_votes;
   chain_apis::read_only plugin(*(this->control), {}, {}, _tracked_votes,
                                fc::microseconds::maximum(), fc::microseconds::maximum(), {});

   // find exact key on unscoped table
   {
      chain_apis::read_only::get_table_rows_params p;
      p.json = true;
      p.code = "kvtest"_n;
      p.table = "geodata";
      p.find = R"({"region":"us","id":1})";
      auto result = get_table_rows_kv(plugin, p, fc::time_point::maximum());
      BOOST_REQUIRE_EQUAL(1u, result.rows.size());
      BOOST_REQUIRE_EQUAL("payload_us1", result.rows[0]["value"]["payload"].as_string());
   }

   // find nonexistent key
   {
      chain_apis::read_only::get_table_rows_params p;
      p.json = true;
      p.code = "kvtest"_n;
      p.table = "geodata";
      p.find = R"({"region":"nowhere","id":99})";
      auto result = get_table_rows_kv(plugin, p, fc::time_point::maximum());
      BOOST_REQUIRE_EQUAL(0u, result.rows.size());
   }

} FC_LOG_AND_RETHROW()

// Test index_name with numeric position
BOOST_FIXTURE_TEST_CASE( get_table_numeric_index_test, validating_tester ) try {
   produce_block();
   create_accounts({"sectest"_n});
   produce_block();

   set_code("sectest"_n, test_contracts::test_kv_sec_query_wasm());
   set_abi("sectest"_n, test_contracts::test_kv_sec_query_abi());
   produce_block();

   push_action("sectest"_n, "adduser"_n, "sectest"_n, mutable_variant_object()("id", 1)("owner", "alice")("balance", 100));
   push_action("sectest"_n, "adduser"_n, "sectest"_n, mutable_variant_object()("id", 2)("owner", "bob")("balance", 200));
   push_action("sectest"_n, "adduser"_n, "sectest"_n, mutable_variant_object()("id", 3)("owner", "alice")("balance", 300));
   produce_block();

   std::optional<sysio::chain_apis::tracked_votes> _tracked_votes;
   chain_apis::read_only plugin(*(this->control), {}, {}, _tracked_votes,
                                fc::microseconds::maximum(), fc::microseconds::maximum(), {});

   // Query by numeric index position "2" (= first secondary index)
   // Should give same results as querying by name "byowner"
   {
      chain_apis::read_only::get_table_rows_params p_name;
      p_name.json = false;
      p_name.code = "sectest"_n;
      p_name.table = "users";
      p_name.index_name = "byowner";
      auto result_name = get_table_rows_kv(plugin, p_name, fc::time_point::maximum());

      chain_apis::read_only::get_table_rows_params p_num;
      p_num.json = false;
      p_num.code = "sectest"_n;
      p_num.table = "users";
      p_num.index_name = "2";
      auto result_num = get_table_rows_kv(plugin, p_num, fc::time_point::maximum());

      BOOST_REQUIRE_EQUAL(result_name.rows.size(), result_num.rows.size());
   }

} FC_LOG_AND_RETHROW()

// Test next_key pagination with scope (next_key should be scope-stripped)
BOOST_FIXTURE_TEST_CASE( get_table_scope_pagination_test, validating_tester ) try {
   produce_block();
   create_accounts({ "sysio.token"_n, "sysio.ram"_n, "sysio.ramfee"_n, "sysio.stake"_n,
      "sysio.bpay"_n, "sysio.vpay"_n, "sysio.saving"_n, "sysio.names"_n });
   create_accounts({"inita"_n});
   produce_block();

   set_code("sysio.token"_n, test_contracts::sysio_token_wasm());
   set_abi("sysio.token"_n, test_contracts::sysio_token_abi());
   set_privileged("sysio.token"_n);
   produce_block();

   push_action("sysio.token"_n, "create"_n, "sysio.token"_n, mutable_variant_object()
      ("issuer", "sysio")("maximum_supply", "1000000000.0000 SYS"));
   push_action("sysio.token"_n, "create"_n, "sysio.token"_n, mutable_variant_object()
      ("issuer", "sysio")("maximum_supply", "1000000000.0000 AAA"));
   push_action("sysio.token"_n, "create"_n, "sysio.token"_n, mutable_variant_object()
      ("issuer", "sysio")("maximum_supply", "1000000000.0000 BBB"));
   issue_tokens(*this, config::system_account_name, "inita"_n, chain::asset::from_string("100.0000 SYS"));
   issue_tokens(*this, config::system_account_name, "inita"_n, chain::asset::from_string("200.0000 AAA"));
   issue_tokens(*this, config::system_account_name, "inita"_n, chain::asset::from_string("300.0000 BBB"));
   produce_block();

   std::optional<sysio::chain_apis::tracked_votes> _tracked_votes;
   chain_apis::read_only plugin(*(this->control), {}, {}, _tracked_votes,
                                fc::microseconds::maximum(), fc::microseconds::maximum(), {});

   // Page 1: limit=1
   chain_apis::read_only::get_table_rows_params p;
   p.json = true;
   p.code = "sysio.token"_n;
   p.table = "accounts";
   p.scope = "inita";
   p.limit = 1;
   auto result = get_table_rows_full(plugin, p, fc::time_point::maximum());
   BOOST_REQUIRE_EQUAL(1u, result.rows.size());
   BOOST_REQUIRE_EQUAL(true, result.more);
   // next_key should NOT contain scope — it's the within-scope key
   BOOST_REQUIRE(!result.next_key.empty());

   // Page 2: use next_key as lower_bound
   p.lower_bound = result.next_key;
   result = get_table_rows_full(plugin, p, fc::time_point::maximum());
   BOOST_REQUIRE_EQUAL(1u, result.rows.size());
   BOOST_REQUIRE_EQUAL(true, result.more);

   // Page 3: last page
   p.lower_bound = result.next_key;
   result = get_table_rows_full(plugin, p, fc::time_point::maximum());
   BOOST_REQUIRE_EQUAL(1u, result.rows.size());
   BOOST_REQUIRE_EQUAL(false, result.more);

} FC_LOG_AND_RETHROW()

// Regression test for AntelopeIO/spring#1379: scope type ambiguity.
// When the ABI declares scope type as "name", the scope string must be
// parsed as a name (not a raw uint64). Verify that querying with a name
// scope returns the correct data, and that the response's next_key
// round-trips correctly.
BOOST_FIXTURE_TEST_CASE( get_table_scope_type_from_abi_test, validating_tester ) try {
   produce_block();
   create_accounts({ "sysio.token"_n, "sysio.ram"_n, "sysio.ramfee"_n, "sysio.stake"_n,
      "sysio.bpay"_n, "sysio.vpay"_n, "sysio.saving"_n, "sysio.names"_n });
   // Account name "11111" is valid (maps to a specific uint64).
   // If parsed as a raw uint64, "11111" = 11111 (decimal) — a completely
   // different value than name("11111").to_uint64_t(). The ABI type
   // must be used to disambiguate.
   create_accounts({"11111"_n, "inita"_n});
   produce_block();

   set_code("sysio.token"_n, test_contracts::sysio_token_wasm());
   set_abi("sysio.token"_n, test_contracts::sysio_token_abi());
   set_privileged("sysio.token"_n);
   produce_block();

   push_action("sysio.token"_n, "create"_n, "sysio.token"_n, mutable_variant_object()
      ("issuer", "sysio")("maximum_supply", "1000000000.0000 SYS"));
   issue_tokens(*this, config::system_account_name, "11111"_n, chain::asset::from_string("100.0000 SYS"));
   issue_tokens(*this, config::system_account_name, "inita"_n, chain::asset::from_string("200.0000 SYS"));
   produce_block();

   std::optional<sysio::chain_apis::tracked_votes> _tracked_votes;
   chain_apis::read_only plugin(*(this->control), {}, {}, _tracked_votes,
                                fc::microseconds::maximum(), fc::microseconds::maximum(), {});

   // Query scope="11111" — must be parsed as name, not uint64
   {
      chain_apis::read_only::get_table_rows_params p;
      p.json = true;
      p.code = "sysio.token"_n;
      p.table = "accounts";
      p.scope = "11111";  // ambiguous: name("11111") vs uint64(11111)
      auto result = get_table_rows_full(plugin, p, fc::time_point::maximum());
      // Should find exactly 1 row for account "11111"
      BOOST_REQUIRE_EQUAL(1u, result.rows.size());
      BOOST_REQUIRE_EQUAL("100.0000 SYS", result.rows[0]["value"]["balance"].as_string());
   }

   // Verify the other scope works independently
   {
      chain_apis::read_only::get_table_rows_params p;
      p.json = true;
      p.code = "sysio.token"_n;
      p.table = "accounts";
      p.scope = "inita";
      auto result = get_table_rows_full(plugin, p, fc::time_point::maximum());
      BOOST_REQUIRE_EQUAL(1u, result.rows.size());
      BOOST_REQUIRE_EQUAL("200.0000 SYS", result.rows[0]["value"]["balance"].as_string());
   }

} FC_LOG_AND_RETHROW()

// Test show_payer with new {key, value, payer} format on unscoped table
BOOST_FIXTURE_TEST_CASE( get_table_show_payer_kv_test, validating_tester ) try {
   produce_block();
   create_accounts({"kvtest"_n});
   produce_block();

   set_code("kvtest"_n, test_contracts::test_kv_map_wasm());
   set_abi("kvtest"_n, test_contracts::test_kv_map_abi());
   produce_block();

   kv_put(*this, "kvtest"_n, "us", 1, "payload_us1", 100);
   produce_block();

   std::optional<sysio::chain_apis::tracked_votes> _tracked_votes;
   chain_apis::read_only plugin(*(this->control), {}, {}, _tracked_votes,
                                fc::microseconds::maximum(), fc::microseconds::maximum(), {});

   // show_payer=true should include payer field
   {
      chain_apis::read_only::get_table_rows_params p;
      p.json = true;
      p.code = "kvtest"_n;
      p.table = "geodata";
      p.show_payer = true;
      auto result = get_table_rows_kv(plugin, p, fc::time_point::maximum());
      BOOST_REQUIRE_EQUAL(1u, result.rows.size());
      BOOST_REQUIRE(result.rows[0].get_object().contains("payer"));
      BOOST_REQUIRE_EQUAL("kvtest", result.rows[0]["payer"].as_string());
   }

   // show_payer=false (default) should NOT include payer field
   {
      chain_apis::read_only::get_table_rows_params p;
      p.json = true;
      p.code = "kvtest"_n;
      p.table = "geodata";
      auto result = get_table_rows_kv(plugin, p, fc::time_point::maximum());
      BOOST_REQUIRE_EQUAL(1u, result.rows.size());
      BOOST_REQUIRE(!result.rows[0].get_object().contains("payer"));
   }

} FC_LOG_AND_RETHROW()

// Gap 1: scope="0" edge case — should query scope=0 (name{}), not "unscoped"
BOOST_FIXTURE_TEST_CASE( get_table_scope_zero_test, validating_tester ) try {
   produce_block();
   create_accounts({ "sysio.token"_n, "sysio.ram"_n, "sysio.ramfee"_n, "sysio.stake"_n,
      "sysio.bpay"_n, "sysio.vpay"_n, "sysio.saving"_n, "sysio.names"_n });
   produce_block();

   set_code("sysio.token"_n, test_contracts::sysio_token_wasm());
   set_abi("sysio.token"_n, test_contracts::sysio_token_abi());
   set_privileged("sysio.token"_n);
   produce_block();

   push_action("sysio.token"_n, "create"_n, "sysio.token"_n, mutable_variant_object()
      ("issuer", "sysio")("maximum_supply", "1000000000.0000 SYS"));
   create_accounts({"inita"_n});
   produce_block();
   issue_tokens(*this, config::system_account_name, "inita"_n,
                chain::asset::from_string("100.0000 SYS"));
   produce_block();

   std::optional<sysio::chain_apis::tracked_votes> _tracked_votes;
   chain_apis::read_only plugin(*(this->control), {}, {}, _tracked_votes,
                                fc::microseconds::maximum(), fc::microseconds::maximum(), {});

   // scope="0" is parsed as uint64(0) = name{} via convert_to_type fallback.
   // No account has scope=0 in sysio.token, so expect 0 rows (not an error).
   {
      chain_apis::read_only::get_table_rows_params p;
      p.json = true;
      p.code = "sysio.token"_n;
      p.table = "accounts";
      p.scope = "0";
      auto result = get_table_rows_full(plugin, p, fc::time_point::maximum());
      BOOST_REQUIRE_EQUAL(0u, result.rows.size());
   }

   // scope="" (empty) means unscoped — returns all scopes' data
   {
      chain_apis::read_only::get_table_rows_params p;
      p.json = true;
      p.code = "sysio.token"_n;
      p.table = "accounts";
      p.scope = "";
      auto result = get_table_rows_full(plugin, p, fc::time_point::maximum());
      // Should find at least inita's rows
      BOOST_REQUIRE(result.rows.size() >= 1u);
   }

   // scope="inita" should find inita's balance
   {
      chain_apis::read_only::get_table_rows_params p;
      p.json = true;
      p.code = "sysio.token"_n;
      p.table = "accounts";
      p.scope = "inita";
      auto result = get_table_rows_full(plugin, p, fc::time_point::maximum());
      BOOST_REQUIRE_EQUAL(1u, result.rows.size());
      BOOST_REQUIRE_EQUAL("100.0000 SYS", result.rows[0]["value"]["balance"].as_string());
   }

} FC_LOG_AND_RETHROW()

// Gap 2: find + upper_bound should error (just like find + lower_bound)
BOOST_FIXTURE_TEST_CASE( get_table_find_upper_bound_error_test, validating_tester ) try {
   produce_block();
   create_accounts({"kvtest"_n});
   produce_block();

   set_code("kvtest"_n, test_contracts::test_kv_map_wasm());
   set_abi("kvtest"_n, test_contracts::test_kv_map_abi());
   produce_block();

   kv_put(*this, "kvtest"_n, "us", 1, "payload_us1", 100);
   produce_block();

   std::optional<sysio::chain_apis::tracked_votes> _tracked_votes;
   chain_apis::read_only plugin(*(this->control), {}, {}, _tracked_votes,
                                fc::microseconds::maximum(), fc::microseconds::maximum(), {});

   // find + upper_bound should error
   {
      chain_apis::read_only::get_table_rows_params p;
      p.json = true;
      p.code = "kvtest"_n;
      p.table = "geodata";
      p.find = R"({"region":"us","id":1})";
      p.upper_bound = R"({"region":"z","id":999})";
      BOOST_CHECK_THROW(
         get_table_rows_kv(plugin, p, fc::time_point::maximum()),
         chain::contract_table_query_exception
      );
   }

   // find + both bounds should error
   {
      chain_apis::read_only::get_table_rows_params p;
      p.json = true;
      p.code = "kvtest"_n;
      p.table = "geodata";
      p.find = R"({"region":"us","id":1})";
      p.lower_bound = R"({"region":"a","id":0})";
      p.upper_bound = R"({"region":"z","id":999})";
      BOOST_CHECK_THROW(
         get_table_rows_kv(plugin, p, fc::time_point::maximum()),
         chain::contract_table_query_exception
      );
   }

} FC_LOG_AND_RETHROW()

// Gap 3: invalid index_name should error
BOOST_FIXTURE_TEST_CASE( get_table_invalid_index_name_test, validating_tester ) try {
   produce_block();
   create_accounts({"sectest"_n});
   produce_block();

   set_code("sectest"_n, test_contracts::test_kv_sec_query_wasm());
   set_abi("sectest"_n, test_contracts::test_kv_sec_query_abi());
   produce_block();

   std::optional<sysio::chain_apis::tracked_votes> _tracked_votes;
   chain_apis::read_only plugin(*(this->control), {}, {}, _tracked_votes,
                                fc::microseconds::maximum(), fc::microseconds::maximum(), {});

   // Named index that doesn't exist
   {
      chain_apis::read_only::get_table_rows_params p;
      p.json = false;
      p.code = "sectest"_n;
      p.table = "users";
      p.index_name = "nonexistent";
      BOOST_CHECK_THROW(
         get_table_rows_kv(plugin, p, fc::time_point::maximum()),
         chain::contract_table_query_exception
      );
   }

   // Numeric position out of range (e.g., "99")
   {
      chain_apis::read_only::get_table_rows_params p;
      p.json = false;
      p.code = "sectest"_n;
      p.table = "users";
      p.index_name = "99";
      BOOST_CHECK_THROW(
         get_table_rows_kv(plugin, p, fc::time_point::maximum()),
         chain::contract_table_query_exception
      );
   }

} FC_LOG_AND_RETHROW()

// Gap 4: Secondary index query with scope (scoped table + secondary index)
BOOST_FIXTURE_TEST_CASE( get_table_scoped_secondary_test, validating_tester ) try {
   produce_block();
   create_accounts({ "sysio.token"_n, "sysio.ram"_n, "sysio.ramfee"_n, "sysio.stake"_n,
      "sysio.bpay"_n, "sysio.vpay"_n, "sysio.saving"_n, "sysio.names"_n });
   create_accounts({"inita"_n, "initb"_n});
   produce_block();

   set_code("sysio.token"_n, test_contracts::sysio_token_wasm());
   set_abi("sysio.token"_n, test_contracts::sysio_token_abi());
   set_privileged("sysio.token"_n);
   produce_block();

   push_action("sysio.token"_n, "create"_n, "sysio.token"_n, mutable_variant_object()
      ("issuer", "sysio")("maximum_supply", "1000000000.0000 SYS"));
   push_action("sysio.token"_n, "create"_n, "sysio.token"_n, mutable_variant_object()
      ("issuer", "sysio")("maximum_supply", "1000000000.0000 AAA"));
   push_action("sysio.token"_n, "create"_n, "sysio.token"_n, mutable_variant_object()
      ("issuer", "sysio")("maximum_supply", "1000000000.0000 BBB"));
   issue_tokens(*this, config::system_account_name, "inita"_n, chain::asset::from_string("100.0000 SYS"));
   issue_tokens(*this, config::system_account_name, "inita"_n, chain::asset::from_string("200.0000 AAA"));
   issue_tokens(*this, config::system_account_name, "inita"_n, chain::asset::from_string("300.0000 BBB"));
   issue_tokens(*this, config::system_account_name, "initb"_n, chain::asset::from_string("400.0000 SYS"));
   produce_block();

   std::optional<sysio::chain_apis::tracked_votes> _tracked_votes;
   chain_apis::read_only plugin(*(this->control), {}, {}, _tracked_votes,
                                fc::microseconds::maximum(), fc::microseconds::maximum(), {});

   // sysio.token "accounts" table uses kv_multi_index — no secondary indexes.
   // Test scoped primary query with multiple tokens per account.
   {
      chain_apis::read_only::get_table_rows_params p;
      p.json = true;
      p.code = "sysio.token"_n;
      p.table = "accounts";
      p.scope = "inita";
      auto result = get_table_rows_full(plugin, p, fc::time_point::maximum());
      // inita has 3 tokens: SYS, AAA, BBB
      BOOST_REQUIRE_EQUAL(3u, result.rows.size());
   }

   // scope=initb should only see 1 token (SYS), not inita's tokens
   {
      chain_apis::read_only::get_table_rows_params p;
      p.json = true;
      p.code = "sysio.token"_n;
      p.table = "accounts";
      p.scope = "initb";
      auto result = get_table_rows_full(plugin, p, fc::time_point::maximum());
      BOOST_REQUIRE_EQUAL(1u, result.rows.size());
      BOOST_REQUIRE_EQUAL("400.0000 SYS", result.rows[0]["value"]["balance"].as_string());
   }

   // Also test scoped + reverse: inita's 3 tokens in reverse order
   {
      chain_apis::read_only::get_table_rows_params p;
      p.json = true;
      p.code = "sysio.token"_n;
      p.table = "accounts";
      p.scope = "inita";
      p.reverse = true;
      auto result = get_table_rows_full(plugin, p, fc::time_point::maximum());
      BOOST_REQUIRE_EQUAL(3u, result.rows.size());
      // Reverse order: SYS > BBB > AAA by symbol code
      BOOST_REQUIRE_EQUAL("100.0000 SYS", result.rows[0]["value"]["balance"].as_string());
      BOOST_REQUIRE_EQUAL("300.0000 BBB", result.rows[1]["value"]["balance"].as_string());
      BOOST_REQUIRE_EQUAL("200.0000 AAA", result.rows[2]["value"]["balance"].as_string());
   }

} FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()

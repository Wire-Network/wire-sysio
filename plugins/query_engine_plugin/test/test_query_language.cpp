#include <sysio/query_engine_plugin/query.hpp>

#include <boost/test/unit_test.hpp>

using namespace sysio::query_engine;
BOOST_AUTO_TEST_SUITE(query_language)

BOOST_AUTO_TEST_CASE(case_quotes_and_owned_ast) {
   query_budget budget({});
   const auto query = parse_query("sElEcT owner AS \"Owner\", COUNT(*) AS records FROM \"sample.one\".positions "
                                  "WHERE amount >= -10.25 AND NOT (owner = 'alice' OR owner IS NULL) "
                                  "GROUP BY owner HAVING COUNT(*) > 1 ORDER BY records DESC LIMIT 0;",
                                  budget);
   BOOST_CHECK_EQUAL(query.owners.front(), "sample.one");
   BOOST_CHECK_EQUAL(*query.limit, 0);
   BOOST_REQUIRE_EQUAL(query.select.size(), 2);
   BOOST_CHECK_EQUAL(*query.select.front().alias, "Owner");
   BOOST_CHECK(query.order_by.front().descending);
}

BOOST_AUTO_TEST_CASE(unsupported_syntax_and_parser_limits) {
   for (const auto* sql : {"SELECT x FROM a.b; SELECT y FROM a.b", "SELECT DISTINCT x FROM a.b", "SELECT x+1 FROM a.b",
                           "SELECT SUM(x) FROM a.b", "SELECT x FROM a.b -- comment", "SELECT x FROM a.b LIMIT -1",
                           "SELECT x FROM a.b WHERE x = 1e2"}) {
      query_budget budget({});
      BOOST_CHECK_THROW(parse_query(sql, budget), query_error);
   }
   query_budget budget({});
   const std::string deep = "SELECT x FROM a.b WHERE " + std::string(constants::max_depth + 1, '(') + "x = 1" +
                            std::string(constants::max_depth + 1, ')');
   BOOST_CHECK_THROW(parse_query(deep, budget), query_error);
   std::string mixed = "SELECT x FROM a.b WHERE ";
   for (uint32_t i = 0; i < constants::max_depth; ++i)
      mixed += "NOT (";
   mixed += "x = 1" + std::string(constants::max_depth, ')');
   query_budget mixed_budget({});
   BOOST_CHECK_THROW(parse_query(mixed, mixed_budget), query_error);
   std::string tokens = "SELECT x FROM a.b WHERE x=1";
   for (uint32_t i = 0; i < constants::max_tokens; ++i)
      tokens += " OR x=1";
   query_config config;
   config.max_query_bytes = tokens.size();
   query_budget token_budget(config);
   BOOST_CHECK_THROW(parse_query(tokens, token_budget), query_error);
}

/// The token pre-scan stops a long stream before the parser allocates a tree: ORDER BY items cost
/// three tokens but one AST node, so this query exceeds the token bound well under the node bound.
BOOST_AUTO_TEST_CASE(token_bound_precedes_node_bound) {
   constexpr uint32_t tokens_per_item = 3;
   std::string ordered = "SELECT x FROM a.b ORDER BY x";
   for (uint32_t i = 0; i < constants::max_tokens / tokens_per_item; ++i)
      ordered += ", x DESC";
   query_budget budget({});
   BOOST_CHECK_EXCEPTION(parse_query(ordered, budget), query_error, [](const auto& error) {
      return error.kind == error_kind::QUERY_LIMIT && error.limit == bound::tokens;
   });
}

/// A keyword becomes an identifier in double quotes: as a field, a table and an output name alike.
BOOST_AUTO_TEST_CASE(quoted_keywords_are_identifiers) {
   query_budget budget({});
   const auto query = parse_query(
      "SELECT \"count\", \"limit\" AS \"order\" FROM \"select\" OWNER 'sysio.roa' ORDER BY \"order\"", budget);
   BOOST_CHECK_EQUAL(query.table, "select");
   BOOST_REQUIRE_EQUAL(query.select.size(), 2);
   BOOST_CHECK_EQUAL(query.select.front().source.path.front(), "count");
   BOOST_CHECK_EQUAL(*query.select.back().alias, "order");
   BOOST_CHECK_EQUAL(query.order_by.front().name, "order");
   query_budget unquoted({});
   BOOST_CHECK_THROW(parse_query("SELECT count FROM sponsorcount OWNER 'sysio.roa'", unquoted), query_error);
}

/// Owner selection is explicit and supports both qualified tables and a SQL owner list.
BOOST_AUTO_TEST_CASE(owner_clause_and_qualified_sources) {
   query_budget budget({});
   auto ast = parse_query("SELECT owner FROM positions OWNER 'alice', bob", budget);
   BOOST_CHECK_EQUAL(ast.table, "positions");
   BOOST_REQUIRE_EQUAL(ast.owners.size(), 2);
   BOOST_CHECK_EQUAL(ast.owners.front(), "alice");
   BOOST_CHECK_EQUAL(ast.owners.back(), "bob");
   for (const auto* sql :
        {"SELECT * FROM positions", "SELECT * FROM alice.positions OWNER bob",
         "SELECT * FROM positions OWNER 'alice', 'alice'", "SELECT * FROM alice.positions JOIN bob.positions"}) {
      query_budget invalid({});
      BOOST_CHECK_THROW(parse_query(sql, invalid), query_error);
   }
}
BOOST_AUTO_TEST_SUITE_END()

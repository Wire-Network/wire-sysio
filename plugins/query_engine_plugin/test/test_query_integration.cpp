#include "query_fixture.hpp"

#include <fc/io/raw.hpp>
#include <sysio/chain/account_object.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>

using namespace sysio::query_engine;
using namespace sysio::query_engine::test;
using namespace sysio::chain::literals;

BOOST_FIXTURE_TEST_SUITE(query_integration, chain_fixture)

BOOST_AUTO_TEST_CASE(grouping_through_production_service) {
   read_queue reads;
   auto engine = std::make_shared<query_engine>(query_config{}, reads.create_api(*validating_node));
   query_http_handler handler(engine);
   const auto response =
      submit(handler, reads,
             "SELECT beneficiary, COUNT(*) AS records, SUM(amount) AS total FROM sample.positions "
             "WHERE amount >= 10 GROUP BY beneficiary HAVING SUM(amount) > 100 ORDER BY total DESC LIMIT 20");
   BOOST_REQUIRE_MESSAGE(response.get_object().contains("result"),
                         fc::json::to_string(response, fc::time_point::maximum()));
   const auto& result = response["result"];
   const auto& rows = result["rows"].get_array();
   BOOST_REQUIRE_EQUAL(rows.size(), 1);
   BOOST_CHECK_EQUAL(rows[0]["beneficiary"].as_string(), "alice");
   BOOST_CHECK_EQUAL(rows[0]["records"].as_string(), "2");
   BOOST_CHECK_EQUAL(rows[0]["total"].as_string(), "150");
   BOOST_CHECK_EQUAL(result["stats"]["groups"].as_string(), "2");
   BOOST_CHECK_EQUAL(result["state"]["block_id"].as_string(), validating_node->head().id().str());
   BOOST_CHECK(!result["state"]["synced"].as_bool());
}

BOOST_AUTO_TEST_CASE(native_pages_owner_isolation_and_ranges) {
   seed(account, 4, 600, "alice"_n, 1);
   seed(other_account, 1, 1, "alice"_n, 999);
   produce_block();
   query_budget budget(relaxed_config());
   uint64_t pages = 0;
   const auto result = evaluate_query(
      "SELECT beneficiary, SUM(amount) AS total FROM sample.positions GROUP BY beneficiary", budget, &pages);
   BOOST_CHECK_EQUAL(pages, 2);
   BOOST_CHECK_EQUAL(budget.scanned_rows, 603);
   BOOST_CHECK_EQUAL(result["rows"][size_t{0}]["total"].as_string(), "750");
   query_budget all(relaxed_config());
   BOOST_CHECK_EQUAL(
      evaluate_query("SELECT COUNT(*) AS n FROM sample.positions", all)["rows"][size_t{0}]["n"].as_string(), "603");
   query_budget other(relaxed_config());
   BOOST_CHECK_EQUAL(
      evaluate_query("SELECT SUM(amount) AS n FROM other.positions", other)["rows"][size_t{0}]["n"].as_string(), "999");
   query_budget combined(relaxed_config());
   uint64_t combined_pages = 0;
   auto merged =
      evaluate_query("SELECT beneficiary, COUNT(*) AS n, SUM(amount) AS total FROM positions OWNER 'sample', 'other' "
                     "GROUP BY beneficiary HAVING total > 100",
                     combined, &combined_pages);
   BOOST_CHECK_EQUAL(combined_pages, 3);
   BOOST_CHECK_EQUAL(combined.scanned_rows, 604);
   BOOST_REQUIRE_EQUAL(merged["rows"].get_array().size(), 1);
   BOOST_CHECK_EQUAL(merged["rows"][size_t{0}]["n"].as_string(), "603");
   BOOST_CHECK_EQUAL(merged["rows"][size_t{0}]["total"].as_string(), "1749");
   BOOST_CHECK_EQUAL(merged["source"]["owners"].get_array().size(), 2);
   BOOST_CHECK_EQUAL(merged["state"]["abis"].get_array().size(), 2);
   query_budget ties(relaxed_config());
   const auto detail = evaluate_query("SELECT amount FROM positions OWNER 'sample', 'other' WHERE key.id = 1", ties);
   BOOST_REQUIRE_EQUAL(detail["rows"].get_array().size(), 2);
   // Chain owner ordering breaks ties before the encoded primary key, independently of OWNER-list order.
   BOOST_CHECK_EQUAL(detail["rows"][size_t{0}]["amount"].as_string(), "999");
   BOOST_CHECK_EQUAL(detail["rows"][size_t{1}]["amount"].as_string(), "60");
   for (const auto* predicate : {"key.id >= 600 AND key.id < 603", "key.id > 599 AND key.id <= 602"}) {
      query_budget range(relaxed_config());
      auto out = evaluate_query(std::string("SELECT key.id AS id FROM sample.positions WHERE ") + predicate, range);
      BOOST_REQUIRE_EQUAL(out["rows"].get_array().size(), 3);
      BOOST_CHECK_EQUAL(range.scanned_rows, 3);
      BOOST_CHECK_EQUAL(out["rows"][size_t{0}]["id"].as_string(), "600");
   }
   query_budget disjunction(relaxed_config());
   const auto fallback =
      evaluate_query("SELECT key.id AS id FROM sample.positions WHERE key.id = 1 OR key.id = 603", disjunction);
   BOOST_CHECK_EQUAL(fallback["rows"].get_array().size(), 2);
   BOOST_CHECK_EQUAL(disjunction.scanned_rows, 603);
}

BOOST_AUTO_TEST_CASE(nulls_having_aliases_empty_inputs_and_limits) {
   for (const auto* condition : {"nullable = 1", "NOT (nullable = 1)", "nullable != 1", "nullable = NULL"}) {
      query_budget budget(relaxed_config());
      BOOST_CHECK(
         evaluate_query(std::string("SELECT beneficiary FROM sample.positions WHERE ") + condition, budget)["rows"]
            .get_array()
            .empty());
   }
   query_budget budget(relaxed_config());
   auto result =
      evaluate_query("SELECT COUNT(*) AS n, COUNT(nullable) AS present, SUM(nullable) AS total, AVG(amount) AS "
                     "mean, MIN(amount) AS low, MAX(amount) AS high FROM sample.positions HAVING n = 3",
                     budget);
   const auto& row = result["rows"][size_t{0}];
   BOOST_CHECK_EQUAL(row["present"].as_string(), "0");
   BOOST_CHECK(row["total"].is_null());
   BOOST_CHECK_EQUAL(row["mean"].as_string(), "56.666666666666666667");
   BOOST_CHECK_EQUAL(row["low"].as_string(), "20");
   BOOST_CHECK_EQUAL(row["high"].as_string(), "90");
   query_budget empty(relaxed_config());
   result =
      evaluate_query("SELECT COUNT(*) AS n, SUM(amount) AS total FROM sample.positions WHERE amount > 999", empty);
   BOOST_CHECK_EQUAL(result["rows"][size_t{0}]["n"].as_string(), "0");
   BOOST_CHECK(result["rows"][size_t{0}]["total"].is_null());
   query_budget grouped(relaxed_config());
   BOOST_CHECK(evaluate_query("SELECT beneficiary FROM sample.positions WHERE amount > 999 GROUP BY beneficiary",
                              grouped)["rows"]
                  .get_array()
                  .empty());
   query_config config;
   config.max_result_rows = 1;
   query_budget cap(relaxed_config(config));
   BOOST_CHECK_THROW(evaluate_query("SELECT beneficiary FROM sample.positions", cap), query_error);
   query_budget limited(relaxed_config(config));
   result = evaluate_query("SELECT amount AS a FROM sample.positions ORDER BY a DESC LIMIT 1", limited);
   BOOST_CHECK_EQUAL(result["rows"][size_t{0}]["a"].as_string(), "90");
   BOOST_CHECK_EQUAL(limited.scanned_rows, 3);
   query_budget zero(relaxed_config());
   BOOST_CHECK(
      evaluate_query("SELECT SUM(amount) AS total FROM sample.positions LIMIT 0", zero)["rows"].get_array().empty());
   for (const auto* sql :
        {"SELECT beneficiary, SUM(amount) AS total FROM sample.positions",
         "SELECT SUM(amount) AS beneficiary FROM sample.positions GROUP BY beneficiary HAVING beneficiary > 1",
         "SELECT beneficiary AS x, amount AS x FROM sample.positions",
         "SELECT beneficiary FROM sample.positions HAVING beneficiary = 'alice'",
         "SELECT amount AS x FROM sample.positions WHERE x > 1",
         "SELECT * FROM sample.positions GROUP BY beneficiary"}) {
      query_budget invalid(relaxed_config());
      BOOST_CHECK_THROW(evaluate_query(sql, invalid), query_error);
   }
}

BOOST_AUTO_TEST_CASE(nested_assets_wide_and_opaque_values) {
   put(1, position(-7));
   push_action(account, wide_action, account, fc::mutable_variant_object()("id", 1));
   produce_block();
   query_budget budget(relaxed_config());
   const auto result = evaluate_query("SELECT * FROM sample.positions WHERE key.id = 1", budget);
   const auto& row = result["rows"][size_t{0}];
   BOOST_CHECK_EQUAL(row["amount"].as_string(), "-7");
   BOOST_CHECK_EQUAL(row["nested"]["numbers"][size_t{0}].as_string(), "9007199254740993");
   BOOST_CHECK_EQUAL(row["quantity"]["precision"].as_string(), "4");
   BOOST_CHECK_EQUAL(row["quantity"]["amount"].as_string(), "1.25");
   BOOST_CHECK_EQUAL(row["created"].as_string(), "2023-11-14T22:13:20.123000");
   BOOST_CHECK(row["enabled"].as_bool());
   query_budget wide(relaxed_config());
   const auto data = evaluate_query("SELECT * FROM sample.wide", wide)["rows"][size_t{0}];
   BOOST_CHECK_EQUAL(data["signed_value"].as_string(), "-170141183460469231731687303715884105728");
   BOOST_CHECK_EQUAL(data["unsigned_value"].as_string(), "340282366920938463463374607431768211455");
   BOOST_CHECK_EQUAL(data["float_value"].as_string(), "0x0000c03f");
   BOOST_CHECK_EQUAL(data["double_value"].as_string(), "0x00000000000004c0");
   // CDT serializes a default binary_extension as its underlying zero value.
   BOOST_CHECK_EQUAL(data["extension"].as_string(), "0");
   for (const auto* sql :
        {"SELECT float_value FROM sample.wide WHERE float_value = 1.5",
         "SELECT COUNT(float_value) AS n FROM sample.wide", "SELECT float_value FROM sample.wide ORDER BY float_value",
         "SELECT nested FROM sample.positions GROUP BY nested"}) {
      query_budget invalid(relaxed_config());
      BOOST_CHECK_THROW(evaluate_query(sql, invalid), query_error);
   }
   put(2, position(90, {}, "2.000 USD"));
   query_budget units(relaxed_config());
   BOOST_CHECK_THROW(evaluate_query("SELECT SUM(quantity) AS total FROM sample.positions", units), query_error);
   // Denominations that differ are unequal under `=`/`!=` and an error under an ordered operator.
   query_budget equality(relaxed_config());
   BOOST_CHECK(
      evaluate_query("SELECT key.id AS id FROM sample.positions WHERE quantity = '1.0000 USD'", equality)["rows"]
         .get_array()
         .empty());
   query_budget inequality(relaxed_config());
   BOOST_CHECK_EQUAL(
      evaluate_query("SELECT key.id AS id FROM sample.positions WHERE quantity != '1.0000 USD'", inequality)["rows"]
         .get_array()
         .size(),
      3);
   query_budget ordering(relaxed_config());
   BOOST_CHECK_EXCEPTION(
      evaluate_query("SELECT key.id AS id FROM sample.positions WHERE quantity < '1.0000 USD'", ordering), query_error,
      [](const auto& error) { return error.kind == error_kind::VALUE_ERROR; });
}

BOOST_AUTO_TEST_CASE(copied_rows_and_abi_survive_updates) {
   local_table_source source(*validating_node);
   query_budget budget(relaxed_config());
   auto ast = parse_query("SELECT SUM(amount) AS total FROM sample.positions", budget);
   auto plan = create_plan(ast, source.describe(ast, budget), budget);
   auto captured = source.capture(plan, budget);
   const auto identity = captured.state["block_id"].as_string();
   put(1, position(600));
   BOOST_CHECK_EQUAL(fc::variant(evaluate(plan, std::move(captured), budget))["rows"][size_t{0}]["total"].as_string(),
                     "170");
   BOOST_CHECK(identity != validating_node->head().id().str());
   query_budget updated(relaxed_config());
   BOOST_CHECK_EQUAL(
      evaluate_query("SELECT SUM(amount) AS total FROM sample.positions", updated)["rows"][size_t{0}]["total"]
         .as_string(),
      "710");
   set_abi(account, sysio::testing::test_contracts::query_fixture_abi().c_str());
   produce_block();
   BOOST_CHECK_EXCEPTION(source.capture(plan, budget), query_error,
                         [](const auto& error) { return error.kind == error_kind::SCHEMA_CHANGED; });
}

BOOST_AUTO_TEST_CASE(capture_budgets_fail_without_partial_results) {
   for (const auto limit : {option::max_scan_rows, option::max_raw_bytes, option::max_memory_bytes,
                            option::max_abi_bytes, option::max_groups}) {
      query_config config;
      if (limit == option::max_scan_rows)
         config.max_scan_rows = 1;
      if (limit == option::max_raw_bytes)
         config.max_raw_bytes = 1;
      if (limit == option::max_memory_bytes) {
         config.max_memory_bytes = 1;
         config.max_raw_bytes = 1;
      }
      if (limit == option::max_abi_bytes)
         config.max_abi_bytes = 1;
      if (limit == option::max_groups)
         config.max_groups = 1;
      query_budget budget(relaxed_config(config));
      BOOST_CHECK_EXCEPTION(
         evaluate_query("SELECT beneficiary, SUM(amount) AS total FROM sample.positions GROUP BY beneficiary", budget),
         query_error, [&](const auto& error) { return error.kind == error_kind::QUERY_LIMIT && error.limit == limit; });
   }
}

/// Every narrowed primary-key range returns exactly the rows of the same predicate under a forced
/// full scan, including empty, open-ended, contradictory and out-of-domain bounds.
BOOST_AUTO_TEST_CASE(key_range_narrowing_matches_a_full_scan) {
   constexpr uint32_t extra_rows = 20;
   constexpr uint64_t total_rows = extra_rows + 3;
   seed(account, 4, extra_rows, "alice"_n, 1);
   produce_block();
   for (const auto* predicate : {"key.id = 5",
                                 "key.id = 0",
                                 "key.id = 24",
                                 "key.id > 5",
                                 "key.id >= 5",
                                 "key.id < 5",
                                 "key.id <= 5",
                                 "key.id > 22",
                                 "key.id >= 23",
                                 "key.id > 23",
                                 "key.id < 1",
                                 "key.id <= 1",
                                 "key.id > 3 AND key.id < 3",
                                 "key.id >= 3 AND key.id <= 3",
                                 "key.id > 5 AND key.id < 9",
                                 "key.id >= 5 AND key.id <= 9",
                                 "key.id > 5 AND key.id > 7",
                                 "key.id < 9 AND key.id < 7",
                                 "key.id = 5 AND key.id = 6",
                                 "key.id = 5 AND key.id > 2",
                                 "key.id = 5 AND key.id < 2",
                                 "key.id != 5",
                                 "5 < key.id",
                                 "9 >= key.id",
                                 "key.id > 18446744073709551615",
                                 "key.id < 18446744073709551615",
                                 "key.id >= 18446744073709551614",
                                 "key.id > -1",
                                 "key.id < -1",
                                 "key.id = 5 AND amount = 1",
                                 "key.id > 4 AND amount > 50"}) {
      query_budget narrowed(relaxed_config());
      const auto rows =
         evaluate_query(std::string("SELECT key.id AS id FROM sample.positions WHERE ") + predicate, narrowed)["rows"];
      query_budget reference(relaxed_config());
      const auto expected = evaluate_query(std::string("SELECT key.id AS id FROM sample.positions WHERE (") +
                                              predicate + ") OR FALSE = TRUE",
                                           reference)["rows"];
      BOOST_CHECK_EQUAL(reference.scanned_rows, total_rows);
      BOOST_CHECK_LE(narrowed.scanned_rows, total_rows);
      BOOST_CHECK_MESSAGE(fc::json::to_string(rows, fc::time_point::maximum()) ==
                             fc::json::to_string(expected, fc::time_point::maximum()),
                          predicate);
   }
   // Narrowing actually happens: these predicates scan exactly their range.
   const std::pair<const char*, uint64_t> narrowed_scans[] = {
      {"key.id = 5",                1         },
      {"key.id > 5 AND key.id < 9", 3         },
      {"key.id >= 20",              4         },
      {"key.id = 5 AND key.id = 6", 0         },
      {"key.id != 5",               total_rows}
   };
   for (const auto& [predicate, scanned] : narrowed_scans) {
      query_budget budget(relaxed_config());
      evaluate_query(std::string("SELECT key.id AS id FROM sample.positions WHERE ") + predicate, budget);
      BOOST_CHECK_MESSAGE(budget.scanned_rows == scanned, predicate << " scanned " << budget.scanned_rows);
   }
}

/// An array whose elements can occupy no bytes (a struct of binary extensions) is skipped element by
/// element, so the field after it decodes from the right offset; a huge count at the end of a row
/// costs nothing and is not an error.
BOOST_AUTO_TEST_CASE(extension_only_struct_arrays_are_skipped_by_element) {
   local_table_source source(*validating_node);
   // Rewrite the row type: `pairs` holds structs of one binary extension, followed by `tail`.
   const auto plan_with_fields = [&](const char* sql, query_budget& budget,
                                     std::vector<sysio::chain::field_def> fields) {
      const auto ast = parse_query(sql, budget);
      auto schemas = source.describe(ast, budget);
      sysio::chain::struct_def pair;
      pair.name = "extension_pair";
      pair.fields = {
         {"a", "uint64$"}
      };
      schemas.front().abi.structs.push_back(pair);
      for (auto& structure : schemas.front().abi.structs)
         if (structure.name == schemas.front().table.type)
            structure.fields = std::move(fields);
      return create_plan(ast, std::move(schemas), budget);
   };
   const auto packed_words = [](std::vector<char> prefix, std::initializer_list<uint64_t> words) {
      for (const auto word : words) {
         const auto packed = fc::raw::pack(word);
         prefix.insert(prefix.end(), packed.begin(), packed.end());
      }
      return prefix;
   };
   query_budget budget(relaxed_config());
   const auto plan = plan_with_fields("SELECT tail FROM sample.positions", budget,
                                      {
                                         {"pairs", "extension_pair[]"},
                                         {"tail",  "uint64"          }
   });
   auto input = source.capture(plan, budget);
   input.rows.resize(1);
   // Two elements carrying their extension, then the tail: skipping no element would read 7 as the tail.
   input.rows.front().row.value = packed_words(fc::raw::pack(fc::unsigned_int(2)), {7, 8, 42});
   BOOST_CHECK_EQUAL(fc::variant(evaluate(plan, std::move(input), budget))["rows"][size_t{0}]["tail"].as_string(),
                     "42");
   query_budget trailing(relaxed_config());
   const auto trailing_plan = plan_with_fields("SELECT head FROM sample.positions", trailing,
                                               {
                                                  {"head",  "uint64"          },
                                                  {"pairs", "extension_pair[]"}
   });
   auto trailing_input = source.capture(trailing_plan, trailing);
   trailing_input.rows.resize(1);
   const auto huge_count = fc::raw::pack(fc::unsigned_int(std::numeric_limits<uint32_t>::max()));
   trailing_input.rows.front().row.value = packed_words({}, {9});
   auto& trailing_bytes = trailing_input.rows.front().row.value;
   trailing_bytes.insert(trailing_bytes.end(), huge_count.begin(), huge_count.end());
   BOOST_CHECK_EQUAL(
      fc::variant(evaluate(trailing_plan, std::move(trailing_input), trailing))["rows"][size_t{0}]["head"].as_string(),
      "9");
}

/// Several `fixed_bytes<32>` fields compile: the checksum alias rewrites the type name after the
/// recursion guard recorded it, and the guard still releases the entry it inserted.
BOOST_AUTO_TEST_CASE(checksum_alias_compiles_for_several_fields) {
   local_table_source source(*validating_node);
   query_budget budget(relaxed_config());
   const auto ast = parse_query("SELECT first, second FROM sample.positions", budget);
   auto schemas = source.describe(ast, budget);
   for (auto& structure : schemas.front().abi.structs)
      if (structure.name == schemas.front().table.type)
         structure.fields = {
            {"first",  "fixed_bytes<32>"},
            {"second", "fixed_bytes<32>"}
         };
   const auto plan = create_plan(ast, std::move(schemas), budget);
   auto input = source.capture(plan, budget);
   input.rows.resize(1);
   constexpr size_t digest_bytes = sizeof(sysio::chain::checksum256_type);
   auto& bytes = input.rows.front().row.value;
   bytes.assign(2 * digest_bytes, '\x11');
   std::fill(bytes.begin() + digest_bytes, bytes.end(), '\x22');
   const auto row = fc::variant(evaluate(plan, std::move(input), budget))["rows"][size_t{0}];
   BOOST_CHECK_EQUAL(row["first"].as_string(), std::string(2 * digest_bytes, '1'));
   BOOST_CHECK_EQUAL(row["second"].as_string(), std::string(2 * digest_bytes, '2'));
}

/// The table name `fasp` hashes to the highest table id, 0xFFFF. A forward query reads it like any
/// other table, and a reverse page without an upper bound starts at the partition's end instead of
/// wrapping the successor id to zero and returning nothing.
BOOST_AUTO_TEST_CASE(highest_table_id_scans_forward_and_in_reverse) {
   constexpr uint16_t highest_table_id = 0xFFFF;
   constexpr uint64_t rows = 3;
   for (uint64_t id = 1; id <= rows; ++id)
      push_action(account, fasp_action, account,
                  fc::mutable_variant_object()("id", id)("row", position(static_cast<int64_t>(id))));
   produce_block();
   local_table_source source(*validating_node);
   query_budget budget(relaxed_config());
   const auto ast = parse_query("SELECT key.id AS id FROM sample.fasp", budget);
   const auto plan = create_plan(ast, source.describe(ast, budget), budget);
   BOOST_REQUIRE_EQUAL(plan.schemas.front()->table.table_id, highest_table_id);
   const auto forward = fc::variant(evaluate(plan, source.capture(plan, budget), budget));
   BOOST_REQUIRE_EQUAL(forward["rows"].get_array().size(), rows);
   BOOST_CHECK_EQUAL(forward["rows"][size_t{0}]["id"].as_string(), "1");
   auto request = plan.scan;
   request.direction = sysio::chain_apis::scan_direction::reverse;
   sysio::chain_apis::table_read_budget read_budget;
   const auto page = sysio::chain_apis::capture_primary_page(*validating_node, request, read_budget);
   BOOST_REQUIRE_EQUAL(page.rows.size(), rows);
   BOOST_CHECK(!page.more);
   BOOST_CHECK(compare_keys(page.rows.front().key, page.rows.back().key) > 0);
}

/// Composite bounds narrow only leading fields; the residual predicate remains authoritative.
BOOST_AUTO_TEST_CASE(composite_prefix_and_nonleading_fallback) {
   for (const auto beneficiary : {"alice"_n, "bob"_n})
      for (uint64_t sequence = 1; sequence <= 3; ++sequence)
         push_action(
            account, composite_action, account,
            fc::mutable_variant_object()("beneficiary", beneficiary)("sequence", sequence)("row", position(sequence)));
   produce_block();
   query_budget prefix(relaxed_config());
   const auto narrowed = evaluate_query("SELECT key.sequence AS n FROM sample.composite "
                                        "WHERE key.beneficiary = 'alice' AND key.sequence >= 2",
                                        prefix);
   BOOST_CHECK_EQUAL(prefix.scanned_rows, 2);
   BOOST_REQUIRE_EQUAL(narrowed["rows"].get_array().size(), 2);
   BOOST_CHECK_EQUAL(narrowed["rows"][size_t{0}]["n"].as_string(), "2");
   query_budget reference(relaxed_config());
   const auto full = evaluate_query("SELECT key.sequence AS n FROM sample.composite "
                                    "WHERE (key.beneficiary = 'alice' AND key.sequence >= 2) OR FALSE = TRUE",
                                    reference);
   BOOST_CHECK_EQUAL(reference.scanned_rows, 6);
   BOOST_CHECK_EQUAL(fc::json::to_string(narrowed["rows"], fc::time_point::maximum()),
                     fc::json::to_string(full["rows"], fc::time_point::maximum()));
   query_budget nonleading(relaxed_config());
   BOOST_CHECK_EQUAL(evaluate_query("SELECT amount FROM sample.composite WHERE key.sequence = 2", nonleading)["rows"]
                        .get_array()
                        .size(),
                     2);
   BOOST_CHECK_EQUAL(nonleading.scanned_rows, 6);
}

/// The owned ABI remains valid after an update; malformed copies of referenced value or key bytes
/// never become binary fallback rows.
BOOST_AUTO_TEST_CASE(owned_abi_and_malformed_rows) {
   local_table_source source(*validating_node);
   query_budget budget(relaxed_config());
   const auto ast = parse_query("SELECT key.id AS id, created, amount FROM sample.positions", budget);
   const auto plan = create_plan(ast, source.describe(ast, budget), budget);
   auto captured = source.capture(plan, budget);
   auto bad_value = captured;
   bad_value.rows.front().row.value.clear();
   auto bad_key = captured;
   bad_key.rows.front().row.key.clear();
   set_abi(account, sysio::testing::test_contracts::query_fixture_abi().c_str());
   produce_block();
   const auto result = fc::variant(evaluate(plan, std::move(captured), budget));
   BOOST_CHECK_EQUAL(result["rows"][size_t{0}]["created"].as_string(), "2023-11-14T22:13:20.123456");
   for (auto* input : {&bad_value, &bad_key}) {
      query_budget invalid(relaxed_config());
      BOOST_CHECK_EXCEPTION(evaluate(plan, std::move(*input), invalid), query_error,
                            [](const auto& error) { return error.kind == error_kind::ROW_DECODE_ERROR; });
   }
   query_budget changed(relaxed_config());
   BOOST_CHECK_EXCEPTION(source.capture(plan, changed), query_error,
                         [](const auto& error) { return error.kind == error_kind::SCHEMA_CHANGED; });
}

/// Missing extension bytes are SQL null, and adversarial crypto sizes fail before allocation.
BOOST_AUTO_TEST_CASE(binary_extension_and_crypto_length_bounds) {
   push_action(account, wide_action, account, fc::mutable_variant_object()("id", 1));
   produce_block();
   local_table_source source(*validating_node);
   query_budget budget(relaxed_config());
   auto ast = parse_query("SELECT extension FROM sample.wide", budget);
   auto plan = create_plan(ast, source.describe(ast, budget), budget);
   auto input = source.capture(plan, budget);
   input.rows.front().row.value.resize(input.rows.front().row.value.size() - sizeof(uint64_t));
   BOOST_CHECK(fc::variant(evaluate(plan, std::move(input), budget))["rows"][size_t{0}]["extension"].is_null());

   for (const auto* type : {"public_key", "signature"}) {
      query_budget compiled(relaxed_config());
      ast = parse_query("SELECT payload FROM sample.positions", compiled);
      auto schemas = source.describe(ast, compiled);
      for (auto& structure : schemas.front().abi.structs)
         if (structure.name == schemas.front().table.type)
            structure.fields = {
               {"payload", type}
            };
      plan = create_plan(ast, std::move(schemas), compiled);
      input = source.capture(plan, compiled);
      auto& bytes = input.rows.front().row.value;
      const bool public_key = std::string(type) == "public_key";
      const auto tag = public_key ? magic_enum::enum_integer(sysio::chain::public_key_type::key_type::wa)
                                  : magic_enum::enum_integer(sysio::chain::signature_type::sig_type::wa);
      bytes = fc::raw::pack(fc::unsigned_int(tag));
      bytes.resize(bytes.size() + (public_key
                                      ? sizeof(fc::crypto::webauthn::public_key::public_key_data_type) + sizeof(uint8_t)
                                      : sizeof(fc::crypto::r1::compact_signature)));
      const auto huge_length = fc::raw::pack(fc::unsigned_int(std::numeric_limits<uint32_t>::max()));
      bytes.insert(bytes.end(), huge_length.begin(), huge_length.end());
      query_budget malformed(relaxed_config());
      BOOST_CHECK_EXCEPTION(evaluate(plan, std::move(input), malformed), query_error,
                            [](const auto& error) { return error.kind == error_kind::ROW_DECODE_ERROR; });
      BOOST_CHECK_LT(malformed.accounted_bytes, defaults::max_memory_bytes);
   }
}

/// Captured bytes retain the old state identity after the validating controller actually switches forks.
BOOST_AUTO_TEST_CASE(captured_state_survives_fork_switch) {
   sysio::testing::tester alternate;
   for (uint32_t number = alternate.control->head().block_num() + 1; number <= control->head().block_num(); ++number)
      alternate.push_block(control->fetch_block_by_number(number));
   const auto common_head = control->head().id();
   BOOST_REQUIRE(alternate.control->head().id() == common_head);
   put(1, position(600));
   local_table_source source(*validating_node);
   query_budget budget(relaxed_config());
   const auto ast = parse_query("SELECT SUM(amount) AS total FROM sample.positions", budget);
   const auto plan = create_plan(ast, source.describe(ast, budget), budget);
   auto captured = source.capture(plan, budget);
   const auto captured_head = captured.state["block_id"].as_string();
   alternate.push_action(account, put_action, account, fc::mutable_variant_object()("id", 1)("row", position(900)));
   const auto fork_block = alternate.produce_block(fc::seconds(1));
   push_block(fork_block);
   validate_push_block(fork_block);
   BOOST_REQUIRE(validating_node->head().id() == fork_block->calculate_id());
   BOOST_REQUIRE(captured_head != validating_node->head().id().str());
   const auto previous = fc::variant(evaluate(plan, std::move(captured), budget));
   BOOST_CHECK_EQUAL(previous["rows"][size_t{0}]["total"].as_string(), "710");
   BOOST_CHECK_EQUAL(previous["state"]["block_id"].as_string(), captured_head);
   query_budget current(relaxed_config());
   const auto next = evaluate_query("SELECT SUM(amount) AS total FROM sample.positions", current);
   BOOST_CHECK_EQUAL(next["rows"][size_t{0}]["total"].as_string(), "1010");
   BOOST_CHECK_EQUAL(next["state"]["block_id"].as_string(), fork_block->calculate_id().str());
}

/// HAVING observes the rational average, including aggregate slots absent from SELECT.
BOOST_AUTO_TEST_CASE(exact_having_and_unselected_aggregates) {
   query_budget exact(relaxed_config());
   const auto result = evaluate_query("SELECT AVG(amount) AS mean FROM sample.positions "
                                      "HAVING AVG(amount) < 56.666666666666666667",
                                      exact);
   BOOST_REQUIRE_EQUAL(result["rows"].get_array().size(), 1);
   BOOST_CHECK_EQUAL(result["rows"][size_t{0}]["mean"].as_string(), "56.666666666666666667");
   query_budget hidden(relaxed_config());
   const auto selected = evaluate_query("SELECT beneficiary FROM sample.positions GROUP BY beneficiary "
                                        "HAVING SUM(amount) > 100",
                                        hidden);
   BOOST_REQUIRE_EQUAL(selected["rows"].get_array().size(), 1);
   BOOST_CHECK_EQUAL(selected["rows"][size_t{0}]["beneficiary"].as_string(), "alice");
}

/// Null keys form one group, sort last in either direction, and follow SQL three-valued logic.
BOOST_AUTO_TEST_CASE(nullable_groups_order_and_predicates) {
   put(1, position(60, fc::variant(1)));
   put(3, position(20, fc::variant(2)));
   for (const auto* direction : {"ASC", "DESC"}) {
      query_budget budget(relaxed_config());
      const auto result =
         evaluate_query(std::string("SELECT nullable AS n, COUNT(*) AS records FROM sample.positions ") +
                           "GROUP BY nullable ORDER BY n " + direction,
                        budget);
      const auto& rows = result["rows"].get_array();
      BOOST_REQUIRE_EQUAL(rows.size(), 3);
      BOOST_CHECK(rows.back()["n"].is_null());
      BOOST_CHECK_EQUAL(rows.front()["n"].as_string(), std::string(direction) == "ASC" ? "1" : "2");
   }
   for (const auto* predicate :
        {"nullable IS NULL", "NOT (nullable IS NOT NULL)", "nullable IS NULL OR (nullable = 1 AND FALSE = TRUE)"}) {
      query_budget budget(relaxed_config());
      const auto result =
         evaluate_query(std::string("SELECT key.id AS id FROM sample.positions WHERE ") + predicate, budget);
      BOOST_REQUIRE_EQUAL(result["rows"].get_array().size(), 1);
      BOOST_CHECK_EQUAL(result["rows"][size_t{0}]["id"].as_string(), "2");
   }
}

/// Each owner must expose compatible types even when both tables contain identical key bytes.
BOOST_AUTO_TEST_CASE(incompatible_owner_schemas_fail_before_capture) {
   auto abi = sysio::chain_apis::get_abi(*validating_node, other_account);
   for (auto& structure : abi.structs)
      for (auto& field : structure.fields)
         if (field.name == "amount" && field.type == "int64")
            field.type = "uint64";
   set_abi(other_account, fc::json::to_string(abi, fc::time_point::maximum()).c_str());
   produce_block();
   query_budget budget(relaxed_config());
   BOOST_CHECK_EXCEPTION(evaluate_query("SELECT amount FROM positions OWNER 'sample', 'other'", budget), query_error,
                         [](const auto& error) { return error.kind == error_kind::QUERY_SEMANTICS; });
   BOOST_CHECK_EQUAL(budget.scanned_rows, 0);
}

/// ABI enums render their member names, group and order by underlying value, and bind name literals.
BOOST_AUTO_TEST_CASE(enum_members_render_names_and_bind_literals) {
   put(2, position(90, {}, "1.2500 SYS", status_closed));
   put(3, position(20, {}, "1.2500 SYS", status_archived));
   query_budget budget(relaxed_config());
   const auto grouped = evaluate_query("SELECT status, COUNT(*) AS n FROM sample.positions GROUP BY status "
                                       "ORDER BY status DESC",
                                       budget);
   const auto& rows = grouped["rows"].get_array();
   BOOST_REQUIRE_EQUAL(rows.size(), 3);
   BOOST_CHECK_EQUAL(rows[0]["status"].as_string(), status_archived);
   BOOST_CHECK_EQUAL(rows[1]["status"].as_string(), status_closed);
   BOOST_CHECK_EQUAL(rows[2]["status"].as_string(), status_open);
   const auto& column = grouped["columns"][size_t{0}];
   BOOST_CHECK_EQUAL(column["logical_type"].as_string(), "enumeration");
   BOOST_CHECK_EQUAL(column["encoding"].as_string(), "text");
   BOOST_CHECK_EQUAL(column["abi_type"].as_string(), "position_status");
   for (const auto* predicate : {"status = 'POSITION_STATUS_CLOSED'", "status = 'CLOSED'", "status = 1",
                                 "status > 'POSITION_STATUS_OPEN' AND status < 'ARCHIVED'"}) {
      query_budget bound(relaxed_config());
      const auto result =
         evaluate_query(std::string("SELECT key.id AS id FROM sample.positions WHERE ") + predicate, bound);
      BOOST_REQUIRE_EQUAL(result["rows"].get_array().size(), 1);
      BOOST_CHECK_EQUAL(result["rows"][size_t{0}]["id"].as_string(), "2");
   }
   query_budget extremes(relaxed_config());
   const auto aggregated =
      evaluate_query("SELECT MIN(status) AS low, MAX(status) AS high FROM sample.positions", extremes);
   BOOST_CHECK_EQUAL(aggregated["rows"][size_t{0}]["low"].as_string(), status_open);
   BOOST_CHECK_EQUAL(aggregated["rows"][size_t{0}]["high"].as_string(), status_archived);
   for (const auto* sql : {"SELECT key.id AS id FROM sample.positions WHERE status = 'MISSING'",
                           "SELECT key.id AS id FROM sample.positions WHERE status = 'a'",
                           "SELECT SUM(status) AS total FROM sample.positions"}) {
      query_budget invalid(relaxed_config());
      BOOST_CHECK_THROW(evaluate_query(sql, invalid), query_error);
   }
   query_budget star(relaxed_config());
   BOOST_CHECK_EQUAL(
      evaluate_query("SELECT * FROM sample.positions WHERE key.id = 3", star)["rows"][size_t{0}]["status"].as_string(),
      status_archived);
}

/// Column names are only consumed by ORDER BY and HAVING, so a field named `key` or `value` projects normally.
BOOST_AUTO_TEST_CASE(fields_named_key_and_value_project_through_select_star) {
   auto abi = sysio::chain_apis::get_abi(*validating_node, other_account);
   for (auto& structure : abi.structs)
      for (auto& field : structure.fields) {
         if (structure.name == "position" && field.name == "memo")
            field.name = "key";
         if (structure.name == "position" && field.name == "enabled")
            field.name = "value";
      }
   set_abi(other_account, fc::json::to_string(abi, fc::time_point::maximum()).c_str());
   seed(other_account, 1, 2, "alice"_n, 5);
   produce_block();
   query_budget budget(relaxed_config());
   const auto result = evaluate_query("SELECT * FROM other.positions ORDER BY key", budget);
   BOOST_REQUIRE_EQUAL(result["rows"].get_array().size(), 2);
   BOOST_CHECK_EQUAL(result["rows"][size_t{0}]["key"].as_string(), "fixture");
   BOOST_CHECK(result["rows"][size_t{0}]["value"].as_bool());
   query_budget filtered(relaxed_config());
   const auto matched = evaluate_query(
      "SELECT key, COUNT(*) AS n FROM other.positions WHERE key = 'fixture' AND value = TRUE GROUP BY key "
      "HAVING key = 'fixture'",
      filtered);
   BOOST_REQUIRE_EQUAL(matched["rows"].get_array().size(), 1);
   BOOST_CHECK_EQUAL(matched["rows"][size_t{0}]["n"].as_string(), "2");
   BOOST_CHECK_EQUAL(
      evaluate_query("SELECT key.id AS id FROM other.positions WHERE key.id = 2", filtered)["rows"].get_array().size(),
      1);
}

/// Decoding only referenced fields and releasing per-row charges keeps large scans within the default
/// memory budget: COUNT(*) reads no row bytes, and a projected scan charges each row once.
BOOST_AUTO_TEST_CASE(decode_charges_do_not_accumulate_across_a_scan) {
   constexpr uint32_t batch_rows = 1024;
   constexpr uint32_t batches = 16;
   for (uint32_t batch = 0; batch < batches; ++batch) {
      seed(account, 4 + uint64_t(batch) * batch_rows, batch_rows, "alice"_n, 1);
      produce_block();
   }
   const auto expected_rows = std::to_string(batch_rows * batches + 3);
   query_budget counting(relaxed_config());
   BOOST_CHECK_EQUAL(
      evaluate_query("SELECT COUNT(*) AS n FROM sample.positions", counting)["rows"][size_t{0}]["n"].as_string(),
      expected_rows);
   BOOST_CHECK_LT(counting.accounted_bytes, defaults::max_memory_bytes / 4);
   query_budget projecting(relaxed_config());
   const auto page = evaluate_query("SELECT * FROM sample.positions LIMIT 10", projecting);
   BOOST_CHECK_EQUAL(page["rows"].get_array().size(), 10);
   BOOST_CHECK_EQUAL(page["stats"]["scanned_rows"].as_string(), expected_rows);
   query_budget filtering(relaxed_config());
   BOOST_CHECK_EQUAL(
      evaluate_query("SELECT COUNT(*) AS n FROM sample.positions WHERE amount = 1", filtering)["rows"][size_t{0}]["n"]
         .as_string(),
      std::to_string(batch_rows * batches));
   BOOST_CHECK_LT(filtering.accounted_bytes, defaults::max_memory_bytes / 4);
   // What evaluation retains does not scale with the rows scanned: the GROUP BY key of every row is
   // released with its decoded fields, so one group over 16k rows retains far less than a leaked
   // per-row key charge would add.
   local_table_source source(*validating_node);
   const auto retained = [&](const char* sql) {
      query_budget budget(relaxed_config());
      const auto ast = parse_query(sql, budget);
      const auto plan = create_plan(ast, source.describe(ast, budget), budget);
      auto input = source.capture(plan, budget);
      const auto before = budget.accounted_bytes;
      evaluate(plan, std::move(input), budget);
      return budget.accounted_bytes - before;
   };
   constexpr uint64_t retained_ceiling = 1 << 20;
   BOOST_CHECK_LT(retained("SELECT beneficiary, SUM(amount) AS total FROM sample.positions GROUP BY beneficiary"),
                  retained_ceiling);
   // A retained container cell is charged by its whole tree: projecting the nested struct of every
   // row costs at least one variant node per row more than projecting a scalar.
   const auto nested = retained("SELECT nested FROM sample.positions LIMIT 10");
   const auto scalar = retained("SELECT amount FROM sample.positions LIMIT 10");
   BOOST_CHECK_GE(nested, scalar + uint64_t(batch_rows * batches + 3) * sizeof(fc::variant));
}

/// Charges track live state: a text extreme is charged once, finalized aggregates a HAVING rejects are
/// released, and the engine reports the budget's high-water mark rather than its final balance.
BOOST_AUTO_TEST_CASE(retained_charges_track_live_state) {
   constexpr uint32_t extra_rows = 600;
   constexpr uint64_t groups = extra_rows + 3;
   seed(account, 4, extra_rows, "alice"_n, 1);
   produce_block();
   local_table_source source(*validating_node);
   struct measurement {
      uint64_t retained;
      uint64_t peak;
      uint64_t final;
   };
   const auto measure = [&](const char* sql) {
      query_budget budget(relaxed_config());
      const auto ast = parse_query(sql, budget);
      const auto plan = create_plan(ast, source.describe(ast, budget), budget);
      auto input = source.capture(plan, budget);
      const auto before = budget.accounted_bytes;
      evaluate(plan, std::move(input), budget);
      return measurement{budget.accounted_bytes - before, budget.peak_accounted_bytes, budget.accounted_bytes};
   };
   // MAX over text retains one copy of the extreme, charged in the accumulator, the projected cell
   // and the rendered cell; MAX over a number retains no text at all.
   const std::string memo = "fixture";
   BOOST_CHECK_GE(measure("SELECT MAX(memo) AS m FROM sample.positions").retained,
                  measure("SELECT MAX(amount) AS m FROM sample.positions").retained + 3 * memo.size());
   // Finalized aggregates are transient: with every group rejected by HAVING, more aggregates over an
   // already bound field cost their accumulators per group, never a finalized vector as well.
   constexpr uint64_t extra_aggregates = 4;
   const auto few = measure("SELECT key.id AS id, COUNT(*) AS n, MIN(amount) AS lo FROM sample.positions "
                            "GROUP BY key.id HAVING n < 0")
                       .retained;
   const auto many = measure("SELECT key.id AS id, COUNT(*) AS n, MIN(amount) AS lo, MAX(amount) AS hi, "
                             "SUM(amount) AS total, AVG(amount) AS mean, COUNT(amount) AS present "
                             "FROM sample.positions GROUP BY key.id HAVING n < 0")
                        .retained;
   BOOST_CHECK_LT(many - few, groups * extra_aggregates * (sizeof(value) + sizeof(integer)) * 2);
   // The engine's peak statistic is the budget's high-water mark, above the balance left once the
   // decoded rows were released.
   const auto manual = measure("SELECT * FROM sample.positions LIMIT 1");
   BOOST_CHECK_GT(manual.peak, manual.final);
   read_queue reads;
   query_engine engine(relaxed_config(), reads.create_api(*validating_node));
   execute(engine, reads, "SELECT * FROM sample.positions LIMIT 1");
   engine.stop();
   BOOST_CHECK_GE(engine.stats().peak_accounted_bytes, manual.peak);
}

/// Timestamps beyond the four-digit-year range render deterministically instead of failing every row.
BOOST_AUTO_TEST_CASE(time_values_outside_the_iso_range_remain_queryable) {
   local_table_source source(*validating_node);
   query_budget budget(relaxed_config());
   const auto ast = parse_query("SELECT key.id AS id, created FROM sample.positions ORDER BY created DESC", budget);
   const auto plan = create_plan(ast, source.describe(ast, budget), budget);
   auto captured = source.capture(plan, budget);
   // Seeded rows carry a null optional, so `created` starts at a fixed offset of the value bytes; the
   // capture lists the three seeded rows in primary-key order (ids 1, 2, 3).
   constexpr size_t created_offset = sizeof(uint64_t) + sizeof(int64_t) + sizeof(uint8_t) + sizeof(sysio::chain::asset);
   const int64_t extremes[] = {std::numeric_limits<int64_t>::max(), std::numeric_limits<int64_t>::min(),
                               int64_t{253402300800000000}};
   BOOST_REQUIRE_EQUAL(captured.rows.size(), 3);
   for (size_t i = 0; i < captured.rows.size(); ++i) {
      auto& bytes = captured.rows[i].row.value;
      BOOST_REQUIRE_GE(bytes.size(), created_offset + sizeof(int64_t));
      std::memcpy(bytes.data() + created_offset, &extremes[i], sizeof(int64_t));
   }
   const auto result = fc::variant(evaluate(plan, std::move(captured), budget));
   const auto& rows = result["rows"].get_array();
   BOOST_REQUIRE_EQUAL(rows.size(), 3);
   BOOST_CHECK_EQUAL(rows[0]["created"].as_string(), "+294247-01-10T04:00:54.775807");
   BOOST_CHECK_EQUAL(rows[1]["created"].as_string(), "+10000-01-01T00:00:00");
   BOOST_CHECK_EQUAL(rows[2]["created"].as_string(), "-290308-12-21T19:59:05.224192");
   BOOST_CHECK_EQUAL(rows[2]["id"].as_string(), "2");
   query_budget filtering(relaxed_config());
   const auto later_ast = parse_query("SELECT COUNT(*) AS n FROM sample.positions WHERE created > "
                                      "'9999-12-31T23:59:59.999999' AND created <= '+294247-01-10T04:00:54.775807'",
                                      filtering);
   const auto later_plan = create_plan(later_ast, source.describe(later_ast, filtering), filtering);
   auto patched = source.capture(later_plan, filtering);
   for (auto& row : patched.rows) {
      const int64_t microseconds = 253402300800000000;
      std::memcpy(row.row.value.data() + created_offset, &microseconds, sizeof(microseconds));
   }
   BOOST_CHECK_EQUAL(
      fc::variant(evaluate(later_plan, std::move(patched), filtering))["rows"][size_t{0}]["n"].as_string(), "3");
}

/// Checksum literals bind case-insensitively at their exact width; the decoder's lowercase form is canonical.
BOOST_AUTO_TEST_CASE(checksum_literals_match_rows_in_any_case) {
   push_action(account, wide_action, account, fc::mutable_variant_object()("id", 1));
   produce_block();
   query_budget budget(relaxed_config());
   const auto digest =
      evaluate_query("SELECT digest FROM sample.wide", budget)["rows"][size_t{0}]["digest"].as_string();
   BOOST_CHECK_EQUAL(digest, fc::sha256::hash(std::string("fixture")).str());
   std::string uppercase = digest;
   std::transform(uppercase.begin(), uppercase.end(), uppercase.begin(),
                  [](unsigned char c) { return std::toupper(c); });
   for (const auto& literal : {digest, uppercase}) {
      query_budget bound(relaxed_config());
      BOOST_CHECK_EQUAL(evaluate_query("SELECT COUNT(*) AS n FROM sample.wide WHERE digest = '" + literal + "'",
                                       bound)["rows"][size_t{0}]["n"]
                           .as_string(),
                        "1");
   }
   for (const auto* literal : {"'ab'", "'zz'"}) {
      query_budget invalid(relaxed_config());
      BOOST_CHECK_THROW(
         evaluate_query(std::string("SELECT COUNT(*) AS n FROM sample.wide WHERE digest = ") + literal, invalid),
         query_error);
   }
}

/// A row larger than the remaining raw-byte budget is rejected before its storage is copied.
BOOST_AUTO_TEST_CASE(oversized_row_fails_before_copy) {
   local_table_source source(*validating_node);
   query_config config;
   query_budget planning(relaxed_config(config));
   const auto ast = parse_query("SELECT * FROM sample.positions", planning);
   const auto plan = create_plan(ast, source.describe(ast, planning), planning);
   config.max_raw_bytes = 1;
   query_budget capture(relaxed_config(config));
   BOOST_CHECK_EXCEPTION(source.capture(plan, capture), query_error, [](const auto& error) {
      return error.kind == error_kind::QUERY_LIMIT && error.limit == option::max_raw_bytes;
   });
   BOOST_CHECK_EQUAL(capture.scanned_rows, 0);
   BOOST_CHECK_EQUAL(capture.raw_bytes, 0);
}
BOOST_AUTO_TEST_SUITE_END()

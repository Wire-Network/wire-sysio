#include "query_fixture.hpp"

#include <fc/io/raw.hpp>
#include <sysio/chain/account_object.hpp>

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
   query_budget budget({});
   uint64_t pages = 0;
   const auto result = evaluate_query(
      "SELECT beneficiary, SUM(amount) AS total FROM sample.positions GROUP BY beneficiary", budget, &pages);
   BOOST_CHECK_EQUAL(pages, 2);
   BOOST_CHECK_EQUAL(budget.scanned_rows, 603);
   BOOST_CHECK_EQUAL(result["rows"][size_t{0}]["total"].as_string(), "750");
   query_budget all({});
   BOOST_CHECK_EQUAL(
      evaluate_query("SELECT COUNT(*) AS n FROM sample.positions", all)["rows"][size_t{0}]["n"].as_string(), "603");
   query_budget other({});
   BOOST_CHECK_EQUAL(
      evaluate_query("SELECT SUM(amount) AS n FROM other.positions", other)["rows"][size_t{0}]["n"].as_string(), "999");
   query_budget combined({});
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
   query_budget ties({});
   const auto detail = evaluate_query("SELECT amount FROM positions OWNER 'sample', 'other' WHERE key.id = 1", ties);
   BOOST_REQUIRE_EQUAL(detail["rows"].get_array().size(), 2);
   // Chain owner ordering breaks ties before the encoded primary key, independently of OWNER-list order.
   BOOST_CHECK_EQUAL(detail["rows"][size_t{0}]["amount"].as_string(), "999");
   BOOST_CHECK_EQUAL(detail["rows"][size_t{1}]["amount"].as_string(), "60");
   for (const auto* predicate : {"key.id >= 600 AND key.id < 603", "key.id > 599 AND key.id <= 602"}) {
      query_budget range({});
      auto out = evaluate_query(std::string("SELECT key.id AS id FROM sample.positions WHERE ") + predicate, range);
      BOOST_REQUIRE_EQUAL(out["rows"].get_array().size(), 3);
      BOOST_CHECK_EQUAL(range.scanned_rows, 3);
      BOOST_CHECK_EQUAL(out["rows"][size_t{0}]["id"].as_string(), "600");
   }
   query_budget disjunction({});
   const auto fallback =
      evaluate_query("SELECT key.id AS id FROM sample.positions WHERE key.id = 1 OR key.id = 603", disjunction);
   BOOST_CHECK_EQUAL(fallback["rows"].get_array().size(), 2);
   BOOST_CHECK_EQUAL(disjunction.scanned_rows, 603);
}

BOOST_AUTO_TEST_CASE(nulls_having_aliases_empty_inputs_and_limits) {
   for (const auto* condition : {"nullable = 1", "NOT (nullable = 1)", "nullable != 1", "nullable = NULL"}) {
      query_budget budget({});
      BOOST_CHECK(
         evaluate_query(std::string("SELECT beneficiary FROM sample.positions WHERE ") + condition, budget)["rows"]
            .get_array()
            .empty());
   }
   query_budget budget({});
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
   query_budget empty({});
   result =
      evaluate_query("SELECT COUNT(*) AS n, SUM(amount) AS total FROM sample.positions WHERE amount > 999", empty);
   BOOST_CHECK_EQUAL(result["rows"][size_t{0}]["n"].as_string(), "0");
   BOOST_CHECK(result["rows"][size_t{0}]["total"].is_null());
   query_budget grouped({});
   BOOST_CHECK(evaluate_query("SELECT beneficiary FROM sample.positions WHERE amount > 999 GROUP BY beneficiary",
                              grouped)["rows"]
                  .get_array()
                  .empty());
   query_config config;
   config.max_result_rows = 1;
   query_budget cap(config);
   BOOST_CHECK_THROW(evaluate_query("SELECT beneficiary FROM sample.positions", cap), query_error);
   query_budget limited(config);
   result = evaluate_query("SELECT amount AS a FROM sample.positions ORDER BY a DESC LIMIT 1", limited);
   BOOST_CHECK_EQUAL(result["rows"][size_t{0}]["a"].as_string(), "90");
   BOOST_CHECK_EQUAL(limited.scanned_rows, 3);
   query_budget zero({});
   BOOST_CHECK(
      evaluate_query("SELECT SUM(amount) AS total FROM sample.positions LIMIT 0", zero)["rows"].get_array().empty());
   for (const auto* sql :
        {"SELECT beneficiary, SUM(amount) AS total FROM sample.positions",
         "SELECT SUM(amount) AS beneficiary FROM sample.positions GROUP BY beneficiary HAVING beneficiary > 1",
         "SELECT beneficiary AS x, amount AS x FROM sample.positions",
         "SELECT beneficiary FROM sample.positions HAVING beneficiary = 'alice'",
         "SELECT amount AS x FROM sample.positions WHERE x > 1",
         "SELECT * FROM sample.positions GROUP BY beneficiary"}) {
      query_budget invalid({});
      BOOST_CHECK_THROW(evaluate_query(sql, invalid), query_error);
   }
}

BOOST_AUTO_TEST_CASE(nested_assets_wide_and_opaque_values) {
   put(1, position(-7));
   push_action(account, wide_action, account, fc::mutable_variant_object()("id", 1));
   produce_block();
   query_budget budget({});
   const auto result = evaluate_query("SELECT * FROM sample.positions WHERE key.id = 1", budget);
   const auto& row = result["rows"][size_t{0}];
   BOOST_CHECK_EQUAL(row["amount"].as_string(), "-7");
   BOOST_CHECK_EQUAL(row["nested"]["numbers"][size_t{0}].as_string(), "9007199254740993");
   BOOST_CHECK_EQUAL(row["quantity"]["precision"].as_string(), "4");
   BOOST_CHECK_EQUAL(row["quantity"]["amount"].as_string(), "1.25");
   BOOST_CHECK_EQUAL(row["created"].as_string(), "2023-11-14T22:13:20.123000");
   BOOST_CHECK(row["enabled"].as_bool());
   query_budget wide({});
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
      query_budget invalid({});
      BOOST_CHECK_THROW(evaluate_query(sql, invalid), query_error);
   }
   put(2, position(90, {}, "2.000 USD"));
   query_budget units({});
   BOOST_CHECK_THROW(evaluate_query("SELECT SUM(quantity) AS total FROM sample.positions", units), query_error);
}

BOOST_AUTO_TEST_CASE(copied_rows_and_abi_survive_updates) {
   local_table_source source(*validating_node);
   query_budget budget({});
   auto ast = parse_query("SELECT SUM(amount) AS total FROM sample.positions", budget);
   auto plan = create_plan(ast, source.describe(ast, budget), budget);
   auto captured = source.capture(plan, budget);
   const auto identity = captured.state["block_id"].as_string();
   put(1, position(600));
   BOOST_CHECK_EQUAL(fc::variant(evaluate(plan, std::move(captured), budget))["rows"][size_t{0}]["total"].as_string(),
                     "170");
   BOOST_CHECK(identity != validating_node->head().id().str());
   query_budget updated({});
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
      query_budget budget(config);
      BOOST_CHECK_EXCEPTION(
         evaluate_query("SELECT beneficiary, SUM(amount) AS total FROM sample.positions GROUP BY beneficiary", budget),
         query_error, [](const auto& error) { return error.kind == error_kind::QUERY_LIMIT; });
   }
}

/// Composite bounds narrow only leading fields; the residual predicate remains authoritative.
BOOST_AUTO_TEST_CASE(composite_prefix_and_nonleading_fallback) {
   for (const auto beneficiary : {"alice"_n, "bob"_n})
      for (uint64_t sequence = 1; sequence <= 3; ++sequence)
         push_action(
            account, composite_action, account,
            fc::mutable_variant_object()("beneficiary", beneficiary)("sequence", sequence)("row", position(sequence)));
   produce_block();
   query_budget prefix({});
   const auto narrowed = evaluate_query("SELECT key.sequence AS n FROM sample.composite "
                                        "WHERE key.beneficiary = 'alice' AND key.sequence >= 2",
                                        prefix);
   BOOST_CHECK_EQUAL(prefix.scanned_rows, 2);
   BOOST_REQUIRE_EQUAL(narrowed["rows"].get_array().size(), 2);
   BOOST_CHECK_EQUAL(narrowed["rows"][size_t{0}]["n"].as_string(), "2");
   query_budget reference({});
   const auto full = evaluate_query("SELECT key.sequence AS n FROM sample.composite "
                                    "WHERE (key.beneficiary = 'alice' AND key.sequence >= 2) OR FALSE = TRUE",
                                    reference);
   BOOST_CHECK_EQUAL(reference.scanned_rows, 6);
   BOOST_CHECK_EQUAL(fc::json::to_string(narrowed["rows"], fc::time_point::maximum()),
                     fc::json::to_string(full["rows"], fc::time_point::maximum()));
   query_budget nonleading({});
   BOOST_CHECK_EQUAL(evaluate_query("SELECT amount FROM sample.composite WHERE key.sequence = 2", nonleading)["rows"]
                        .get_array()
                        .size(),
                     2);
   BOOST_CHECK_EQUAL(nonleading.scanned_rows, 6);
}

/// The owned ABI remains valid after an update; malformed copies never become binary fallback rows.
BOOST_AUTO_TEST_CASE(owned_abi_and_malformed_rows) {
   local_table_source source(*validating_node);
   query_budget budget({});
   const auto ast = parse_query("SELECT created, amount FROM sample.positions", budget);
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
      query_budget invalid({});
      BOOST_CHECK_EXCEPTION(evaluate(plan, std::move(*input), invalid), query_error,
                            [](const auto& error) { return error.kind == error_kind::ROW_DECODE_ERROR; });
   }
   query_budget changed({});
   BOOST_CHECK_EXCEPTION(source.capture(plan, changed), query_error,
                         [](const auto& error) { return error.kind == error_kind::SCHEMA_CHANGED; });
}

/// Missing extension bytes are SQL null, and adversarial crypto sizes fail before allocation.
BOOST_AUTO_TEST_CASE(binary_extension_and_crypto_length_bounds) {
   push_action(account, wide_action, account, fc::mutable_variant_object()("id", 1));
   produce_block();
   local_table_source source(*validating_node);
   query_budget budget({});
   auto ast = parse_query("SELECT extension FROM sample.wide", budget);
   auto plan = create_plan(ast, source.describe(ast, budget), budget);
   auto input = source.capture(plan, budget);
   input.rows.front().row.value.resize(input.rows.front().row.value.size() - sizeof(uint64_t));
   BOOST_CHECK(fc::variant(evaluate(plan, std::move(input), budget))["rows"][size_t{0}]["extension"].is_null());

   for (const auto* type : {"public_key", "signature"}) {
      query_budget compiled({});
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
      query_budget malformed({});
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
   query_budget budget({});
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
   query_budget current({});
   const auto next = evaluate_query("SELECT SUM(amount) AS total FROM sample.positions", current);
   BOOST_CHECK_EQUAL(next["rows"][size_t{0}]["total"].as_string(), "1010");
   BOOST_CHECK_EQUAL(next["state"]["block_id"].as_string(), fork_block->calculate_id().str());
}

/// HAVING observes the rational average, including aggregate slots absent from SELECT.
BOOST_AUTO_TEST_CASE(exact_having_and_unselected_aggregates) {
   query_budget exact({});
   const auto result = evaluate_query("SELECT AVG(amount) AS mean FROM sample.positions "
                                      "HAVING AVG(amount) < 56.666666666666666667",
                                      exact);
   BOOST_REQUIRE_EQUAL(result["rows"].get_array().size(), 1);
   BOOST_CHECK_EQUAL(result["rows"][size_t{0}]["mean"].as_string(), "56.666666666666666667");
   query_budget hidden({});
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
      query_budget budget({});
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
      query_budget budget({});
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
   query_budget budget({});
   BOOST_CHECK_EXCEPTION(evaluate_query("SELECT amount FROM positions OWNER 'sample', 'other'", budget), query_error,
                         [](const auto& error) { return error.kind == error_kind::QUERY_SEMANTICS; });
   BOOST_CHECK_EQUAL(budget.scanned_rows, 0);
}

/// A row larger than the remaining raw-byte budget is rejected before its storage is copied.
BOOST_AUTO_TEST_CASE(oversized_row_fails_before_copy) {
   local_table_source source(*validating_node);
   query_config config;
   query_budget planning(config);
   const auto ast = parse_query("SELECT * FROM sample.positions", planning);
   const auto plan = create_plan(ast, source.describe(ast, planning), planning);
   config.max_raw_bytes = 1;
   query_budget capture(config);
   BOOST_CHECK_EXCEPTION(source.capture(plan, capture), query_error, [](const auto& error) {
      return error.kind == error_kind::QUERY_LIMIT && error.limit == option::max_raw_bytes;
   });
   BOOST_CHECK_EQUAL(capture.scanned_rows, 0);
   BOOST_CHECK_EQUAL(capture.raw_bytes, 0);
}
BOOST_AUTO_TEST_SUITE_END()

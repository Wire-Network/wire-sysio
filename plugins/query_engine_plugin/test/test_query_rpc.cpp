#include "query_fixture.hpp"
#include "query_schema.hpp"

#include <fc/io/json.hpp>
#include <fc/variant_object.hpp>
#include <sysio/query_engine_plugin/query.hpp>
#include <sysio/query_engine_plugin/query_config.hpp>

#include <boost/test/unit_test.hpp>

#include <magic_enum/magic_enum.hpp>

#include <set>

using namespace sysio::query_engine;
using namespace sysio::query_engine::test;

namespace {
/// Parse the plugin's options exactly as appbase hands them to parse_config.
query_config parse_arguments(const std::vector<std::string>& arguments) {
   namespace po = boost::program_options;
   po::options_description options;
   add_options(options);
   po::variables_map variables;
   po::store(po::command_line_parser(arguments).options(options).run(), variables);
   po::notify(variables);
   return parse_config(variables);
}
/// Keep number-token ID tests independent of any JSON serializer's coercion rules.
query_request parse_id_request(const std::string& id) {
   query_budget budget({});
   return parse_request("{\"jsonrpc\":\"2.0\",\"id\":" + id +
                           ",\"method\":\"query.execute\",\"params\":{\"query\":\"SELECT * FROM sample.wide\"}}",
                        budget);
}
} // namespace

BOOST_AUTO_TEST_SUITE(query_rpc)

BOOST_AUTO_TEST_CASE(exact_rpc_ids_and_unicode) {
   for (const auto* id : {"4294967295", "-4294967295", "42.0", "4.2e1", "-0", "null", "\"9007199254740993\""})
      BOOST_CHECK_NO_THROW(parse_id_request(id));
   for (const auto* id : {"4294967296", "-4294967296", "0.1", "1e100", "true", "[]", "{}"})
      BOOST_CHECK_EXCEPTION(parse_id_request(id), query_error,
                            [](const auto& error) { return error.kind == error_kind::INVALID_REQUEST; });
   BOOST_CHECK_EQUAL(parse_id_request("4.2e1").id.as_int64(), 42);
   const std::string glyph = "\xc3\xa9";
   std::string id;
   for (size_t i = 0; i < constants::max_id_characters; ++i)
      id += glyph;
   BOOST_CHECK_EQUAL(parse_id_request("\"" + id + "\"").id.as_string(), id);
   BOOST_CHECK_THROW(parse_id_request("\"" + id + glyph + "\""), query_error);
}

BOOST_AUTO_TEST_CASE(envelopes_notifications_and_invocation_errors) {
   for (const auto* json : {"[]", "{}", "null", "{\"jsonrpc\":2.0,\"method\":\"query.execute\"}",
                            "{\"jsonrpc\":\"2.0\",\"jsonrpc\":\"2.0\",\"method\":\"query.execute\"}",
                            "{\"jsonrpc\":\"2.0\",\"method\":\"query.execute\",\"extra\":true}"}) {
      query_budget budget({});
      BOOST_CHECK_EXCEPTION(parse_request(json, budget), query_error,
                            [](const auto& error) { return error.kind == error_kind::INVALID_REQUEST; });
   }
   query_budget malformed({});
   BOOST_CHECK_EXCEPTION(parse_request("{", malformed), query_error,
                         [](const auto& error) { return error.kind == error_kind::PARSE_ERROR; });
   query_budget unknown({});
   const auto notification = parse_request("{\"jsonrpc\":\"2.0\",\"method\":\"unknown\"}", unknown);
   BOOST_CHECK(notification.notification);
   BOOST_REQUIRE(notification.invocation_error);
   BOOST_CHECK(notification.invocation_error->kind == error_kind::METHOD_NOT_FOUND);
   query_budget parameters({});
   const auto invalid =
      parse_request("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"query.execute\",\"params\":{\"query\":1}}", parameters);
   BOOST_CHECK_EQUAL(invalid.id.as_int64(), 7);
   BOOST_REQUIRE(invalid.invocation_error);
   BOOST_CHECK(invalid.invocation_error->kind == error_kind::INVALID_PARAMS);
}

BOOST_AUTO_TEST_CASE(startup_rejects_invalid_unsigned_options) {
   for (const auto* value : {"-1", "0", "4294967296", "1.5", "nonsense"})
      BOOST_CHECK_THROW(parse_arguments({std::string("--") + option::worker_threads, value}), query_error);
   query_config config;
   config.max_capture = config.timeout + std::chrono::microseconds(1);
   BOOST_CHECK_THROW(config.validate(), query_error);
   config = {};
   config.max_capture = std::chrono::microseconds::zero();
   BOOST_CHECK_THROW(config.validate(), query_error);
   config = {};
   config.timeout = std::chrono::milliseconds::zero();
   BOOST_CHECK_THROW(config.validate(), query_error);
   config = {};
   config.max_memory_bytes = std::numeric_limits<uint64_t>::max();
   BOOST_CHECK_THROW(config.validate(), query_error);
}

/// A short request timeout clamps the default capture bound instead of failing validation; an
/// explicit bound stands as given and still may not exceed the timeout.
BOOST_AUTO_TEST_CASE(default_capture_bound_follows_a_short_timeout) {
   const auto clamped = parse_arguments({flag(option::timeout_ms), "20"});
   BOOST_CHECK(clamped.timeout == std::chrono::milliseconds(20));
   BOOST_CHECK(clamped.max_capture == std::chrono::milliseconds(20));
   BOOST_CHECK(parse_arguments({}).max_capture == defaults::max_capture);
   BOOST_CHECK(parse_arguments({flag(option::max_capture_ms), "5"}).max_capture == std::chrono::milliseconds(5));
   BOOST_CHECK_THROW(parse_arguments({flag(option::timeout_ms), "20", flag(option::max_capture_ms), "30"}),
                     query_error);
}

/// Error envelopes carry the numeric code, retryability, source position, limit name and the request ID.
BOOST_AUTO_TEST_CASE(error_envelopes_carry_code_retryability_position_and_id) {
   query_budget budget({});
   const auto request = parse_request(
      R"({"jsonrpc":"2.0","id":"probe","method":"query.execute","params":{"query":"SELECT * FROM sample.wide"}})",
      budget);
   const auto syntax =
      create_error(&request, query_error(error_kind::QUERY_SYNTAX, "Invalid query syntax", source_span{2, 7}));
   validate_document(fc::json::to_string(syntax, fc::time_point::maximum()), "query-response");
   BOOST_CHECK_EQUAL(syntax["jsonrpc"].as_string(), constants::version);
   BOOST_CHECK_EQUAL(syntax["id"].as_string(), "probe");
   BOOST_CHECK_EQUAL(syntax["error"]["code"].as_int64(), -32010);
   BOOST_CHECK_EQUAL(syntax["error"]["message"].as_string(), "Invalid query syntax");
   const auto& data = syntax["error"]["data"];
   BOOST_CHECK_EQUAL(data["kind"].as_string(), "QUERY_SYNTAX");
   BOOST_CHECK(!data["retryable"].as_bool());
   BOOST_CHECK_EQUAL(data["line"].as_uint64(), 2);
   BOOST_CHECK_EQUAL(data["column"].as_uint64(), 7);
   BOOST_CHECK(data["limit"].is_null());
   const auto limit = create_error(&request, query_error(error_kind::QUERY_LIMIT, "Query resource limit exceeded",
                                                         std::nullopt, option::max_scan_rows));
   validate_document(fc::json::to_string(limit, fc::time_point::maximum()), "query-response");
   BOOST_CHECK_EQUAL(limit["error"]["code"].as_int64(), -32012);
   BOOST_CHECK_EQUAL(limit["error"]["data"]["limit"].as_string(), option::max_scan_rows);
   BOOST_CHECK(limit["error"]["data"]["line"].is_null());
   BOOST_CHECK(limit["error"]["data"]["column"].is_null());
   const auto busy = create_error(nullptr, query_error(error_kind::QUERY_BUSY, "Query queue is full"));
   BOOST_CHECK(busy["id"].is_null());
   BOOST_CHECK_EQUAL(busy["error"]["code"].as_int64(), -32014);
   BOOST_CHECK(busy["error"]["data"]["retryable"].as_bool());
   std::set<int> codes;
   for (const auto kind : magic_enum::enum_values<error_kind>()) {
      BOOST_CHECK(codes.insert(error_code(kind)).second);
      const bool transient = kind == error_kind::QUERY_TIMEOUT || kind == error_kind::QUERY_BUSY ||
                             kind == error_kind::SCHEMA_CHANGED || kind == error_kind::STATE_UNAVAILABLE ||
                             kind == error_kind::QUERY_CANCELLED;
      BOOST_CHECK_EQUAL(retryable(kind), transient);
   }
   BOOST_CHECK_EQUAL(error_code(error_kind::PARSE_ERROR), -32700);
   BOOST_CHECK_EQUAL(error_code(error_kind::INTERNAL_ERROR), -32603);
}
BOOST_AUTO_TEST_SUITE_END()

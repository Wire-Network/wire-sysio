#include <fc/io/json.hpp>
#include <fc/variant_object.hpp>
#include <sysio/query_engine_plugin/query.hpp>
#include <sysio/query_engine_plugin/query_config.hpp>

#include <boost/test/unit_test.hpp>

using namespace sysio::query_engine_plugin;

namespace {
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
   namespace po = boost::program_options;
   po::options_description options;
   add_options(options);
   for (const auto* value : {"-1", "0", "4294967296", "1.5", "nonsense"}) {
      const std::vector<std::string> arguments = {std::string("--") + option::worker_threads, value};
      po::variables_map variables;
      po::store(po::command_line_parser(arguments).options(options).run(), variables);
      po::notify(variables);
      BOOST_CHECK_THROW(parse_config(variables), query_error);
   }
   query_config config;
   config.max_capture_ms = config.timeout_ms + 1;
   BOOST_CHECK_THROW(config.validate(), query_error);
   config = {};
   config.max_memory_bytes = std::numeric_limits<uint64_t>::max();
   BOOST_CHECK_THROW(config.validate(), query_error);
}
BOOST_AUTO_TEST_SUITE_END()

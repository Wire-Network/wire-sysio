#pragma once
#include <fc/io/json.hpp>
#include <fc/variant_object.hpp>
#include <sysio/query_engine_plugin/query.hpp>

#include <boost/test/unit_test.hpp>

#include <rapidjson/document.h>
#include <rapidjson/schema.h>
#include <rapidjson/stringbuffer.h>

#include <algorithm>
#include <fstream>
#include <optional>
#include <regex>
#include <set>
#include <string>

namespace sysio::query_engine::test {
/// The `id` every test request envelope carries.
inline constexpr auto request_id = "test";
/// A query over the fixture's wide table, for envelope tests that never execute it.
inline constexpr auto wide_select_sql = "SELECT * FROM sample.wide";

/// The production `query.execute` envelope whose params carry `query` plus `members`, each written
/// `,"name":value` and spelled verbatim so number tokens reach the parser, route or validator exactly
/// as written. No second route implementation is involved.
inline std::string request_body(const std::string& sql, const std::string& members = {}) {
   const auto json = [](const std::string& text) {
      return fc::json::to_string(fc::variant(text), fc::time_point::maximum());
   };
   return R"({"jsonrpc":)" + json(constants::version) + R"(,"id":)" + json(request_id) + R"(,"method":)" +
          json(constants::method) + R"(,"params":{)" + json(request_field::query) + ":" + json(sql) + members + "}}";
}

/// Read a checked-in schema/example independently of the test's working directory.
inline std::string read_document(const std::string& relative) {
   std::ifstream input(std::string(QUERY_PLUGIN_SOURCE_DIR) + "/" + relative);
   BOOST_REQUIRE_MESSAGE(input.good(), relative);
   return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

/// Check the exact serialized body against the shipped Draft-04 schema, returning the JSON pointer of
/// the first violation, or nothing when the body is valid.
inline std::optional<std::string> schema_violation(const std::string& body, const std::string& schema_name) {
   rapidjson::Document schema_json, document;
   schema_json.Parse(read_document("schema/" + schema_name + ".schema.json").c_str());
   BOOST_REQUIRE(!schema_json.HasParseError());
   document.Parse(body.c_str());
   BOOST_REQUIRE_MESSAGE(!document.HasParseError(), body);
   rapidjson::SchemaDocument schema(schema_json);
   rapidjson::SchemaValidator validator(schema);
   if (document.Accept(validator))
      return std::nullopt;
   rapidjson::StringBuffer path;
   validator.GetInvalidDocumentPointer().StringifyUriFragment(path);
   return std::string(path.GetString());
}

/// Validate the exact serialized body using the shipped Draft-04 schema.
inline void validate_document(const std::string& body, const std::string& schema_name) {
   const auto violation = schema_violation(body, schema_name);
   BOOST_REQUIRE_MESSAGE(!violation, schema_name << " at " << violation.value_or("") << ": " << body);
}

/// Dynamic columns impose invariants which a static JSON schema cannot express.
inline void validate_response(const std::string& body) {
   static const std::regex unsigned_decimal("^(0|[1-9][0-9]*)$");
   static const std::regex signed_integer("^-?(0|[1-9][0-9]*)$");
   static const std::regex decimal("^-?(0|[1-9][0-9]*)(\\.[0-9]+)?$");
   static const std::regex ieee_hex("^0x([0-9a-f]{8}|[0-9a-f]{16}|[0-9a-f]{32})$");
   validate_document(body, "query-response");
   const auto response = fc::json::from_string(body);
   if (!response.get_object().contains("result"))
      return;
   const auto& result = response["result"];
   std::set<std::string> names;
   for (const auto& column : result["columns"].get_array())
      BOOST_REQUIRE(names.insert(column["name"].as_string()).second);
   const auto& rows = result["rows"].get_array();
   BOOST_CHECK_EQUAL(result["stats"]["returned_rows"].as_string(), std::to_string(rows.size()));
   // The page window agrees with the rows and work counters it describes.
   const auto& page = result[response_field::page];
   BOOST_CHECK_EQUAL(page[response_field::returned_rows].as_string(),
                     result[response_field::stats][response_field::returned_rows].as_string());
   const auto offset = std::stoull(page[response_field::offset].as_string());
   const auto total = std::stoull(page[response_field::total_rows].as_string());
   BOOST_CHECK_LE(rows.size(), total - std::min(offset, total));
   BOOST_CHECK_EQUAL(page[response_field::has_more].as_bool(), std::min(offset, total) + rows.size() < total);
   if (!page[response_field::limit].is_null())
      BOOST_CHECK_LE(rows.size(), std::stoull(page[response_field::limit].as_string()));
   for (const auto& row : rows) {
      BOOST_REQUIRE_EQUAL(row.get_object().size(), names.size());
      for (const auto& column : result["columns"].get_array()) {
         const auto name = column["name"].as_string();
         BOOST_REQUIRE(row.get_object().contains(name));
         const auto& cell = row[name.c_str()];
         if (cell.is_null()) {
            BOOST_CHECK(column["nullable"].as_bool());
            continue;
         }
         const auto encoding = column["encoding"].as_string();
         if (encoding == "boolean")
            BOOST_CHECK(cell.is_bool());
         else if (encoding == "asset_object") {
            BOOST_REQUIRE(cell.is_object());
            BOOST_REQUIRE(cell["amount"].is_string());
            BOOST_CHECK(std::regex_match(cell["amount"].as_string(), decimal));
            BOOST_CHECK(cell["symbol"].is_string());
            BOOST_REQUIRE(cell["precision"].is_string());
            BOOST_CHECK(std::regex_match(cell["precision"].as_string(), unsigned_decimal));
            const bool extended = column["logical_type"].as_string() == "extended_asset";
            BOOST_CHECK_EQUAL(cell.get_object().size(), extended ? 4u : 3u);
            if (extended)
               BOOST_CHECK(cell["contract"].is_string());
         } else if (encoding != "json") {
            BOOST_REQUIRE(cell.is_string());
            if (encoding == "decimal_string")
               BOOST_CHECK(std::regex_match(
                  cell.as_string(), column["logical_type"].as_string() == "integer" ? signed_integer : decimal));
            else if (encoding == "ieee_hex")
               BOOST_CHECK(std::regex_match(cell.as_string(), ieee_hex));
         }
      }
   }
   const auto& owners = result["source"]["owners"].get_array();
   const auto& abis = result["state"]["abis"].get_array();
   BOOST_REQUIRE_EQUAL(owners.size(), abis.size());
   for (size_t i = 0; i < owners.size(); ++i)
      BOOST_CHECK_EQUAL(owners[i].as_string(), abis[i]["owner"].as_string());
}
} // namespace sysio::query_engine::test

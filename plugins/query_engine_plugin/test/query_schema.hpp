#pragma once
#include <fc/io/json.hpp>
#include <fc/variant_object.hpp>

#include <boost/test/unit_test.hpp>

#include <rapidjson/document.h>
#include <rapidjson/schema.h>
#include <rapidjson/stringbuffer.h>

#include <fstream>
#include <regex>
#include <set>

namespace sysio::query_engine_plugin::test {
/// Read a checked-in schema/example independently of the test's working directory.
inline std::string read_document(const std::string& relative) {
   std::ifstream input(std::string(QUERY_PLUGIN_SOURCE_DIR) + "/" + relative);
   BOOST_REQUIRE_MESSAGE(input.good(), relative);
   return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

/// Validate the exact serialized body using the shipped Draft-04 schema.
inline void validate_document(const std::string& body, const std::string& schema_name) {
   rapidjson::Document schema_json, document;
   schema_json.Parse(read_document("schema/" + schema_name + ".schema.json").c_str());
   BOOST_REQUIRE(!schema_json.HasParseError());
   document.Parse(body.c_str());
   BOOST_REQUIRE_MESSAGE(!document.HasParseError(), body);
   rapidjson::SchemaDocument schema(schema_json);
   rapidjson::SchemaValidator validator(schema);
   const bool valid = document.Accept(validator);
   rapidjson::StringBuffer path;
   validator.GetInvalidDocumentPointer().StringifyUriFragment(path);
   BOOST_REQUIRE_MESSAGE(valid, schema_name << " at " << path.GetString() << ": " << body);
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
} // namespace sysio::query_engine_plugin::test

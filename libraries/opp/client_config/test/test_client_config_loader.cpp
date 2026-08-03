#include <sysio/opp/config/client_config_loader.hpp>

#include <boost/test/unit_test.hpp>

#include <fc/filesystem.hpp>

#include <fstream>
#include <string>
#include <string_view>

namespace config = sysio::opp::config;

namespace {

std::filesystem::path write_configuration(fc::temp_directory& directory,
                                          std::string_view contents,
                                          std::string_view name = "client-config.json") {
   const auto path = directory.path() / std::string(name);
   std::ofstream output(path, std::ios::binary);
   output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
   output.close();
   return path;
}

config::client_config_reason rejection_reason(std::string_view contents) {
   fc::temp_directory directory;
   const auto path = write_configuration(directory, contents);
   try {
      (void) config::load_evm_client_configuration_file(path);
      BOOST_FAIL("expected configuration rejection");
   } catch (const config::client_config_exception& rejection) {
      return rejection.reason();
   }
   return config::client_config_reason::proto_json_invalid;
}

constexpr std::string_view valid_configuration = R"json({
  "schema_version": 1,
  "clients": [{
    "connection": {
      "client_id": "ethereum-mainnet",
      "signature_provider_id": "signer-a",
      "rpc_url": "https://rpc.example.invalid/v1"
    },
    "chain_id": 1,
    "transaction_policy": {
      "max_priority_fee_per_gas_wei": "2000000000",
      "max_fee_per_gas_wei": "100000000000",
      "max_gas_limit": "2000000",
      "max_total_native_cost_wei": "250000000000000000"
    }
  }, {
    "connection": {
      "client_id": "ethereum-sepolia",
      "signature_provider_id": "signer-b",
      "rpc_url": "http://127.0.0.1:8545"
    },
    "chain_id": 11155111
  }]
})json";

} // namespace

BOOST_AUTO_TEST_SUITE(opp_client_config_loader_tests)

BOOST_AUTO_TEST_CASE(loads_generated_model_with_optional_policy) {
   fc::temp_directory directory;
   const auto configuration = config::load_evm_client_configuration_file(
      write_configuration(directory, valid_configuration));

   BOOST_REQUIRE(configuration.has_schema_version());
   BOOST_CHECK_EQUAL(configuration.schema_version(), 1u);
   BOOST_REQUIRE_EQUAL(configuration.clients_size(), 2);
   BOOST_CHECK_EQUAL(configuration.clients(0).connection().client_id(), "ethereum-mainnet");
   BOOST_CHECK_EQUAL(configuration.clients(0).chain_id(), 1u);
   BOOST_REQUIRE(configuration.clients(0).has_transaction_policy());
   BOOST_CHECK_EQUAL(configuration.clients(0).transaction_policy().max_gas_limit(), "2000000");
   BOOST_CHECK(!configuration.clients(1).has_transaction_policy());
}

BOOST_AUTO_TEST_CASE(accepts_transport_valid_bracketed_ipv6_host) {
   fc::temp_directory directory;
   const auto path = write_configuration(directory, R"json({
      "schema_version":1,
      "clients":[{
        "connection":{"client_id":"a","signature_provider_id":"prod/signing","rpc_url":"http://[::1]:8545"},
        "chain_id":1
      }]
   })json");
   BOOST_CHECK_NO_THROW(config::load_evm_client_configuration_file(path));
}

BOOST_AUTO_TEST_CASE(rejects_file_bounds_and_invalid_proto_json) {
   fc::temp_directory directory;
   try {
      (void) config::load_evm_client_configuration_file(directory.path() / "missing.json");
      BOOST_FAIL("expected unreadable file rejection");
   } catch (const config::client_config_exception& rejection) {
      BOOST_CHECK(rejection.reason() == config::client_config_reason::file_unreadable);
   }

   const std::string oversized(config::max_client_configuration_file_size + 1, ' ');
   try {
      (void) config::load_evm_client_configuration_file(
         write_configuration(directory, oversized, "oversized.json"));
      BOOST_FAIL("expected oversized file rejection");
   } catch (const config::client_config_exception& rejection) {
      BOOST_CHECK(rejection.reason() == config::client_config_reason::file_too_large);
   }

   BOOST_CHECK(rejection_reason("[]") == config::client_config_reason::proto_json_invalid);
   BOOST_CHECK(rejection_reason("{") == config::client_config_reason::proto_json_invalid);

   std::string embedded_nul = std::string(valid_configuration) + '\0' + "{}";
   BOOST_CHECK(rejection_reason(embedded_nul) == config::client_config_reason::proto_json_invalid);
}

BOOST_AUTO_TEST_CASE(rejects_unknown_fields_and_proto_json_invalid_values) {
   BOOST_CHECK(rejection_reason(R"json({"schema_version":1,"clients":[],"unexpected":true})json") ==
               config::client_config_reason::proto_json_invalid);
   BOOST_CHECK(rejection_reason(R"json({"schema_version":1,"clients":true})json") ==
               config::client_config_reason::proto_json_invalid);
}

BOOST_AUTO_TEST_CASE(follows_standard_proto_json_value_semantics) {
   fc::temp_directory directory;
   const auto configuration = config::load_evm_client_configuration_file(
      write_configuration(directory, R"json({
         "schema_version":"1",
         "clients":[{
           "connection":{"client_id":"a","signature_provider_id":"s","rpc_url":"http://localhost"},
           "chain_id":1.0,
           "transaction_policy":null
         }]
      })json"));

   BOOST_REQUIRE(configuration.has_schema_version());
   BOOST_CHECK_EQUAL(configuration.schema_version(), 1u);
   BOOST_REQUIRE_EQUAL(configuration.clients_size(), 1);
   BOOST_CHECK_EQUAL(configuration.clients(0).chain_id(), 1u);
   BOOST_CHECK(!configuration.clients(0).has_transaction_policy());
}

BOOST_AUTO_TEST_CASE(rejects_semantically_invalid_configuration) {
   BOOST_CHECK(rejection_reason(R"json({"schema_version":2,"clients":[]})json") ==
               config::client_config_reason::schema_version_unsupported);
   BOOST_CHECK(rejection_reason(R"json({"schema_version":1,"clients":[]})json") ==
               config::client_config_reason::client_missing);
   BOOST_CHECK(rejection_reason(R"json({"schema_version":1,"clients":[{"chain_id":1}]})json") ==
               config::client_config_reason::connection_missing);
   BOOST_CHECK(rejection_reason(R"json({
      "schema_version":1,
      "clients":[{
        "connection":{"client_id":"bad/id","signature_provider_id":"s","rpc_url":"http://localhost"},
        "chain_id":1
      }]
   })json") == config::client_config_reason::client_identifier_invalid);
   BOOST_CHECK(rejection_reason(R"json({
      "schema_version":1,
      "clients":[{
        "connection":{"client_id":"a","signature_provider_id":"","rpc_url":"http://localhost"},
        "chain_id":1
      }]
   })json") == config::client_config_reason::signature_provider_identifier_invalid);
   BOOST_CHECK(rejection_reason(R"json({
      "schema_version":1,
      "clients":[{
        "connection":{"client_id":"a","signature_provider_id":"s","rpc_url":"file:///tmp/rpc"},
        "chain_id":1
      }]
   })json") == config::client_config_reason::rpc_url_invalid);
   BOOST_CHECK(rejection_reason(R"json({
      "schema_version":1,
      "clients":[{
        "connection":{"client_id":"a","signature_provider_id":"s","rpc_url":"http://#"},
        "chain_id":1
      }]
   })json") == config::client_config_reason::rpc_url_invalid);
   BOOST_CHECK(rejection_reason(R"json({
      "schema_version":1,
      "clients":[{
        "connection":{"client_id":"a","signature_provider_id":"s","rpc_url":"http://host:80:90"},
        "chain_id":1
      }]
   })json") == config::client_config_reason::rpc_url_invalid);
   BOOST_CHECK(rejection_reason(R"json({
      "schema_version":1,
      "clients":[{
        "connection":{"client_id":"a","signature_provider_id":"s","rpc_url":"http://example.invalid#secret"},
        "chain_id":1
      }]
   })json") == config::client_config_reason::rpc_url_invalid);
   BOOST_CHECK(rejection_reason(R"json({
      "schema_version":1,
      "clients":[{
        "connection":{"client_id":"a","signature_provider_id":"s","rpc_url":"http://localhost"},
        "chain_id":0
      }]
   })json") == config::client_config_reason::chain_id_invalid);
   BOOST_CHECK(rejection_reason(R"json({
      "schema_version":1,
      "clients":[
        {"connection":{"client_id":"a","signature_provider_id":"s","rpc_url":"http://localhost"},"chain_id":1},
        {"connection":{"client_id":"a","signature_provider_id":"t","rpc_url":"http://localhost"},"chain_id":2}
      ]
   })json") == config::client_config_reason::client_duplicate);
}

BOOST_AUTO_TEST_CASE(rejects_incomplete_noncanonical_and_inconsistent_policy) {
   BOOST_CHECK(rejection_reason(R"json({
      "schema_version":1,
      "clients":[{
        "connection":{"client_id":"a","signature_provider_id":"s","rpc_url":"http://localhost"},
        "chain_id":1,
        "transaction_policy":{"max_fee_per_gas_wei":"10"}
      }]
   })json") == config::client_config_reason::policy_incomplete);
   BOOST_CHECK(rejection_reason(R"json({
      "schema_version":1,
      "clients":[{
        "connection":{"client_id":"a","signature_provider_id":"s","rpc_url":"http://localhost"},
        "chain_id":1,
        "transaction_policy":{
          "max_priority_fee_per_gas_wei":"01",
          "max_fee_per_gas_wei":"10",
          "max_gas_limit":"100",
          "max_total_native_cost_wei":"1000"
        }
      }]
   })json") == config::client_config_reason::policy_value_invalid);
   BOOST_CHECK(rejection_reason(R"json({
      "schema_version":1,
      "clients":[{
        "connection":{"client_id":"a","signature_provider_id":"s","rpc_url":"http://localhost"},
        "chain_id":1,
        "transaction_policy":{
          "max_priority_fee_per_gas_wei":"11",
          "max_fee_per_gas_wei":"10",
          "max_gas_limit":"100",
          "max_total_native_cost_wei":"1000"
        }
      }]
   })json") == config::client_config_reason::policy_fee_relationship_invalid);
}

BOOST_AUTO_TEST_CASE(redacts_unknown_fields_and_urls_from_failures) {
   constexpr std::string_view secret = "https://user:password@example.invalid/rpc?token=secret";
   fc::temp_directory directory;
   const auto path = write_configuration(directory, R"json({
      "schema_version":1,
      "clients":[{
        "connection":{
          "client_id":"a",
          "signature_provider_id":"s",
          "rpc_url":"https://user:password@example.invalid/rpc?token=secret"
        },
        "chain_id":1
      }],
      "unknown":true
   })json");
   try {
      (void) config::load_evm_client_configuration_file(path);
      BOOST_FAIL("expected unknown-field rejection");
   } catch (const config::client_config_exception& rejection) {
      BOOST_CHECK(rejection.observed().find(secret) == std::string::npos);
      BOOST_CHECK(rejection.to_detail_string().find(secret) == std::string::npos);
   }
}

BOOST_AUTO_TEST_SUITE_END()

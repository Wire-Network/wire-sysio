#include <boost/test/unit_test.hpp>

#include <gsl-lite/gsl-lite.hpp>

#include <fc/crypto/ethereum/ethereum_utils.hpp>
#include <fc/crypto/private_key.hpp>
#include <fc/filesystem.hpp>
#include <fc/network/ethereum/ethereum_rlp_encoder.hpp>

#include <sysio/outpost_ethereum_client_plugin.hpp>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <functional>
#include <string>
#include <utility>
#include <vector>

using namespace fc::crypto;
using namespace fc::crypto::ethereum;
using namespace fc::network::ethereum;
namespace ethabi = fc::network::ethereum::abi;

namespace {

constexpr std::string_view fake_rpc_url = "http://127.0.0.1:1";
constexpr std::string_view contract_address = "5FbDB2315678afecb367f032d93F642f64180aa3";
constexpr std::string_view transaction_hash =
   "0x1111111111111111111111111111111111111111111111111111111111111111";
constexpr std::string_view signer_public_key =
   "0x8318535b54105d4a7aae60c08fc45f9687181b4fdfc625bd1a753fa7397fed7535"
   "47f11ca8696646f2f3acb08e31016afac23e630c5d11f59f61fef57b0d2aa5";
constexpr std::string_view signer_private_key =
   "0xac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80";
constexpr std::string_view max_uint256_decimal =
   "115792089237316195423570985008687907853269984665640564039457584007913129639935";

ethereum_transaction_policy bounded_policy(std::string client_id = "client-a") {
   return ethereum_transaction_policy{
      .client_id = std::move(client_id),
      .chain_id = 31337,
      .max_priority_fee_per_gas = 10,
      .max_fee_per_gas = 100,
      .max_gas_limit = 1000,
      .max_total_native_cost = 100000,
   };
}

ethabi::contract no_argument_function(std::string name) {
   return ethabi::contract{
      .name = std::move(name),
      .type = ethabi::invoke_target_type::function,
      .inputs = {},
      .outputs = {},
   };
}

ethabi::contract bytes_argument_function(std::string name) {
   return ethabi::contract{
      .name = std::move(name),
      .type = ethabi::invoke_target_type::function,
      .inputs = {ethabi::component_type{"data", ethabi::data_type::bytes}},
      .outputs = {},
   };
}

ethabi::contract uint32_argument_function(std::string name) {
   return ethabi::contract{
      .name = std::move(name),
      .type = ethabi::invoke_target_type::function,
      .inputs = {ethabi::component_type{"wireEpochIndex", ethabi::data_type::uint32}},
      .outputs = {},
   };
}

signature_provider_ptr make_recording_signer(std::atomic<size_t>& sign_count) {
   auto private_key = fc::crypto::private_key::generate(fc::crypto::private_key::key_type::em);
   auto ethereum_key = private_key.get<fc::em::private_key_shim>();
   auto provider = std::make_shared<signature_provider_t>();
   provider->target_chain = chain_kind_ethereum;
   provider->key_type = chain_key_type_ethereum;
   provider->key_name = "recording-signer";
   provider->public_key = private_key.get_public_key();
   provider->private_key.reset();
   provider->sign = [ethereum_key, &sign_count](const fc::sha256& digest) {
      ++sign_count;
      const fc::crypto::keccak256 ethereum_digest{digest.str()};
      return signature(signature::storage_type(ethereum_key.sign_keccak256(ethereum_digest)));
   };
   return provider;
}

class recording_ethereum_client final : public ethereum_client {
public:
   recording_ethereum_client(const signature_provider_ptr& provider,
                             ethereum_transaction_policy   policy)
      : ethereum_client(provider, std::string(fake_rpc_url), std::move(policy)) {}

   fc::variant execute(const std::string& method, const fc::variant& params) override {
      methods.emplace_back(method);
      if (method == "eth_maxPriorityFeePerGas") return fc::variant("0xa");
      if (method == "eth_getBlockByNumber") {
         return fc::variant(fc::mutable_variant_object("baseFeePerGas", "0x2d"));
      }
      if (method == "eth_estimateGas") {
         estimate_gas_params = params;
         return fc::variant("0x342");
      }
      if (method == "eth_getTransactionCount") return fc::variant("0x0");
      if (method == "eth_sendRawTransaction") {
         ++broadcast_count;
         return fc::variant(std::string(transaction_hash));
      }
      FC_THROW_EXCEPTION(fc::invalid_arg_exception, "unexpected fake RPC method {}", method);
   }

   std::vector<std::string> methods;
   fc::variant              estimate_gas_params;
   size_t                   broadcast_count = 0;
};

eip1559_tx exact_transaction() {
   return eip1559_tx{
      .chain_id = 31337,
      .nonce = 0,
      .max_priority_fee_per_gas = 10,
      .max_fee_per_gas = 100,
      .gas_limit = 1000,
      .to = fc::crypto::ethereum::to_address(std::string(contract_address)),
      .value = 0,
      .data = {},
      .access_list = {},
   };
}

void expect_policy_rejection(const std::function<void()>& operation) {
   BOOST_CHECK_THROW(operation(), ethereum_transaction_policy_exception);
}

std::filesystem::path write_policy_file(fc::temp_directory& directory, std::string_view contents) {
   const auto path = directory.path() / "ethereum-transaction-policy.json";
   std::ofstream output{path};
   output << contents;
   output.close();
   return path;
}

void with_initialized_outpost_plugin(
   const std::filesystem::path& policy_file,
   const std::vector<std::string>& client_specs,
   const std::function<void(sysio::outpost_ethereum_client_plugin&)>& inspect_plugin) {
   auto reset_application = gsl_lite::finally([] { appbase::application::reset_app_singleton(); });
   appbase::scoped_app test_application{};

   const std::string signature_spec =
      "signer-a,ethereum,ethereum," + std::string(signer_public_key) + ",KEY:" +
      std::string(signer_private_key);
   std::vector<std::string> arguments{
      "test_outpost_ethereum_transaction_policy",
      "--signature-provider",
      signature_spec,
      "--outpost-ethereum-transaction-policy-file",
      policy_file.string(),
   };
   for (const auto& client_spec : client_specs) {
      arguments.emplace_back("--outpost-ethereum-client");
      arguments.emplace_back(client_spec);
   }

   std::vector<char*> argv;
   argv.reserve(arguments.size());
   for (auto& argument : arguments) argv.emplace_back(argument.data());

   if (!test_application->initialize<sysio::outpost_ethereum_client_plugin>(argv.size(), argv.data())) {
      FC_THROW_EXCEPTION(fc::invalid_arg_exception, "test application initialization returned false");
   }
   inspect_plugin(test_application->get_plugin<sysio::outpost_ethereum_client_plugin>());
}

std::vector<sysio::ethereum_client_entry_ptr>
initialize_outpost_plugin(const std::filesystem::path& policy_file,
                          const std::vector<std::string>& client_specs) {
   std::vector<sysio::ethereum_client_entry_ptr> clients;
   with_initialized_outpost_plugin(
      policy_file,
      client_specs,
      [&](auto& plugin) { clients = plugin.get_clients(); });
   return clients;
}

ethereum_transaction_policy_reason startup_rejection_reason(
   const std::filesystem::path& policy_file,
   const std::vector<std::string>& client_specs) {
   try {
      initialize_outpost_plugin(policy_file, client_specs);
      BOOST_FAIL("expected startup policy rejection");
   } catch (const ethereum_transaction_policy_exception& rejection) {
      return rejection.reason();
   }
   return ethereum_transaction_policy_reason::configuration_schema_invalid;
}

} // namespace

BOOST_AUTO_TEST_SUITE(outpost_ethereum_transaction_policy_tests)

BOOST_AUTO_TEST_CASE(final_signing_boundary_rejects_without_signing_or_broadcasting) {
   std::atomic<size_t> sign_count = 0;
   const auto provider = make_recording_signer(sign_count);
   auto client = std::make_shared<recording_ethereum_client>(provider, bounded_policy());
   const auto contract = no_argument_function("submit");

   std::vector<eip1559_tx> rejected_transactions;
   auto transaction = exact_transaction();
   ++transaction.max_priority_fee_per_gas;
   rejected_transactions.emplace_back(transaction);
   transaction = exact_transaction();
   ++transaction.max_fee_per_gas;
   rejected_transactions.emplace_back(transaction);
   transaction = exact_transaction();
   ++transaction.gas_limit;
   rejected_transactions.emplace_back(transaction);
   transaction = exact_transaction();
   transaction.gas_limit = 999;
   transaction.value = 101;
   rejected_transactions.emplace_back(transaction);
   transaction = exact_transaction();
   transaction.max_fee_per_gas = 9;
   rejected_transactions.emplace_back(transaction);
   transaction = exact_transaction();
   ++transaction.chain_id;
   rejected_transactions.emplace_back(transaction);

   for (const auto& rejected : rejected_transactions) {
      expect_policy_rejection([&] { client->execute_contract_tx_fn(rejected, contract); });
   }

   auto wide_policy = bounded_policy("wide-client");
   const fc::uint256 maximum{std::string(max_uint256_decimal)};
   wide_policy.max_priority_fee_per_gas = maximum;
   wide_policy.max_fee_per_gas = maximum;
   wide_policy.max_gas_limit = maximum;
   wide_policy.max_total_native_cost = maximum;
   auto wide_client = std::make_shared<recording_ethereum_client>(provider, wide_policy);

   transaction = exact_transaction();
   transaction.max_priority_fee_per_gas = 1;
   transaction.max_fee_per_gas = maximum;
   transaction.gas_limit = 2;
   expect_policy_rejection([&] { wide_client->execute_contract_tx_fn(transaction, contract); });

   transaction.max_fee_per_gas = maximum - 1;
   transaction.gas_limit = 1;
   transaction.value = 2;
   expect_policy_rejection([&] { wide_client->execute_contract_tx_fn(transaction, contract); });

   BOOST_CHECK_EQUAL(sign_count.load(), 0u);
   BOOST_CHECK_EQUAL(client->broadcast_count, 0u);
   BOOST_CHECK_EQUAL(wide_client->broadcast_count, 0u);
}

BOOST_AUTO_TEST_CASE(exact_caps_reach_the_signer_and_broadcast_once) {
   std::atomic<size_t> sign_count = 0;
   const auto provider = make_recording_signer(sign_count);
   auto client = std::make_shared<recording_ethereum_client>(provider, bounded_policy());

   const auto result = client->execute_contract_tx_fn(exact_transaction(), no_argument_function("submit"));
   BOOST_CHECK_EQUAL(result.as_string(), transaction_hash);
   BOOST_CHECK_EQUAL(sign_count.load(), 1u);
   BOOST_CHECK_EQUAL(client->broadcast_count, 1u);
}

BOOST_AUTO_TEST_CASE(two_clients_enforce_their_own_runtime_policies) {
   std::atomic<size_t> sign_count = 0;
   const auto provider = make_recording_signer(sign_count);

   auto client_a = std::make_shared<recording_ethereum_client>(provider, bounded_policy("client-a"));
   auto policy_b = bounded_policy("client-b");
   policy_b.chain_id = 1;
   policy_b.max_fee_per_gas = 99;
   auto client_b = std::make_shared<recording_ethereum_client>(provider, policy_b);

   BOOST_CHECK_NO_THROW(
      client_a->execute_contract_tx_fn(exact_transaction(), no_argument_function("submit")));

   auto transaction_b = exact_transaction();
   transaction_b.chain_id = 1;
   expect_policy_rejection(
      [&] { client_b->execute_contract_tx_fn(transaction_b, no_argument_function("submit")); });

   BOOST_CHECK_EQUAL(sign_count.load(), 1u);
   BOOST_CHECK_EQUAL(client_a->broadcast_count, 1u);
   BOOST_CHECK_EQUAL(client_b->broadcast_count, 0u);
}

BOOST_AUTO_TEST_CASE(default_transaction_uses_priority_fee_in_estimate_payload_and_local_chain_id) {
   std::atomic<size_t> sign_count = 0;
   const auto provider = make_recording_signer(sign_count);
   auto client = std::make_shared<recording_ethereum_client>(provider, bounded_policy());

   const auto transaction = client->create_default_tx(
      std::string(contract_address), no_argument_function("submit"));
   BOOST_CHECK_EQUAL(transaction.chain_id, 31337);
   BOOST_CHECK_EQUAL(transaction.max_priority_fee_per_gas, 10);
   BOOST_CHECK_EQUAL(transaction.max_fee_per_gas, 100);
   BOOST_CHECK_EQUAL(transaction.gas_limit, 1000);

   const fc::uint256 wide_priority{"18446744073709551617"};
   const fc::uint256 wide_max_fee{"18446744073709551618"};
   client->estimate_gas(std::string(contract_address),
                        no_argument_function("submit"),
                        std::string{},
                        ethereum_client::gas_config_t{
                           .tip = wide_priority,
                           .max_fee_per_gas = wide_max_fee,
                        });

   const auto estimate_params = client->estimate_gas_params.get_array();
   BOOST_REQUIRE_EQUAL(estimate_params.size(), 1u);
   const auto estimate_transaction = estimate_params.front().get_object();
   BOOST_CHECK_EQUAL(estimate_transaction["maxPriorityFeePerGas"].as_string(),
                     "0x10000000000000001");
   BOOST_CHECK_EQUAL(estimate_transaction["maxFeePerGas"].as_string(),
                     "0x10000000000000002");
   BOOST_CHECK_EQUAL(std::ranges::count(client->methods, "eth_maxPriorityFeePerGas"), 1u);
   BOOST_CHECK_EQUAL(std::ranges::count(client->methods, "eth_getBlockByNumber"), 1u);
   BOOST_CHECK(std::ranges::find(client->methods, "eth_chainId") == client->methods.end());
   BOOST_CHECK_EQUAL(sign_count.load(), 0u);
}

BOOST_AUTO_TEST_CASE(all_typed_write_wrappers_share_the_policy_enforced_path) {
   std::atomic<size_t> sign_count = 0;
   const auto provider = make_recording_signer(sign_count);
   auto policy = bounded_policy();
   policy.max_gas_limit = 999;
   auto client = std::make_shared<recording_ethereum_client>(provider, policy);

   sysio::opp_inbound_contract_client inbound{
      client,
      std::string(contract_address),
      {bytes_argument_function("epochIn"), no_argument_function("nextEpochIndex")},
   };
   std::string envelope = "01";
   expect_policy_rejection([&] { inbound.epoch_in(envelope); });

   sysio::operator_registry_contract_client registry{
      client,
      std::string(contract_address),
      {bytes_argument_function("commit")},
   };
   std::string commitment = "01";
   expect_policy_rejection([&] { registry.commit(commitment); });

   sysio::opp_contract_client opp{
      client,
      std::string(contract_address),
      {uint32_argument_function("emitOutboundEnvelope"), no_argument_function("getLatestOutboundEnvelope")},
   };
   uint32_t epoch = 1;
   expect_policy_rejection([&] { opp.emit_outbound_envelope(epoch); });

   BOOST_CHECK_EQUAL(sign_count.load(), 0u);
   BOOST_CHECK_EQUAL(client->broadcast_count, 0u);
}

BOOST_AUTO_TEST_CASE(policy_file_loads_two_independent_client_chain_entries) {
   fc::temp_directory directory;
   const auto path = write_policy_file(directory, R"json({
      "version": 1,
      "policies": [
         {
            "client_id": "ethereum-mainnet",
            "chain_id": "1",
            "max_priority_fee_per_gas_wei": "2000000000",
            "max_fee_per_gas_wei": "100000000000",
            "max_gas_limit": "2000000",
            "max_total_native_cost_wei": "250000000000000000"
         },
         {
            "client_id": "ethereum-sepolia",
            "chain_id": "11155111",
            "max_priority_fee_per_gas_wei": "3000000000",
            "max_fee_per_gas_wei": "120000000000",
            "max_gas_limit": "3000000",
            "max_total_native_cost_wei": "500000000000000000"
         }
      ]
   })json");

   const auto policies = sysio::load_ethereum_transaction_policy_file(path);
   BOOST_REQUIRE_EQUAL(policies.size(), 2u);
   BOOST_CHECK_EQUAL(policies.at("ethereum-mainnet").chain_id, 1);
   BOOST_CHECK_EQUAL(policies.at("ethereum-sepolia").chain_id, 11155111);
   BOOST_CHECK(policies.at("ethereum-mainnet").max_fee_per_gas !=
               policies.at("ethereum-sepolia").max_fee_per_gas);
}

BOOST_AUTO_TEST_CASE(policy_file_rejects_schema_duplicates_ranges_and_sensitive_unknown_fields) {
   fc::temp_directory directory;
   expect_policy_rejection([&] {
      sysio::load_ethereum_transaction_policy_file(directory.path() / "missing.json");
   });

   auto path = write_policy_file(directory, R"json({
      "version": 1,
      "policies": [{
         "client_id": "client-a",
         "client_id": "client-b",
         "chain_id": "31337",
         "max_priority_fee_per_gas_wei": "10",
         "max_fee_per_gas_wei": "100",
         "max_gas_limit": "1000",
         "max_total_native_cost_wei": "100000"
      }]
   })json");
   expect_policy_rejection([&] { sysio::load_ethereum_transaction_policy_file(path); });

   path = write_policy_file(directory, R"json({
      "version": 1,
      "policies": [{
         "client_id": "client-a",
         "chain_id": "31337",
         "max_priority_fee_per_gas_wei": "10",
         "max_fee_per_gas_wei": "100",
         "max_gas_limit": "1000",
         "max_total_native_cost_wei": "100000"
      }, {
         "client_id": "client-a",
         "chain_id": "31337",
         "max_priority_fee_per_gas_wei": "10",
         "max_fee_per_gas_wei": "100",
         "max_gas_limit": "1000",
         "max_total_native_cost_wei": "100000"
      }]
   })json");
   try {
      sysio::load_ethereum_transaction_policy_file(path);
      BOOST_FAIL("expected duplicate policy rejection");
   } catch (const ethereum_transaction_policy_exception& rejection) {
      BOOST_CHECK(rejection.reason() ==
                  ethereum_transaction_policy_reason::configuration_client_duplicate);
   }

   path = write_policy_file(directory, R"json({
      "version": 1,
      "policies": [{
         "client_id": "client-a",
         "chain_id": "4294967296",
         "max_priority_fee_per_gas_wei": "10",
         "max_fee_per_gas_wei": "100",
         "max_gas_limit": "1000",
         "max_total_native_cost_wei": "100000"
      }]
   })json");
   expect_policy_rejection([&] { sysio::load_ethereum_transaction_policy_file(path); });

   constexpr std::string_view sensitive_url = "https://user:password@example.invalid/rpc?token=secret";
   path = write_policy_file(directory, R"json({
      "version": 1,
      "policies": [],
      "https://user:password@example.invalid/rpc?token=secret": true
   })json");
   try {
      sysio::load_ethereum_transaction_policy_file(path);
      BOOST_FAIL("expected unknown-field rejection");
   } catch (const ethereum_transaction_policy_exception& rejection) {
      BOOST_CHECK(rejection.observed().find(sensitive_url) == std::string::npos);
      BOOST_CHECK(rejection.to_detail_string().find(sensitive_url) == std::string::npos);
   }
}

BOOST_AUTO_TEST_CASE(plugin_startup_requires_exact_client_coverage_and_matching_chain) {
   fc::temp_directory directory;
   const std::vector<std::string> client_specs{
      "client-a,signer-a,http://127.0.0.1:1,31337",
   };

   auto path = write_policy_file(directory, R"json({"version":1,"policies":[]})json");
   BOOST_CHECK(startup_rejection_reason(path, client_specs) ==
               ethereum_transaction_policy_reason::configuration_client_missing);

   path = write_policy_file(directory, R"json({
      "version": 1,
      "policies": [{
         "client_id": "orphan-client",
         "chain_id": "31337",
         "max_priority_fee_per_gas_wei": "10",
         "max_fee_per_gas_wei": "100",
         "max_gas_limit": "1000",
         "max_total_native_cost_wei": "100000"
      }]
   })json");
   BOOST_CHECK(startup_rejection_reason(path, client_specs) ==
               ethereum_transaction_policy_reason::configuration_client_unknown);

   path = write_policy_file(directory, R"json({
      "version": 1,
      "policies": [{
         "client_id": "client-a",
         "chain_id": "1",
         "max_priority_fee_per_gas_wei": "10",
         "max_fee_per_gas_wei": "100",
         "max_gas_limit": "1000",
         "max_total_native_cost_wei": "100000"
      }]
   })json");
   BOOST_CHECK(startup_rejection_reason(path, client_specs) ==
               ethereum_transaction_policy_reason::configuration_chain_id_mismatch);
}

BOOST_AUTO_TEST_CASE(plugin_startup_attaches_two_matching_client_chain_policies) {
   fc::temp_directory directory;
   const auto path = write_policy_file(directory, R"json({
      "version": 1,
      "policies": [{
         "client_id": "client-a",
         "chain_id": "31337",
         "max_priority_fee_per_gas_wei": "10",
         "max_fee_per_gas_wei": "100",
         "max_gas_limit": "1000",
         "max_total_native_cost_wei": "100000"
      }, {
         "client_id": "client-b",
         "chain_id": "1",
         "max_priority_fee_per_gas_wei": "20",
         "max_fee_per_gas_wei": "200",
         "max_gas_limit": "2000",
         "max_total_native_cost_wei": "200000"
      }]
   })json");
   const auto clients = initialize_outpost_plugin(
      path,
      {"client-a,signer-a,http://127.0.0.1:1,31337",
       "client-b,signer-a,http://127.0.0.1:1,1"});
   BOOST_REQUIRE_EQUAL(clients.size(), 2u);
   BOOST_CHECK_EQUAL(clients.front()->id, "client-a");
   BOOST_CHECK_EQUAL(clients.front()->chain_id, 31337);
   BOOST_CHECK_EQUAL(clients.front()->client->transaction_policy().max_total_native_cost, 100000);
   BOOST_CHECK_EQUAL(clients.back()->id, "client-b");
   BOOST_CHECK_EQUAL(clients.back()->chain_id, 1);
   BOOST_CHECK_EQUAL(clients.back()->client->transaction_policy().max_total_native_cost, 200000);
}

BOOST_AUTO_TEST_CASE(outpost_factory_rejects_client_policy_chain_mismatch) {
   fc::temp_directory directory;
   const auto path = write_policy_file(directory, R"json({
      "version": 1,
      "policies": [{
         "client_id": "client-a",
         "chain_id": "31337",
         "max_priority_fee_per_gas_wei": "10",
         "max_fee_per_gas_wei": "100",
         "max_gas_limit": "1000",
         "max_total_native_cost_wei": "100000"
      }]
   })json");

   with_initialized_outpost_plugin(
      path,
      {"client-a,signer-a,http://127.0.0.1:1,31337"},
      [&](auto& plugin) {
         try {
            plugin.create_outpost_client(
               "client-a",
               1,
               1,
               std::string(contract_address),
               std::string(contract_address),
               std::string(contract_address));
            BOOST_FAIL("expected client/outpost chain mismatch rejection");
         } catch (const ethereum_transaction_policy_exception& rejection) {
            BOOST_CHECK(rejection.reason() ==
                        ethereum_transaction_policy_reason::configuration_chain_id_mismatch);
            BOOST_CHECK_EQUAL(rejection.observed(), "1");
            BOOST_REQUIRE(rejection.allowed().has_value());
            BOOST_CHECK_EQUAL(*rejection.allowed(), "31337");
         }
      });
}

BOOST_AUTO_TEST_CASE(plugin_startup_redacts_an_invalid_authenticated_rpc_url) {
   fc::temp_directory directory;
   const auto path = write_policy_file(directory, R"json({
      "version": 1,
      "policies": [{
         "client_id": "client-a",
         "chain_id": "31337",
         "max_priority_fee_per_gas_wei": "10",
         "max_fee_per_gas_wei": "100",
         "max_gas_limit": "1000",
         "max_total_native_cost_wei": "100000"
      }]
   })json");
   constexpr std::string_view sensitive_url =
      "http://user:password@localhost:not-a-port/rpc?token=secret";
   try {
      initialize_outpost_plugin(
         path,
         {"client-a,signer-a," + std::string(sensitive_url) + ",31337"});
      BOOST_FAIL("expected invalid URL rejection");
   } catch (const ethereum_transaction_policy_exception& rejection) {
      BOOST_CHECK(rejection.reason() ==
                  ethereum_transaction_policy_reason::configuration_schema_invalid);
      BOOST_CHECK(rejection.observed().find(sensitive_url) == std::string::npos);
      BOOST_CHECK(rejection.to_detail_string().find(sensitive_url) == std::string::npos);
   }
}

BOOST_AUTO_TEST_SUITE_END()

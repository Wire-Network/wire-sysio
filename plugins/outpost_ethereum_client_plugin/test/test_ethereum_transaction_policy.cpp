#include <boost/test/unit_test.hpp>

#include <gsl-lite/gsl-lite.hpp>

#include <fc/crypto/ethereum/ethereum_utils.hpp>
#include <fc/crypto/private_key.hpp>
#include <fc/filesystem.hpp>
#include <fc/network/ethereum/ethereum_rlp_encoder.hpp>

#include <sysio/outpost_ethereum_client_plugin.hpp>

#include <boost/asio.hpp>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <thread>
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
using tcp = boost::asio::ip::tcp;

/** One-shot JSON-RPC endpoint used to preserve coverage of the legacy three-field client form. */
class chain_id_rpc_server {
public:
   chain_id_rpc_server()
      : _acceptor(_io, tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0))
      , _port(_acceptor.local_endpoint().port())
      , _worker([this] { serve(); }) {}

   chain_id_rpc_server(const chain_id_rpc_server&) = delete;
   chain_id_rpc_server& operator=(const chain_id_rpc_server&) = delete;

   ~chain_id_rpc_server() {
      boost::system::error_code error;
      _acceptor.close(error);
      boost::asio::io_context io;
      tcp::socket socket(io);
      socket.connect(tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), _port), error);
      if (_worker.joinable()) _worker.join();
   }

   std::string url() const {
      return "http://127.0.0.1:" + std::to_string(_port);
   }

private:
   void serve() {
      boost::system::error_code error;
      tcp::socket socket(_io);
      _acceptor.accept(socket, error);
      if (error) return;

      boost::asio::streambuf request;
      boost::asio::read_until(socket, request, "\r\n\r\n", error);
      if (error) return;

      constexpr std::string_view response_body =
         R"json({"jsonrpc":"2.0","id":1,"result":"0x7a69"})json";
      std::ostringstream response;
      response << "HTTP/1.1 200 OK\r\n"
               << "Content-Type: application/json\r\n"
               << "Content-Length: " << response_body.size() << "\r\n"
               << "Connection: close\r\n\r\n"
               << response_body;
      const auto response_text = response.str();
      boost::asio::write(socket, boost::asio::buffer(response_text), error);
   }

   boost::asio::io_context _io;
   tcp::acceptor           _acceptor;
   uint16_t                _port;
   std::thread             _worker;
};

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

std::filesystem::path write_client_configuration_file(fc::temp_directory& directory,
                                                      std::string_view    contents) {
   const auto path = directory.path() / "ethereum-client-config.json";
   std::ofstream output{path};
   output << contents;
   output.close();
   return path;
}

void with_initialized_outpost_plugin(
   const std::vector<std::string>& configuration_arguments,
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
   };
   arguments.insert(arguments.end(), configuration_arguments.begin(), configuration_arguments.end());

   std::vector<char*> argv;
   argv.reserve(arguments.size());
   for (auto& argument : arguments) argv.emplace_back(argument.data());

   if (!test_application->initialize<sysio::outpost_ethereum_client_plugin>(argv.size(), argv.data())) {
      FC_THROW_EXCEPTION(fc::invalid_arg_exception, "test application initialization returned false");
   }
   inspect_plugin(test_application->get_plugin<sysio::outpost_ethereum_client_plugin>());
}

std::vector<sysio::ethereum_client_entry_ptr>
initialize_outpost_plugin(const std::vector<std::string>& configuration_arguments) {
   std::vector<sysio::ethereum_client_entry_ptr> clients;
   with_initialized_outpost_plugin(
      configuration_arguments,
      [&](auto& plugin) { clients = plugin.get_clients(); });
   return clients;
}

ethereum_transaction_policy_reason startup_rejection_reason(
   const std::vector<std::string>& configuration_arguments) {
   try {
      initialize_outpost_plugin(configuration_arguments);
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
   const auto& maximum = maximum_ethereum_transaction_policy_value();
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

BOOST_AUTO_TEST_CASE(default_policy_uses_the_maximum_value_for_every_expenditure_cap) {
   const auto policy = sysio::make_default_ethereum_transaction_policy("client-a", 31337);
   const auto& maximum = maximum_ethereum_transaction_policy_value();
   BOOST_CHECK_EQUAL(policy.chain_id, 31337);
   BOOST_CHECK_EQUAL(policy.max_priority_fee_per_gas, maximum);
   BOOST_CHECK_EQUAL(policy.max_fee_per_gas, maximum);
   BOOST_CHECK_EQUAL(policy.max_gas_limit, maximum);
   BOOST_CHECK_EQUAL(policy.max_total_native_cost, maximum);
}

BOOST_AUTO_TEST_CASE(unified_file_loads_explicit_and_default_policies) {
   fc::temp_directory directory;
   const auto path = write_client_configuration_file(directory, R"json({
      "version": 1,
      "clients": [{
         "client_id": "ethereum-mainnet",
         "signature_provider_id": "signer-a",
         "rpc_url": "http://127.0.0.1:1",
         "chain_id": "1",
         "transaction_policy": {
            "max_priority_fee_per_gas_wei": "2000000000",
            "max_fee_per_gas_wei": "100000000000",
            "max_gas_limit": "2000000",
            "max_total_native_cost_wei": "250000000000000000"
         }
      }, {
         "client_id": "ethereum-sepolia",
         "signature_provider_id": "signer-a",
         "rpc_url": "http://127.0.0.1:1",
         "chain_id": "11155111"
      }]
   })json");

   const auto configurations = sysio::load_ethereum_client_configuration_file(path);
   const auto& maximum = maximum_ethereum_transaction_policy_value();
   BOOST_REQUIRE_EQUAL(configurations.size(), 2u);
   BOOST_CHECK_EQUAL(configurations.at("ethereum-mainnet").chain_id, 1);
   BOOST_CHECK_EQUAL(configurations.at("ethereum-mainnet").policy.max_fee_per_gas, 100000000000ULL);
   BOOST_CHECK_EQUAL(configurations.at("ethereum-sepolia").chain_id, 11155111);
   BOOST_CHECK_EQUAL(configurations.at("ethereum-sepolia").policy.max_fee_per_gas, maximum);
}

BOOST_AUTO_TEST_CASE(unified_file_rejects_schema_duplicates_ranges_and_sensitive_unknown_fields) {
   fc::temp_directory directory;
   expect_policy_rejection([&] {
      sysio::load_ethereum_client_configuration_file(directory.path() / "missing.json");
   });

   auto path = write_client_configuration_file(directory, R"json({
      "version": 1,
      "clients": [{
         "client_id": "client-a",
         "client_id": "client-b",
         "signature_provider_id": "signer-a",
         "rpc_url": "http://127.0.0.1:1",
         "chain_id": "31337"
      }]
   })json");
   expect_policy_rejection([&] { sysio::load_ethereum_client_configuration_file(path); });

   path = write_client_configuration_file(directory, R"json({
      "version": 1,
      "clients": [{
         "client_id": "client-a",
         "signature_provider_id": "signer-a",
         "rpc_url": "http://127.0.0.1:1",
         "chain_id": "31337"
      }, {
         "client_id": "client-a",
         "signature_provider_id": "signer-a",
         "rpc_url": "http://127.0.0.1:2",
         "chain_id": "31337"
      }]
   })json");
   try {
      sysio::load_ethereum_client_configuration_file(path);
      BOOST_FAIL("expected duplicate client rejection");
   } catch (const ethereum_transaction_policy_exception& rejection) {
      BOOST_CHECK(rejection.reason() ==
                  ethereum_transaction_policy_reason::configuration_client_duplicate);
   }

   path = write_client_configuration_file(directory, R"json({
      "version": 1,
      "clients": [{
         "client_id": "client-a",
         "signature_provider_id": "signer-a",
         "rpc_url": "http://127.0.0.1:1",
         "chain_id": "4294967296"
      }]
   })json");
   expect_policy_rejection([&] { sysio::load_ethereum_client_configuration_file(path); });

   constexpr std::string_view sensitive_url = "https://user:password@example.invalid/rpc?token=secret";
   path = write_client_configuration_file(directory, R"json({
      "version": 1,
      "clients": [],
      "https://user:password@example.invalid/rpc?token=secret": true
   })json");
   try {
      sysio::load_ethereum_client_configuration_file(path);
      BOOST_FAIL("expected unknown-field rejection");
   } catch (const ethereum_transaction_policy_exception& rejection) {
      BOOST_CHECK(rejection.observed().find(sensitive_url) == std::string::npos);
      BOOST_CHECK(rejection.to_detail_string().find(sensitive_url) == std::string::npos);
   }
}

BOOST_AUTO_TEST_CASE(plugin_startup_attaches_unified_client_policies) {
   fc::temp_directory directory;
   const auto path = write_client_configuration_file(directory, R"json({
      "version": 1,
      "clients": [{
         "client_id": "client-a",
         "signature_provider_id": "signer-a",
         "rpc_url": "http://127.0.0.1:1",
         "chain_id": "31337",
         "transaction_policy": {
            "max_priority_fee_per_gas_wei": "10",
            "max_fee_per_gas_wei": "100",
            "max_gas_limit": "1000",
            "max_total_native_cost_wei": "100000"
         }
      }, {
         "client_id": "client-b",
         "signature_provider_id": "signer-a",
         "rpc_url": "http://127.0.0.1:1",
         "chain_id": "1"
      }]
   })json");
   const auto clients = initialize_outpost_plugin(
      {"--outpost-ethereum-client-config-file", path.string()});
   const auto& maximum = maximum_ethereum_transaction_policy_value();
   BOOST_REQUIRE_EQUAL(clients.size(), 2u);
   BOOST_CHECK_EQUAL(clients.front()->id, "client-a");
   BOOST_CHECK_EQUAL(clients.front()->chain_id, 31337);
   BOOST_CHECK_EQUAL(clients.front()->client->transaction_policy().max_total_native_cost, 100000);
   BOOST_CHECK_EQUAL(clients.back()->id, "client-b");
   BOOST_CHECK_EQUAL(clients.back()->chain_id, 1);
   BOOST_CHECK_EQUAL(clients.back()->client->transaction_policy().max_total_native_cost, maximum);
}

BOOST_AUTO_TEST_CASE(legacy_client_option_uses_default_policy_with_explicit_chain_id) {
   const auto clients = initialize_outpost_plugin(
      {"--outpost-ethereum-client", "client-a,signer-a,http://127.0.0.1:1,31337"});
   const auto& maximum = maximum_ethereum_transaction_policy_value();
   BOOST_REQUIRE_EQUAL(clients.size(), 1u);
   BOOST_CHECK_EQUAL(clients.front()->chain_id, 31337);
   BOOST_CHECK_EQUAL(clients.front()->client->transaction_policy().max_fee_per_gas, maximum);
}

BOOST_AUTO_TEST_CASE(legacy_three_field_client_resolves_chain_id_from_rpc) {
   chain_id_rpc_server rpc_server;
   const auto clients = initialize_outpost_plugin(
      {"--outpost-ethereum-client", "client-a,signer-a," + rpc_server.url()});
   BOOST_REQUIRE_EQUAL(clients.size(), 1u);
   BOOST_CHECK_EQUAL(clients.front()->chain_id, 31337);
   BOOST_CHECK_EQUAL(clients.front()->client->get_chain_id(), 31337);
}

BOOST_AUTO_TEST_CASE(plugin_startup_rejects_mixed_unified_and_legacy_modes) {
   fc::temp_directory directory;
   const auto path = write_client_configuration_file(
      directory, R"json({"version":1,"clients":[]})json");
   BOOST_CHECK(startup_rejection_reason(
      {"--outpost-ethereum-client-config-file",
       path.string(),
       "--outpost-ethereum-client",
       "client-a,signer-a,http://127.0.0.1:1,31337"}) ==
               ethereum_transaction_policy_reason::configuration_schema_invalid);
}

BOOST_AUTO_TEST_CASE(outpost_factory_rejects_client_policy_chain_mismatch) {
   fc::temp_directory directory;
   const auto path = write_client_configuration_file(directory, R"json({
      "version": 1,
      "clients": [{
         "client_id": "client-a",
         "signature_provider_id": "signer-a",
         "rpc_url": "http://127.0.0.1:1",
         "chain_id": "31337"
      }]
   })json");

   with_initialized_outpost_plugin(
      {"--outpost-ethereum-client-config-file", path.string()},
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
   constexpr std::string_view sensitive_url =
      "http://user:password@localhost:not-a-port/rpc?token=secret";
   const auto path = write_client_configuration_file(directory, R"json({
      "version": 1,
      "clients": [{
         "client_id": "client-a",
         "signature_provider_id": "signer-a",
         "rpc_url": "http://user:password@localhost:not-a-port/rpc?token=secret",
         "chain_id": "31337"
      }]
   })json");
   try {
      initialize_outpost_plugin(
         {"--outpost-ethereum-client-config-file", path.string()});
      BOOST_FAIL("expected invalid URL rejection");
   } catch (const ethereum_transaction_policy_exception& rejection) {
      BOOST_CHECK(rejection.reason() ==
                  ethereum_transaction_policy_reason::configuration_schema_invalid);
      BOOST_CHECK(rejection.observed().find(sensitive_url) == std::string::npos);
      BOOST_CHECK(rejection.to_detail_string().find(sensitive_url) == std::string::npos);
   }
}

BOOST_AUTO_TEST_SUITE_END()

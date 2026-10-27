#include <boost/test/unit_test.hpp>
#include "startup_rpc_server.hpp"

#include <gsl-lite/gsl-lite.hpp>

#include <fc/crypto/ethereum/ethereum_utils.hpp>
#include <fc/crypto/hex.hpp>
#include <fc/crypto/keccak256.hpp>
#include <fc/crypto/private_key.hpp>
#include <fc/filesystem.hpp>
#include <fc/network/ethereum/ethereum_rlp_encoder.hpp>
#include <fc/slug_name.hpp>

#include <sysio/outpost_ethereum_client_plugin.hpp>
#include <sysio/opp/config/client_config_loader.hpp>

#include <boost/asio.hpp>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <functional>
#include <limits>
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

const std::string fake_rpc_url = startup_test::unused_url();
constexpr std::string_view contract_address = "5FbDB2315678afecb367f032d93F642f64180aa3";
/// A hash the recording node reports when told to misreport one.
constexpr std::string_view transaction_hash =
   "0x1111111111111111111111111111111111111111111111111111111111111111";
/// The JSON-RPC error code a node refuses a transaction with (geth's invalid-input code).
constexpr int refused_send_rpc_code = -32000;
constexpr std::string_view signer_public_key =
   "0x8318535b54105d4a7aae60c08fc45f9687181b4fdfc625bd1a753fa7397fed7535"
   "47f11ca8696646f2f3acb08e31016afac23e630c5d11f59f61fef57b0d2aa5";
constexpr std::string_view signer_private_key =
   "0xac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80";
constexpr std::string_view rpc_url_placeholder_a = "RPC_URL_A";
constexpr std::string_view rpc_url_placeholder_b = "RPC_URL_B";
constexpr std::string_view test_chain_id_quantity = "0x7a69";
constexpr std::string_view ethereum_mainnet_chain_id_quantity = "0x1";
/// Receipt deadline for a wrapper call whose receipt is already there.
constexpr int64_t receipt_deadline_seconds = 30;
/// Receipt deadline for a wrapper call whose receipt never appears: far below
/// the library's default retry budget, so a wait that ignored it would show.
constexpr int64_t short_receipt_deadline_ms = 300;
/// A receipt wait longer than the library's default retry budget.
constexpr int64_t long_receipt_wait_seconds = 90;
using tcp = boost::asio::ip::tcp;

/** Registry-backed fixture shared with deferred-discovery regressions. */
class chain_id_rpc_server : public startup_test::rpc_server {
public:
   explicit chain_id_rpc_server(std::string_view quantity = test_chain_id_quantity)
      : rpc_server("\"" + std::string(quantity) + "\"") {}
};

/** Return a compact positive policy used by plugin enforcement tests. */
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

/** Build a function ABI without input arguments. */
ethabi::contract no_argument_function(std::string name) {
   return ethabi::contract{
      .name = std::move(name),
      .type = ethabi::invoke_target_type::function,
      .inputs = {},
      .outputs = {},
   };
}

/** Build the whole-envelope `epochIn(uint32,bytes)` function ABI. */
ethabi::contract whole_envelope_epoch_in_function(std::string name) {
   return ethabi::contract{
      .name = std::move(name),
      .type = ethabi::invoke_target_type::function,
      .inputs = {ethabi::component_type{"epochIndex", ethabi::data_type::uint32},
                 ethabi::component_type{"envelopeData", ethabi::data_type::bytes}},
      .outputs = {},
   };
}

/** Build the `epochDeliveries(uint32,address)` view ABI. */
ethabi::contract epoch_deliveries_function(std::string name) {
   return ethabi::contract{
      .name = std::move(name),
      .type = ethabi::invoke_target_type::function,
      .inputs = {ethabi::component_type{"epochIndex", ethabi::data_type::uint32},
                 ethabi::component_type{"operator_", ethabi::data_type::address}},
      .outputs = {},
   };
}

/** Build the `pendingConsensusForDigest(bytes32)` view ABI. */
ethabi::contract bytes32_argument_function(std::string name) {
   return ethabi::contract{
      .name = std::move(name),
      .type = ethabi::invoke_target_type::function,
      .inputs = {ethabi::component_type{"digest", ethabi::data_type::bytes32}},
      .outputs = {},
   };
}

/** Build a function ABI with one uint16 argument. */
ethabi::contract uint16_argument_function(std::string name) {
   return ethabi::contract{
      .name = std::move(name),
      .type = ethabi::invoke_target_type::function,
      .inputs = {ethabi::component_type{"attestationType", ethabi::data_type::uint16}},
      .outputs = {},
   };
}

/** Build a function ABI with one uint32 argument. */
ethabi::contract uint32_argument_function(std::string name) {
   return ethabi::contract{
      .name = std::move(name),
      .type = ethabi::invoke_target_type::function,
      .inputs = {ethabi::component_type{"wireEpochIndex", ethabi::data_type::uint32}},
      .outputs = {},
   };
}

/** Create an Ethereum signer that records every private-key operation. */
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

/** How the recording node answers one `eth_sendRawTransaction`. */
enum class send_outcome {
   /// Accepted: the node reports the hash of the bytes.
   accepted,
   /// Refused: the node answers with a JSON-RPC error.
   refused,
   /// Accepted, but the response is lost on the way back.
   response_lost,
   /// Accepted, and the node reports a hash other than the bytes'.
   misreported
};

/** Deterministic Ethereum client that records RPC and broadcast activity. */
class recording_ethereum_client final : public ethereum_client {
public:
   /** Construct a recording client with the supplied signer and local policy. */
   recording_ethereum_client(const signature_provider_ptr& provider,
                             ethereum_transaction_policy   policy)
      : ethereum_client(provider, std::string(fake_rpc_url), std::move(policy)) {}

   /** Return deterministic responses for the RPC methods used by transaction construction. */
   fc::variant execute(const std::string& method, const fc::variant& params) override {
      methods.emplace_back(method);
      if (method == "eth_maxPriorityFeePerGas") return fc::variant("0xa");
      if (method == "eth_getBlockByNumber") {
         return fc::variant(fc::mutable_variant_object("baseFeePerGas", "0x2d"));
      }
      if (method == "eth_estimateGas") {
         estimate_gas_params = params;
         return fc::variant(estimate_response);
      }
      if (method == "eth_getTransactionCount") return fc::variant("0x0");
      if (method == "eth_sendRawTransaction") {
         ++broadcast_count;
         const auto raw = params.get_array().front().as_string();
         const auto hash = "0x" + fc::crypto::keccak256::hash(std::span<const uint8_t>(fc::from_hex(raw))).str();
         sent_hashes.push_back(hash);
         switch (next_send_outcome) {
         case send_outcome::accepted:
            return fc::variant(hash);
         case send_outcome::refused:
            throw fc::network::json_rpc::json_rpc_error(refused_send_rpc_code, "replacement transaction underpriced");
         case send_outcome::response_lost:
            FC_THROW_EXCEPTION(fc::timeout_exception, "the send's response was lost");
         case send_outcome::misreported:
            return fc::variant(std::string(transaction_hash));
         }
      }
      if (method == "eth_getTransactionReceipt") {
         ++receipt_reads;
         return receipt_response;
      }
      if (method == "eth_blockNumber") return fc::variant(block_number_response);
      FC_THROW_EXCEPTION(fc::invalid_arg_exception, "unexpected fake RPC method {}", method);
   }

   /** Route idempotent calls through the same deterministic recorder. */
   fc::variant execute_idempotent(const std::string& method, const fc::variant& params) override {
      return execute(method, params);
   }

   std::vector<std::string> methods;
   fc::variant              estimate_gas_params;
   /** What `eth_estimateGas` answers; 834 by default, which buffers to 1000. */
   std::string              estimate_response = "0x342";
   /** What `eth_getTransactionReceipt` answers; null (not yet included) by default. */
   fc::variant              receipt_response;
   /** What `eth_blockNumber` answers. */
   std::string              block_number_response = "0x10";
   size_t                   receipt_reads   = 0;
   size_t                   broadcast_count = 0;
   /** `keccak256` of every raw transaction sent, in order. */
   std::vector<std::string> sent_hashes;
   /** How the node answers the next send. */
   send_outcome             next_send_outcome = send_outcome::accepted;
};

/** Return a transaction that exactly reaches the bounded policy values. */
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

/** Assert that an operation is rejected by transaction-policy enforcement. */
void expect_policy_rejection(const std::function<void()>& operation) {
   BOOST_CHECK_THROW(operation(), ethereum_transaction_policy_exception);
}

/** Write a plugin configuration fixture to an isolated temporary file. */
std::filesystem::path write_client_configuration_file(fc::temp_directory& directory,
                                                      std::string_view    contents) {
   const auto path = directory.path() / "ethereum-client-config.json";
   std::ofstream output{path};
   output << contents;
   output.close();
   return path;
}

/** Replace one named RPC URL placeholder in a test configuration document. */
void bind_rpc_url(std::string& configuration,
                  std::string_view placeholder,
                  std::string_view rpc_url) {
   const auto position = configuration.find(placeholder);
   if (position == std::string::npos) {
      FC_THROW_EXCEPTION(fc::invalid_arg_exception,
                         "missing RPC URL placeholder in test configuration");
   }
   configuration.replace(position, placeholder.size(), rpc_url);
}

/** Initialize an isolated plugin instance and expose it to a test callback. */
void with_initialized_outpost_plugin(
   const std::vector<std::string>& configuration_arguments,
   const std::function<void(sysio::outpost_ethereum_client_plugin&)>& inspect_plugin,
   std::string_view signature_provider_id = "signer-a",
   std::string_view signature_provider_chain = "ethereum") {
   auto reset_application = gsl_lite::finally([] { appbase::application::reset_app_singleton(); });
   appbase::scoped_app test_application{};

   const std::string signature_spec =
      std::string(signature_provider_id) + "," + std::string(signature_provider_chain) +
      ",ethereum," + std::string(signer_public_key) + ",KEY:" +
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

/** Initialize an isolated plugin instance and return its published clients. */
std::vector<sysio::ethereum_client_entry_ptr>
initialize_outpost_plugin(const std::vector<std::string>& configuration_arguments,
                          std::string_view signature_provider_id = "signer-a",
                          std::string_view signature_provider_chain = "ethereum") {
   std::vector<sysio::ethereum_client_entry_ptr> clients;
   with_initialized_outpost_plugin(
      configuration_arguments,
      [&](auto& plugin) { clients = plugin.get_clients(); },
      signature_provider_id,
      signature_provider_chain);
   return clients;
}

/** Return the client count visible after a file configuration fails during initialization. */
std::size_t initialize_rejected_file_and_observe_published_clients(
   const std::filesystem::path& configuration_file) {
   auto reset_application = gsl_lite::finally([] { appbase::application::reset_app_singleton(); });
   appbase::scoped_app test_application{};

   const std::string signature_spec =
      "signer-a,ethereum,ethereum," + std::string(signer_public_key) + ",KEY:" +
      std::string(signer_private_key);
   std::vector<std::string> arguments{
      "test_outpost_ethereum_transaction_policy",
      "--signature-provider",
      signature_spec,
      "--outpost-ethereum-client-config-file",
      configuration_file.string(),
   };
   std::vector<char*> argv;
   argv.reserve(arguments.size());
   for (auto& argument : arguments) argv.emplace_back(argument.data());

   try {
      (void) test_application->initialize<sysio::outpost_ethereum_client_plugin>(
         argv.size(), argv.data());
      BOOST_FAIL("expected file configuration rejection");
   } catch (const sysio::chain::plugin_config_exception&) {
      return test_application->get_plugin<sysio::outpost_ethereum_client_plugin>()
         .get_clients()
         .size();
   }
   return std::numeric_limits<std::size_t>::max();
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
   BOOST_REQUIRE_EQUAL(client->sent_hashes.size(), 1u);
   BOOST_CHECK_EQUAL(result.as_string(), client->sent_hashes.front());
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

/// A gas-limit floor raises an under-estimate to the floor, leaves a larger
/// buffered estimate alone, and is refused — never clamped — above the policy
/// ceiling.
BOOST_AUTO_TEST_CASE(gas_limit_floor_raises_an_under_estimate_and_is_policy_bounded) {
   std::atomic<size_t> sign_count = 0;
   const auto provider = make_recording_signer(sign_count);
   auto policy = bounded_policy();
   policy.max_gas_limit = 2000;
   auto client = std::make_shared<recording_ethereum_client>(provider, policy);

   // The fake estimate is 834 -> buffered 1000.
   BOOST_CHECK_EQUAL(
      client->create_default_tx(std::string(contract_address), no_argument_function("submit"), {}, 0).gas_limit,
      1000);
   BOOST_CHECK_EQUAL(
      client->create_default_tx(std::string(contract_address), no_argument_function("submit"), {}, 900).gas_limit,
      1000);
   BOOST_CHECK_EQUAL(
      client->create_default_tx(std::string(contract_address), no_argument_function("submit"), {}, 1500).gas_limit,
      1500);
   BOOST_CHECK_EQUAL(
      client->create_default_tx(std::string(contract_address), no_argument_function("submit"), {}, 2000).gas_limit,
      2000);
   expect_policy_rejection([&] {
      client->create_default_tx(std::string(contract_address), no_argument_function("submit"), {}, 2001);
   });
   BOOST_CHECK_EQUAL(sign_count.load(), 0u);
   BOOST_CHECK_EQUAL(client->broadcast_count, 0u);
}

/// A cap IS the limit: the transaction is sent with exactly it and the estimate
/// is only a pre-flight under it (its value is not an input, and the
/// buffered-estimate ceiling check does not apply). A cap below the floor or
/// above the policy ceiling is refused.
BOOST_AUTO_TEST_CASE(gas_limit_cap_is_the_limit_and_runs_the_estimate_under_it) {
   std::atomic<size_t> sign_count = 0;
   const auto provider = make_recording_signer(sign_count);
   auto policy = bounded_policy();
   policy.max_gas_limit = 2000;
   auto client = std::make_shared<recording_ethereum_client>(provider, policy);

   // No cap: the estimate payload carries no `gas`.
   client->create_default_tx(std::string(contract_address), no_argument_function("submit"), {}, 0, 0);
   BOOST_CHECK(!client->estimate_gas_params.get_array().front().get_object().contains("gas"));

   // The fake estimate is 834 -> buffered 1000; a cap of 900 is sent as 900 ...
   BOOST_CHECK_EQUAL(
      client->create_default_tx(std::string(contract_address), no_argument_function("submit"), {}, 0, 900).gas_limit,
      900);
   BOOST_CHECK_EQUAL(
      client->estimate_gas_params.get_array().front().get_object()["gas"].as_string(), "0x384");
   // ... and a cap of 1200 as 1200: the estimate's value is not an input to the limit.
   BOOST_CHECK_EQUAL(
      client->create_default_tx(std::string(contract_address), no_argument_function("submit"), {}, 0, 1200).gas_limit,
      1200);
   BOOST_CHECK_EQUAL(
      client->estimate_gas_params.get_array().front().get_object()["gas"].as_string(), "0x4b0");
   // Floor and cap equal: the transaction carries exactly that.
   BOOST_CHECK_EQUAL(
      client->create_default_tx(std::string(contract_address), no_argument_function("submit"), {}, 1500, 1500).gas_limit,
      1500);
   // A buffered estimate over the policy is a rejection without a cap and
   // irrelevant with one: 1000 buffers to 1200 > 1000, yet a 1000 cap is sent.
   client->estimate_response = "0x3e8";
   policy.max_gas_limit      = 1000;
   auto tight_client = std::make_shared<recording_ethereum_client>(provider, policy);
   tight_client->estimate_response = "0x3e8";
   expect_policy_rejection([&] {
      tight_client->create_default_tx(std::string(contract_address), no_argument_function("submit"), {}, 0, 0);
   });
   BOOST_CHECK_EQUAL(
      tight_client->create_default_tx(std::string(contract_address), no_argument_function("submit"), {}, 0, 1000).gas_limit,
      1000);
   // A cap above the policy ceiling is a rejection, not a clamp.
   expect_policy_rejection([&] {
      client->create_default_tx(std::string(contract_address), no_argument_function("submit"), {}, 0, 2001);
   });
   // A cap below the floor is a caller error.
   BOOST_CHECK_THROW(
      client->create_default_tx(std::string(contract_address), no_argument_function("submit"), {}, 1500, 900),
      fc::exception);
   BOOST_CHECK_EQUAL(sign_count.load(), 0u);
   BOOST_CHECK_EQUAL(client->broadcast_count, 0u);
   BOOST_CHECK_EQUAL(tight_client->broadcast_count, 0u);
}

/// The confirmation wait returns the receipt (block number and logs) so a
/// caller can pin its next reads to the block holding the effect; a reverted
/// receipt is an error, and the depth wait still applies.
BOOST_AUTO_TEST_CASE(wait_for_receipt_returns_the_confirmed_receipt) {
   std::atomic<size_t> sign_count = 0;
   const auto provider = make_recording_signer(sign_count);
   auto client = std::make_shared<recording_ethereum_client>(provider, bounded_policy());
   const std::string hash{transaction_hash};
   const fc::variants logs{fc::mutable_variant_object("address", std::string(contract_address))};
   client->receipt_response = fc::mutable_variant_object("status", "0x1")("blockNumber", "0x10")("logs", logs);

   const auto receipt = client->wait_for_receipt(hash);
   BOOST_REQUIRE(receipt.is_object());
   BOOST_CHECK_EQUAL(receipt.get_object()["blockNumber"].as_string(), "0x10");
   BOOST_CHECK_EQUAL(receipt.get_object()["logs"].get_array().size(), 1u);
   BOOST_CHECK_EQUAL(client->receipt_reads, 1u);
   BOOST_CHECK_EQUAL(client->wait_for_confirmation(hash), hash);

   // Two confirmations: the receipt's block plus one, which the fake chain has reached.
   ethereum_confirm_options two_deep = ethereum_confirm_option_defaults;
   two_deep.confirmations = 2;
   client->block_number_response = "0x11";
   BOOST_CHECK(client->wait_for_receipt(hash, two_deep).is_object());
   BOOST_CHECK(std::ranges::count(client->methods, "eth_blockNumber") == 1);

   client->receipt_response = fc::mutable_variant_object("status", "0x0")("blockNumber", "0x10");
   BOOST_CHECK_THROW(client->wait_for_receipt(hash), fc::exception);
   BOOST_CHECK_EQUAL(sign_count.load(), 0u);
}

/// One `epochIn`'s options fund it to exactly its budget and wait for its
/// receipt as long as asked, past the library's default retry budget too.
BOOST_AUTO_TEST_CASE(delivery_confirm_options_carry_the_budget_and_the_receipt_wait) {
   constexpr uint64_t budget  = 1'234'567;
   const auto         options = sysio::delivery_confirm_options(budget, fc::seconds(long_receipt_wait_seconds));
   BOOST_CHECK_EQUAL(options.gas_limit_floor, budget);
   BOOST_CHECK_EQUAL(options.gas_limit_cap, budget);
   BOOST_CHECK(options.retry.total_timeout == fc::seconds(long_receipt_wait_seconds));
   BOOST_CHECK(options.retry.total_timeout > ethereum_confirm_option_defaults.retry.total_timeout);
}

/// The wrapper stops waiting for a receipt at the given deadline, not at a
/// budget of its own.
BOOST_AUTO_TEST_CASE(epoch_in_waits_for_its_receipt_until_the_given_deadline) {
   std::atomic<size_t> sign_count = 0;
   const auto provider = make_recording_signer(sign_count);
   auto client = std::make_shared<recording_ethereum_client>(provider, bounded_policy());
   sysio::opp_inbound_contract_client inbound{
      client,
      std::string(contract_address),
      {whole_envelope_epoch_in_function("epochIn"), no_argument_function("nextEpochIndex"),
       uint32_argument_function("dispatchSpill"), epoch_deliveries_function("epochDeliveries"),
       no_argument_function("pendingEpochHash"), bytes32_argument_function("pendingConsensusForDigest"),
       uint16_argument_function("attestationHandlers")},
   };
   uint32_t                   epoch_index = 1;
   std::string                envelope    = "01";
   sysio::ethereum_submission submission;

   // No receipt ever appears.
   const auto start = fc::time_point::now();
   BOOST_CHECK_THROW(
      inbound.epoch_in(epoch_index, envelope, 999, start + fc::milliseconds(short_receipt_deadline_ms), submission),
      fc::timeout_exception);
   const auto waited = fc::time_point::now() - start;

   BOOST_CHECK(waited >= fc::milliseconds(short_receipt_deadline_ms));
   BOOST_CHECK(waited < ethereum_confirm_option_defaults.retry.total_timeout);
   BOOST_CHECK_GE(client->receipt_reads, 1u);
   // The call was sent; only the wait gave up. The submission still says what
   // was sent, so the caller can settle it later.
   BOOST_CHECK_EQUAL(client->broadcast_count, 1u);
   BOOST_CHECK(submission.purpose == sysio::submission_purpose::epoch_in);
   BOOST_CHECK(submission.state == sysio::submission_state::pending);
   BOOST_CHECK_EQUAL(submission.epoch_index, epoch_index);
   BOOST_CHECK(submission.transaction.nonce == fc::uint256{0});
   BOOST_CHECK(submission.transaction.gas_limit == fc::uint256{999});
   BOOST_CHECK(!submission.transaction.data.empty());
   // The hash was computed from the signed bytes, and is the one the node saw.
   BOOST_REQUIRE_EQUAL(submission.attempts.size(), 1u);
   BOOST_REQUIRE_EQUAL(client->sent_hashes.size(), 1u);
   BOOST_CHECK_EQUAL(submission.attempts.front().tx_hash, client->sent_hashes.front());
   BOOST_CHECK(submission.attempts.front().fees.max_priority_fee_per_gas == fc::uint256{10});
   BOOST_CHECK(submission.attempts.front().fees.max_fee_per_gas == fc::uint256{100});
}

/// The attempt is recorded before the send and stays whatever the send does:
/// a refusal propagates and keeps it, a lost response keeps it, and a node
/// that reports another hash for the bytes has that hash recorded beside the
/// computed one, once however often it does so.
BOOST_AUTO_TEST_CASE(a_submission_attempt_survives_every_send_outcome) {
   std::atomic<size_t> sign_count = 0;
   const auto provider = make_recording_signer(sign_count);
   auto client = std::make_shared<recording_ethereum_client>(provider, bounded_policy());
   const sysio::ethereum_transaction_fees fees{.max_priority_fee_per_gas = 10, .max_fee_per_gas = 100};

   /// What one send with `outcome` leaves in a submission, and whether the node's refusal propagated.
   struct outcome_result {
      sysio::ethereum_submission submission;
      bool                       refused = false;
   };
   const auto send = [&](send_outcome outcome) {
      outcome_result result{.submission = {.transaction = exact_transaction()}};
      const auto signed_tx = client->sign_transaction(result.submission.transaction, "submit");
      result.submission.attempts.push_back({.tx_hash = signed_tx.tx_hash, .fees = fees});
      client->next_send_outcome = outcome;
      try {
         sysio::send_submission_attempt(*client, signed_tx, result.submission);
      } catch (const fc::network::json_rpc::json_rpc_error&) {
         result.refused = true;
      } catch (const fc::timeout_exception&) {
      }
      // The computed hash is the one the node derives from the bytes.
      BOOST_CHECK_EQUAL(signed_tx.tx_hash, client->sent_hashes.back());
      return result;
   };

   const auto accepted = send(send_outcome::accepted);
   BOOST_CHECK(!accepted.refused);
   BOOST_CHECK_EQUAL(accepted.submission.attempts.size(), 1u);
   const auto refused = send(send_outcome::refused);
   BOOST_CHECK(refused.refused);
   BOOST_CHECK_EQUAL(refused.submission.attempts.size(), 1u);
   const auto lost = send(send_outcome::response_lost);
   BOOST_CHECK(!lost.refused);
   BOOST_CHECK_EQUAL(lost.submission.attempts.size(), 1u);
   auto misreported = send(send_outcome::misreported);
   BOOST_REQUIRE_EQUAL(misreported.submission.attempts.size(), 2u);
   BOOST_CHECK_EQUAL(misreported.submission.attempts.back().tx_hash, transaction_hash);

   // The same bytes sent again, and misreported again, record nothing new.
   const auto signed_tx = client->sign_transaction(misreported.submission.transaction, "submit");
   BOOST_REQUIRE_EQUAL(signed_tx.tx_hash, misreported.submission.attempts.front().tx_hash);
   sysio::send_submission_attempt(*client, signed_tx, misreported.submission);
   BOOST_CHECK_EQUAL(misreported.submission.attempts.size(), 2u);
}

/// A first `epochIn` the node refuses keeps its signing: the refusal
/// propagates with the hash still on the submission, so the relay journals the
/// call instead of sending another at the next nonce. The node may hold the
/// bytes anyway (already known), or another backend behind the endpoint may.
BOOST_AUTO_TEST_CASE(epoch_in_keeps_a_first_send_the_node_refused) {
   std::atomic<size_t> sign_count = 0;
   const auto provider = make_recording_signer(sign_count);
   auto client = std::make_shared<recording_ethereum_client>(provider, bounded_policy());
   sysio::opp_inbound_contract_client inbound{
      client,
      std::string(contract_address),
      {whole_envelope_epoch_in_function("epochIn"), no_argument_function("nextEpochIndex"),
       uint32_argument_function("dispatchSpill"), epoch_deliveries_function("epochDeliveries"),
       no_argument_function("pendingEpochHash"), bytes32_argument_function("pendingConsensusForDigest"),
       uint16_argument_function("attestationHandlers")},
   };
   uint32_t                   epoch_index = 1;
   std::string                envelope    = "01";
   sysio::ethereum_submission submission;
   client->next_send_outcome = send_outcome::refused;

   BOOST_CHECK_THROW(inbound.epoch_in(epoch_index, envelope, 999,
                                      fc::time_point::now() + fc::milliseconds(short_receipt_deadline_ms), submission),
                     fc::network::json_rpc::json_rpc_error);
   BOOST_CHECK_EQUAL(client->broadcast_count, 1u);
   BOOST_CHECK_EQUAL(client->receipt_reads, 0u);
   BOOST_REQUIRE_EQUAL(submission.attempts.size(), 1u);
   BOOST_CHECK_EQUAL(submission.attempts.front().tx_hash, client->sent_hashes.front());
}

/// Signing checks the policy first: a transaction past its caps is refused
/// before the signer is asked, and nothing is sent either way.
BOOST_AUTO_TEST_CASE(sign_transaction_signs_within_the_policy_without_sending) {
   std::atomic<size_t> sign_count = 0;
   const auto provider = make_recording_signer(sign_count);
   auto client = std::make_shared<recording_ethereum_client>(provider, bounded_policy());

   const auto signed_tx = client->sign_transaction(exact_transaction(), "submit");
   BOOST_CHECK_EQUAL(sign_count.load(), 1u);
   const auto signed_bytes = fc::from_hex(signed_tx.raw_hex);
   const auto expected_hash = fc::crypto::keccak256::hash(std::span<const uint8_t>(signed_bytes));
   BOOST_CHECK_EQUAL(signed_tx.tx_hash, "0x" + expected_hash.str());

   auto past_cap = exact_transaction();
   ++past_cap.max_priority_fee_per_gas;
   expect_policy_rejection([&] { client->sign_transaction(past_cap, "submit"); });
   BOOST_CHECK_EQUAL(sign_count.load(), 1u);
   BOOST_CHECK_EQUAL(client->broadcast_count, 0u);
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
      {whole_envelope_epoch_in_function("epochIn"), no_argument_function("nextEpochIndex"),
       uint32_argument_function("dispatchSpill"), epoch_deliveries_function("epochDeliveries"),
       no_argument_function("pendingEpochHash"), bytes32_argument_function("pendingConsensusForDigest"),
       uint16_argument_function("attestationHandlers")},
   };
   // OPPInbound's one write wrapper: a budget above the policy ceiling (999
   // here) is refused before signing; one at the ceiling is sent with exactly
   // it and returns the confirmed receipt.
   uint32_t    epoch_index = 1;
   std::string envelope = "01";
   const auto  deadline = fc::time_point::now() + fc::seconds(receipt_deadline_seconds);
   sysio::ethereum_submission submission;
   expect_policy_rejection([&] { inbound.epoch_in(epoch_index, envelope, 1000, deadline, submission); });
   BOOST_CHECK(submission.attempts.empty());
   client->receipt_response = fc::mutable_variant_object("status", "0x1")("blockNumber", "0x10")("logs", fc::variants{});
   const auto receipt = inbound.epoch_in(epoch_index, envelope, 999, deadline, submission);
   BOOST_CHECK_EQUAL(receipt.block_number, fc::uint256{16});
   BOOST_CHECK(receipt.logs.empty());
   // The budget reached the pre-flight as the gas the call must fit in.
   BOOST_CHECK_EQUAL(client->estimate_gas_params.get_array().front().get_object()["gas"].as_string(), "0x3e7");
   BOOST_CHECK_EQUAL(sign_count.load(), 1u);
   BOOST_CHECK_EQUAL(client->broadcast_count, 1u);

   sysio::opp_contract_client opp{
      client,
      std::string(contract_address),
      {uint32_argument_function("emitOutboundEnvelope"), no_argument_function("getLatestOutboundEnvelope")},
   };
   uint32_t epoch = 1;
   expect_policy_rejection([&] { opp.emit_outbound_envelope(epoch); });

   sysio::syndication_pool_contract_client pool{
      client,
      std::string(contract_address),
      {no_argument_function("realizeYield")},
   };
   expect_policy_rejection([&] { pool.realize_yield(); });

   // Only the in-policy delivery reached the signer and the wire.
   BOOST_CHECK_EQUAL(sign_count.load(), 1u);
   BOOST_CHECK_EQUAL(client->broadcast_count, 1u);
}

BOOST_AUTO_TEST_CASE(plugin_startup_attaches_unified_client_policies) {
   fc::temp_directory directory;
   chain_id_rpc_server rpc_server_a;
   chain_id_rpc_server rpc_server_b{ethereum_mainnet_chain_id_quantity};
   std::string configuration = R"json({
      "schema_version": 1,
      "clients": [{
         "connection": {
            "client_id": "client-a",
            "signature_provider_id": "signer-a",
            "rpc_url": "RPC_URL_A"
         },
         "chain_id": 31337,
         "transaction_policy": {
            "max_priority_fee_per_gas_wei": "10",
            "max_fee_per_gas_wei": "100",
            "max_gas_limit": "1000",
            "max_total_native_cost_wei": "100000"
         }
      }, {
         "connection": {
            "client_id": "client-b",
            "signature_provider_id": "signer-a",
            "rpc_url": "RPC_URL_B"
         },
         "chain_id": 1
      }]
   })json";
   bind_rpc_url(configuration, rpc_url_placeholder_a, rpc_server_a.url());
   bind_rpc_url(configuration, rpc_url_placeholder_b, rpc_server_b.url());
   const auto path = write_client_configuration_file(directory, configuration);
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

BOOST_AUTO_TEST_CASE(file_configuration_accepts_nonempty_provider_identifier) {
   fc::temp_directory directory;
   chain_id_rpc_server rpc_server;
   std::string configuration = R"json({
      "schema_version":1,
      "clients":[{
         "connection":{
            "client_id":"client-a",
            "signature_provider_id":"prod/signing",
            "rpc_url":"RPC_URL_A"
         },
         "chain_id":31337
      }]
   })json";
   bind_rpc_url(configuration, rpc_url_placeholder_a, rpc_server.url());
   const auto path = write_client_configuration_file(directory, configuration);
   const auto clients = initialize_outpost_plugin(
      {"--outpost-ethereum-client-config-file", path.string()}, "prod/signing");
   BOOST_REQUIRE_EQUAL(clients.size(), 1u);
   BOOST_CHECK_EQUAL(clients.front()->id, "client-a");
}

BOOST_AUTO_TEST_CASE(file_configuration_uses_local_id_without_rpc_validation) {
   fc::temp_directory directory;
   chain_id_rpc_server rpc_server;
   std::string configuration = R"json({
      "schema_version":1,
      "clients":[{
         "connection":{
            "client_id":"client-a",
            "signature_provider_id":"signer-a",
            "rpc_url":"RPC_URL_A"
         },
         "chain_id":1
      }]
   })json";
   bind_rpc_url(configuration, rpc_url_placeholder_a, rpc_server.url());
   const auto path = write_client_configuration_file(directory, configuration);

   BOOST_CHECK_NO_THROW(initialize_outpost_plugin({"--outpost-ethereum-client-config-file", path.string()}));
   BOOST_CHECK_EQUAL(rpc_server.requests.load(), 0u);
}

BOOST_AUTO_TEST_CASE(file_configuration_rejects_wrong_chain_provider) {
   fc::temp_directory directory;
   std::string configuration = R"json({
      "schema_version":1,
      "clients":[{
         "connection":{
            "client_id":"client-a",
            "signature_provider_id":"signer-a",
            "rpc_url":"RPC_URL_A"
         },
         "chain_id":31337
      }]
   })json";
   bind_rpc_url(configuration, rpc_url_placeholder_a, startup_test::unused_url());
   const auto path = write_client_configuration_file(directory, configuration);
   BOOST_CHECK_THROW(
      initialize_outpost_plugin(
         {"--outpost-ethereum-client-config-file", path.string()}, "signer-a", "wire"),
      sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(file_configuration_does_not_publish_a_partial_client_map) {
   fc::temp_directory directory;
   chain_id_rpc_server rpc_server;
   std::string configuration = R"json({
      "schema_version":1,
      "clients":[{
         "connection":{
            "client_id":"valid-first",
            "signature_provider_id":"signer-a",
            "rpc_url":"RPC_URL_A"
         },
         "chain_id":31337
      },{
         "connection":{
            "client_id":"invalid-second",
            "signature_provider_id":"missing-signer",
            "rpc_url":"RPC_URL_B"
         },
         "chain_id":1
      }]
   })json";
   bind_rpc_url(configuration, rpc_url_placeholder_a, rpc_server.url());
   bind_rpc_url(configuration, rpc_url_placeholder_b, startup_test::unused_url());
   const auto path = write_client_configuration_file(directory, configuration);
   BOOST_CHECK_EQUAL(initialize_rejected_file_and_observe_published_clients(path), 0u);
}

BOOST_AUTO_TEST_CASE(cli_client_option_uses_default_policy_with_explicit_chain_id) {
   chain_id_rpc_server rpc_server;
   const auto clients = initialize_outpost_plugin(
      {"--outpost-ethereum-client", "client-a,signer-a," + rpc_server.url() + ",31337"});
   const auto& maximum = maximum_ethereum_transaction_policy_value();
   BOOST_REQUIRE_EQUAL(clients.size(), 1u);
   BOOST_CHECK_EQUAL(clients.front()->chain_id, 31337);
   BOOST_CHECK_EQUAL(clients.front()->client->transaction_policy().max_fee_per_gas, maximum);
}

BOOST_AUTO_TEST_CASE(cli_client_option_preserves_nonempty_identifier_compatibility) {
   chain_id_rpc_server rpc_server;
   const auto clients = initialize_outpost_plugin(
      {"--outpost-ethereum-client", "cli/client,prod/signing," + rpc_server.url() + ",31337"},
      "prod/signing");
   BOOST_REQUIRE_EQUAL(clients.size(), 1u);
   BOOST_CHECK_EQUAL(clients.front()->id, "cli/client");
   BOOST_CHECK_EQUAL(clients.front()->client->transaction_policy().client_id, "cli-client");
}

BOOST_AUTO_TEST_CASE(cli_three_field_client_defers_chain_id_without_signing_client) {
   chain_id_rpc_server rpc_server;
   const auto clients = initialize_outpost_plugin(
      {"--outpost-ethereum-client", "client-a,signer-a," + rpc_server.url()});
   BOOST_REQUIRE_EQUAL(clients.size(), 1u);
   BOOST_CHECK_EQUAL(clients.front()->chain_id, 0u);
   BOOST_CHECK(!clients.front()->client);
   BOOST_CHECK_EQUAL(rpc_server.requests.load(), 0u);
}

BOOST_AUTO_TEST_CASE(plugin_startup_rejects_mixed_unified_and_cli_modes) {
   fc::temp_directory directory;
   const auto path = write_client_configuration_file(
      directory, R"json({"schema_version":1,"clients":[]})json");
   BOOST_CHECK_THROW(initialize_outpost_plugin(
      {"--outpost-ethereum-client-config-file",
       path.string(),
       "--outpost-ethereum-client",
       "client-a,signer-a," + startup_test::unused_url() + ",31337"}),
      sysio::chain::plugin_config_exception);
}

/// CLI clients read at the default confirmation unless the read options say otherwise.
BOOST_AUTO_TEST_CASE(cli_clients_read_at_the_configured_confirmation) {
   chain_id_rpc_server rpc_server;
   const std::string   spec = "client-a,signer-a," + rpc_server.url() + ",31337";

   auto clients = initialize_outpost_plugin({"--outpost-ethereum-client", spec});
   BOOST_REQUIRE_EQUAL(clients.size(), 1u);
   auto confirmation = clients.front()->read_confirmation;
   BOOST_CHECK(confirmation.tag == block_tag_t::latest);
   BOOST_CHECK_EQUAL(confirmation.confirmations, default_read_confirmations);
   BOOST_CHECK(confirmation.max_finality_lag == default_max_finality_lag);

   clients = initialize_outpost_plugin({"--outpost-ethereum-client", spec, "--outpost-ethereum-read-confirmations",
                                        "6", "--outpost-ethereum-read-max-finality-lag-sec", "600"});
   confirmation = clients.front()->read_confirmation;
   BOOST_CHECK(confirmation.tag == block_tag_t::latest);
   BOOST_CHECK_EQUAL(confirmation.confirmations, 6u);
   BOOST_CHECK(confirmation.max_finality_lag == fc::seconds(600));

   for (const auto tag : {block_tag_t::finalized, block_tag_t::safe}) {
      clients = initialize_outpost_plugin({"--outpost-ethereum-client", spec, "--outpost-ethereum-read-block-tag",
                                           fc::network::ethereum::to_string(tag)});
      BOOST_CHECK(clients.front()->read_confirmation.tag == tag);
   }

   // A three-field client waiting on chain-id discovery carries it too.
   clients = initialize_outpost_plugin({"--outpost-ethereum-client", "client-a,signer-a," + rpc_server.url(),
                                        "--outpost-ethereum-read-block-tag", "safe"});
   BOOST_CHECK(clients.front()->read_confirmation.tag == block_tag_t::safe);
}

/// Read options that name no readable confirmation, or a depth beside a tag
/// that has none, are configuration errors.
BOOST_AUTO_TEST_CASE(cli_read_options_refuse_what_no_read_can_be_made_at) {
   const std::string spec = "client-a,signer-a," + startup_test::unused_url() + ",31337";
   for (const auto& options : std::vector<std::vector<std::string>>{
           {"--outpost-ethereum-read-block-tag", "bogus"},
           {"--outpost-ethereum-read-block-tag", "pending"},
           {"--outpost-ethereum-read-block-tag", "earliest"},
           {"--outpost-ethereum-read-confirmations", "0"},
           {"--outpost-ethereum-read-max-finality-lag-sec", "0"},
           {"--outpost-ethereum-read-block-tag", "safe", "--outpost-ethereum-read-confirmations", "12"},
           {"--outpost-ethereum-read-block-tag", "finalized", "--outpost-ethereum-read-max-finality-lag-sec", "600"},
        }) {
      std::vector<std::string> arguments{"--outpost-ethereum-client", spec};
      arguments.insert(arguments.end(), options.begin(), options.end());
      BOOST_CHECK_THROW(initialize_outpost_plugin(arguments), sysio::chain::plugin_config_exception);
   }
}

/// A configuration file sets the read confirmation per client, defaulting a
/// client that leaves it out, and refuses the CLI read options beside it.
BOOST_AUTO_TEST_CASE(file_configuration_sets_the_read_confirmation_per_client) {
   fc::temp_directory  directory;
   chain_id_rpc_server rpc_server_a;
   chain_id_rpc_server rpc_server_b{ethereum_mainnet_chain_id_quantity};
   chain_id_rpc_server rpc_server_c;
   std::string configuration = R"json({
      "schema_version": 1,
      "clients": [{
         "connection": {"client_id": "client-a", "signature_provider_id": "signer-a", "rpc_url": "RPC_URL_A"},
         "chain_id": 31337,
         "read_confirmation": {"block_tag": "EVM_READ_BLOCK_TAG_SAFE"}
      }, {
         "connection": {"client_id": "client-b", "signature_provider_id": "signer-a", "rpc_url": "RPC_URL_B"},
         "chain_id": 1,
         "read_confirmation": {"confirmations": 20}
      }, {
         "connection": {"client_id": "client-c", "signature_provider_id": "signer-a", "rpc_url": "RPC_URL_C"},
         "chain_id": 11155111
      }]
   })json";
   bind_rpc_url(configuration, rpc_url_placeholder_a, rpc_server_a.url());
   bind_rpc_url(configuration, rpc_url_placeholder_b, rpc_server_b.url());
   bind_rpc_url(configuration, "RPC_URL_C", rpc_server_c.url());
   const auto path = write_client_configuration_file(directory, configuration);

   const auto clients = initialize_outpost_plugin({"--outpost-ethereum-client-config-file", path.string()});
   BOOST_REQUIRE_EQUAL(clients.size(), 3u);
   BOOST_CHECK(clients[0]->read_confirmation.tag == block_tag_t::safe);
   BOOST_CHECK(clients[1]->read_confirmation.tag == block_tag_t::latest);
   BOOST_CHECK_EQUAL(clients[1]->read_confirmation.confirmations, 20u);
   BOOST_CHECK(clients[1]->read_confirmation.max_finality_lag == default_max_finality_lag);
   BOOST_CHECK(clients[2]->read_confirmation.tag == block_tag_t::latest);
   BOOST_CHECK_EQUAL(clients[2]->read_confirmation.confirmations, default_read_confirmations);

   BOOST_CHECK_THROW(initialize_outpost_plugin({"--outpost-ethereum-client-config-file", path.string(),
                                                "--outpost-ethereum-read-block-tag", "safe"}),
                     sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(outpost_factory_reports_client_configuration_chain_mismatch) {
   fc::temp_directory directory;
   chain_id_rpc_server rpc_server;
   std::string configuration = R"json({
      "schema_version": 1,
      "clients": [{
         "connection": {
            "client_id": "client-a",
            "signature_provider_id": "signer-a",
            "rpc_url": "RPC_URL_A"
         },
         "chain_id": 31337
      }]
   })json";
   bind_rpc_url(configuration, rpc_url_placeholder_a, rpc_server.url());
   const auto path = write_client_configuration_file(directory, configuration);

   with_initialized_outpost_plugin(
      {"--outpost-ethereum-client-config-file", path.string()},
      [&](auto& plugin) {
         const auto chain_code = fc::slug_name{"ETH"}.value;
         try {
            plugin.create_outpost_client(
               "client-a",
               chain_code,
               1,
               std::string(contract_address),
               std::string(contract_address));
            BOOST_FAIL("expected client/outpost chain mismatch rejection");
         } catch (const sysio::chain::plugin_config_exception& rejection) {
            const auto detail = rejection.to_detail_string();
            BOOST_CHECK(detail.find("reason_code=configuration_chain_id_mismatch") != std::string::npos);
            BOOST_CHECK(detail.find("chain=ETH") != std::string::npos);
            BOOST_CHECK(detail.find("client_id=client-a") != std::string::npos);
            BOOST_CHECK(detail.find("registry_chain_id=1") != std::string::npos);
            BOOST_CHECK(detail.find("client_chain_id=31337") != std::string::npos);
            BOOST_CHECK(detail.find("transaction policy") == std::string::npos);
         }
      });
}

BOOST_AUTO_TEST_CASE(plugin_startup_redacts_an_invalid_authenticated_rpc_url) {
   fc::temp_directory directory;
   constexpr std::string_view sensitive_url =
      "http://user:password@localhost:not-a-port/rpc?token=secret";
   const auto path = write_client_configuration_file(directory, R"json({
      "schema_version": 1,
      "clients": [{
         "connection": {
            "client_id": "client-a",
            "signature_provider_id": "signer-a",
            "rpc_url": "http://user:password@localhost:not-a-port/rpc?token=secret"
         },
         "chain_id": 31337
      }]
   })json");
   try {
      initialize_outpost_plugin(
         {"--outpost-ethereum-client-config-file", path.string()});
      BOOST_FAIL("expected invalid URL rejection");
   } catch (const sysio::opp::config::client_config_exception& rejection) {
      BOOST_CHECK(rejection.reason() == sysio::opp::config::client_config_reason::rpc_url_invalid);
      BOOST_CHECK(rejection.observed().find(sensitive_url) == std::string::npos);
      BOOST_CHECK(rejection.to_detail_string().find(sensitive_url) == std::string::npos);
   }
}

BOOST_AUTO_TEST_SUITE_END()

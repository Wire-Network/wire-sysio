#include <gsl-lite/gsl-lite.hpp>

#include <boost/test/unit_test.hpp>
#include <boost/dll.hpp>
#include <boost/process/v1/io.hpp>


#include <atomic>
#include <chrono>
#include <iomanip>
#include <thread>
#include <optional>
#include <set>
#include <sstream>

#include <fc/crypto/ethereum/ethereum_types.hpp>
#include <fc/crypto/ethereum/ethereum_utils.hpp>
#include <fc/crypto/signer.hpp>
#include <fc/network/ethereum/ethereum_client.hpp>
#include <fc/network/ethereum/ethereum_abi.hpp>
#include <fc/network/ethereum/ethereum_rlp_encoder.hpp>

#include <sysio/chain/types.hpp>
#include <sysio/signature_provider_manager_plugin/signature_provider_manager_plugin.hpp>
#include <fc-test/build_info.hpp>
#include <fc-test/crypto_utils.hpp>
#include <fc-test/one_shot_http_server.hpp>

#include <sysio/outpost_ethereum_client_plugin.hpp>
#include <sysio/outpost_ethereum_client_plugin/outpost_ethereum_client.hpp>
#include <sysio/opp/opp.hpp>
#include <sysio/opp/opp.pb.h>

using namespace std::literals;

using namespace fc::crypto;
using namespace fc::crypto::ethereum;
using namespace fc::network::ethereum;
namespace eth = fc::network::ethereum;

using namespace fc::test;

using sysio::signature_provider_manager_plugin;

namespace {
/* RLP encoding test data 01 */
std::pair<std::string, std::string> test_str_01{"test123", "c88774657374313233"};

/* RLP vector of encoding tests */
std::vector<std::pair<std::string, std::string>> test_str_pairs{
   test_str_01
};

std::string test_tx_01_sig{"setNumber(uint256)"};
std::vector<std::string> test_tx_01_sig_params{"60"};
std::string test_tx_01_sig_encoded{"3fb5c1cb000000000000000000000000000000000000000000000000000000000000003c"};

/* RLP tx 01 */
eip1559_tx test_tx_01{
   .chain_id = 31337,
   .nonce = 13,
   .max_priority_fee_per_gas = 2000000000,
   .max_fee_per_gas = 2000101504,
   .gas_limit = 0x18c80,
   .to = to_address("5FbDB2315678afecb367f032d93F642f64180aa3"),
   .value = 0,
   .data = fc::from_hex(test_tx_01_sig_encoded),
   .access_list = {}
};

/* RLP Encoded result of `test_tx_01` */
std::vector<std::uint8_t> test_tx_01_unsigned_result{
   0x02, 0xf8, 0x4e, 0x82, 0x7a, 0x69, 0x0d, 0x84, 0x77, 0x35, 0x94, 0x00,
   0x84, 0x77, 0x37, 0x20, 0x80, 0x83, 0x01, 0x8c, 0x80, 0x94, 0x5f, 0xbd,
   0xb2, 0x31, 0x56, 0x78, 0xaf, 0xec, 0xb3, 0x67, 0xf0, 0x32, 0xd9, 0x3f,
   0x64, 0x2f, 0x64, 0x18, 0x0a, 0xa3, 0x80, 0xa4, 0x3f, 0xb5, 0xc1, 0xcb,
   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3c, 0xc0
};


std::string test_tx_01_r      = "93166a3ed10a4050dce7261c4ca8bcba16a1731117c453a326a1742c959b33f0";
std::string test_tx_01_s      = "7c17a232cd69ce93f21a30579a2a94309b2d71918043134b4c5df5788078a0e4";
fc::uint256 test_tx_01_v      = 0;
std::string test_tx_01_result =
   "02f84e827a690d8477359400847737208083018c80945fbdb2315678afecb367f032d93f642f64180aa380a43fb5c1cb000000000000000000000000000000000000000000000000000000000000003cc0";

}

namespace {

namespace bp = boost::process;
namespace bfs = boost::filesystem;
std::string program_name{"test_outpost_ethereum_client_plugin"};
/**
 * Sig provider tester app resources
 */
struct sig_provider_tester {

   appbase::scoped_app app{};

   sysio::signature_provider_manager_plugin& plugin() { return app->get_plugin<signature_provider_manager_plugin>(); }
};

/**
 * Creates a tester/app scoped instance
 *
 * @tparam args additional args to pass to `scoped_app`
 * @return `unique_ptr<sig_provider_tester>`
 */

// Overload that accepts a vector of strings for arguments
std::unique_ptr<sig_provider_tester> create_app(const std::vector<std::string>& args) {
   auto tester = std::make_unique<sig_provider_tester>();

   // Build argv as vector<char*> pointing to the underlying string buffers
   std::vector<char*> argv;
   argv.reserve(args.size() + 1);
   argv.push_back(program_name.data()); // program name
   for (auto& s : args) {
      argv.push_back(const_cast<char*>(s.c_str()));
   }

   BOOST_CHECK(tester->app->initialize<sysio::signature_provider_manager_plugin>(argv.size(), argv.data()));

   return tester;
}

template <typename... Args>
   requires((std::same_as<std::decay_t<Args>, std::string>) && ...)
std::unique_ptr<sig_provider_tester> create_app(Args&&... extra_args) {
   std::vector<std::string> args_vec = {std::forward<Args>(extra_args)...};
   return create_app(args_vec);
}

constexpr std::string_view test_contract_abi_counter_json_file_01 = "ethereum-abi-counter-01.json";
using namespace fc::network::ethereum;
auto counter_abi_filename = fc::test::get_test_fixtures_path() / boost::filesystem::path(test_contract_abi_counter_json_file_01);
auto counter_abis = [](){return fc::network::ethereum::abi::parse_contracts(std::filesystem::path(counter_abi_filename.generic_string()));};

struct ethereum_contract_test_counter_client : fc::network::ethereum::ethereum_contract_client {

   ethereum_contract_tx_fn<fc::variant, fc::uint256> set_number;
   ethereum_contract_call_fn<fc::variant> get_number;
   ethereum_contract_test_counter_client(const ethereum_client_ptr& client,
                                         const address_compat_type& contract_address_compat)
      : ethereum_contract_client(client, contract_address_compat, counter_abis()),
   set_number(create_tx<fc::variant, fc::uint256>(get_abi("setNumber"))),
   get_number(create_call<fc::variant>(get_abi("number"))) {

   };
};

}

namespace {

constexpr std::string_view opp_abi_fixture = "ethereum-abi-opp-current.json";
constexpr std::string_view opp_inbound_abi_fixture = "ethereum-abi-opp-inbound-current.json";
constexpr std::string_view hex_prefix = "0x";
constexpr std::string_view emit_outbound_envelope_abi_name = "emitOutboundEnvelope";
constexpr std::string_view emit_outbound_envelope_selector = "a3ad9cc3";
constexpr std::string_view test_opp_address = "5FbDB2315678afecb367f032d93F642f64180aa3";
constexpr std::string_view latest_slot_test_rpc_url = "http://127.0.0.1:1";
constexpr std::string_view latest_slot_test_entry_id = "latest-slot-test";
constexpr std::string_view latest_slot_test_private_key =
   "0xac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80";
constexpr std::string_view latest_slot_test_public_key =
   "0x8318535b54105d4a7aae60c08fc45f9687181b4fdfc625bd1a753fa7397fed7535"
   "47f11ca8696646f2f3acb08e31016afac23e630c5d11f59f61fef57b0d2aa5";
constexpr size_t evm_abi_word_bytes = 32;
constexpr size_t hex_chars_per_byte = 2;
constexpr size_t evm_abi_word_hex_chars = evm_abi_word_bytes * hex_chars_per_byte;
constexpr size_t evm_function_selector_bytes = 4;
constexpr size_t evm_function_selector_hex_chars = evm_function_selector_bytes * hex_chars_per_byte;
constexpr size_t latest_outbound_return_head_words = 2;
constexpr uint64_t latest_outbound_data_offset_bytes = latest_outbound_return_head_words * evm_abi_word_bytes;
constexpr size_t emit_outbound_envelope_call_hex_chars =
   evm_function_selector_hex_chars + evm_abi_word_hex_chars;
constexpr uint64_t test_outpost_chain_code = 1;
constexpr uint32_t test_evm_chain_id = 31337;
constexpr uint32_t test_wire_epoch = 7;
constexpr uint32_t test_stale_wire_epoch = test_wire_epoch - 1;
constexpr uint32_t test_different_wire_epoch = test_wire_epoch + 1;
constexpr int64_t test_rpc_deadline_seconds = 1;
constexpr size_t rpc_length_oversized_envelope_bytes = sysio::OPP_MAX_ENVELOPE_BYTES + 1;
constexpr char malformed_envelope_byte = static_cast<char>(0xff);
constexpr char oversized_envelope_fill_byte = static_cast<char>(0x01);

/** Build a one-shot JSON-RPC endpoint reporting Anvil's chain id (31337). */
fc::test::one_shot_http_server chain_id_rpc_server(std::string result_json = "\"0x7a69\"") {
   return fc::test::one_shot_http_server{
      R"json({"jsonrpc":"2.0","id":1,"result":)json" + result_json + "}",
      "eth_chainId"};
}

/** Build the canonical named Ethereum signature-provider test spec. */
std::string named_ethereum_signature_provider(std::string name = "signer-a",
                                              std::string chain_kind = "ethereum") {
   return name + "," + chain_kind + ",ethereum," + std::string(latest_slot_test_public_key) +
          ",KEY:" + std::string(latest_slot_test_private_key);
}

/** Build a provider with Ethereum targeting but a valid non-Ethereum key type. */
std::string ethereum_target_with_wire_key_provider() {
   const auto fixture = fc::test::load_keygen_fixture("wire", 1);
   return fc::crypto::to_signature_provider_spec(
      "signer-a",
      fc::crypto::chain_kind_ethereum,
      fixture.chain_key_type,
      fixture.public_key,
      fc::test::to_private_key_spec(fixture.private_key));
}

/** Initialize the complete outpost plugin with the supplied configuration arguments. */
void initialize_outpost_plugin(const std::vector<std::string>& configuration_arguments) {
   appbase::scoped_app test_application{};
   std::vector<std::string> arguments{"test_outpost_ethereum_client_plugin"};
   arguments.insert(arguments.end(), configuration_arguments.begin(), configuration_arguments.end());

   std::vector<char*> argv;
   argv.reserve(arguments.size());
   for (auto& argument : arguments) {
      argv.emplace_back(argument.data());
   }

   BOOST_REQUIRE(test_application->initialize<sysio::outpost_ethereum_client_plugin>(argv.size(), argv.data()));
}

auto load_abi_fixture(std::string_view filename) {
   auto path = fc::test::get_test_fixtures_path() / bfs::path(filename);
   return fc::network::ethereum::abi::parse_contracts(std::filesystem::path(path.generic_string()));
}

/// Encode an unsigned integer as one 32-byte Ethereum ABI word.
std::string abi_word(uint64_t value) {
   std::ostringstream stream;
   stream << std::hex << std::setfill('0') << std::setw(evm_abi_word_hex_chars) << value;
   return stream.str();
}

/// Encode the raw return bytes for `getLatestOutboundEnvelope()`.
std::string encode_latest_outbound_result(uint32_t epoch, const std::vector<char>& data) {
   auto data_hex = data.empty() ? std::string{} : fc::to_hex(data.data(), data.size());
   data_hex.append(
      (evm_abi_word_hex_chars - (data_hex.size() % evm_abi_word_hex_chars)) % evm_abi_word_hex_chars,
      '0');
   return std::string(hex_prefix) + abi_word(epoch) + abi_word(latest_outbound_data_offset_bytes) +
          abi_word(data.size()) + data_hex;
}

/// Serialize a minimal protobuf envelope carrying only its epoch index.
std::vector<char> serialize_envelope(uint32_t epoch) {
   sysio::opp::Envelope envelope;
   envelope.set_epoch_index(epoch);
   const auto serialized = envelope.SerializeAsString();
   return {serialized.begin(), serialized.end()};
}

} // anonymous namespace

BOOST_AUTO_TEST_SUITE(outpost_ethereum_client_plugin)

// ---------------------------------------------------------------------------
//  Startup configuration validation
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(startup_accepts_matching_named_signer_and_remote_chain_id) {
   auto rpc_server = chain_id_rpc_server();
   BOOST_CHECK_NO_THROW(initialize_outpost_plugin({
      "--signature-provider",
      named_ethereum_signature_provider(),
      "--outpost-ethereum-client",
      "client-a,signer-a," + rpc_server.url() + ",31337",
   }));
}

BOOST_AUTO_TEST_CASE(startup_accepts_three_field_client_without_remote_chain_id_check) {
   fc::test::connection_closing_http_server rpc_server;
   BOOST_CHECK_NO_THROW(initialize_outpost_plugin({
      "--signature-provider",
      named_ethereum_signature_provider(),
      "--outpost-ethereum-client",
      "client-a,signer-a," + rpc_server.url(),
   }));
}

BOOST_AUTO_TEST_CASE(startup_accepts_maximum_registered_chain_id) {
   auto rpc_server = chain_id_rpc_server("\"0xffffffff\"");
   BOOST_CHECK_NO_THROW(initialize_outpost_plugin({
      "--signature-provider",
      named_ethereum_signature_provider(),
      "--outpost-ethereum-client",
      "client-a,signer-a," + rpc_server.url() + ",4294967295",
   }));
}

BOOST_AUTO_TEST_CASE(startup_rejects_client_without_matching_named_signature_provider) {
   auto rpc_server = chain_id_rpc_server();
   BOOST_CHECK_THROW(initialize_outpost_plugin({
      "--signature-provider",
      named_ethereum_signature_provider("other-signer"),
      "--outpost-ethereum-client",
      "client-a,signer-a," + rpc_server.url() + ",31337",
   }), sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(startup_rejects_anonymous_signature_provider_reference) {
   auto rpc_server = chain_id_rpc_server();
   const std::string anonymous_provider =
      "ethereum,ethereum," + std::string(latest_slot_test_public_key) +
      ",KEY:" + std::string(latest_slot_test_private_key);
   BOOST_CHECK_THROW(initialize_outpost_plugin({
      "--signature-provider",
      anonymous_provider,
      "--outpost-ethereum-client",
      "client-a,key-0," + rpc_server.url() + ",31337",
   }), sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(startup_rejects_named_signer_for_wrong_chain) {
   auto rpc_server = chain_id_rpc_server();
   BOOST_CHECK_THROW(initialize_outpost_plugin({
      "--signature-provider",
      named_ethereum_signature_provider("signer-a", "wire"),
      "--outpost-ethereum-client",
      "client-a,signer-a," + rpc_server.url() + ",31337",
   }), sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(startup_rejects_named_signer_with_wrong_key_type) {
   auto rpc_server = chain_id_rpc_server();
   BOOST_CHECK_THROW(initialize_outpost_plugin({
      "--signature-provider",
      ethereum_target_with_wire_key_provider(),
      "--outpost-ethereum-client",
      "client-a,signer-a," + rpc_server.url() + ",31337",
   }), sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(startup_rejects_chain_id_mismatch_with_rpc_endpoint) {
   auto rpc_server = chain_id_rpc_server();
   BOOST_CHECK_THROW(initialize_outpost_plugin({
      "--signature-provider",
      named_ethereum_signature_provider(),
      "--outpost-ethereum-client",
      "client-a,signer-a," + rpc_server.url() + ",1",
   }), sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(startup_rejects_unavailable_rpc_when_chain_id_is_explicit) {
   fc::test::connection_closing_http_server rpc_server;
   BOOST_CHECK_THROW(initialize_outpost_plugin({
      "--signature-provider",
      named_ethereum_signature_provider(),
      "--outpost-ethereum-client",
      "client-a,signer-a," + rpc_server.url() + ",31337",
   }), sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(startup_rejects_invalid_remote_chain_id) {
   auto rpc_server = chain_id_rpc_server("\"not-a-chain-id\"");
   BOOST_CHECK_THROW(initialize_outpost_plugin({
      "--signature-provider",
      named_ethereum_signature_provider(),
      "--outpost-ethereum-client",
      "client-a,signer-a," + rpc_server.url() + ",31337",
   }), sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(startup_rejects_malformed_configured_chain_id_as_plugin_configuration) {
   auto rpc_server = chain_id_rpc_server();
   BOOST_CHECK_THROW(initialize_outpost_plugin({
      "--signature-provider",
      named_ethereum_signature_provider(),
      "--outpost-ethereum-client",
      "client-a,signer-a," + rpc_server.url() + ",not-a-chain-id",
   }), sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(startup_rejects_chain_id_that_cannot_match_registered_outpost_id) {
   auto rpc_server = chain_id_rpc_server();
   BOOST_CHECK_THROW(initialize_outpost_plugin({
      "--signature-provider",
      named_ethereum_signature_provider(),
      "--outpost-ethereum-client",
      "client-a,signer-a," + rpc_server.url() + ",4294998633",
   }), sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(startup_rejects_configured_chain_id_wider_than_uint256_without_wraparound) {
   auto rpc_server = chain_id_rpc_server();
   BOOST_CHECK_THROW(initialize_outpost_plugin({
      "--signature-provider",
      named_ethereum_signature_provider(),
      "--outpost-ethereum-client",
      "client-a,signer-a," + rpc_server.url() +
         ",115792089237316195423570985008687907853269984665640564039457584007913129671273",
   }), sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(startup_rejects_remote_chain_id_wider_than_uint256_without_wraparound) {
   auto rpc_server = chain_id_rpc_server(
      "\"0x10000000000000000000000000000000000000000000000000000000000007a69\"");
   BOOST_CHECK_THROW(initialize_outpost_plugin({
      "--signature-provider",
      named_ethereum_signature_provider(),
      "--outpost-ethereum-client",
      "client-a,signer-a," + rpc_server.url() + ",31337",
   }), sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(one_shot_http_server_destruction_without_request_does_not_block) {
   fc::test::one_shot_http_server unused_server{
      R"json({"jsonrpc":"2.0","id":1,"result":"0x1"})json",
      "eth_chainId"};
}

// ---------------------------------------------------------------------------
//  OPP typed contract client tests
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(opp_contract_client_construction) try {
   auto abis = load_abi_fixture(opp_abi_fixture);
   BOOST_CHECK(!abis.empty());

   // Construction resolves every required ABI entry. A null RPC client is
   // sufficient here because the generated callables are not invoked.
   auto client = std::make_shared<sysio::opp_contract_client>(
      ethereum_client_ptr{},
      address_compat_type{std::string(test_opp_address)},
      abis);
   BOOST_REQUIRE(client);
   BOOST_CHECK(client->emit_outbound_envelope);
   BOOST_CHECK(client->get_latest_outbound_envelope);

   // Verify the live relay surface is present and the retired finalizer is not.
   bool has_emit = false, has_latest = false, has_finalize = false;
   for (auto& c : abis) {
      if (c.name == emit_outbound_envelope_abi_name) has_emit = true;
      if (c.name == "getLatestOutboundEnvelope") has_latest = true;
      if (c.name == "finalizeEpoch") has_finalize = true;
   }
   BOOST_CHECK(has_emit);
   BOOST_CHECK(has_latest);
   BOOST_CHECK(!has_finalize);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(opp_inbound_contract_client_construction) try {
   auto abis = load_abi_fixture(opp_inbound_abi_fixture);
   BOOST_CHECK(!abis.empty());

   bool has_epoch_in = false, has_next_epoch = false;
   for (auto& c : abis) {
      if (c.name == "epochIn") has_epoch_in = true;
      if (c.name == "nextEpochIndex") has_next_epoch = true;
   }
   BOOST_CHECK(has_epoch_in);
   BOOST_CHECK(has_next_epoch);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(epoch_in_abi_encoding_with_bytes_param) try {
   auto abis = load_abi_fixture(opp_inbound_abi_fixture);

   // Find the epochIn ABI entry
   const eth::abi::contract* epoch_in_abi = nullptr;
   for (auto& c : abis) {
      if (c.name == "epochIn") { epoch_in_abi = &c; break; }
   }
   BOOST_REQUIRE(epoch_in_abi != nullptr);
   BOOST_CHECK_EQUAL(epoch_in_abi->inputs.size(), 1u);
   BOOST_CHECK(epoch_in_abi->inputs[0].type == eth::abi::data_type::bytes);

   // Encode with 1 param (hex-encoded bytes) — this is what the batch operator does
   std::string test_envelope_hex = "120c0a040800100012040800100028deeef5ce06300138";
   auto encoded = contract_encode_data(*epoch_in_abi, std::vector<fc::variant>{fc::variant(test_envelope_hex)});
   BOOST_CHECK(!encoded.empty());

   // The encoded data should start with the epochIn selector (0xcfae3118)
   BOOST_CHECK(encoded.substr(0, 8) == "cfae3118");

   // Verify that encoding with 0 params throws (the bug we fixed)
   BOOST_CHECK_THROW(
      contract_encode_data(*epoch_in_abi, std::vector<fc::variant>{}),
      fc::assert_exception
   );
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(emit_outbound_envelope_abi_encoding_wire_epoch) try {
   auto abis = load_abi_fixture(opp_abi_fixture);

   const eth::abi::contract* emit_abi = nullptr;
   for (auto& c : abis) {
      if (c.name == emit_outbound_envelope_abi_name) { emit_abi = &c; break; }
   }
   BOOST_REQUIRE(emit_abi != nullptr);
   BOOST_REQUIRE_EQUAL(emit_abi->inputs.size(), 1u);
   BOOST_CHECK(emit_abi->inputs[0].type == eth::abi::data_type::uint32);

   // Encoding carries the WIRE epoch expected by the Solidity recovery call.
   auto encoded = contract_encode_data(
      *emit_abi,
      std::vector<fc::variant>{fc::variant(uint64_t{test_wire_epoch})});
   BOOST_CHECK(!encoded.empty());
   BOOST_CHECK(encoded.substr(0, evm_function_selector_hex_chars) == emit_outbound_envelope_selector);
   BOOST_CHECK_EQUAL(encoded.size(), emit_outbound_envelope_call_hex_chars);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(emit_outbound_envelope_recovery_wrapper_forwards_wire_epoch) try {
   auto abis = load_abi_fixture(opp_abi_fixture);
   auto client = std::make_shared<sysio::opp_contract_client>(
      ethereum_client_ptr{},
      address_compat_type{std::string(test_opp_address)},
      abis);

   uint32_t observed_epoch = 0;
   std::string observed_call_data;
   client->emit_outbound_envelope =
      [&](uint32_t& wire_epoch) -> fc::variant {
         observed_epoch = wire_epoch;
         observed_call_data = contract_encode_data(
            client->get_abi(std::string(emit_outbound_envelope_abi_name)),
            std::vector<fc::variant>{fc::variant(uint64_t{wire_epoch})});
         return fc::variant(observed_call_data);
      };

   // Replace network submission at the typed callable boundary, then invoke
   // the recovery surface exposed for operator tooling. The mock sink encodes
   // with the production ABI so the assertion covers both argument forwarding
   // and the exact transaction call data without requiring a live EVM node.
   uint32_t wire_epoch = test_wire_epoch;
   const auto result = client->emit_outbound_envelope(wire_epoch);
   const auto expected_call_data =
      std::string(emit_outbound_envelope_selector) + abi_word(test_wire_epoch);
   BOOST_CHECK_EQUAL(observed_epoch, test_wire_epoch);
   BOOST_CHECK_EQUAL(observed_call_data, expected_call_data);
   BOOST_CHECK_EQUAL(result.as_string(), expected_call_data);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(read_inbound_envelope_validates_latest_slot) try {
   auto clean_app = gsl_lite::finally([]() {
      appbase::application::reset_app_singleton();
   });
   auto tester = create_app();
   auto private_key_spec = to_private_key_spec(std::string(latest_slot_test_private_key));
   auto sig_provider = tester->plugin().create_provider(
      std::string(latest_slot_test_entry_id),
      chain_kind_ethereum,
      chain_key_type_ethereum,
      std::string(latest_slot_test_public_key),
      private_key_spec);

   const std::string rpc_url{latest_slot_test_rpc_url};
   auto eth_client = std::make_shared<ethereum_client>(
      sig_provider,
      std::variant<std::string, fc::url>{rpc_url},
      fc::uint256{test_evm_chain_id});
   auto abis = load_abi_fixture(opp_abi_fixture);
   const std::string opp_address{test_opp_address};
   auto typed_opp = eth_client->get_contract<sysio::opp_contract_client>(opp_address, abis);

   auto entry = std::make_shared<sysio::ethereum_client_entry_t>();
   entry->id = latest_slot_test_entry_id;
   entry->url = rpc_url;
   entry->signature_provider = sig_provider;
   entry->client = eth_client;
   entry->chain_id = test_evm_chain_id;

   sysio::outpost_ethereum_client outpost(
      entry,
      opp_address,
      "",
      "",
      abis,
      test_outpost_chain_code,
      test_evm_chain_id);

   auto set_response = [&](std::string response) {
      typed_opp->get_latest_outbound_envelope =
         [response = std::move(response)](const block_number_or_tag_t& block) -> fc::variant {
            BOOST_CHECK(std::holds_alternative<block_tag_t>(block));
            BOOST_CHECK(std::get<block_tag_t>(block) == block_tag_t::finalized);
            return fc::variant(response);
         };
   };

   const auto matching = serialize_envelope(test_wire_epoch);
   set_response(encode_latest_outbound_result(test_wire_epoch, matching));
   BOOST_CHECK(outpost.read_inbound_envelope(
      test_wire_epoch,
      fc::seconds(test_rpc_deadline_seconds)) == matching);

   set_response(encode_latest_outbound_result(test_stale_wire_epoch, matching));
   BOOST_CHECK(outpost.read_inbound_envelope(
      test_wire_epoch,
      fc::seconds(test_rpc_deadline_seconds)).empty());

   set_response(encode_latest_outbound_result(test_wire_epoch, {}));
   BOOST_CHECK(outpost.read_inbound_envelope(
      test_wire_epoch,
      fc::seconds(test_rpc_deadline_seconds)).empty());

   set_response(encode_latest_outbound_result(
      test_wire_epoch,
      std::vector<char>{malformed_envelope_byte}));
   BOOST_CHECK(outpost.read_inbound_envelope(
      test_wire_epoch,
      fc::seconds(test_rpc_deadline_seconds)).empty());

   set_response(encode_latest_outbound_result(
      test_wire_epoch,
      serialize_envelope(test_different_wire_epoch)));
   BOOST_CHECK(outpost.read_inbound_envelope(
      test_wire_epoch,
      fc::seconds(test_rpc_deadline_seconds)).empty());

   // A bytes value one byte over the envelope cap necessarily makes the
   // complete `(uint32, bytes)` ABI result exceed the RPC hex-length cap.
   // This case therefore verifies the pre-decode RPC boundary, not the later
   // decoded-byte defense-in-depth check.
   std::vector<char> rpc_length_oversized(
      rpc_length_oversized_envelope_bytes,
      oversized_envelope_fill_byte);
   set_response(encode_latest_outbound_result(test_wire_epoch, rpc_length_oversized));
   BOOST_CHECK(outpost.read_inbound_envelope(
      test_wire_epoch,
      fc::seconds(test_rpc_deadline_seconds)).empty());
} FC_LOG_AND_RETHROW();

// ---------------------------------------------------------------------------
//  Original tests
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(can_encode_tx_01) try {
   using namespace fc::crypto;

   auto              empty_msg_hash = fc::crypto::ethereum::hash_message(ethereum::to_uint8_span(""));
   std::stringstream ss;
   for (auto byte : std::span(empty_msg_hash.data(), empty_msg_hash.data_size())) {
      ss << std::hex << std::setfill('0') << std::setw(2)
         << static_cast<unsigned>(byte);
   }
   // auto empty_msg_hash_hex = fc::to_hex(reinterpret_cast<const char*>(empty_msg_hash.data()), empty_msg_hash.size());
   auto empty_msg_hash_hex = ss.str();
   BOOST_CHECK("c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470" == empty_msg_hash_hex);

   auto actual_unsigned = rlp::encode_eip1559_unsigned_typed(test_tx_01);

   BOOST_CHECK(std::memcmp(actual_unsigned.data(), test_tx_01_unsigned_result.data(), 81) == 0);
   auto actual_unsigned_hex = rlp::to_hex(actual_unsigned, false);
   BOOST_CHECK_EQUAL(actual_unsigned_hex, test_tx_01_result);

   auto clean_app = gsl_lite::finally([]() {
      appbase::application::reset_app_singleton();
   });
   // Load fixture
   auto private_key_spec = to_private_key_spec("0xac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80");

   auto  tester           = create_app();
   auto& sig_provider_mgr = tester->plugin();

   auto sig_provider =
      sig_provider_mgr.create_provider(
         "eth-01",
         chain_kind_ethereum,
         chain_key_type_ethereum,
         "0x8318535b54105d4a7aae60c08fc45f9687181b4fdfc625bd1a753fa7397fed753547f11ca8696646f2f3acb08e31016afac23e630c5d11f59f61fef57b0d2aa5",
         private_key_spec);


   // Provider should be retrievable
   // Sign raw unsigned TX bytes — eth_client_signer hashes with keccak256 internally
   fc::crypto::eth_client_signer eth_signer(*sig_provider);
   auto sig = eth_signer.sign(std::span<const uint8_t>(actual_unsigned));
   BOOST_CHECK(sig.contains<fc::em::signature_shim>());
   auto&      sig_shim          = sig.get<fc::em::signature_shim>();
   auto&      sig_data          = sig_shim.serialize();
   eip1559_tx test_tx_01_signed = test_tx_01;
   std::copy_n(sig_data.begin(), 32, test_tx_01_signed.r.begin());
   std::copy_n(sig_data.begin() + 32, 32, test_tx_01_signed.s.begin());
   test_tx_01_signed.v = sig_data[64] - 27; // recovery id
   BOOST_CHECK(rlp::to_hex(test_tx_01_signed.r, false) == test_tx_01_r);
   BOOST_CHECK(rlp::to_hex(test_tx_01_signed.s, false) == test_tx_01_s);
   BOOST_CHECK(test_tx_01_signed.v == test_tx_01_v);

} FC_LOG_AND_RETHROW();

// ---------------------------------------------------------------------------
//  Regression: signed EIP-1559 RLP must strip leading zero bytes from r/s
//
//  Captured from a live dev cluster run where the batch operator's signed
//  envelope transaction was rejected by anvil/reth with:
//      -32602 Failed to decode transaction
//      (alloy reported: "leading zero")
//
//  The captured raw tx had signature s = 0x00 9b bd d7 ... — its most
//  significant byte was 0x00. The EIP-1559 RLP encoder emitted r/s as
//  fixed-width 32-byte strings (0xa0 || 32 bytes), which is a non-minimal
//  integer encoding per Ethereum Yellow Paper / EIP-2718 and is rejected by
//  strict decoders.
//
//  This test reconstructs the exact failing tx (same chain_id, nonce, fees,
//  to, data payload, access_list, v, r, s) and asserts the encoder produces
//  the minimally-encoded canonical wire form anvil accepts.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(eip1559_signed_rlp_strips_leading_zero_in_s) try {
   // Exact envelope tx data from the cluster log — the 612-byte calldata
   // emitted by the batch operator plugin calling epochIn(bytes).
   const std::string failing_tx_data_hex =
      "cfae31180000000000000000000000000000000000000000000000000000000000000020"
      "000000000000000000000000000000000000000000000000000000000000020d120c0a04"
      "080010001204080010002894df84cf06300b3800c202f1030a1e0a0c0a04080010001204"
      "08001000220608001000180028003894df84cf0612ce0308011288030893dc0310fe021a"
      "fe020a5e0a0b0a0962617463686f702e6112250802122102ba5734d8f7091719471e7f7e"
      "d6b9df170dc70cc661ca05e688601ad984f068b0122408031220d1add206fd583eb3f410"
      "272cfdab07822e6a90ca104457b89d1a86df858a3f2b180220030a5e0a0b0a0962617463"
      "686f702e62122508021221039d9031e97dd78ff8c15aa86939de9b1e791066a0224e331b"
      "c962a2099a7b1f0412240803122087c4b5c0029c4e1f3085f57aa814f7042f212de28f26"
      "ca4932e7c948a1347f37180220030a5e0a0b0a0962617463686f702e6312250802122102"
      "20b871f3ced029e14472ec4ebc3c0448164942b123aa6af91a3386c1c403e0eb12240803"
      "1220e05be92e22b4f0dc862c98f909317b59e60d1ab17860bc9d8d25745976b97f0f1802"
      "20030a5c0a090a0775777269742e6112250802122103bf6ee64a8d2fdc551ec8bb9ef862"
      "ef6b4bcb1805cdc520c3aa5866c0575fd3b512240803122051639799f4dfc297a0b08405"
      "6e6b69349cf0b6c6800a108afc74fc37d2e49fde18032000123f088fdc0310371a370802"
      "100b1a0f0a0d0801120962617463686f702e611a0f0a0d0801120962617463686f702e63"
      "1a0f0a0d0801120962617463686f702e6200000000000000000000000000000000000000";
   auto failing_tx_data = fc::from_hex(failing_tx_data_hex);
   BOOST_REQUIRE_EQUAL(failing_tx_data.size(), 612u);

   eip1559_tx failing_tx{
      .chain_id = 31337, // anvil default
      .nonce = 3,
      .max_priority_fee_per_gas = 1000000000,
      .max_fee_per_gas = 1000000016,
      .gas_limit = 0xac2e4,
      .to = to_address("f953b3a269d80e3eb0f2947630da976b896a8c5b"),
      .value = 0,
      .data = failing_tx_data,
      .access_list = {},
      .v = 1, // y_parity
   };
   // Signature s starting with a 0x00 byte — the case that triggered the bug.
   auto r_bytes = fc::from_hex("bfb585dea94d9c84f7d43779800f87c21eae3f5288a1234ce079c3d44bfe5d8f");
   auto s_bytes = fc::from_hex("009bbdd7843fc8c472bb43782c0d06979a532783a02fe3aa5e6e1477530521f0");
   BOOST_REQUIRE_EQUAL(r_bytes.size(), 32u);
   BOOST_REQUIRE_EQUAL(s_bytes.size(), 32u);
   BOOST_REQUIRE_EQUAL(static_cast<uint8_t>(s_bytes[0]), 0x00u);
   std::copy_n(r_bytes.begin(), 32, failing_tx.r.begin());
   std::copy_n(s_bytes.begin(), 32, failing_tx.s.begin());

   auto encoded = rlp::encode_eip1559_signed_typed(failing_tx);
   auto encoded_hex = rlp::to_hex(encoded, false);

   // The canonical/minimal wire form: outer list length 0x2d2 (not 0x2d3 that
   // the buggy fixed-width encoding produces); s encoded as 31-byte integer
   // (prefix 0x9f), leading 0x00 byte stripped.
   const std::string expected_fixed_hex =
      "02f902d2827a6903843b9aca00843b9aca10830ac2e4"
      "94f953b3a269d80e3eb0f2947630da976b896a8c5b"
      "80"
      "b90264" + failing_tx_data_hex +
      "c001"
      "a0bfb585dea94d9c84f7d43779800f87c21eae3f5288a1234ce079c3d44bfe5d8f"
      "9f9bbdd7843fc8c472bb43782c0d06979a532783a02fe3aa5e6e1477530521f0";

   BOOST_CHECK_EQUAL(encoded_hex, expected_fixed_hex);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_SUITE_END()

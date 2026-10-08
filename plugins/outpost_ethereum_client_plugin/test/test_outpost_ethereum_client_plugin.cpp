#include <gsl-lite/gsl-lite.hpp>

#include <boost/test/unit_test.hpp>
#include <boost/dll.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/process/v1/io.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <memory>
#include <thread>
#include <optional>
#include <set>
#include <sstream>
#include <vector>

#include <fc/crypto/ethereum/ethereum_types.hpp>
#include <fc/crypto/ethereum/ethereum_utils.hpp>
#include <fc/crypto/signer.hpp>
#include <fc/network/ethereum/ethereum_client.hpp>
#include <fc/network/ethereum/ethereum_abi.hpp>
#include <fc/network/ethereum/ethereum_rlp_encoder.hpp>
#include <fc/network/ethereum/ethereum_transaction_policy.hpp>
#include <fc/network/http/http_client.hpp>
#include <fc/network/json_rpc/json_rpc_client.hpp>
#include <magic_enum/magic_enum.hpp>

#include <sysio/chain/exceptions.hpp>
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
constexpr std::string_view http_scheme_prefix = "http://";
/** Prefix identifying the bounded transport category in chain-id startup diagnostics. */
constexpr std::string_view last_failure_detail_prefix = "last_failure=";
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
constexpr size_t transient_chain_id_failures = 1;
constexpr uint32_t test_wire_epoch = 7;
constexpr uint32_t test_stale_wire_epoch = test_wire_epoch - 1;
constexpr uint32_t test_different_wire_epoch = test_wire_epoch + 1;
constexpr int64_t test_rpc_deadline_seconds = 1;
constexpr size_t rpc_length_oversized_envelope_bytes = sysio::OPP_MAX_ENVELOPE_BYTES + 1;
constexpr char malformed_envelope_byte = static_cast<char>(0xff);
constexpr char oversized_envelope_fill_byte = static_cast<char>(0x01);

// ── Whole-envelope `epochIn` fixtures ────────────────────────────────────
constexpr std::string_view test_opp_inbound_address = "e7f1725E7734CE288F8367e1Bb143E90bb3F0512";
/// keccak256("epochIn(uint32,bytes)")[0..4).
constexpr std::string_view epoch_in_selector = "004e356a";
/// keccak256("epochIn(uint32,uint16,uint16,uint32,bytes)")[0..4) — the retired chunked form.
constexpr std::string_view retired_chunked_epoch_in_selector = "c3e558bc";
/// keccak256("epochIn(bytes)")[0..4) — the retired single-transaction form.
constexpr std::string_view retired_single_bytes_epoch_in_selector = "cfae3118";
constexpr size_t epoch_in_input_count = 2;
/// The digest consensus settled on, a digest that diverges from it, and the
/// zero word the outpost reports for a relay that never delivered.
constexpr std::string_view settled_digest_word =
   "1111111111111111111111111111111111111111111111111111111111111111";
constexpr std::string_view divergent_digest_word =
   "2222222222222222222222222222222222222222222222222222222222222222";
constexpr std::string_view zero_digest_word =
   "0000000000000000000000000000000000000000000000000000000000000000";
/// A mid-size envelope — large enough that a chunked relay would have split it.
constexpr size_t mid_envelope_bytes = 20'000;
/// `derive_buffered_gas_limit` applies a ×1.2 buffer, so a policy pinned at
/// EIP-7825's per-transaction cap rejects any estimate above 13 981 013.
constexpr uint64_t under_cap_gas_estimate = 13'000'000;
constexpr uint64_t under_cap_buffered_gas_limit = 15'600'000;
constexpr uint64_t over_cap_gas_estimate = 14'000'000;
/// A policy ceiling below EIP-7825's cap, to prove the floor follows the policy.
constexpr uint64_t below_cap_policy_gas_limit = 9'000'000;

/// JSON-RPC error code anvil/geth report for a reverted call.
constexpr int contract_revert_rpc_code = 3;
/// JSON-RPC parse error: the node never executed the call, so nothing on chain changed.
constexpr int json_rpc_parse_error_code = -32700;

/// A one-address custom error the client has no reading for — the same shape as
/// `AccessManagedUnauthorized(address)`, a different selector.
constexpr std::string_view unrelated_address_error_selector = "dca69e67";
/// Any address that is not this relay's signer, for the revert naming a different operator.
constexpr std::string_view test_other_operator_address = "0x00000000000000000000000000000000deadbeef";

/** ABI-encode a one-address custom error the way a node returns it in `error.data`. */
std::string encode_address_revert(std::string_view selector, std::string_view address_hex) {
   std::string_view address = address_hex;
   if (address.starts_with("0x") || address.starts_with("0X")) address.remove_prefix(2);
   return "0x" + std::string(selector) + std::string(64 - address.size(), '0') + std::string(address);
}

/** Build a one-shot JSON-RPC endpoint reporting Anvil's chain id (31337). */
fc::test::one_shot_http_server chain_id_rpc_server(std::string result_json = "\"0x7a69\"") {
   return fc::test::one_shot_http_server{
      R"json({"jsonrpc":"2.0","id":1,"result":)json" + result_json + "}",
      "eth_chainId"};
}

/** Loopback RPC fixture that resets one connection before returning a valid chain id. */
class transient_chain_id_rpc_server {
public:
   /** Start the two-attempt fixture on an ephemeral loopback port. */
   transient_chain_id_rpc_server()
      : _acceptor(_io, tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0))
      , _port(_acceptor.local_endpoint().port())
      , _worker([this] { serve(); }) {}

   transient_chain_id_rpc_server(const transient_chain_id_rpc_server&) = delete;
   transient_chain_id_rpc_server& operator=(const transient_chain_id_rpc_server&) = delete;

   /** Unblock any outstanding accepts and join the worker. */
   ~transient_chain_id_rpc_server() {
      for (size_t attempt = 0; attempt <= transient_chain_id_failures; ++attempt) {
         boost::system::error_code error;
         boost::asio::io_context io;
         tcp::socket socket(io);
         socket.connect(tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), _port), error);
         socket.close(error);
      }
      if (_worker.joinable()) _worker.join();
   }

   /** Return the loopback URL selected for the fixture. */
   std::string url() const {
      return "http://127.0.0.1:" + std::to_string(_port);
   }

private:
   using tcp = boost::asio::ip::tcp;

   /** Reset the first request, then serve a valid `eth_chainId` response. */
   void serve() {
      for (size_t attempt = 0; attempt < transient_chain_id_failures; ++attempt) {
         boost::system::error_code error;
         tcp::socket socket(_io);
         _acceptor.accept(socket, error);
         if (error) return;
         socket.set_option(boost::asio::socket_base::linger(true, 0), error);
         socket.close(error);
      }

      boost::system::error_code error;
      tcp::socket socket(_io);
      _acceptor.accept(socket, error);
      if (error) return;
      boost::beast::flat_buffer request_buffer;
      boost::beast::http::request<boost::beast::http::string_body> request;
      boost::beast::http::read(socket, request_buffer, request, error);
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

/// Encode the raw return bytes for the `dispatchSpill(uint32)` view — four
/// static outputs, so the words are simply concatenated with no offset table.
std::string encode_dispatch_spill_result(bool     tipped,
                                         uint16_t dispatched,
                                         bool     complete,
                                         bool     finalized) {
   return std::string(hex_prefix) + abi_word(tipped ? 1 : 0) + abi_word(dispatched) +
          abi_word(complete ? 1 : 0) + abi_word(finalized ? 1 : 0);
}

/// Encode the raw return bytes for a single-`bytes32` view.
std::string encode_word_result(std::string_view word) {
   BOOST_REQUIRE_EQUAL(word.size(), evm_abi_word_hex_chars);
   return std::string(hex_prefix) + std::string(word);
}

/// Encode the raw return bytes for the single-output `nextEpochIndex()` view.
std::string encode_next_epoch_index_result(uint32_t next_epoch_index) {
   return std::string(hex_prefix) + abi_word(next_epoch_index);
}

/// ABI-encode one address as the single word an `address` getter returns.
std::string encode_address_word(std::string_view address_hex) {
   std::string_view address = address_hex;
   if (address.starts_with("0x") || address.starts_with("0X")) address.remove_prefix(2);
   return std::string(hex_prefix) + std::string(evm_abi_word_hex_chars - address.size(), '0') +
          std::string(address);
}

/// One `epochIn` invocation as the stubbed typed wrapper observed it.
struct observed_delivery_call {
   uint32_t    epoch_index;
   std::string envelope_hex;
   uint64_t    gas_budget;
};

/// The head block the scripted chain reports when a tick starts; each
/// confirmed call lands one block later.
constexpr uint64_t test_head_block = 100;
/// Fee RPC answers for the scripted chain: a 10 wei tip over a 45 wei base
/// fee, so the derived `max_fee_per_gas` is 100 wei and the policy's
/// total-cost term never binds under the maximum policy.
constexpr std::string_view test_priority_fee_hex = "0xa";
constexpr std::string_view test_base_fee_hex     = "0x2d";
/// Event topics of the `OPPInbound` events a delivery receipt carries —
/// `keccak256` of each signature, written out so the tests pin the client's own
/// hashing rather than restate it.
constexpr std::string_view epoch_delivery_topic =
   "cafc00e7c35f462868d78e8228850e885bd0870978967c446c6bb67122a3fb28";
constexpr std::string_view epoch_consensus_topic =
   "a77ed82a4a8c2f7b412630e92b1c8f59e6645b05820a8b0ed43ae5fac990491c";
constexpr std::string_view epoch_dispatch_progressed_topic =
   "1aaaa8b219dd84dddbfc5836221ae6ca620f0ec0711a5fb907846becfacc9500";
constexpr std::string_view epoch_complete_topic =
   "e784b22f2061de501b77364cab02ee109528afe4ddc8d423d076f6b49021b58e";
/// Selectors of the `epochIn` refusals the relay classifies, written out the
/// same way (`keccak256(signature)[0..4]`).
constexpr std::string_view dispatch_underfunded_selector       = "e76ff350";
constexpr std::string_view handler_gas_exhausted_selector      = "a117d554";
constexpr std::string_view non_sequential_epoch_selector       = "fa7d07a0";
constexpr std::string_view operator_already_delivered_selector = "208216ac";
constexpr std::string_view not_active_operator_selector        = "abc01454";
constexpr std::string_view digest_mismatch_selector            = "40d6b4cd";
/// `keccak256` of the empty byte string, pinning `envelope_digest_word`.
constexpr std::string_view keccak256_of_empty =
   "c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470";
/// A group of three: a strict majority is two.
constexpr uint32_t test_group_size = 3;
constexpr uint32_t test_majority   = test_group_size / 2 + 1;
/// A policy ceiling under what a full-cap envelope needs, for the refusal to build on it.
constexpr uint64_t undeliverable_policy_gas_limit = 2'000'000;
/// One ether, for the total-cost ceiling.
constexpr uint64_t one_ether_wei = 1'000'000'000'000'000'000ULL;
constexpr uint64_t hundred_gwei  = 100'000'000'000ULL;

/// The chain-level RPCs the relay issues outside the typed wrappers
/// (`eth_blockNumber`, the fee pair), answered from a script so the real
/// client never dials its dead endpoint for them. Anything else still does,
/// and fails loudly.
struct scripted_rpc {
   fc::uint256 head_block{test_head_block};
   size_t      block_number_reads = 0;
};

class scripted_ethereum_client final : public ethereum_client {
public:
   scripted_ethereum_client(const fc::crypto::signature_provider_ptr& provider, ethereum_transaction_policy policy,
                            std::shared_ptr<scripted_rpc> script)
      : ethereum_client(provider, std::variant<std::string, fc::url>{std::string(latest_slot_test_rpc_url)},
                        std::move(policy))
      , _script(std::move(script)) {}

   fc::variant execute(const std::string& method, const fc::variant& params) override {
      if (const auto scripted = answer(method)) return *scripted;
      return ethereum_client::execute(method, params);
   }
   fc::variant execute_idempotent(const std::string& method, const fc::variant& params) override {
      if (const auto scripted = answer(method)) return *scripted;
      return ethereum_client::execute_idempotent(method, params);
   }

private:
   std::optional<fc::variant> answer(const std::string& method) {
      if (method == "eth_blockNumber") {
         ++_script->block_number_reads;
         return fc::variant(format_rpc_quantity(_script->head_block));
      }
      if (method == "eth_maxPriorityFeePerGas") return fc::variant(std::string(test_priority_fee_hex));
      if (method == "eth_getBlockByNumber") {
         return fc::variant(fc::mutable_variant_object("baseFeePerGas", std::string(test_base_fee_hex)));
      }
      return std::nullopt;
   }

   std::shared_ptr<scripted_rpc> _script;
};

/// Encode the raw return bytes for the `pendingConsensusForDigest(bytes32)`
/// view — five static outputs, concatenated.
std::string encode_pending_consensus_result(uint32_t next_epoch, uint32_t agreeing, uint32_t group_size,
                                            uint64_t epoch_started_at, uint32_t epoch_duration_sec) {
   return std::string(hex_prefix) + abi_word(next_epoch) + abi_word(agreeing) + abi_word(group_size) +
          abi_word(epoch_started_at) + abi_word(epoch_duration_sec);
}

/// One receipt log entry as a node returns it.
fc::variant receipt_log(std::string_view address, const std::vector<std::string>& topics, std::string data_hex) {
   fc::variants topic_variants;
   for (const auto& topic : topics) topic_variants.emplace_back(topic);
   return fc::mutable_variant_object("address", std::string(address))("topics", std::move(topic_variants))(
      "data", std::string(hex_prefix) + std::move(data_hex));
}

std::string prefixed(std::string_view word) { return std::string(hex_prefix) + std::string(word); }

fc::variant epoch_delivery_log(std::string_view address, uint32_t epoch, std::string_view operator_hex,
                               std::string_view digest_word) {
   return receipt_log(address, {prefixed(epoch_delivery_topic), prefixed(abi_word(epoch)),
                                encode_address_word(operator_hex)},
                      std::string(digest_word));
}
fc::variant epoch_consensus_log(std::string_view address, uint32_t epoch, std::string_view digest_word,
                                uint32_t count) {
   return receipt_log(address, {prefixed(epoch_consensus_topic), prefixed(abi_word(epoch))},
                      std::string(digest_word) + abi_word(count));
}
fc::variant epoch_dispatch_progressed_log(std::string_view address, uint32_t epoch, uint16_t dispatched) {
   return receipt_log(address, {prefixed(epoch_dispatch_progressed_topic), prefixed(abi_word(epoch))},
                      abi_word(dispatched));
}
fc::variant epoch_complete_log(std::string_view address, uint32_t epoch) {
   return receipt_log(address, {prefixed(epoch_complete_topic)}, abi_word(epoch));
}

/// A node's refusal of `epochIn` with one of the contract's custom errors: the
/// execution-reverted code and the selector followed by `words` zero words.
fc::network::json_rpc::json_rpc_error epoch_in_refusal(std::string_view selector, size_t words) {
   std::string data = prefixed(selector);
   for (size_t i = 0; i < words; ++i) data += abi_word(0);
   return fc::network::json_rpc::json_rpc_error(contract_revert_rpc_code, "execution reverted", fc::variant(data));
}

/// Harness binding a real `outpost_ethereum_client` to a stubbed OPPInbound
/// wrapper over a scripted chain.
///
/// `ethereum_client::get_contract` caches one typed wrapper per address, so a
/// wrapper materialized here is the SAME object the client resolves in its
/// constructor — replacing its `std::function` members intercepts every RPC at
/// the typed callable boundary, exactly as the `getLatestOutboundEnvelope`
/// coverage above does. The stubbed `epochIn` confirms in a fresh block each
/// time and answers with a receipt the case scripts; every view records the
/// block it was pinned to.
struct whole_envelope_delivery_fixture {
   ~whole_envelope_delivery_fixture() {
      outpost.reset();
      inbound.reset();
      tester.reset();
      appbase::application::reset_app_singleton();
   }

   std::unique_ptr<sig_provider_tester>                tester;
   std::shared_ptr<scripted_rpc>                       rpc;
   std::shared_ptr<sysio::opp_inbound_contract_client> inbound;
   std::unique_ptr<sysio::outpost_ethereum_client>     outpost;

   /// Every `epochIn` that confirmed.
   std::vector<observed_delivery_call> delivery_calls;
   /// The budget of every `epochIn` attempt, refused ones included.
   std::vector<uint64_t>               attempted_budgets;
   /// Per attempt, in order, a refusal the stub throws instead of confirming;
   /// attempts past the end confirm.
   std::vector<std::optional<fc::network::json_rpc::json_rpc_error>> refusals;
   size_t                              next_epoch_reads = 0;
   size_t                              spill_reads      = 0;
   size_t                              settlement_reads = 0;
   size_t                              consensus_reads  = 0;
   /// The block parameter of every stubbed view read, in order.
   std::vector<std::string>            read_blocks;
   /// The digest every `pendingConsensusForDigest` read asked about.
   std::vector<std::string>            consensus_digests;

   /// Responses the stubbed `nextEpochIndex` view returns, one per read; the
   /// last one repeats. The default is an outpost on the delivered epoch.
   std::vector<std::string> next_epoch_index_responses{encode_next_epoch_index_result(test_wire_epoch)};
   /// Responses the stubbed `dispatchSpill` view returns, one per read in
   /// order; the last one repeats. The default is the never-tipped cursor.
   std::vector<std::string> spill_responses{encode_dispatch_spill_result(false, 0, false, false)};
   /// Responses the stubbed `epochDeliveries(epoch, self)` view returns, one
   /// per read; the last repeats. The default: nothing recorded on the first
   /// read, a record on every read after it (the delivery landed).
   std::vector<std::string> own_delivery_responses{encode_word_result(zero_digest_word),
                                                   encode_word_result(settled_digest_word)};
   /// Response the stubbed `pendingEpochHash` view returns.
   std::string pending_hash_response = encode_word_result(settled_digest_word);
   /// Response the stubbed `pendingConsensusForDigest` view returns. The
   /// default: this relay alone has delivered, the boundary long past.
   std::string consensus_response = encode_pending_consensus_result(test_wire_epoch, 1, test_group_size, 0, 0);
   /// Receipt logs per confirmed call, in order; the last repeats. The default
   /// is the delivery record alone. `std::nullopt` entries use the default.
   std::vector<std::optional<fc::variants>> receipt_logs;
   /// Wall-clock the FIRST stubbed `epochIn` burns before returning, standing in
   /// for a slow chain. Applied only to the first call so a deadline set below
   /// it expires deterministically at the next pre-flight check.
   std::chrono::milliseconds first_call_delay{0};

   template <typename T>
   static const T& response_at(const std::vector<T>& responses, size_t n) {
      return responses[std::min(n, responses.size() - 1)];
   }

   /// Script the outpost as settled on `envelope`'s own digest with this relay
   /// as its deliverer — the shape every continuation case needs. The record
   /// appears from the `recorded_from_read`-th read on: 0 when the tick finds
   /// it already there (a resume), 1 when the tick's own delivery lands it.
   void settle_on(const std::vector<char>& envelope, size_t recorded_from_read = 0) {
      const auto digest = sysio::outpost_ethereum_client_detail::envelope_digest_word(envelope);
      own_delivery_responses.assign(recorded_from_read, encode_word_result(zero_digest_word));
      own_delivery_responses.push_back(encode_word_result(digest));
      pending_hash_response = encode_word_result(digest);
   }

   /// The distinct blocks the view reads were pinned to, adjacent repeats collapsed.
   std::vector<std::string> blocks_read() const {
      std::vector<std::string> blocks;
      for (const auto& block : read_blocks) {
         if (blocks.empty() || blocks.back() != block) blocks.push_back(block);
      }
      return blocks;
   }
};

/// Build an envelope of exactly `size` bytes whose content varies per index, so
/// a mis-sliced payload cannot accidentally compare equal to the right one.
std::vector<char> make_envelope(size_t size) {
   std::vector<char> envelope(size);
   for (size_t i = 0; i < size; ++i) {
      envelope[i] = static_cast<char>((i * 31 + 7) & 0xff);
   }
   return envelope;
}

/// Serialize a minimal protobuf envelope carrying only its epoch index.
std::vector<char> serialize_envelope(uint32_t epoch) {
   sysio::opp::Envelope envelope;
   envelope.set_epoch_index(epoch);
   const auto serialized = envelope.SerializeAsString();
   return {serialized.begin(), serialized.end()};
}

/// Serialize a protobuf envelope whose one message carries `attestations`
/// attestations of `payload_bytes` each — the shape the delivery budget is
/// sized from.
std::vector<char> serialize_envelope_with_attestations(uint32_t epoch, uint32_t attestations,
                                                       size_t payload_bytes) {
   sysio::opp::Envelope envelope;
   envelope.set_epoch_index(epoch);
   auto* payload = envelope.add_messages()->mutable_payload();
   for (uint32_t i = 0; i < attestations; ++i) {
      auto* entry = payload->add_attestations();
      entry->set_data(std::string(payload_bytes, static_cast<char>('a' + (i % 26))));
      entry->set_data_size(static_cast<uint32_t>(payload_bytes));
   }
   const auto serialized = envelope.SerializeAsString();
   return {serialized.begin(), serialized.end()};
}

/// The app, signer, chain connection and client entry every relay fixture
/// stands on. The connection points at a port nothing listens on, so a wrapper
/// a case forgot to stub fails loudly instead of dialing anything; the few
/// chain-level RPCs the relay issues itself are answered by `rpc`.
struct relay_test_stack {
   std::unique_ptr<sig_provider_tester>             tester;
   fc::crypto::signature_provider_ptr               sig_provider;
   std::shared_ptr<scripted_rpc>                    rpc;
   ethereum_client_ptr                              eth_client;
   std::shared_ptr<sysio::ethereum_client_entry_t>  entry;
};

/// The maximum policy, with `max_gas_limit` overridden when a case needs a ceiling.
ethereum_transaction_policy relay_test_policy(fc::uint256 max_gas_limit = maximum_ethereum_transaction_policy_value()) {
   return ethereum_transaction_policy{
      .client_id = std::string(latest_slot_test_entry_id),
      .chain_id = test_evm_chain_id,
      .max_priority_fee_per_gas = maximum_ethereum_transaction_policy_value(),
      .max_fee_per_gas = maximum_ethereum_transaction_policy_value(),
      .max_gas_limit = max_gas_limit,
      .max_total_native_cost = maximum_ethereum_transaction_policy_value(),
   };
}

relay_test_stack create_relay_test_stack(ethereum_transaction_policy transaction_policy = relay_test_policy()) {
   relay_test_stack stack;
   stack.tester = create_app();

   stack.sig_provider = stack.tester->plugin().create_provider(
      std::string(latest_slot_test_entry_id),
      chain_kind_ethereum,
      chain_key_type_ethereum,
      std::string(latest_slot_test_public_key),
      to_private_key_spec(std::string(latest_slot_test_private_key)));

   stack.rpc        = std::make_shared<scripted_rpc>();
   stack.eth_client = std::make_shared<scripted_ethereum_client>(stack.sig_provider, std::move(transaction_policy),
                                                                 stack.rpc);

   stack.entry = std::make_shared<sysio::ethereum_client_entry_t>();
   stack.entry->id = latest_slot_test_entry_id;
   stack.entry->signature_provider = stack.sig_provider;
   stack.entry->client = stack.eth_client;
   stack.entry->chain_id = test_evm_chain_id;
   return stack;
}

/// Stand up a `whole_envelope_delivery_fixture`: a real `outpost_ethereum_client`
/// whose OPPInbound wrapper has every typed callable replaced by a recording
/// stub. The caller owns the returned fixture; the stubs capture it by
/// reference, so it must not be moved after this returns.
std::unique_ptr<whole_envelope_delivery_fixture> create_whole_envelope_delivery_fixture() {
   auto fixture = std::make_unique<whole_envelope_delivery_fixture>();
   auto stack = create_relay_test_stack();
   fixture->tester = std::move(stack.tester);
   fixture->rpc    = stack.rpc;
   auto eth_client = stack.eth_client;

   auto              abis = load_abi_fixture(opp_inbound_abi_fixture);
   const std::string inbound_address{test_opp_inbound_address};
   fixture->inbound =
      eth_client->get_contract<sysio::opp_inbound_contract_client>(inbound_address, abis);
   BOOST_REQUIRE(fixture->inbound);

   auto* raw = fixture.get();
   raw->inbound->epoch_in = [raw, inbound_address](uint32_t& epoch_index, std::string& envelope_hex,
                                                   uint64_t gas_budget) -> sysio::epoch_in_receipt {
      const size_t attempt = raw->attempted_budgets.size();
      raw->attempted_budgets.push_back(gas_budget);
      if (attempt < raw->refusals.size() && raw->refusals[attempt]) throw *raw->refusals[attempt];

      raw->delivery_calls.push_back(observed_delivery_call{epoch_index, envelope_hex, gas_budget});
      if (raw->delivery_calls.size() == 1 && raw->first_call_delay.count() > 0) {
         std::this_thread::sleep_for(raw->first_call_delay);
      }
      const size_t call = raw->delivery_calls.size();
      sysio::epoch_in_receipt receipt;
      receipt.tx_hash      = std::string(hex_prefix) + abi_word(call);
      receipt.block_number = fc::uint256{test_head_block + call};
      const auto scripted  = raw->receipt_logs.empty()
                                ? std::nullopt
                                : whole_envelope_delivery_fixture::response_at(raw->receipt_logs, call - 1);
      receipt.logs = scripted ? *scripted
                              : fc::variants{epoch_delivery_log(inbound_address, epoch_index,
                                                                raw->outpost->signer_address_hex(),
                                                                settled_digest_word)};
      return receipt;
   };
   // Every bookkeeping read is pinned to a block number — the head the tick
   // started at, or the block the last call confirmed in — never a tag.
   const auto note_pinned = [raw](const block_number_or_tag_t& block) {
      BOOST_REQUIRE(std::holds_alternative<std::string>(block));
      raw->read_blocks.push_back(std::get<std::string>(block));
   };
   raw->inbound->next_epoch_index = [raw, note_pinned](const block_number_or_tag_t& block) -> fc::variant {
      note_pinned(block);
      return fc::variant(
         whole_envelope_delivery_fixture::response_at(raw->next_epoch_index_responses, raw->next_epoch_reads++));
   };
   raw->inbound->dispatch_spill =
      [raw, note_pinned](const block_number_or_tag_t& block, uint32_t& epoch_index) -> fc::variant {
         note_pinned(block);
         BOOST_CHECK_EQUAL(epoch_index, test_wire_epoch);
         return fc::variant(whole_envelope_delivery_fixture::response_at(raw->spill_responses, raw->spill_reads++));
      };
   raw->inbound->epoch_deliveries =
      [raw, note_pinned](const block_number_or_tag_t& block, uint32_t& epoch_index,
                         std::string& operator_address) -> fc::variant {
         note_pinned(block);
         BOOST_CHECK_EQUAL(epoch_index, test_wire_epoch);
         BOOST_CHECK_EQUAL(operator_address, raw->outpost->signer_address_hex());
         return fc::variant(
            whole_envelope_delivery_fixture::response_at(raw->own_delivery_responses, raw->settlement_reads++));
      };
   raw->inbound->pending_epoch_hash = [raw, note_pinned](const block_number_or_tag_t& block) -> fc::variant {
      note_pinned(block);
      return fc::variant(raw->pending_hash_response);
   };
   raw->inbound->pending_consensus_for_digest =
      [raw, note_pinned](const block_number_or_tag_t& block, std::string& digest) -> fc::variant {
         note_pinned(block);
         ++raw->consensus_reads;
         raw->consensus_digests.push_back(digest);
         return fc::variant(raw->consensus_response);
      };

   fixture->outpost = std::make_unique<sysio::outpost_ethereum_client>(
      stack.entry,
      /*opp_addr=*/std::string{},
      inbound_address,
      abis,
      test_outpost_chain_code,
      test_evm_chain_id);
   return fixture;
}

/// Assert that every observed `epochIn` carried `envelope` whole, addressed to
/// `epoch_index`.
void check_delivery_calls(const std::vector<observed_delivery_call>& calls,
                          const std::vector<char>&                   envelope,
                          uint32_t                                   epoch_index,
                          size_t                                     expected_calls) {
   BOOST_REQUIRE_EQUAL(calls.size(), expected_calls);
   const auto expected_hex = fc::to_hex(envelope.data(), static_cast<uint32_t>(envelope.size()));
   for (const auto& call : calls) {
      BOOST_CHECK_EQUAL(call.epoch_index, epoch_index);
      BOOST_CHECK_EQUAL(call.envelope_hex, expected_hex);
   }
}

/// The block parameter a read pinned to `block` carries.
std::string pinned_block_hex(uint64_t block) { return format_rpc_quantity(fc::uint256{block}); }

// ── `crank_outpost` fixtures ─────────────────────────────────────────────
constexpr std::string_view syndication_pool_abi_fixture = "ethereum-abi-syndication-pool.json";
constexpr std::string_view zero_evm_address = "0x0000000000000000000000000000000000000000";
/// `ATTESTATION_BLACKHOLE` in wire-ethereum's `OPPCommon.sol`.
constexpr std::string_view attestation_blackhole_address = "0x000000000000000000000000000000000000dead";
constexpr std::string_view test_syndication_pool_address = "0xCf7Ed3AccA5a467e9e704C703E8D87F634fB0Fc9";
constexpr std::string_view test_moved_syndication_pool_address = "0xDc64a140Aa3E981100a9becA4E685f962f0cF6C9";
/// `keccak256("WIRE_NoYield()")[0..4]` and the pool's other two refusals, written out so these
/// tests pin the client's own hashing of the signatures rather than restating it.
constexpr std::string_view no_yield_selector             = "053716d1";
constexpr std::string_view yield_below_deadband_selector = "45441430";
constexpr std::string_view pool_underbacked_selector     = "358cc7e9";
/// `keccak256("EnforcedPause()")[0..4]`: OpenZeppelin `Pausable`'s refusal, which
/// `realizeYield()`'s `whenNotPaused` raises while the pool's panic role has frozen it.
constexpr std::string_view enforced_pause_selector       = "d93c0665";
/// `AccessManagedUnauthorized(address)`: what a signer without the `yield_operator` role gets.
constexpr std::string_view access_managed_unauthorized_selector = "068ca9d8";
constexpr uint64_t test_yield_delta    = 5;
constexpr uint64_t test_yield_deadband = 10;

/// ABI-encode a two-`uint64` custom error the way a node returns it in `error.data`.
std::string encode_two_word_revert(std::string_view selector, uint64_t first, uint64_t second) {
   return std::string(hex_prefix) + std::string(selector) + abi_word(first) + abi_word(second);
}

/// A `crank_outpost` stand: a real `outpost_ethereum_client` over the OPPInbound ABI (plus the
/// pool's, unless the case leaves it out), its `attestationHandlers` view stubbed, and a pool
/// wrapper bound through `bind_stub_pool` whose `realizeYield` records the call or throws what
/// the case scripts.
struct crank_fixture {
   ~crank_fixture() {
      pool.reset();
      outpost.reset();
      inbound.reset();
      tester.reset();
      appbase::application::reset_app_singleton();
   }

   std::unique_ptr<sig_provider_tester>                     tester;
   ethereum_client_ptr                                      eth_client;
   std::vector<fc::network::ethereum::abi::contract>        abis;
   std::shared_ptr<sysio::opp_inbound_contract_client>      inbound;
   std::shared_ptr<sysio::syndication_pool_contract_client> pool;
   std::unique_ptr<sysio::outpost_ethereum_client>          outpost;

   /// Every attestation type the stubbed `attestationHandlers` view was asked for.
   std::vector<uint16_t> handler_reads;
   /// Response the stubbed view returns; the default is `address(0)`, nothing registered.
   std::string handler_response = encode_address_word(zero_evm_address);
   size_t      realize_calls = 0;
   /// When set, the stubbed `realizeYield` write throws it.
   std::optional<fc::network::json_rpc::json_rpc_error> realize_failure;

   /// Bind a pool wrapper at `address` whose `realizeYield` is this fixture's recording stub.
   void bind_stub_pool(std::string_view address) {
      const std::string pool_address{address};
      pool = eth_client->get_contract<sysio::syndication_pool_contract_client>(pool_address, abis);
      BOOST_REQUIRE(pool);
      pool->realize_yield = [this]() -> fc::variant {
         ++realize_calls;
         if (realize_failure) throw *realize_failure;
         return fc::variant(std::string(hex_prefix) + abi_word(realize_calls));
      };
      outpost->bind_syndication_pool(pool_address, pool);
   }
};

std::unique_ptr<crank_fixture> create_crank_fixture(bool with_pool_abi = true) {
   auto fixture = std::make_unique<crank_fixture>();
   auto stack = create_relay_test_stack();
   fixture->tester     = std::move(stack.tester);
   fixture->eth_client = stack.eth_client;

   fixture->abis = load_abi_fixture(opp_inbound_abi_fixture);
   if (with_pool_abi) {
      const auto pool_abis = load_abi_fixture(syndication_pool_abi_fixture);
      fixture->abis.insert(fixture->abis.end(), pool_abis.begin(), pool_abis.end());
   }
   const std::string inbound_address{test_opp_inbound_address};
   fixture->inbound =
      fixture->eth_client->get_contract<sysio::opp_inbound_contract_client>(inbound_address, fixture->abis);
   BOOST_REQUIRE(fixture->inbound);

   auto* raw = fixture.get();
   raw->inbound->attestation_handlers =
      [raw](const block_number_or_tag_t& block, uint16_t& attestation_type) -> fc::variant {
         // The routing table is configuration, not delivered content: read at `latest`.
         BOOST_CHECK(std::holds_alternative<block_tag_t>(block));
         BOOST_CHECK(std::get<block_tag_t>(block) == block_tag_t::latest);
         raw->handler_reads.push_back(attestation_type);
         return fc::variant(raw->handler_response);
      };

   fixture->outpost = std::make_unique<sysio::outpost_ethereum_client>(
      stack.entry,
      /*opp_addr=*/std::string{},
      inbound_address,
      fixture->abis,
      test_outpost_chain_code,
      test_evm_chain_id);
   return fixture;
}

} // anonymous namespace

BOOST_AUTO_TEST_SUITE(outpost_ethereum_client_plugin)

BOOST_AUTO_TEST_CASE(authenticated_transport_options_are_registered) {
   sysio::outpost_ethereum_client_plugin plugin;
   boost::program_options::options_description cli, cfg;
   plugin.set_program_options(cli, cfg);

   std::set<std::string> option_names;
   for (const auto& option : cfg.options())
      option_names.insert(option->long_name());

   BOOST_CHECK(option_names.contains("outpost-ethereum-additional-ca-file"));
   BOOST_CHECK(option_names.contains("outpost-ethereum-additional-ca-path"));
   BOOST_CHECK(option_names.contains("outpost-ethereum-proxy"));
   BOOST_CHECK(option_names.contains("outpost-ethereum-client-config-file"));
}

// ---------------------------------------------------------------------------
//  Startup configuration validation
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(startup_accepts_explicit_locally_authoritative_chain_id) {
   auto rpc_server = chain_id_rpc_server();
   BOOST_CHECK_NO_THROW(initialize_outpost_plugin({
      "--signature-provider",
      named_ethereum_signature_provider(),
      "--outpost-ethereum-client",
      "client-a,signer-a," + rpc_server.url() + ",31337",
   }));
}

BOOST_AUTO_TEST_CASE(startup_resolves_three_field_client_chain_id_from_rpc) {
   auto rpc_server = chain_id_rpc_server();
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

BOOST_AUTO_TEST_CASE(startup_rejects_explicit_chain_id_mismatch) {
   auto rpc_server = chain_id_rpc_server();
   BOOST_CHECK_THROW(initialize_outpost_plugin({
      "--signature-provider",
      named_ethereum_signature_provider(),
      "--outpost-ethereum-client",
      "client-a,signer-a," + rpc_server.url() + ",1",
   }), sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(startup_rejects_unavailable_rpc_after_bounded_grace) {
   fc::test::connection_closing_http_server rpc_server;
   const auto safe_endpoint = rpc_server.url();
   const auto sensitive_url =
      "http://operator:super-secret@" + safe_endpoint.substr(http_scheme_prefix.size()) +
      "/rpc?token=secret";
   try {
      initialize_outpost_plugin({
         "--signature-provider",
         named_ethereum_signature_provider(),
         "--outpost-ethereum-client",
         "client-a,signer-a," + sensitive_url + ",31337",
      });
      BOOST_FAIL("expected unavailable RPC rejection");
   } catch (const sysio::chain::plugin_config_exception& rejection) {
      const auto detail = rejection.to_detail_string();
      BOOST_CHECK(detail.find("client-a") != std::string::npos);
      BOOST_CHECK(detail.find("endpoint=" + safe_endpoint) != std::string::npos);
      const auto io_failure =
         std::string(last_failure_detail_prefix) +
         std::string(fc::http::failure_kind_name(fc::http::failure_kind::io));
      const auto connect_failure =
         std::string(last_failure_detail_prefix) +
         std::string(fc::http::failure_kind_name(fc::http::failure_kind::connect));
      BOOST_CHECK(detail.find(io_failure) != std::string::npos ||
                  detail.find(connect_failure) != std::string::npos);
      BOOST_CHECK(detail.find("super-secret") == std::string::npos);
      BOOST_CHECK(detail.find("token=secret") == std::string::npos);
   }
}

BOOST_AUTO_TEST_CASE(startup_retries_transient_chain_id_transport_failure) {
   transient_chain_id_rpc_server rpc_server;
   BOOST_CHECK_NO_THROW(initialize_outpost_plugin({
      "--signature-provider",
      named_ethereum_signature_provider(),
      "--outpost-ethereum-client",
      "client-a,signer-a," + rpc_server.url() + ",31337",
   }));
}

BOOST_AUTO_TEST_CASE(startup_rejects_invalid_remote_chain_id) {
   auto rpc_server = chain_id_rpc_server("\"not-a-chain-id\"");
   BOOST_CHECK_THROW(initialize_outpost_plugin({
      "--signature-provider",
      named_ethereum_signature_provider(),
      "--outpost-ethereum-client",
      "client-a,signer-a," + rpc_server.url(),
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
      "client-a,signer-a," + rpc_server.url(),
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

   // Every ABI entry the typed `opp_inbound_contract_client` binds at
   // construction: the whole-envelope write and the five views the
   // deliver-or-continue decision reads. The staged-chunk surface is gone.
   bool has_epoch_in = false, has_next_epoch = false, has_spill = false;
   bool has_deliveries = false, has_pending_hash = false, has_pending_consensus = false;
   bool has_discard = false, has_chunk_state = false;
   for (auto& c : abis) {
      if (c.name == "epochIn") has_epoch_in = true;
      if (c.name == "nextEpochIndex") has_next_epoch = true;
      if (c.name == "dispatchSpill") has_spill = true;
      if (c.name == "epochDeliveries") has_deliveries = true;
      if (c.name == "pendingEpochHash") has_pending_hash = true;
      if (c.name == "pendingConsensusForDigest") has_pending_consensus = true;
      if (c.name == "discardEnvelopeChunks") has_discard = true;
      if (c.name == "envelopeChunkState") has_chunk_state = true;
   }
   BOOST_CHECK(has_epoch_in);
   BOOST_CHECK(has_next_epoch);
   BOOST_CHECK(has_spill);
   BOOST_CHECK(has_deliveries);
   BOOST_CHECK(has_pending_hash);
   BOOST_CHECK(has_pending_consensus);
   BOOST_CHECK(!has_discard);
   BOOST_CHECK(!has_chunk_state);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(epoch_in_abi_encoding_whole_envelope) try {
   auto abis = load_abi_fixture(opp_inbound_abi_fixture);

   // Find the epochIn ABI entry
   const eth::abi::contract* epoch_in_abi = nullptr;
   for (auto& c : abis) {
      if (c.name == "epochIn") { epoch_in_abi = &c; break; }
   }
   BOOST_REQUIRE(epoch_in_abi != nullptr);

   // Whole-envelope delivery: (uint32 epochIndex, bytes envelopeData).
   BOOST_REQUIRE_EQUAL(epoch_in_abi->inputs.size(), epoch_in_input_count);
   BOOST_CHECK(epoch_in_abi->inputs[0].type == eth::abi::data_type::uint32);
   BOOST_CHECK(epoch_in_abi->inputs[1].type == eth::abi::data_type::bytes);

   // Encode the two params — this is what the batch operator does.
   std::string test_envelope_hex = "120c0a040800100012040800100028deeef5ce06300138";
   auto encoded = contract_encode_data(
      *epoch_in_abi,
      std::vector<fc::variant>{fc::variant(uint64_t{test_wire_epoch}), fc::variant(test_envelope_hex)});
   BOOST_CHECK(!encoded.empty());

   // keccak256("epochIn(uint32,bytes)")[0..4) — neither retired form's
   // selector survives.
   BOOST_CHECK_EQUAL(encoded.substr(0, evm_function_selector_hex_chars),
                     std::string(epoch_in_selector));
   BOOST_CHECK(encoded.substr(0, evm_function_selector_hex_chars) !=
               std::string(retired_chunked_epoch_in_selector));
   BOOST_CHECK(encoded.substr(0, evm_function_selector_hex_chars) !=
               std::string(retired_single_bytes_epoch_in_selector));

   // Verify that encoding with 0 params still throws
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
   ethereum_transaction_policy transaction_policy{
      .client_id = std::string(latest_slot_test_entry_id),
      .chain_id = test_evm_chain_id,
      .max_priority_fee_per_gas = maximum_ethereum_transaction_policy_value(),
      .max_fee_per_gas = maximum_ethereum_transaction_policy_value(),
      .max_gas_limit = maximum_ethereum_transaction_policy_value(),
      .max_total_native_cost = maximum_ethereum_transaction_policy_value(),
   };
   auto eth_client = std::make_shared<ethereum_client>(
      sig_provider,
      std::variant<std::string, fc::url>{rpc_url},
      std::move(transaction_policy));
   auto abis = load_abi_fixture(opp_abi_fixture);
   const std::string opp_address{test_opp_address};
   auto typed_opp = eth_client->get_contract<sysio::opp_contract_client>(opp_address, abis);

   auto entry = std::make_shared<sysio::ethereum_client_entry_t>();
   entry->id = latest_slot_test_entry_id;
   entry->signature_provider = sig_provider;
   entry->client = eth_client;
   entry->chain_id = test_evm_chain_id;

   sysio::outpost_ethereum_client outpost(
      entry,
      opp_address,
      "",
      abis,
      test_outpost_chain_code,
      test_evm_chain_id);

   const auto expected_caller = eth_client->get_signer_address();
   const auto actual_caller = outpost.authenticated_caller_address();
   BOOST_CHECK_EQUAL_COLLECTIONS(
      expected_caller.begin(), expected_caller.end(),
      actual_caller.begin(), actual_caller.end());

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
//  Whole-envelope WIRE -> Ethereum delivery
// ---------------------------------------------------------------------------

/// The most an `epochIn` may be funded with follows the client's policy
/// ceiling and never exceeds EIP-7825's cap; the options a call is sent with
/// pin floor and cap to the one budget, so the pre-flight runs under it.
BOOST_AUTO_TEST_CASE(delivery_gas_ceiling_is_the_policy_ceiling_bounded_by_the_cap) try {
   auto policy = relay_test_policy();
   BOOST_CHECK_EQUAL(sysio::delivery_gas_ceiling(policy), sysio::EIP_7825_TX_GAS_CAP);

   policy.max_gas_limit = fc::uint256{sysio::EIP_7825_TX_GAS_CAP};
   BOOST_CHECK_EQUAL(sysio::delivery_gas_ceiling(policy), sysio::EIP_7825_TX_GAS_CAP);

   policy.max_gas_limit = fc::uint256{below_cap_policy_gas_limit};
   BOOST_CHECK_EQUAL(sysio::delivery_gas_ceiling(policy), below_cap_policy_gas_limit);

   const auto options = sysio::delivery_confirm_options(below_cap_policy_gas_limit);
   BOOST_CHECK_EQUAL(options.gas_limit_floor, below_cap_policy_gas_limit);
   BOOST_CHECK_EQUAL(options.gas_limit_cap, below_cap_policy_gas_limit);
} FC_LOG_AND_RETHROW();

/// At a fee, the ceiling is also what the policy's total-cost term pays for:
/// one ether at a hundred gwei is ten million gas, under the cap; a zero fee
/// or an ample budget leaves the static ceiling in force.
BOOST_AUTO_TEST_CASE(delivery_gas_ceiling_is_bounded_by_the_total_cost_at_the_fee) try {
   auto policy = relay_test_policy();
   policy.max_total_native_cost = fc::uint256{one_ether_wei};
   constexpr uint64_t affordable_at_hundred_gwei = one_ether_wei / hundred_gwei;
   static_assert(affordable_at_hundred_gwei < sysio::EIP_7825_TX_GAS_CAP);

   BOOST_CHECK_EQUAL(sysio::delivery_gas_ceiling(policy, fc::uint256{hundred_gwei}), affordable_at_hundred_gwei);
   BOOST_CHECK_EQUAL(sysio::delivery_gas_ceiling(policy, fc::uint256{0}), sysio::EIP_7825_TX_GAS_CAP);
   BOOST_CHECK_EQUAL(sysio::delivery_gas_ceiling(policy, fc::uint256{1}), sysio::EIP_7825_TX_GAS_CAP);
   // The static ceiling still binds when it is the lower of the two.
   policy.max_gas_limit = fc::uint256{below_cap_policy_gas_limit};
   BOOST_CHECK_EQUAL(sysio::delivery_gas_ceiling(policy, fc::uint256{1}), below_cap_policy_gas_limit);
   BOOST_CHECK_EQUAL(sysio::delivery_gas_ceiling(policy, fc::uint256{hundred_gwei}),
                     std::min(affordable_at_hundred_gwei, below_cap_policy_gas_limit));
} FC_LOG_AND_RETHROW();

/// A delivery is funded to what it has left to carry — fixed cost, bytes, an
/// allowance per attestation still to dispatch, the emit — doubled per spill
/// already met this tick and never past the ceiling; an envelope the relay
/// cannot read is funded to the ceiling outright.
BOOST_AUTO_TEST_CASE(delivery_gas_budget_follows_the_remaining_work_and_escalates_to_the_ceiling) try {
   constexpr uint64_t bytes   = 1'000;
   constexpr uint64_t ceiling = sysio::EIP_7825_TX_GAS_CAP;
   const auto budget = [&](uint32_t remaining, uint32_t escalations) {
      return sysio::delivery_gas_budget(sysio::delivery_gas_request{
         .envelope_bytes = bytes, .remaining_attestations = remaining, .escalations = escalations, .ceiling = ceiling});
   };
   const uint64_t base = sysio::DELIVERY_FIXED_GAS + bytes * sysio::DELIVERY_GAS_PER_BYTE + sysio::EMIT_GAS_ALLOWANCE;

   BOOST_CHECK_EQUAL(budget(0, 0), base);
   BOOST_CHECK_EQUAL(budget(1, 0), base + sysio::ATTESTATION_GAS_ALLOWANCE);
   BOOST_CHECK_EQUAL(budget(3, 0), base + 3 * sysio::ATTESTATION_GAS_ALLOWANCE);
   // Each spill doubles, and the ceiling is where doubling stops.
   BOOST_CHECK_EQUAL(budget(1, 1), 2 * (base + sysio::ATTESTATION_GAS_ALLOWANCE));
   BOOST_CHECK_EQUAL(budget(1, 2), ceiling);
   BOOST_CHECK_EQUAL(budget(1, 40), ceiling);
   // A full envelope's worth of attestations is above the ceiling on its own.
   BOOST_CHECK_EQUAL(budget(70, 0), ceiling);
   // Unreadable: the ceiling, whatever else is known.
   BOOST_CHECK_EQUAL(sysio::delivery_gas_budget(sysio::delivery_gas_request{
                        .envelope_bytes = bytes, .remaining_attestations = std::nullopt, .escalations = 0, .ceiling = ceiling}),
                     ceiling);
   // Counting is the relay's own read of the envelope.
   BOOST_CHECK_EQUAL(*sysio::outpost_ethereum_client_detail::count_envelope_attestations(
                        serialize_envelope_with_attestations(test_wire_epoch, 3, 16)),
                     3u);
   BOOST_CHECK(!sysio::outpost_ethereum_client_detail::count_envelope_attestations(make_envelope(mid_envelope_bytes)));
   // The least ceiling the relay accepts is one full-cap delivery's worth.
   BOOST_CHECK_EQUAL(sysio::DELIVERY_MINIMUM_GAS_CEILING,
                     sysio::DELIVERY_FIXED_GAS + sysio::OPP_MAX_ENVELOPE_BYTES * sysio::DELIVERY_GAS_PER_BYTE +
                        sysio::ATTESTATION_GAS_ALLOWANCE + sysio::EMIT_GAS_ALLOWANCE);
   BOOST_CHECK(sysio::DELIVERY_MINIMUM_GAS_CEILING < sysio::EIP_7825_TX_GAS_CAP);
} FC_LOG_AND_RETHROW();

/// A relay that delivers is refused a policy whose ceiling cannot complete a
/// full-cap envelope; one that only reads the outpost (no OPPInbound address)
/// is not, since it never funds a delivery.
BOOST_AUTO_TEST_CASE(relay_refuses_a_policy_ceiling_below_one_full_delivery) try {
   static_assert(undeliverable_policy_gas_limit < sysio::DELIVERY_MINIMUM_GAS_CEILING);
   auto stack = create_relay_test_stack(relay_test_policy(fc::uint256{undeliverable_policy_gas_limit}));
   auto abis  = load_abi_fixture(opp_inbound_abi_fixture);
   const auto opp_abis = load_abi_fixture(opp_abi_fixture);
   abis.insert(abis.end(), opp_abis.begin(), opp_abis.end());

   BOOST_CHECK_THROW(sysio::outpost_ethereum_client(stack.entry, std::string{}, std::string(test_opp_inbound_address),
                                                    abis, test_outpost_chain_code, test_evm_chain_id),
                     sysio::chain::plugin_config_exception);
   BOOST_CHECK_NO_THROW(sysio::outpost_ethereum_client(stack.entry, std::string(test_opp_address), std::string{},
                                                       abis, test_outpost_chain_code, test_evm_chain_id));

   stack.tester.reset();
   appbase::application::reset_app_singleton();
} FC_LOG_AND_RETHROW();

/// Across a tipped-and-spilled epoch the budget tracks the outpost's cursor:
/// everything before the tip, what remains after it, only the emit once
/// dispatch is complete — each continuation doubled for the spill before it.
/// Every read is pinned: the head when the tick starts, then the block each
/// confirmed call landed in.
BOOST_AUTO_TEST_CASE(delivery_funds_each_call_to_its_remaining_work) try {
   auto fixture  = create_whole_envelope_delivery_fixture();
   auto envelope = serialize_envelope_with_attestations(test_wire_epoch, 2, 64);
   fixture->spill_responses = {
      encode_dispatch_spill_result(false, 0, false, false),   // before the delivery
      encode_dispatch_spill_result(true, 1, false, false),    // tipped, spilled after 1
      encode_dispatch_spill_result(true, 2, true, false),     // dispatched, emit outstanding
      encode_dispatch_spill_result(true, 2, true, true),      // finalized
   };
   fixture->settle_on(envelope, /*recorded_from_read=*/1);

   const auto tx = fixture->outpost->deliver_outbound_envelope(
      test_wire_epoch, envelope, fc::seconds(test_rpc_deadline_seconds));
   BOOST_CHECK(!tx.empty());
   check_delivery_calls(fixture->delivery_calls, envelope, test_wire_epoch, 3);

   const auto expected = [&](uint32_t remaining, uint32_t escalations) {
      return sysio::delivery_gas_budget(sysio::delivery_gas_request{
         .envelope_bytes         = envelope.size(),
         .remaining_attestations = remaining,
         .escalations            = escalations,
         .ceiling                = sysio::EIP_7825_TX_GAS_CAP});
   };
   BOOST_CHECK_EQUAL(fixture->delivery_calls[0].gas_budget, expected(2, 0));
   BOOST_CHECK_EQUAL(fixture->delivery_calls[1].gas_budget, expected(1, 1));
   BOOST_CHECK_EQUAL(fixture->delivery_calls[2].gas_budget, expected(0, 2));
   BOOST_CHECK(fixture->delivery_calls[0].gas_budget < fixture->delivery_calls[1].gas_budget);
   BOOST_CHECK(fixture->delivery_calls[0].gas_budget < sysio::EIP_7825_TX_GAS_CAP);

   // One head read, then a read at each of the three receipt blocks.
   BOOST_CHECK_EQUAL(fixture->rpc->block_number_reads, 1u);
   const std::vector<std::string> expected_blocks{pinned_block_hex(test_head_block), pinned_block_hex(test_head_block + 1),
                                                  pinned_block_hex(test_head_block + 2), pinned_block_hex(test_head_block + 3)};
   const auto blocks = fixture->blocks_read();
   BOOST_CHECK_EQUAL_COLLECTIONS(blocks.begin(), blocks.end(), expected_blocks.begin(), expected_blocks.end());
} FC_LOG_AND_RETHROW();

/// The deliver-or-continue decision table, exercised without an EVM node.
BOOST_AUTO_TEST_CASE(delivery_decision_table) try {
   namespace detail = sysio::outpost_ethereum_client_detail;
   using action     = detail::delivery_action;
   using settlement = detail::delivery_settlement;

   const detail::dispatch_spill untipped{};
   const detail::dispatch_spill mid_flight{.tipped = true, .dispatched = 3, .complete = false, .finalized = false};
   const detail::dispatch_spill emit_pending{.tipped = true, .dispatched = 9, .complete = true, .finalized = false};
   const detail::dispatch_spill finalized{.tipped = true, .dispatched = 9, .complete = true, .finalized = true};
   const auto decide = [](uint32_t next, const detail::dispatch_spill& spill, settlement s, bool majority) {
      return detail::decide_delivery(next, test_wire_epoch, spill, s, majority);
   };

   // Not tipped, nothing recorded: deliver.
   BOOST_CHECK(decide(test_wire_epoch, untipped, settlement::never_delivered, false) == action::deliver);
   BOOST_CHECK(decide(test_wire_epoch, untipped, settlement::never_delivered, true) == action::deliver);
   // Not tipped, recorded: re-deliver only when the outpost's own majority view says it would tip.
   BOOST_CHECK(decide(test_wire_epoch, untipped, settlement::recorded, true) == action::retry_consensus);
   BOOST_CHECK(decide(test_wire_epoch, untipped, settlement::recorded, false) == action::await_peers);

   // Tipped and unfinished: only the settled digest's deliverer continues.
   BOOST_CHECK(decide(test_wire_epoch, mid_flight, settlement::settled, false) == action::continue_dispatch);
   BOOST_CHECK(decide(test_wire_epoch, emit_pending, settlement::settled, false) == action::continue_dispatch);
   BOOST_CHECK(decide(test_wire_epoch, mid_flight, settlement::never_delivered, false) == action::wait_for_deliverer);
   BOOST_CHECK(decide(test_wire_epoch, mid_flight, settlement::divergent, false) == action::wait_for_deliverer);
   BOOST_CHECK(decide(test_wire_epoch, emit_pending, settlement::divergent, true) == action::wait_for_deliverer);

   // Closed, by either signal: nothing to send.
   BOOST_CHECK(decide(test_wire_epoch, finalized, settlement::settled, false) == action::already_finalized);
   BOOST_CHECK(decide(test_different_wire_epoch, untipped, settlement::never_delivered, false) == action::already_finalized);
   BOOST_CHECK(decide(test_different_wire_epoch, mid_flight, settlement::settled, false) == action::already_finalized);

   // The outpost has not reached this epoch: a delivery would be refused as non-sequential.
   BOOST_CHECK(decide(test_stale_wire_epoch, untipped, settlement::never_delivered, false) == action::outpost_behind);
   BOOST_CHECK(decide(test_stale_wire_epoch, mid_flight, settlement::settled, false) == action::outpost_behind);
} FC_LOG_AND_RETHROW();

/// This relay's record is classified against the settled digest only once the
/// epoch tipped; a malformed word is an error, never "nothing recorded".
BOOST_AUTO_TEST_CASE(delivery_settlement_classification) try {
   namespace detail = sysio::outpost_ethereum_client_detail;
   using settlement = detail::delivery_settlement;

   BOOST_CHECK(detail::classify_settlement(zero_digest_word, settled_digest_word, false) == settlement::never_delivered);
   BOOST_CHECK(detail::classify_settlement(zero_digest_word, settled_digest_word, true) == settlement::never_delivered);
   BOOST_CHECK(detail::classify_settlement(settled_digest_word, settled_digest_word, false) == settlement::recorded);
   BOOST_CHECK(detail::classify_settlement(divergent_digest_word, settled_digest_word, false) == settlement::recorded);
   BOOST_CHECK(detail::classify_settlement(settled_digest_word, settled_digest_word, true) == settlement::settled);
   BOOST_CHECK(detail::classify_settlement(divergent_digest_word, settled_digest_word, true) == settlement::divergent);
   // Checksum casing on either side does not split a match.
   const std::string upper{"1111111111111111111111111111111111111111111111111111111111111111"};
   BOOST_CHECK(detail::classify_settlement(upper, settled_digest_word, true) == settlement::settled);

   BOOST_CHECK_THROW(detail::classify_settlement("", settled_digest_word, true), fc::exception);
   BOOST_CHECK_THROW(detail::classify_settlement(settled_digest_word, "0x", true), fc::exception);
   BOOST_CHECK_THROW(detail::classify_settlement(std::string(settled_digest_word) + "0", settled_digest_word, true),
                     fc::exception);
} FC_LOG_AND_RETHROW();

/// The contract's path-2 predicate, mirrored: the boundary elapsed AND a
/// strict majority agreeing; an empty group never tips.
BOOST_AUTO_TEST_CASE(majority_tip_is_the_contracts_path_2_predicate) try {
   namespace detail = sysio::outpost_ethereum_client_detail;
   constexpr uint64_t started  = 1'000;
   constexpr uint32_t duration = 60;
   constexpr uint64_t boundary = started + duration;
   const auto view = [](uint32_t agreeing, uint32_t group) {
      return detail::pending_consensus{.next_epoch = test_wire_epoch, .agreeing = agreeing, .group_size = group,
                                       .epoch_started_at = started, .epoch_duration_sec = duration};
   };

   BOOST_CHECK(detail::majority_tip_reachable(view(test_majority, test_group_size), boundary));
   BOOST_CHECK(detail::majority_tip_reachable(view(test_group_size, test_group_size), boundary + 1));
   BOOST_CHECK(!detail::majority_tip_reachable(view(test_majority, test_group_size), boundary - 1));
   BOOST_CHECK(!detail::majority_tip_reachable(view(test_majority - 1, test_group_size), boundary));
   BOOST_CHECK(!detail::majority_tip_reachable(view(0, 0), boundary));
   // A group of one tips on its own delivery; the strict majority of five is three.
   BOOST_CHECK(detail::majority_tip_reachable(view(1, 1), boundary));
   BOOST_CHECK(detail::majority_tip_reachable(view(3, 5), boundary));
   BOOST_CHECK(!detail::majority_tip_reachable(view(2, 5), boundary));
} FC_LOG_AND_RETHROW();

/// The progress invariant: every field the outpost moves counts as progress,
/// and nothing else does.
BOOST_AUTO_TEST_CASE(advanced_tracks_every_cursor_field) try {
   namespace detail = sysio::outpost_ethereum_client_detail;
   const detail::delivery_progress before{.next_epoch_index = test_wire_epoch,
                                          .spill            = {},
                                          .own_digest       = std::string(zero_digest_word),
                                          .settled_digest   = std::string(zero_digest_word)};
   BOOST_CHECK(!detail::advanced(before, before));

   auto recorded = before;
   recorded.own_digest = std::string(settled_digest_word);
   BOOST_CHECK(detail::advanced(before, recorded));
   auto tipped = before;
   tipped.spill.tipped = true;
   BOOST_CHECK(detail::advanced(before, tipped));
   auto dispatched = tipped;
   dispatched.spill.dispatched = 1;
   BOOST_CHECK(detail::advanced(tipped, dispatched));
   BOOST_CHECK(!detail::advanced(dispatched, dispatched));
   auto complete = dispatched;
   complete.spill.complete = true;
   BOOST_CHECK(detail::advanced(dispatched, complete));
   auto finalized = complete;
   finalized.spill.finalized = true;
   BOOST_CHECK(detail::advanced(complete, finalized));
   auto next_epoch = before;
   next_epoch.next_epoch_index = test_different_wire_epoch;
   BOOST_CHECK(detail::advanced(before, next_epoch));
   // The settled digest changing on its own is another epoch's business, not progress here.
   auto resettled = before;
   resettled.settled_digest = std::string(settled_digest_word);
   BOOST_CHECK(!detail::advanced(before, resettled));
} FC_LOG_AND_RETHROW();

/// The `epochIn` refusals are identified by selector AND shape, exactly as the
/// pool's are, and only for a call the node executed.
BOOST_AUTO_TEST_CASE(epoch_in_revert_selectors_are_pinned) try {
   namespace detail = sysio::outpost_ethereum_client_detail;
   using revert     = detail::epoch_in_revert;
   const auto data  = [](std::string_view selector, size_t words) {
      return epoch_in_refusal(selector, words).data.as_string();
   };

   BOOST_CHECK(detail::classify_epoch_in_revert(contract_revert_rpc_code, data(dispatch_underfunded_selector, 2)) ==
               revert::dispatch_underfunded);
   BOOST_CHECK(detail::classify_epoch_in_revert(contract_revert_rpc_code, data(handler_gas_exhausted_selector, 3)) ==
               revert::handler_gas_exhausted);
   BOOST_CHECK(detail::classify_epoch_in_revert(contract_revert_rpc_code, data(non_sequential_epoch_selector, 2)) ==
               revert::non_sequential_epoch);
   BOOST_CHECK(detail::classify_epoch_in_revert(contract_revert_rpc_code, data(operator_already_delivered_selector, 2)) ==
               revert::operator_already_delivered);
   BOOST_CHECK(detail::classify_epoch_in_revert(contract_revert_rpc_code, data(not_active_operator_selector, 1)) ==
               revert::not_active_operator);
   BOOST_CHECK(detail::classify_epoch_in_revert(contract_revert_rpc_code, data(digest_mismatch_selector, 2)) ==
               revert::digest_mismatch);

   // Wrong shape, wrong selector, a protocol error, or no bytes: not a known refusal.
   BOOST_CHECK(!detail::classify_epoch_in_revert(contract_revert_rpc_code, data(dispatch_underfunded_selector, 1)));
   BOOST_CHECK(!detail::classify_epoch_in_revert(contract_revert_rpc_code, data(not_active_operator_selector, 2)));
   BOOST_CHECK(!detail::classify_epoch_in_revert(contract_revert_rpc_code, data(no_yield_selector, 0)));
   BOOST_CHECK(!detail::classify_epoch_in_revert(json_rpc_parse_error_code, data(dispatch_underfunded_selector, 2)));
   BOOST_CHECK(!detail::classify_epoch_in_revert(contract_revert_rpc_code, ""));
   BOOST_CHECK(!detail::classify_epoch_in_revert(contract_revert_rpc_code, "0x"));
} FC_LOG_AND_RETHROW();

/// A confirmed call's receipt is summarised from the `OPPInbound` events in it,
/// for this epoch and this relay only; logs from other contracts, other epochs
/// and other operators are ignored.
BOOST_AUTO_TEST_CASE(delivery_receipts_summarise_the_outposts_events) try {
   namespace detail = sysio::outpost_ethereum_client_detail;
   const std::string inbound{test_opp_inbound_address};
   const std::string self{test_other_operator_address};
   const std::string other{test_syndication_pool_address};

   const auto nothing = detail::summarize_delivery_receipt({}, inbound, test_wire_epoch, self);
   BOOST_CHECK(!nothing.recorded && !nothing.tipped && !nothing.dispatched && !nothing.finalized);

   const fc::variants full{
      epoch_delivery_log(inbound, test_wire_epoch, self, settled_digest_word),
      epoch_consensus_log(inbound, test_wire_epoch, settled_digest_word, test_group_size),
      epoch_dispatch_progressed_log(inbound, test_wire_epoch, 4),
      epoch_complete_log(inbound, test_wire_epoch),
   };
   const auto summary = detail::summarize_delivery_receipt(full, inbound, test_wire_epoch, self);
   BOOST_CHECK(summary.recorded);
   BOOST_CHECK(summary.tipped);
   BOOST_REQUIRE(summary.dispatched.has_value());
   BOOST_CHECK_EQUAL(*summary.dispatched, 4u);
   BOOST_CHECK(summary.finalized);
   // Addresses compare without their checksum casing.
   std::string upper_inbound = inbound;
   std::ranges::transform(upper_inbound, upper_inbound.begin(), [](unsigned char c) { return std::toupper(c); });
   BOOST_CHECK(detail::summarize_delivery_receipt(full, upper_inbound, test_wire_epoch, self).recorded);

   const fc::variants foreign{
      epoch_delivery_log(other, test_wire_epoch, self, settled_digest_word),            // another contract
      epoch_delivery_log(inbound, test_wire_epoch, other, settled_digest_word),         // another operator
      epoch_consensus_log(inbound, test_different_wire_epoch, settled_digest_word, 1),  // another epoch
      epoch_dispatch_progressed_log(inbound, test_different_wire_epoch, 4),
      epoch_complete_log(inbound, test_different_wire_epoch),
      fc::variant("not a log object"),
   };
   const auto ignored = detail::summarize_delivery_receipt(foreign, inbound, test_wire_epoch, self);
   BOOST_CHECK(!ignored.recorded && !ignored.tipped && !ignored.dispatched && !ignored.finalized);

   // The receipt parser keeps the block and the logs, and refuses a receipt without a block.
   const auto parsed = sysio::parse_epoch_in_receipt(
      "0x1", fc::mutable_variant_object("blockNumber", pinned_block_hex(test_head_block))("logs", full));
   BOOST_CHECK_EQUAL(parsed.tx_hash, "0x1");
   BOOST_CHECK_EQUAL(parsed.block_number, fc::uint256{test_head_block});
   BOOST_CHECK_EQUAL(parsed.logs.size(), full.size());
   BOOST_CHECK(sysio::parse_epoch_in_receipt("0x1", fc::mutable_variant_object("blockNumber", "0x1")).logs.empty());
   BOOST_CHECK_THROW(sysio::parse_epoch_in_receipt("0x1", fc::mutable_variant_object("logs", full)), fc::exception);
   BOOST_CHECK_THROW(sysio::parse_epoch_in_receipt("0x1", fc::variant("0x1")), fc::exception);

   // The digest the relay compares against the settled one is keccak256 of the bytes.
   BOOST_CHECK_EQUAL(detail::envelope_digest_word({}), keccak256_of_empty);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(delivery_rejects_empty_and_over_cap_envelopes) try {
   auto fixture = create_whole_envelope_delivery_fixture();

   BOOST_CHECK_THROW(fixture->outpost->deliver_outbound_envelope(
                        test_wire_epoch, {}, fc::seconds(test_rpc_deadline_seconds)),
                     fc::assert_exception);

   std::vector<char> over_cap(sysio::OPP_MAX_ENVELOPE_BYTES + 1, oversized_envelope_fill_byte);
   BOOST_CHECK_THROW(fixture->outpost->deliver_outbound_envelope(
                        test_wire_epoch, over_cap, fc::seconds(test_rpc_deadline_seconds)),
                     fc::assert_exception);

   BOOST_CHECK_EQUAL(fixture->delivery_calls.size(), 0u);
   BOOST_CHECK_EQUAL(fixture->next_epoch_reads, 0u);
   BOOST_CHECK_EQUAL(fixture->spill_reads, 0u);
} FC_LOG_AND_RETHROW();

/// The dominant case: the whole envelope goes in ONE call, and once the
/// delivery is recorded without tipping (the rest of the group has yet to
/// deliver) the tick stops rather than re-sending a paid no-op.
BOOST_AUTO_TEST_CASE(delivery_sends_the_whole_envelope_in_one_call) try {
   auto fixture  = create_whole_envelope_delivery_fixture();
   auto envelope = make_envelope(sysio::OPP_MAX_ENVELOPE_BYTES);

   const auto tx = fixture->outpost->deliver_outbound_envelope(
      test_wire_epoch, envelope, fc::seconds(test_rpc_deadline_seconds));

   BOOST_CHECK(!tx.empty());
   check_delivery_calls(fixture->delivery_calls, envelope, test_wire_epoch, 1);
   // Opaque bytes carry no attestation count to size a budget from: the
   // ceiling, and the contract judges them.
   BOOST_CHECK_EQUAL(fixture->delivery_calls[0].gas_budget, sysio::EIP_7825_TX_GAS_CAP);
   // Read at the head before the delivery, and once more at the receipt's
   // block after it to learn it was recorded and did not tip.
   BOOST_CHECK_EQUAL(fixture->next_epoch_reads, 2u);
   BOOST_CHECK_EQUAL(fixture->spill_reads, 2u);
   BOOST_CHECK_EQUAL(fixture->settlement_reads, 2u);
   const std::vector<std::string> expected_blocks{pinned_block_hex(test_head_block), pinned_block_hex(test_head_block + 1)};
   const auto blocks = fixture->blocks_read();
   BOOST_CHECK_EQUAL_COLLECTIONS(blocks.begin(), blocks.end(), expected_blocks.begin(), expected_blocks.end());
   // The tip check ran in the delivery itself: no consensus read follows a send.
   BOOST_CHECK_EQUAL(fixture->consensus_reads, 0u);
} FC_LOG_AND_RETHROW();

/// A consensus retry against an epoch the outpost has already moved past is
/// abandoned before a single transaction is signed — it would only buy a late
/// no-op — and the EMPTY tx id marks the epoch handled.
BOOST_AUTO_TEST_CASE(delivery_skips_when_the_outpost_epoch_advanced) try {
   auto fixture  = create_whole_envelope_delivery_fixture();
   auto envelope = make_envelope(mid_envelope_bytes);
   fixture->next_epoch_index_responses = {encode_next_epoch_index_result(test_different_wire_epoch)};

   const auto tx = fixture->outpost->deliver_outbound_envelope(
      test_wire_epoch, envelope, fc::seconds(test_rpc_deadline_seconds));

   BOOST_CHECK(tx.empty());
   BOOST_CHECK_EQUAL(fixture->delivery_calls.size(), 0u);
   BOOST_CHECK_EQUAL(fixture->next_epoch_reads, 1u);
} FC_LOG_AND_RETHROW();

/// The cursor's own `finalized` flag closes the epoch even when the epoch
/// cursor read has not caught up with it.
BOOST_AUTO_TEST_CASE(delivery_skips_an_epoch_the_cursor_reports_finalized) try {
   auto fixture  = create_whole_envelope_delivery_fixture();
   auto envelope = make_envelope(mid_envelope_bytes);
   fixture->spill_responses = {encode_dispatch_spill_result(true, 5, true, true)};

   const auto tx = fixture->outpost->deliver_outbound_envelope(
      test_wire_epoch, envelope, fc::seconds(test_rpc_deadline_seconds));

   BOOST_CHECK(tx.empty());
   BOOST_CHECK_EQUAL(fixture->delivery_calls.size(), 0u);
   BOOST_CHECK_EQUAL(fixture->next_epoch_reads, 1u);
} FC_LOG_AND_RETHROW();

/// An outpost still on an earlier epoch cannot take this delivery yet: nothing
/// is sent, and the tick ends in the retry exception rather than marking the
/// epoch handled.
BOOST_AUTO_TEST_CASE(delivery_waits_for_an_outpost_that_is_behind) try {
   auto fixture  = create_whole_envelope_delivery_fixture();
   auto envelope = make_envelope(mid_envelope_bytes);
   fixture->next_epoch_index_responses = {encode_next_epoch_index_result(test_stale_wire_epoch)};

   BOOST_CHECK_THROW(fixture->outpost->deliver_outbound_envelope(
                        test_wire_epoch, envelope, fc::seconds(test_rpc_deadline_seconds)),
                     sysio::chain::outpost_delivery_incomplete_exception);
   BOOST_CHECK_EQUAL(fixture->delivery_calls.size(), 0u);
   BOOST_CHECK_EQUAL(fixture->attempted_budgets.size(), 0u);
} FC_LOG_AND_RETHROW();

/// A delivery that tips and spills is continued in the SAME tick: the relay
/// re-supplies the envelope until the outpost reports the epoch finalized.
BOOST_AUTO_TEST_CASE(delivery_that_tips_and_spills_continues_in_the_same_tick) try {
   auto fixture  = create_whole_envelope_delivery_fixture();
   auto envelope = make_envelope(sysio::OPP_MAX_ENVELOPE_BYTES);
   fixture->spill_responses = {
      encode_dispatch_spill_result(false, 0, false, false),   // before the delivery
      encode_dispatch_spill_result(true, 4, false, false),    // tipped, spilled after 4
      encode_dispatch_spill_result(true, 11, true, false),    // dispatched, emit outstanding
      encode_dispatch_spill_result(true, 11, true, true),     // finalized
   };
   fixture->settle_on(envelope, /*recorded_from_read=*/1);

   const auto tx = fixture->outpost->deliver_outbound_envelope(
      test_wire_epoch, envelope, fc::seconds(test_rpc_deadline_seconds));

   BOOST_CHECK(!tx.empty());
   // The delivery, then two continuations — every one carrying the whole envelope.
   check_delivery_calls(fixture->delivery_calls, envelope, test_wire_epoch, 3);
   BOOST_CHECK_EQUAL(fixture->spill_reads, 4u);
   BOOST_CHECK_EQUAL(fixture->settlement_reads, 4u);
} FC_LOG_AND_RETHROW();

/// Resume: a tick that finds the epoch already tipped on this relay's digest
/// picks the continuation up without a fresh delivery — the contract would
/// refuse one, and the relay does not pay to find that out.
BOOST_AUTO_TEST_CASE(delivery_continues_a_tipped_epoch_it_delivered_until_it_finalizes) try {
   auto fixture  = create_whole_envelope_delivery_fixture();
   auto envelope = make_envelope(mid_envelope_bytes);
   fixture->spill_responses = {
      encode_dispatch_spill_result(true, 3, false, false),
      encode_dispatch_spill_result(true, 3, true, false),
      encode_dispatch_spill_result(true, 3, true, true),
   };
   fixture->settle_on(envelope);

   const auto tx = fixture->outpost->deliver_outbound_envelope(
      test_wire_epoch, envelope, fc::seconds(test_rpc_deadline_seconds));

   BOOST_CHECK(!tx.empty());
   check_delivery_calls(fixture->delivery_calls, envelope, test_wire_epoch, 2);
   BOOST_CHECK_EQUAL(fixture->settlement_reads, 3u);
} FC_LOG_AND_RETHROW();

/// A continuation is sent only with the bytes consensus settled on: an
/// envelope whose digest is not the settled one is this relay's problem to
/// report, not the outpost's to refuse for a fee.
BOOST_AUTO_TEST_CASE(delivery_does_not_continue_with_an_envelope_that_is_not_the_settled_digest) try {
   auto fixture  = create_whole_envelope_delivery_fixture();
   auto envelope = make_envelope(mid_envelope_bytes);
   fixture->spill_responses        = {encode_dispatch_spill_result(true, 3, false, false)};
   fixture->own_delivery_responses = {encode_word_result(settled_digest_word)};
   fixture->pending_hash_response  = encode_word_result(settled_digest_word);

   const auto tx = fixture->outpost->deliver_outbound_envelope(
      test_wire_epoch, envelope, fc::seconds(test_rpc_deadline_seconds));

   BOOST_CHECK(tx.empty());
   BOOST_CHECK_EQUAL(fixture->attempted_budgets.size(), 0u);
} FC_LOG_AND_RETHROW();

/// A relay that never delivered has no claim on a tipped epoch's
/// continuation: its deliverers carry it, and this relay sends nothing.
BOOST_AUTO_TEST_CASE(delivery_leaves_a_tipped_epoch_it_did_not_deliver_to_its_deliverers) try {
   auto fixture  = create_whole_envelope_delivery_fixture();
   auto envelope = make_envelope(mid_envelope_bytes);
   fixture->spill_responses        = {encode_dispatch_spill_result(true, 2, false, false)};
   fixture->own_delivery_responses = {encode_word_result(zero_digest_word)};

   const auto tx = fixture->outpost->deliver_outbound_envelope(
      test_wire_epoch, envelope, fc::seconds(test_rpc_deadline_seconds));

   BOOST_CHECK(tx.empty());
   BOOST_CHECK_EQUAL(fixture->delivery_calls.size(), 0u);
   BOOST_CHECK_EQUAL(fixture->settlement_reads, 1u);
} FC_LOG_AND_RETHROW();

/// A relay whose delivery settled on a DIFFERENT digest — a minority envelope
/// — must not resume the epoch from its own divergent bytes.
BOOST_AUTO_TEST_CASE(delivery_does_not_continue_from_a_minority_digest) try {
   auto fixture  = create_whole_envelope_delivery_fixture();
   auto envelope = make_envelope(mid_envelope_bytes);
   fixture->spill_responses        = {encode_dispatch_spill_result(true, 2, false, false)};
   fixture->own_delivery_responses = {encode_word_result(divergent_digest_word)};

   const auto tx = fixture->outpost->deliver_outbound_envelope(
      test_wire_epoch, envelope, fc::seconds(test_rpc_deadline_seconds));

   BOOST_CHECK(tx.empty());
   BOOST_CHECK_EQUAL(fixture->delivery_calls.size(), 0u);
} FC_LOG_AND_RETHROW();

/// A recorded, untipped delivery is re-sent only when the outpost's own
/// consensus view says the re-send would tip it — the boundary elapsed and a
/// strict majority agreeing — and only once per tick. The ordinary first
/// delivery of a tick (`delivery_sends_the_whole_envelope_in_one_call`) is the
/// recorded-untipped-after-a-send case, which returns normally.
BOOST_AUTO_TEST_CASE(delivery_retries_consensus_only_when_the_outposts_majority_view_allows) try {
   const uint64_t now = fc::time_point::now().sec_since_epoch();
   constexpr uint32_t duration = 60;
   const auto consensus = [&](uint32_t agreeing, uint64_t started_at) {
      return encode_pending_consensus_result(test_wire_epoch, agreeing, test_group_size, started_at, duration);
   };

   // Boundary elapsed, majority agreeing: one re-delivery, then the tick stops.
   {
      auto fixture  = create_whole_envelope_delivery_fixture();
      auto envelope = make_envelope(mid_envelope_bytes);
      fixture->own_delivery_responses = {encode_word_result(settled_digest_word)};
      fixture->consensus_response     = consensus(test_majority, now - duration - 1);

      const auto tx = fixture->outpost->deliver_outbound_envelope(
         test_wire_epoch, envelope, fc::seconds(test_rpc_deadline_seconds));

      BOOST_CHECK(!tx.empty());
      check_delivery_calls(fixture->delivery_calls, envelope, test_wire_epoch, 1);
      BOOST_CHECK_EQUAL(fixture->consensus_reads, 1u);
      BOOST_REQUIRE_EQUAL(fixture->consensus_digests.size(), 1u);
      BOOST_CHECK_EQUAL(fixture->consensus_digests.front(), prefixed(settled_digest_word));
   }
   // Majority not reached: nothing to send.
   {
      auto fixture  = create_whole_envelope_delivery_fixture();
      auto envelope = make_envelope(mid_envelope_bytes);
      fixture->own_delivery_responses = {encode_word_result(settled_digest_word)};
      fixture->consensus_response     = consensus(test_majority - 1, now - duration - 1);

      const auto tx = fixture->outpost->deliver_outbound_envelope(
         test_wire_epoch, envelope, fc::seconds(test_rpc_deadline_seconds));

      BOOST_CHECK(tx.empty());
      BOOST_CHECK_EQUAL(fixture->attempted_budgets.size(), 0u);
      BOOST_CHECK_EQUAL(fixture->consensus_reads, 1u);
   }
   // Boundary not elapsed on the outpost: nothing to send, however many agree
   // — and not a used-up retry either: the epoch stays open for the next tick.
   {
      auto fixture  = create_whole_envelope_delivery_fixture();
      auto envelope = make_envelope(mid_envelope_bytes);
      fixture->own_delivery_responses = {encode_word_result(settled_digest_word)};
      fixture->consensus_response     = consensus(test_group_size, now + duration);

      BOOST_CHECK_THROW(fixture->outpost->deliver_outbound_envelope(
                           test_wire_epoch, envelope, fc::seconds(test_rpc_deadline_seconds)),
                        sysio::chain::outpost_delivery_incomplete_exception);
      BOOST_CHECK_EQUAL(fixture->attempted_budgets.size(), 0u);
      BOOST_CHECK_EQUAL(fixture->consensus_reads, 1u);
   }
} FC_LOG_AND_RETHROW();

/// A call that confirms but moves nothing — no record, no tip, no dispatch
/// progress — was under-funded for the attestation at the cursor: the next is
/// funded double, and a call at the ceiling that still moves nothing ends the
/// tick in the retry exception instead of a fourth paid no-op.
BOOST_AUTO_TEST_CASE(delivery_escalates_after_a_call_that_advanced_nothing_and_stops_at_the_ceiling) try {
   auto fixture  = create_whole_envelope_delivery_fixture();
   auto envelope = serialize_envelope_with_attestations(test_wire_epoch, 1, 16);
   // The outpost never records the delivery, whatever is sent.
   fixture->own_delivery_responses = {encode_word_result(zero_digest_word)};
   fixture->receipt_logs           = {fc::variants{}};

   BOOST_CHECK_THROW(fixture->outpost->deliver_outbound_envelope(
                        test_wire_epoch, envelope, fc::seconds(test_rpc_deadline_seconds)),
                     sysio::chain::outpost_delivery_incomplete_exception);

   const auto expected = [&](uint32_t escalations) {
      return sysio::delivery_gas_budget(sysio::delivery_gas_request{
         .envelope_bytes         = envelope.size(),
         .remaining_attestations = 1,
         .escalations            = escalations,
         .ceiling                = sysio::EIP_7825_TX_GAS_CAP});
   };
   BOOST_REQUIRE_EQUAL(expected(2), sysio::EIP_7825_TX_GAS_CAP);
   check_delivery_calls(fixture->delivery_calls, envelope, test_wire_epoch, 3);
   BOOST_CHECK_EQUAL(fixture->delivery_calls[0].gas_budget, expected(0));
   BOOST_CHECK_EQUAL(fixture->delivery_calls[1].gas_budget, expected(1));
   BOOST_CHECK_EQUAL(fixture->delivery_calls[2].gas_budget, sysio::EIP_7825_TX_GAS_CAP);
   // Each read after a send was pinned to that send's block.
   BOOST_CHECK_EQUAL(fixture->blocks_read().size(), 4u);

   // The next tick finds the same stall and reports it the same way, without
   // paying for the under-funded calls again: straight to the ceiling.
   fixture->delivery_calls.clear();
   BOOST_CHECK_THROW(fixture->outpost->deliver_outbound_envelope(
                        test_wire_epoch, envelope, fc::seconds(test_rpc_deadline_seconds)),
                     sysio::chain::outpost_delivery_incomplete_exception);
   BOOST_CHECK_EQUAL(fixture->delivery_calls.size(), 3u);
} FC_LOG_AND_RETHROW();

/// A node refusal for gas below the ceiling is retried once at the ceiling;
/// one at the ceiling is the stalled-epoch report, with nothing sent.
BOOST_AUTO_TEST_CASE(delivery_retries_a_gas_refusal_at_the_ceiling_and_reports_one_there) try {
   {
      auto fixture  = create_whole_envelope_delivery_fixture();
      auto envelope = serialize_envelope_with_attestations(test_wire_epoch, 2, 64);
      fixture->refusals = {epoch_in_refusal(dispatch_underfunded_selector, 2)};

      const auto tx = fixture->outpost->deliver_outbound_envelope(
         test_wire_epoch, envelope, fc::seconds(test_rpc_deadline_seconds));

      BOOST_CHECK(!tx.empty());
      check_delivery_calls(fixture->delivery_calls, envelope, test_wire_epoch, 1);
      BOOST_REQUIRE_EQUAL(fixture->attempted_budgets.size(), 2u);
      BOOST_CHECK(fixture->attempted_budgets[0] < sysio::EIP_7825_TX_GAS_CAP);
      BOOST_CHECK_EQUAL(fixture->attempted_budgets[1], sysio::EIP_7825_TX_GAS_CAP);
      BOOST_CHECK_EQUAL(fixture->delivery_calls[0].gas_budget, sysio::EIP_7825_TX_GAS_CAP);
   }
   {
      auto fixture  = create_whole_envelope_delivery_fixture();
      auto envelope = make_envelope(mid_envelope_bytes);   // unreadable: funded to the ceiling outright
      fixture->refusals = {epoch_in_refusal(handler_gas_exhausted_selector, 3)};

      BOOST_CHECK_THROW(fixture->outpost->deliver_outbound_envelope(
                           test_wire_epoch, envelope, fc::seconds(test_rpc_deadline_seconds)),
                        sysio::chain::outpost_delivery_incomplete_exception);
      BOOST_CHECK_EQUAL(fixture->delivery_calls.size(), 0u);
      BOOST_REQUIRE_EQUAL(fixture->attempted_budgets.size(), 1u);
      BOOST_CHECK_EQUAL(fixture->attempted_budgets[0], sysio::EIP_7825_TX_GAS_CAP);
   }
   // Any other refusal is the job's to see.
   {
      auto fixture  = create_whole_envelope_delivery_fixture();
      auto envelope = make_envelope(mid_envelope_bytes);
      fixture->refusals = {fc::network::json_rpc::json_rpc_error(json_rpc_parse_error_code, "parse error")};

      BOOST_CHECK_THROW(fixture->outpost->deliver_outbound_envelope(
                           test_wire_epoch, envelope, fc::seconds(test_rpc_deadline_seconds)),
                        fc::network::json_rpc::json_rpc_error);
      BOOST_CHECK_EQUAL(fixture->attempted_budgets.size(), 1u);
   }
} FC_LOG_AND_RETHROW();

/// A refusal that says the outpost moved under the read — here, the epoch
/// finalized between the read and the send — is answered by a fresh read at
/// the head, not a guess; one that survives fresh reads is reported.
BOOST_AUTO_TEST_CASE(delivery_rereads_at_the_head_after_a_stale_refusal) try {
   {
      auto fixture  = create_whole_envelope_delivery_fixture();
      auto envelope = make_envelope(mid_envelope_bytes);
      fixture->refusals                   = {epoch_in_refusal(non_sequential_epoch_selector, 2)};
      fixture->next_epoch_index_responses = {encode_next_epoch_index_result(test_wire_epoch),
                                             encode_next_epoch_index_result(test_different_wire_epoch)};

      const auto tx = fixture->outpost->deliver_outbound_envelope(
         test_wire_epoch, envelope, fc::seconds(test_rpc_deadline_seconds));

      BOOST_CHECK(tx.empty());
      BOOST_CHECK_EQUAL(fixture->delivery_calls.size(), 0u);
      BOOST_CHECK_EQUAL(fixture->attempted_budgets.size(), 1u);
      BOOST_CHECK_EQUAL(fixture->next_epoch_reads, 2u);
      BOOST_CHECK_EQUAL(fixture->rpc->block_number_reads, 2u);
   }
   {
      auto fixture  = create_whole_envelope_delivery_fixture();
      auto envelope = make_envelope(mid_envelope_bytes);
      fixture->refusals = {epoch_in_refusal(not_active_operator_selector, 1),
                           epoch_in_refusal(not_active_operator_selector, 1),
                           epoch_in_refusal(not_active_operator_selector, 1)};
      fixture->own_delivery_responses = {encode_word_result(zero_digest_word)};

      BOOST_CHECK_THROW(fixture->outpost->deliver_outbound_envelope(
                           test_wire_epoch, envelope, fc::seconds(test_rpc_deadline_seconds)),
                        fc::network::json_rpc::json_rpc_error);
      BOOST_CHECK_EQUAL(fixture->delivery_calls.size(), 0u);
      BOOST_CHECK_EQUAL(fixture->attempted_budgets.size(), 3u);
   }
} FC_LOG_AND_RETHROW();

/// A tick sends at most `MAX_CONTINUATIONS_PER_TICK` calls; an epoch still
/// open after that is handed to the next tick through the retry exception,
/// never marked handled.
BOOST_AUTO_TEST_CASE(delivery_ends_the_tick_at_the_continuation_bound) try {
   constexpr uint16_t continuation_bound = 32;
   auto fixture  = create_whole_envelope_delivery_fixture();
   auto envelope = make_envelope(mid_envelope_bytes);
   // Every call dispatches one more attestation and spills again.
   for (uint16_t dispatched = 1; dispatched <= continuation_bound + 1; ++dispatched) {
      fixture->spill_responses.push_back(encode_dispatch_spill_result(true, dispatched, false, false));
   }
   fixture->spill_responses.erase(fixture->spill_responses.begin());   // drop the never-tipped default
   fixture->settle_on(envelope);

   BOOST_CHECK_THROW(fixture->outpost->deliver_outbound_envelope(
                        test_wire_epoch, envelope, fc::seconds(test_rpc_deadline_seconds)),
                     sysio::chain::outpost_delivery_incomplete_exception);
   check_delivery_calls(fixture->delivery_calls, envelope, test_wire_epoch, continuation_bound);
   // The bound was decided from a read AFTER the last send, at its block.
   BOOST_CHECK_EQUAL(fixture->spill_reads, continuation_bound + 1u);
} FC_LOG_AND_RETHROW();

/// The cursor readers fail closed: a spill word that does not decode, or a
/// digest that is not one 32-byte word, is an error, never "not tipped" or
/// "never delivered" — either of which would send a fresh delivery into an
/// epoch that may already have settled.
BOOST_AUTO_TEST_CASE(delivery_fails_closed_on_malformed_cursor_reads) try {
   {
      auto fixture  = create_whole_envelope_delivery_fixture();
      auto envelope = make_envelope(mid_envelope_bytes);
      fixture->spill_responses = {std::string(hex_prefix) + abi_word(1)};   // one word of four
      BOOST_CHECK_THROW(fixture->outpost->deliver_outbound_envelope(
                           test_wire_epoch, envelope, fc::seconds(test_rpc_deadline_seconds)),
                        fc::exception);
      BOOST_CHECK_EQUAL(fixture->attempted_budgets.size(), 0u);
   }
   {
      auto fixture  = create_whole_envelope_delivery_fixture();
      auto envelope = make_envelope(mid_envelope_bytes);
      fixture->own_delivery_responses = {std::string(hex_prefix)};   // no word at all
      BOOST_CHECK_THROW(fixture->outpost->deliver_outbound_envelope(
                           test_wire_epoch, envelope, fc::seconds(test_rpc_deadline_seconds)),
                        fc::exception);
      BOOST_CHECK_EQUAL(fixture->attempted_budgets.size(), 0u);
   }
   {
      auto fixture  = create_whole_envelope_delivery_fixture();
      auto envelope = make_envelope(mid_envelope_bytes);
      fixture->next_epoch_index_responses = {"0xzz"};
      // The ABI decoder's own refusal of a non-hex response, not an fc::exception.
      BOOST_CHECK_THROW(fixture->outpost->deliver_outbound_envelope(
                           test_wire_epoch, envelope, fc::seconds(test_rpc_deadline_seconds)),
                        std::exception);
      BOOST_CHECK_EQUAL(fixture->attempted_budgets.size(), 0u);
   }
} FC_LOG_AND_RETHROW();

/// A transport failure on a continuation propagates after the delivery that
/// landed stands: the tick abandons, nothing is rolled back, and the next tick
/// resumes from the outpost's cursor.
BOOST_AUTO_TEST_CASE(delivery_propagates_a_transport_failure_on_a_continuation) try {
   auto fixture  = create_whole_envelope_delivery_fixture();
   auto envelope = make_envelope(sysio::OPP_MAX_ENVELOPE_BYTES);
   fixture->spill_responses = {
      encode_dispatch_spill_result(false, 0, false, false),
      encode_dispatch_spill_result(true, 4, false, false),
   };
   fixture->settle_on(envelope, /*recorded_from_read=*/1);
   fixture->refusals = {std::nullopt, fc::network::json_rpc::json_rpc_error(json_rpc_parse_error_code, "gateway timeout")};

   BOOST_CHECK_THROW(fixture->outpost->deliver_outbound_envelope(
                        test_wire_epoch, envelope, fc::seconds(test_rpc_deadline_seconds)),
                     fc::network::json_rpc::json_rpc_error);
   check_delivery_calls(fixture->delivery_calls, envelope, test_wire_epoch, 1);
   BOOST_CHECK_EQUAL(fixture->attempted_budgets.size(), 2u);
} FC_LOG_AND_RETHROW();

/// Mid-sequence deadline expiry — the scenario the cursor exists for.
///
/// The delivery burns more wall clock than the whole budget, so the
/// continuation's pre-flight `throw_if_past_deadline` fires: the tick abandons
/// after the delivery landed rather than rolling anything back. The next tick
/// reads the cursor and continues from it instead of re-delivering.
BOOST_AUTO_TEST_CASE(delivery_abandons_on_deadline_then_continues_next_tick) try {
   auto fixture  = create_whole_envelope_delivery_fixture();
   auto envelope = make_envelope(sysio::OPP_MAX_ENVELOPE_BYTES);

   constexpr int64_t delivery_budget_ms = 50;
   fixture->first_call_delay = std::chrono::milliseconds{delivery_budget_ms * 4};
   fixture->spill_responses = {
      encode_dispatch_spill_result(false, 0, false, false),
      encode_dispatch_spill_result(true, 6, false, false),
   };
   fixture->settle_on(envelope, /*recorded_from_read=*/1);

   BOOST_CHECK_THROW(fixture->outpost->deliver_outbound_envelope(
                        test_wire_epoch, envelope, fc::milliseconds(delivery_budget_ms)),
                     fc::timeout_exception);

   // Exactly the delivery landed, carrying the whole envelope.
   check_delivery_calls(fixture->delivery_calls, envelope, test_wire_epoch, 1);

   // Next tick: the outpost reports the epoch tipped on our digest with six
   // attestations routed, and the relay continues rather than re-delivering.
   fixture->delivery_calls.clear();
   fixture->first_call_delay = std::chrono::milliseconds{0};
   fixture->spill_reads = 0;
   fixture->spill_responses = {
      encode_dispatch_spill_result(true, 6, false, false),
      encode_dispatch_spill_result(true, 6, true, true),
   };

   const auto tx = fixture->outpost->deliver_outbound_envelope(
      test_wire_epoch, envelope, fc::seconds(test_rpc_deadline_seconds));

   BOOST_CHECK(!tx.empty());
   check_delivery_calls(fixture->delivery_calls, envelope, test_wire_epoch, 1);
} FC_LOG_AND_RETHROW();

// ── `crank_outpost` ──────────────────────────────────────────────────────

/// The pool's three refusals are identified by selector AND shape, exactly as the `epochIn`
/// refusals are: a neighbouring error, a role error, a refusal with the wrong argument count,
/// and no bytes at all are none of them.
BOOST_AUTO_TEST_CASE(realize_yield_refusal_selectors_are_pinned) try {
   namespace crank = sysio::outpost_ethereum_client_detail;
   using refusal   = crank::realize_yield_refusal;

   BOOST_CHECK(crank::classify_realize_yield_revert(std::string(hex_prefix) + std::string(no_yield_selector)) ==
               refusal::no_yield);
   BOOST_CHECK(crank::classify_realize_yield_revert(encode_two_word_revert(
                  yield_below_deadband_selector, test_yield_delta, test_yield_deadband)) ==
               refusal::below_deadband);
   BOOST_CHECK(crank::classify_realize_yield_revert(encode_two_word_revert(
                  pool_underbacked_selector, test_yield_delta, test_yield_deadband)) ==
               refusal::underbacked);
   BOOST_CHECK(crank::classify_realize_yield_revert(std::string(hex_prefix) +
                                                    std::string(enforced_pause_selector)) == refusal::paused);
   // `EnforcedPause()` takes no arguments; one with a word attached is not it.
   BOOST_CHECK(!crank::classify_realize_yield_revert(
      std::string(hex_prefix) + std::string(enforced_pause_selector) + abi_word(1)));

   BOOST_CHECK(!crank::classify_realize_yield_revert(
      std::string(hex_prefix) + std::string(no_yield_selector) + abi_word(1)));
   BOOST_CHECK(!crank::classify_realize_yield_revert(
      std::string(hex_prefix) + std::string(pool_underbacked_selector) + abi_word(1)));
   BOOST_CHECK(!crank::classify_realize_yield_revert(
      encode_address_revert(access_managed_unauthorized_selector, test_other_operator_address)));
   BOOST_CHECK(!crank::classify_realize_yield_revert(
      encode_address_revert(unrelated_address_error_selector, test_other_operator_address)));
   BOOST_CHECK(!crank::classify_realize_yield_revert(""));
   BOOST_CHECK(!crank::classify_realize_yield_revert("0x"));
} FC_LOG_AND_RETHROW();

/// The handler word decodes to the registered address, and only a real one is routable:
/// `address(0)` and the blackhole are "no pool", and a malformed word is nothing at all.
BOOST_AUTO_TEST_CASE(handler_word_decoding_and_routability) try {
   namespace crank = sysio::outpost_ethereum_client_detail;
   const std::string pool{test_syndication_pool_address};

   const auto decoded = crank::address_from_word(encode_address_word(pool));
   BOOST_REQUIRE(decoded.has_value());
   BOOST_CHECK(crank::same_evm_address(*decoded, pool));
   BOOST_CHECK(crank::is_routable_handler(*decoded));

   const auto zero = crank::address_from_word(encode_address_word(zero_evm_address));
   BOOST_REQUIRE(zero.has_value());
   BOOST_CHECK(!crank::is_routable_handler(*zero));
   const auto blackhole = crank::address_from_word(encode_address_word(attestation_blackhole_address));
   BOOST_REQUIRE(blackhole.has_value());
   BOOST_CHECK(!crank::is_routable_handler(*blackhole));
   BOOST_CHECK(!crank::is_routable_handler(""));

   BOOST_CHECK(!crank::address_from_word(""));
   BOOST_CHECK(!crank::address_from_word("0x"));
   // A bare address is not a word.
   BOOST_CHECK(!crank::address_from_word(pool));
   auto dirty_pad = encode_address_word(pool);
   dirty_pad[hex_prefix.size()] = '1';
   BOOST_CHECK(!crank::address_from_word(dirty_pad));
   auto not_hex = encode_address_word(pool);
   not_hex.back() = 'g';
   BOOST_CHECK(!crank::address_from_word(not_hex));
} FC_LOG_AND_RETHROW();

/// An ABI set without `realizeYield` is an outpost deployment that predates the pool: the crank
/// asks the outpost nothing.
BOOST_AUTO_TEST_CASE(crank_outpost_is_idle_without_the_pool_abi) try {
   auto fixture = create_crank_fixture(/*with_pool_abi=*/false);
   fixture->handler_response = encode_address_word(test_syndication_pool_address);

   fixture->outpost->crank_outpost(test_wire_epoch, fc::seconds(test_rpc_deadline_seconds));

   BOOST_CHECK(fixture->handler_reads.empty());
   BOOST_CHECK(fixture->outpost->syndication_pool_address().empty());
} FC_LOG_AND_RETHROW();

/// Until the outpost routes `DESYNDICATE_LIQ` somewhere real there is no pool: `address(0)` and
/// the blackhole both leave the crank idle, with nothing bound and nothing sent.
BOOST_AUTO_TEST_CASE(crank_outpost_is_idle_until_the_outpost_registers_a_pool) try {
   const auto desyndicate_liq = static_cast<uint16_t>(
      magic_enum::enum_integer(sysio::opp::types::ATTESTATION_TYPE_DESYNDICATE_LIQ));
   for (const auto unregistered : {zero_evm_address, attestation_blackhole_address}) {
      BOOST_TEST_CONTEXT(unregistered) {
         auto fixture = create_crank_fixture();
         fixture->handler_response = encode_address_word(unregistered);

         fixture->outpost->crank_outpost(test_wire_epoch, fc::seconds(test_rpc_deadline_seconds));

         BOOST_REQUIRE_EQUAL(fixture->handler_reads.size(), 1u);
         BOOST_CHECK_EQUAL(fixture->handler_reads.front(), desyndicate_liq);
         BOOST_CHECK(fixture->outpost->syndication_pool_address().empty());
      }
   }
} FC_LOG_AND_RETHROW();

/// The pool the outpost names is the pool that is cranked, once per epoch, and it stays bound.
BOOST_AUTO_TEST_CASE(crank_outpost_realizes_yield_on_the_registered_pool) try {
   auto fixture = create_crank_fixture();
   fixture->handler_response = encode_address_word(test_syndication_pool_address);
   fixture->bind_stub_pool(test_syndication_pool_address);

   fixture->outpost->crank_outpost(test_wire_epoch, fc::seconds(test_rpc_deadline_seconds));
   fixture->outpost->crank_outpost(test_wire_epoch + 1, fc::seconds(test_rpc_deadline_seconds));

   BOOST_CHECK_EQUAL(fixture->handler_reads.size(), 2u);
   BOOST_CHECK_EQUAL(fixture->realize_calls, 2u);
   BOOST_CHECK(sysio::outpost_ethereum_client_detail::same_evm_address(
      fixture->outpost->syndication_pool_address(), test_syndication_pool_address));
} FC_LOG_AND_RETHROW();

/// When the outpost re-routes `DESYNDICATE_LIQ` (an upgrade to a new proxy) the crank follows:
/// it binds a wrapper to the new address before sending, and the stale stub is never called.
/// The fresh wrapper dials the fixture's dead endpoint, so the send fails -- the evidence that
/// the moved address, not the stub, was driven.
BOOST_AUTO_TEST_CASE(crank_outpost_rebinds_when_the_registered_pool_moves) try {
   auto fixture = create_crank_fixture();
   fixture->bind_stub_pool(test_syndication_pool_address);
   fixture->handler_response = encode_address_word(test_moved_syndication_pool_address);

   BOOST_CHECK_THROW(fixture->outpost->crank_outpost(test_wire_epoch, fc::seconds(test_rpc_deadline_seconds)),
                     std::exception);

   BOOST_CHECK_EQUAL(fixture->realize_calls, 0u);
   BOOST_CHECK(sysio::outpost_ethereum_client_detail::same_evm_address(
      fixture->outpost->syndication_pool_address(), test_moved_syndication_pool_address));
} FC_LOG_AND_RETHROW();

/// The pool's own refusals are outcomes of the crank, not failures: nothing to report is
/// debug-quiet, an underbacked pool is a warning, a paused (frozen) pool is info, and none of
/// them propagate.
BOOST_AUTO_TEST_CASE(crank_outpost_reads_the_pools_own_refusals_as_outcomes) try {
   const std::vector<std::pair<std::string, std::string>> refusals{
      {"WIRE_NoYield()", std::string(hex_prefix) + std::string(no_yield_selector)},
      {"WIRE_YieldBelowDeadband(uint64,uint64)",
       encode_two_word_revert(yield_below_deadband_selector, test_yield_delta, test_yield_deadband)},
      {"WIRE_PoolUnderbacked(uint64,uint64)",
       encode_two_word_revert(pool_underbacked_selector, test_yield_delta, test_yield_deadband)},
      {"EnforcedPause()", std::string(hex_prefix) + std::string(enforced_pause_selector)},
   };
   for (const auto& [description, revert_data] : refusals) {
      BOOST_TEST_CONTEXT(description) {
         auto fixture = create_crank_fixture();
         fixture->handler_response = encode_address_word(test_syndication_pool_address);
         fixture->bind_stub_pool(test_syndication_pool_address);
         fixture->realize_failure = fc::network::json_rpc::json_rpc_error(
            contract_revert_rpc_code, "execution reverted", fc::variant{revert_data});

         fixture->outpost->crank_outpost(test_wire_epoch, fc::seconds(test_rpc_deadline_seconds));

         BOOST_CHECK_EQUAL(fixture->realize_calls, 1u);
      }
   }
} FC_LOG_AND_RETHROW();

/// Everything else the send comes back with is the job's to log as a failed crank: a role error,
/// a refusal of the wrong shape, no revert bytes, and a protocol error that never ran the call.
BOOST_AUTO_TEST_CASE(crank_outpost_propagates_every_other_failure) try {
   const std::vector<std::pair<std::string, fc::network::json_rpc::json_rpc_error>> failures{
      {"a signer without the yield_operator role",
       fc::network::json_rpc::json_rpc_error(
          contract_revert_rpc_code, "execution reverted",
          fc::variant{encode_address_revert(access_managed_unauthorized_selector, test_other_operator_address)})},
      {"WIRE_NoYield() with a stray argument",
       fc::network::json_rpc::json_rpc_error(
          contract_revert_rpc_code, "execution reverted",
          fc::variant{std::string(hex_prefix) + std::string(no_yield_selector) + abi_word(1)})},
      {"no revert bytes at all",
       fc::network::json_rpc::json_rpc_error(contract_revert_rpc_code, "execution reverted",
                                             fc::variant{std::string{}})},
      {"a protocol error",
       fc::network::json_rpc::json_rpc_error(json_rpc_parse_error_code, "parse error", fc::variant{})},
   };
   for (const auto& [description, failure] : failures) {
      BOOST_TEST_CONTEXT(description) {
         auto fixture = create_crank_fixture();
         fixture->handler_response = encode_address_word(test_syndication_pool_address);
         fixture->bind_stub_pool(test_syndication_pool_address);
         fixture->realize_failure = failure;

         BOOST_CHECK_THROW(
            fixture->outpost->crank_outpost(test_wire_epoch, fc::seconds(test_rpc_deadline_seconds)),
            fc::exception);
         BOOST_CHECK_EQUAL(fixture->realize_calls, 1u);
      }
   }
} FC_LOG_AND_RETHROW();

/// A spent deadline abandons the crank before it asks the outpost anything.
BOOST_AUTO_TEST_CASE(crank_outpost_abandons_on_an_expired_deadline) try {
   auto fixture = create_crank_fixture();
   fixture->handler_response = encode_address_word(test_syndication_pool_address);
   fixture->bind_stub_pool(test_syndication_pool_address);

   BOOST_CHECK_THROW(fixture->outpost->crank_outpost(test_wire_epoch, fc::microseconds(0)), fc::exception);

   BOOST_CHECK(fixture->handler_reads.empty());
   BOOST_CHECK_EQUAL(fixture->realize_calls, 0u);
} FC_LOG_AND_RETHROW();

/// An EVM client policy must bound `max_gas_limit` at EIP-7825's per-transaction
/// cap. `derive_buffered_gas_limit` applies a x1.2 buffer to the node's
/// estimate, so an estimate that itself fits the cap can still produce a
/// cap-invalid transaction — the policy is what rejects it, and a policy-free
/// client would sign it.
BOOST_AUTO_TEST_CASE(gas_limit_policy_bounds_the_buffered_limit_at_the_eip_7825_cap) try {
   const ethereum_transaction_policy policy{
      .client_id = std::string(latest_slot_test_entry_id),
      .chain_id = test_evm_chain_id,
      .max_priority_fee_per_gas = maximum_ethereum_transaction_policy_value(),
      .max_fee_per_gas = maximum_ethereum_transaction_policy_value(),
      .max_gas_limit = fc::uint256{sysio::EIP_7825_TX_GAS_CAP},
      .max_total_native_cost = maximum_ethereum_transaction_policy_value(),
   };

   BOOST_CHECK_EQUAL(derive_buffered_gas_limit(policy, fc::uint256{under_cap_gas_estimate}),
                     fc::uint256{under_cap_buffered_gas_limit});

   try {
      derive_buffered_gas_limit(policy, fc::uint256{over_cap_gas_estimate});
      BOOST_FAIL("expected the buffered gas limit to breach the EIP-7825 cap");
   } catch (const ethereum_transaction_policy_exception& rejection) {
      BOOST_CHECK(rejection.reason() ==
                  ethereum_transaction_policy_reason::gas_limit_cap_exceeded);
   }
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

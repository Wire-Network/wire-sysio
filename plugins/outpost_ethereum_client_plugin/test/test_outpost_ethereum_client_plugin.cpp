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

// ── Chunked `epochIn` fixtures ───────────────────────────────────────────
constexpr std::string_view test_opp_inbound_address = "e7f1725E7734CE288F8367e1Bb143E90bb3F0512";
/// keccak256("epochIn(uint32,uint16,uint16,uint32,bytes)")[0..4).
constexpr std::string_view epoch_in_selector = "c3e558bc";
/// keccak256("epochIn(bytes)")[0..4) — the retired single-transaction form.
constexpr std::string_view retired_single_bytes_epoch_in_selector = "cfae3118";
constexpr size_t evm_address_bytes = 20;
constexpr size_t evm_address_hex_chars = evm_address_bytes * hex_chars_per_byte;
constexpr size_t epoch_in_input_count = 5;
/// A peer operator's address, used to prove an unowned staging header is ignored.
constexpr std::string_view foreign_operator_address =
   "0x90F79bf6EB2c4f870365E785982E1f101E93b906";
/// Envelope sizes exercising the chunk loop: three full chunks + a remainder,
/// and the exact platform cap (four full chunks).
constexpr size_t three_chunk_envelope_bytes = 2 * sysio::ETHEREUM_MAX_CHUNK_BYTES + 3'616;
/// `derive_buffered_gas_limit` applies a ×1.2 buffer, so a policy pinned at
/// EIP-7825's per-transaction cap rejects any estimate above 13 981 013.
constexpr uint64_t eip_7825_tx_gas_cap = 16'777'216;
constexpr uint64_t under_cap_gas_estimate = 13'000'000;
constexpr uint64_t under_cap_buffered_gas_limit = 15'600'000;
constexpr uint64_t over_cap_gas_estimate = 14'000'000;
/// JSON-RPC error code anvil/geth report for a reverted call.
constexpr int contract_revert_rpc_code = 3;

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

/// 2^70 encoded as one 32-byte ABI word — a `storedBytes` value that no uint64
/// can hold, used to prove the diagnostics-only field cannot veto a resume.
std::string stored_bytes_above_uint64_word() {
   const std::string value = "4" + std::string(17, '0');   // 2^70 == 0x4 << 68
   return std::string(evm_abi_word_hex_chars - value.size(), '0') + value;
}

/// Encode the raw return bytes for `envelopeChunkState(address)` — six static
/// outputs, so the words are simply concatenated with no offset table.
///
/// `stored_bytes_word`, when non-empty, replaces the encoded `storedBytes` word
/// so a test can present a uint256 beyond the uint64 range.
std::string encode_envelope_chunk_state_result(uint32_t         epoch_index,
                                               std::string_view owner_address,
                                               uint16_t         total_chunks,
                                               uint16_t         received_chunks,
                                               uint32_t         total_bytes,
                                               uint64_t         stored_bytes,
                                               std::string_view stored_bytes_word = {}) {
   std::string owner{owner_address};
   if (owner.starts_with(hex_prefix)) owner.erase(0, hex_prefix.size());
   BOOST_REQUIRE_EQUAL(owner.size(), evm_address_hex_chars);
   const std::string stored =
      stored_bytes_word.empty() ? abi_word(stored_bytes) : std::string(stored_bytes_word);
   BOOST_REQUIRE_EQUAL(stored.size(), evm_abi_word_hex_chars);
   return std::string(hex_prefix) + abi_word(epoch_index) +
          std::string(evm_abi_word_hex_chars - evm_address_hex_chars, '0') + owner +
          abi_word(total_chunks) + abi_word(received_chunks) + abi_word(total_bytes) + stored;
}

/// Encode the raw return bytes for the single-output `nextEpochIndex()` view.
std::string encode_next_epoch_index_result(uint32_t next_epoch_index) {
   return std::string(hex_prefix) + abi_word(next_epoch_index);
}

/// One `epochIn` invocation as the stubbed typed wrapper observed it.
struct observed_chunk_call {
   uint32_t    epoch_index;
   uint16_t    chunk_index;
   uint16_t    total_chunks;
   uint32_t    total_bytes;
   std::string chunk_hex;
};

/// Harness binding a real `outpost_ethereum_client` to a stubbed OPPInbound
/// wrapper.
///
/// `ethereum_client::get_contract` caches one typed wrapper per address, so a
/// wrapper materialized here is the SAME object the client resolves in its
/// constructor — replacing its `std::function` members intercepts every RPC at
/// the typed callable boundary, exactly as the `getLatestOutboundEnvelope`
/// coverage above does.
struct chunked_delivery_fixture {
   ~chunked_delivery_fixture() {
      outpost.reset();
      inbound.reset();
      tester.reset();
      appbase::application::reset_app_singleton();
   }

   std::unique_ptr<sig_provider_tester>                tester;
   std::shared_ptr<sysio::opp_inbound_contract_client> inbound;
   std::unique_ptr<sysio::outpost_ethereum_client>     outpost;

   std::vector<observed_chunk_call> chunk_calls;
   size_t                           chunk_state_reads = 0;
   size_t                           discard_calls     = 0;
   size_t                           next_epoch_reads  = 0;

   /// Response the stubbed `envelopeChunkState` view returns; the default is
   /// the all-zero (never staged) header.
   std::string chunk_state_response =
      encode_envelope_chunk_state_result(0, std::string(evm_address_hex_chars, '0'), 0, 0, 0, 0);
   /// Response the stubbed `nextEpochIndex` view returns.
   std::string next_epoch_index_response = encode_next_epoch_index_result(0);
   /// When set, the stubbed `discardEnvelopeChunks` write throws it.
   std::optional<fc::network::json_rpc::json_rpc_error> discard_failure;
   /// Wall-clock the FIRST stubbed `epochIn` burns before returning, standing in
   /// for a slow chain. Applied only to the first call so a deadline set below
   /// it expires deterministically at the SECOND chunk's pre-flight check.
   std::chrono::milliseconds first_chunk_delay{0};
};

/// Build an envelope of exactly `size` bytes whose content varies per index, so
/// a mis-sliced chunk cannot accidentally compare equal to the right one.
std::vector<char> make_chunked_envelope(size_t size) {
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

/// Stand up a `chunked_delivery_fixture`: a real `outpost_ethereum_client`
/// whose OPPInbound wrapper has every typed callable replaced by a recording
/// stub. The caller owns the returned fixture; the stubs capture it by
/// reference, so it must not be moved after this returns.
std::unique_ptr<chunked_delivery_fixture> create_chunked_delivery_fixture() {
   auto fixture = std::make_unique<chunked_delivery_fixture>();
   fixture->tester = create_app();

   auto sig_provider = fixture->tester->plugin().create_provider(
      std::string(latest_slot_test_entry_id),
      chain_kind_ethereum,
      chain_key_type_ethereum,
      std::string(latest_slot_test_public_key),
      to_private_key_spec(std::string(latest_slot_test_private_key)));

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
      std::variant<std::string, fc::url>{std::string(latest_slot_test_rpc_url)},
      std::move(transaction_policy));

   auto              abis = load_abi_fixture(opp_inbound_abi_fixture);
   const std::string inbound_address{test_opp_inbound_address};
   fixture->inbound =
      eth_client->get_contract<sysio::opp_inbound_contract_client>(inbound_address, abis);
   BOOST_REQUIRE(fixture->inbound);

   auto* raw = fixture.get();
   raw->inbound->epoch_in = [raw](uint32_t&    epoch_index,
                                  uint16_t&    chunk_index,
                                  uint16_t&    total_chunks,
                                  uint32_t&    total_bytes,
                                  std::string& chunk_hex) -> fc::variant {
      raw->chunk_calls.push_back(
         observed_chunk_call{epoch_index, chunk_index, total_chunks, total_bytes, chunk_hex});
      if (raw->chunk_calls.size() == 1 && raw->first_chunk_delay.count() > 0) {
         std::this_thread::sleep_for(raw->first_chunk_delay);
      }
      return fc::variant(std::string(hex_prefix) + abi_word(raw->chunk_calls.size()));
   };
   raw->inbound->envelope_chunk_state =
      [raw](const block_number_or_tag_t& block, std::string& operator_address) -> fc::variant {
         // The resume read is our OWN staging high-water mark, so it must be
         // taken at `latest` — reading it at `finalized` would replay chunks
         // staged in unfinalized blocks on every tick.
         BOOST_CHECK(std::holds_alternative<block_tag_t>(block));
         BOOST_CHECK(std::get<block_tag_t>(block) == block_tag_t::latest);
         BOOST_CHECK(!operator_address.empty());
         ++raw->chunk_state_reads;
         return fc::variant(raw->chunk_state_response);
      };
   raw->inbound->discard_envelope_chunks = [raw]() -> fc::variant {
      ++raw->discard_calls;
      if (raw->discard_failure) throw *raw->discard_failure;
      return fc::variant(std::string(hex_prefix) + abi_word(0));
   };
   raw->inbound->next_epoch_index = [raw](const block_number_or_tag_t& block) -> fc::variant {
      BOOST_CHECK(std::holds_alternative<block_tag_t>(block));
      BOOST_CHECK(std::get<block_tag_t>(block) == block_tag_t::latest);
      ++raw->next_epoch_reads;
      return fc::variant(raw->next_epoch_index_response);
   };

   auto entry = std::make_shared<sysio::ethereum_client_entry_t>();
   entry->id = latest_slot_test_entry_id;
   entry->signature_provider = sig_provider;
   entry->client = eth_client;
   entry->chain_id = test_evm_chain_id;

   fixture->outpost = std::make_unique<sysio::outpost_ethereum_client>(
      entry,
      /*opp_addr=*/std::string{},
      inbound_address,
      /*operator_registry_addr=*/std::string{},
      abis,
      test_outpost_chain_code,
      test_evm_chain_id);
   return fixture;
}

/// Assert that `calls` covers `[first_chunk, total_chunks)` of `envelope` with
/// exact slices: every non-final chunk is exactly `ETHEREUM_MAX_CHUNK_BYTES`
/// and the final one carries the remainder.
void check_chunk_call_sequence(const std::vector<observed_chunk_call>& calls,
                               const std::vector<char>&                envelope,
                               uint32_t                                epoch_index,
                               uint16_t                                first_chunk) {
   const auto total_chunks = sysio::outpost_ethereum_client_detail::chunk_count_for(envelope.size());
   BOOST_REQUIRE_EQUAL(calls.size(), static_cast<size_t>(total_chunks - first_chunk));

   for (size_t i = 0; i < calls.size(); ++i) {
      const auto& call  = calls[i];
      const auto  chunk = static_cast<uint16_t>(first_chunk + i);
      BOOST_CHECK_EQUAL(call.epoch_index, epoch_index);
      BOOST_CHECK_EQUAL(call.chunk_index, chunk);
      BOOST_CHECK_EQUAL(call.total_chunks, total_chunks);
      BOOST_CHECK_EQUAL(call.total_bytes, static_cast<uint32_t>(envelope.size()));

      const size_t offset = static_cast<size_t>(chunk) * sysio::ETHEREUM_MAX_CHUNK_BYTES;
      const size_t length =
         std::min(sysio::ETHEREUM_MAX_CHUNK_BYTES, envelope.size() - offset);
      if (chunk + 1 < total_chunks) {
         BOOST_CHECK_EQUAL(length, sysio::ETHEREUM_MAX_CHUNK_BYTES);
      }
      BOOST_CHECK_EQUAL(call.chunk_hex,
                        fc::to_hex(envelope.data() + offset, static_cast<uint32_t>(length)));
   }
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
   // construction: the chunked write, the owner-bound resume read, the
   // recovery write, and the epoch view the resume path falls back on.
   bool has_epoch_in = false, has_next_epoch = false;
   bool has_discard = false, has_chunk_state = false;
   for (auto& c : abis) {
      if (c.name == "epochIn") has_epoch_in = true;
      if (c.name == "nextEpochIndex") has_next_epoch = true;
      if (c.name == "discardEnvelopeChunks") has_discard = true;
      if (c.name == "envelopeChunkState") has_chunk_state = true;
   }
   BOOST_CHECK(has_epoch_in);
   BOOST_CHECK(has_next_epoch);
   BOOST_CHECK(has_discard);
   BOOST_CHECK(has_chunk_state);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(epoch_in_abi_encoding_with_chunk_params) try {
   auto abis = load_abi_fixture(opp_inbound_abi_fixture);

   // Find the epochIn ABI entry
   const eth::abi::contract* epoch_in_abi = nullptr;
   for (auto& c : abis) {
      if (c.name == "epochIn") { epoch_in_abi = &c; break; }
   }
   BOOST_REQUIRE(epoch_in_abi != nullptr);

   // Chunked delivery: (uint32 epochIndex, uint16 chunkIndex, uint16 totalChunks,
   //                    uint32 totalBytes, bytes chunkData)
   BOOST_REQUIRE_EQUAL(epoch_in_abi->inputs.size(), epoch_in_input_count);
   BOOST_CHECK(epoch_in_abi->inputs[0].type == eth::abi::data_type::uint32);
   BOOST_CHECK(epoch_in_abi->inputs[1].type == eth::abi::data_type::uint16);
   BOOST_CHECK(epoch_in_abi->inputs[2].type == eth::abi::data_type::uint16);
   BOOST_CHECK(epoch_in_abi->inputs[3].type == eth::abi::data_type::uint32);
   BOOST_CHECK(epoch_in_abi->inputs[4].type == eth::abi::data_type::bytes);

   // Encode the five chunk params — this is what the batch operator now does.
   std::string test_chunk_hex = "120c0a040800100012040800100028deeef5ce06300138";
   auto encoded = contract_encode_data(
      *epoch_in_abi,
      std::vector<fc::variant>{fc::variant(uint64_t{test_wire_epoch}),
                               fc::variant(uint64_t{0}),
                               fc::variant(uint64_t{1}),
                               fc::variant(uint64_t{test_chunk_hex.size() / hex_chars_per_byte}),
                               fc::variant(test_chunk_hex)});
   BOOST_CHECK(!encoded.empty());

   // keccak256("epochIn(uint32,uint16,uint16,uint32,bytes)")[0..4) — the old
   // single-`bytes` selector (0xcfae3118) is gone with the old signature.
   BOOST_CHECK_EQUAL(encoded.substr(0, evm_function_selector_hex_chars),
                     std::string(epoch_in_selector));
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
//  Chunked WIRE -> Ethereum envelope delivery
// ---------------------------------------------------------------------------

/// Pin the compiled chunk constants and the ceil-division that derives
/// `totalChunks`. These values are mirrored in wire-ethereum's
/// `OPPCommon.sol`; a silent drift on either side breaks every delivery, so
/// both sides carry an equivalent pin.
BOOST_AUTO_TEST_CASE(envelope_chunk_count_math) try {
   namespace chunking = sysio::outpost_ethereum_client_detail;

   BOOST_CHECK_EQUAL(sysio::ETHEREUM_MAX_CHUNK_BYTES, 8'192u);
   // Word-aligned so OPPInbound's staging-cell writes stay whole-word.
   BOOST_CHECK_EQUAL(sysio::ETHEREUM_MAX_CHUNK_BYTES % evm_abi_word_bytes, 0u);
   BOOST_CHECK_EQUAL(sysio::OPP_MAX_ENVELOPE_BYTES, 32'768u);

   BOOST_CHECK_EQUAL(chunking::chunk_count_for(1), 1u);
   BOOST_CHECK_EQUAL(chunking::chunk_count_for(sysio::ETHEREUM_MAX_CHUNK_BYTES), 1u);
   BOOST_CHECK_EQUAL(chunking::chunk_count_for(sysio::ETHEREUM_MAX_CHUNK_BYTES + 1), 2u);
   BOOST_CHECK_EQUAL(chunking::chunk_count_for(2 * sysio::ETHEREUM_MAX_CHUNK_BYTES), 2u);
   BOOST_CHECK_EQUAL(chunking::chunk_count_for(2 * sysio::ETHEREUM_MAX_CHUNK_BYTES + 1), 3u);
   BOOST_CHECK_EQUAL(chunking::chunk_count_for(3 * sysio::ETHEREUM_MAX_CHUNK_BYTES), 3u);
   // The platform cap is an exact multiple: four full chunks, no remainder.
   BOOST_CHECK_EQUAL(chunking::chunk_count_for(sysio::OPP_MAX_ENVELOPE_BYTES), 4u);
   BOOST_CHECK_EQUAL(sysio::OPP_MAX_ENVELOPE_BYTES % sysio::ETHEREUM_MAX_CHUNK_BYTES, 0u);

   // Ragged tail: the first chunks are exactly MAX_CHUNK_BYTES, the last is
   // the remainder.
   BOOST_CHECK_EQUAL(chunking::chunk_count_for(three_chunk_envelope_bytes), 3u);
   BOOST_CHECK_EQUAL(three_chunk_envelope_bytes - 2 * sysio::ETHEREUM_MAX_CHUNK_BYTES, 3'616u);

   // Unlike Solana — whose relay sends `chunks + 1` transactions because a
   // zero-data terminal call triggers finalization — Ethereum finalizes inline
   // on the chunk that completes the envelope, so the transaction count equals
   // the chunk count exactly. The delivery cases below pin that by counting
   // observed `epochIn` calls against `chunk_count_for`.
} FC_LOG_AND_RETHROW();

/// The resume decision table, exercised without an EVM node.
BOOST_AUTO_TEST_CASE(chunk_resume_decision_table) try {
   namespace chunking = sysio::outpost_ethereum_client_detail;
   using action = chunking::chunk_resume_action;

   // Same address, EIP-55 checksummed and all-lower-case — the ABI decoder
   // returns the latter, operators and tooling quote the former.
   const std::string self{"0xf39Fd6e51aad88F6F4ce6aB8827279cffFb92266"};
   const std::string self_lower{"0xf39fd6e51aad88f6f4ce6ab8827279cfffb92266"};
   const uint16_t    total_chunks = 4;
   const uint32_t    total_bytes  = sysio::OPP_MAX_ENVELOPE_BYTES;

   const auto decide = [&](const chunking::envelope_chunk_state& staged) {
      return chunking::decide_chunk_resume(staged, self, test_wire_epoch, total_chunks, total_bytes);
   };

   // Address comparison tolerates prefix and EIP-55 casing differences.
   BOOST_CHECK(chunking::same_evm_address(self, self_lower));
   BOOST_CHECK(chunking::same_evm_address(self, self.substr(hex_prefix.size())));
   BOOST_CHECK(!chunking::same_evm_address(self, foreign_operator_address));
   BOOST_CHECK(!chunking::same_evm_address("", self));

   // No header at all -> start at chunk 0.
   BOOST_CHECK(decide({}).action == action::start_fresh);
   BOOST_CHECK_EQUAL(decide({}).start_chunk, 0u);

   // A peer's header is not ours to continue or discard.
   chunking::envelope_chunk_state foreign{
      test_wire_epoch, std::string(foreign_operator_address), total_chunks, 2, total_bytes, 0};
   BOOST_CHECK(decide(foreign).action == action::start_fresh);
   BOOST_CHECK_EQUAL(decide(foreign).start_chunk, 0u);

   // Our own header, same epoch and shape -> resume at the high-water mark.
   chunking::envelope_chunk_state matching{test_wire_epoch, self, total_chunks, 2, total_bytes, 0};
   BOOST_CHECK(decide(matching).action == action::resume);
   BOOST_CHECK_EQUAL(decide(matching).start_chunk, 2u);

   // Same epoch, different shape -> discard the superseded cells, restart.
   chunking::envelope_chunk_state wrong_chunk_count = matching;
   wrong_chunk_count.total_chunks = total_chunks - 1;
   BOOST_CHECK(decide(wrong_chunk_count).action == action::discard_and_restart);

   chunking::envelope_chunk_state wrong_size = matching;
   wrong_size.total_bytes = total_bytes - 1;
   BOOST_CHECK(decide(wrong_size).action == action::discard_and_restart);

   // The final chunk is never stored, so receivedChunks can never reach
   // totalChunks; a header claiming otherwise is unusable.
   chunking::envelope_chunk_state over_received = matching;
   over_received.received_chunks = total_chunks;
   BOOST_CHECK(decide(over_received).action == action::discard_and_restart);

   // Our own header for a different epoch -> confirm against nextEpochIndex.
   chunking::envelope_chunk_state stale = matching;
   stale.epoch_index = test_stale_wire_epoch;
   BOOST_CHECK(decide(stale).action == action::confirm_epoch_advanced);
   BOOST_CHECK_EQUAL(decide(stale).start_chunk, 0u);
} FC_LOG_AND_RETHROW();

/// Guard rails before any transaction is signed.
BOOST_AUTO_TEST_CASE(delivery_rejects_empty_and_over_cap_envelopes) try {
   auto fixture = create_chunked_delivery_fixture();

   BOOST_CHECK_THROW(fixture->outpost->deliver_outbound_envelope(
                        test_wire_epoch, {}, fc::seconds(test_rpc_deadline_seconds)),
                     fc::assert_exception);

   std::vector<char> over_cap(sysio::OPP_MAX_ENVELOPE_BYTES + 1, oversized_envelope_fill_byte);
   BOOST_CHECK_THROW(fixture->outpost->deliver_outbound_envelope(
                        test_wire_epoch, over_cap, fc::seconds(test_rpc_deadline_seconds)),
                     fc::assert_exception);

   BOOST_CHECK_EQUAL(fixture->chunk_calls.size(), 0u);
   BOOST_CHECK_EQUAL(fixture->chunk_state_reads, 0u);
} FC_LOG_AND_RETHROW();

/// The dominant case: an envelope at or below one chunk is a single
/// transaction that stages nothing, so the resume read is skipped entirely.
BOOST_AUTO_TEST_CASE(single_chunk_delivery_skips_the_resume_read) try {
   auto fixture  = create_chunked_delivery_fixture();
   auto envelope = make_chunked_envelope(sysio::ETHEREUM_MAX_CHUNK_BYTES);

   const auto tx = fixture->outpost->deliver_outbound_envelope(
      test_wire_epoch, envelope, fc::seconds(test_rpc_deadline_seconds));

   BOOST_CHECK(!tx.empty());
   BOOST_CHECK_EQUAL(fixture->chunk_state_reads, 0u);
   BOOST_CHECK_EQUAL(fixture->discard_calls, 0u);
   BOOST_CHECK_EQUAL(fixture->next_epoch_reads, 0u);
   check_chunk_call_sequence(fixture->chunk_calls, envelope, test_wire_epoch, 0);
} FC_LOG_AND_RETHROW();

/// A multi-chunk delivery with nothing staged sends every chunk in order, each
/// exactly MAX_CHUNK_BYTES but the last.
BOOST_AUTO_TEST_CASE(multi_chunk_delivery_sends_every_chunk_in_order) try {
   auto fixture  = create_chunked_delivery_fixture();
   auto envelope = make_chunked_envelope(three_chunk_envelope_bytes);

   fixture->outpost->deliver_outbound_envelope(
      test_wire_epoch, envelope, fc::seconds(test_rpc_deadline_seconds));

   BOOST_CHECK_EQUAL(fixture->chunk_state_reads, 1u);
   BOOST_CHECK_EQUAL(fixture->discard_calls, 0u);
   check_chunk_call_sequence(fixture->chunk_calls, envelope, test_wire_epoch, 0);
} FC_LOG_AND_RETHROW();

/// Resume: an own, matching header advances the start index to the on-chain
/// high-water mark. Without this a tick whose budget covers k transactions
/// re-sends chunks 0..k-1 forever and never reaches chunk k.
BOOST_AUTO_TEST_CASE(multi_chunk_delivery_resumes_from_the_staged_high_water_mark) try {
   auto fixture  = create_chunked_delivery_fixture();
   auto envelope = make_chunked_envelope(three_chunk_envelope_bytes);

   constexpr uint16_t staged_chunks = 2;
   fixture->chunk_state_response = encode_envelope_chunk_state_result(
      test_wire_epoch,
      fixture->outpost->signer_address_hex(),
      sysio::outpost_ethereum_client_detail::chunk_count_for(envelope.size()),
      staged_chunks,
      static_cast<uint32_t>(envelope.size()),
      static_cast<uint64_t>(staged_chunks) * sysio::ETHEREUM_MAX_CHUNK_BYTES);

   fixture->outpost->deliver_outbound_envelope(
      test_wire_epoch, envelope, fc::seconds(test_rpc_deadline_seconds));

   BOOST_CHECK_EQUAL(fixture->chunk_state_reads, 1u);
   BOOST_CHECK_EQUAL(fixture->discard_calls, 0u);
   check_chunk_call_sequence(fixture->chunk_calls, envelope, test_wire_epoch, staged_chunks);
} FC_LOG_AND_RETHROW();

/// `storedBytes` is a uint256 the relay only logs. A value beyond the uint64
/// range must NOT invalidate the header: doing so would drop back to chunk 0
/// and re-upload cells the outpost already holds.
BOOST_AUTO_TEST_CASE(multi_chunk_delivery_resumes_despite_an_oversized_stored_bytes_field) try {
   auto fixture  = create_chunked_delivery_fixture();
   auto envelope = make_chunked_envelope(three_chunk_envelope_bytes);

   constexpr uint16_t staged_chunks = 2;
   fixture->chunk_state_response = encode_envelope_chunk_state_result(
      test_wire_epoch,
      fixture->outpost->signer_address_hex(),
      sysio::outpost_ethereum_client_detail::chunk_count_for(envelope.size()),
      staged_chunks,
      static_cast<uint32_t>(envelope.size()),
      /*stored_bytes=*/0,
      stored_bytes_above_uint64_word());

   fixture->outpost->deliver_outbound_envelope(
      test_wire_epoch, envelope, fc::seconds(test_rpc_deadline_seconds));

   BOOST_CHECK_EQUAL(fixture->discard_calls, 0u);
   check_chunk_call_sequence(fixture->chunk_calls, envelope, test_wire_epoch, staged_chunks);
} FC_LOG_AND_RETHROW();

/// Mid-sequence deadline expiry — the scenario resume exists for.
///
/// The first chunk burns more wall clock than the whole delivery budget, so the
/// SECOND chunk's pre-flight `throw_if_past_deadline` fires: the tick abandons
/// after a PARTIAL, correctly-ordered prefix rather than rolling back. The next
/// cron tick then resumes from the on-chain high-water mark instead of
/// re-sending chunk 0 forever (the livelock this design removes).
BOOST_AUTO_TEST_CASE(multi_chunk_delivery_abandons_on_deadline_then_resumes_next_tick) try {
   auto fixture  = create_chunked_delivery_fixture();
   auto envelope = make_chunked_envelope(three_chunk_envelope_bytes);
   BOOST_REQUIRE_EQUAL(
      sysio::outpost_ethereum_client_detail::chunk_count_for(envelope.size()), 3u);

   constexpr int64_t delivery_budget_ms = 50;
   fixture->first_chunk_delay = std::chrono::milliseconds{delivery_budget_ms * 4};

   BOOST_CHECK_THROW(fixture->outpost->deliver_outbound_envelope(
                        test_wire_epoch, envelope, fc::milliseconds(delivery_budget_ms)),
                     fc::timeout_exception);

   // Exactly one chunk landed, and it is a correct prefix of the envelope.
   BOOST_REQUIRE_EQUAL(fixture->chunk_calls.size(), 1u);
   BOOST_CHECK_EQUAL(fixture->chunk_calls[0].chunk_index, 0u);
   BOOST_CHECK_EQUAL(fixture->chunk_calls[0].chunk_hex,
                     fc::to_hex(envelope.data(), sysio::ETHEREUM_MAX_CHUNK_BYTES));

   // Next tick: the outpost reports the one staged chunk, and the relay picks
   // up at chunk 1 rather than restarting.
   fixture->chunk_calls.clear();
   fixture->first_chunk_delay = std::chrono::milliseconds{0};
   fixture->chunk_state_response = encode_envelope_chunk_state_result(
      test_wire_epoch,
      fixture->outpost->signer_address_hex(),
      sysio::outpost_ethereum_client_detail::chunk_count_for(envelope.size()),
      1,
      static_cast<uint32_t>(envelope.size()),
      sysio::ETHEREUM_MAX_CHUNK_BYTES);

   fixture->outpost->deliver_outbound_envelope(
      test_wire_epoch, envelope, fc::seconds(test_rpc_deadline_seconds));

   BOOST_CHECK_EQUAL(fixture->discard_calls, 0u);
   check_chunk_call_sequence(fixture->chunk_calls, envelope, test_wire_epoch, 1);
} FC_LOG_AND_RETHROW();

/// A staging header owned by a peer is never adopted and never discarded — the
/// delivery simply starts at chunk 0 and lets the contract's ownership guard
/// arbitrate.
BOOST_AUTO_TEST_CASE(multi_chunk_delivery_ignores_a_peer_owned_header) try {
   auto fixture  = create_chunked_delivery_fixture();
   auto envelope = make_chunked_envelope(three_chunk_envelope_bytes);

   fixture->chunk_state_response = encode_envelope_chunk_state_result(
      test_wire_epoch,
      foreign_operator_address,
      sysio::outpost_ethereum_client_detail::chunk_count_for(envelope.size()),
      2,
      static_cast<uint32_t>(envelope.size()),
      0);

   fixture->outpost->deliver_outbound_envelope(
      test_wire_epoch, envelope, fc::seconds(test_rpc_deadline_seconds));

   BOOST_CHECK_EQUAL(fixture->discard_calls, 0u);
   BOOST_CHECK_EQUAL(fixture->next_epoch_reads, 0u);
   check_chunk_call_sequence(fixture->chunk_calls, envelope, test_wire_epoch, 0);
} FC_LOG_AND_RETHROW();

/// A CURRENT-epoch header describing a different envelope is discarded first,
/// then the upload restarts from chunk 0.
BOOST_AUTO_TEST_CASE(multi_chunk_delivery_discards_a_superseded_current_epoch_header) try {
   auto fixture  = create_chunked_delivery_fixture();
   auto envelope = make_chunked_envelope(three_chunk_envelope_bytes);

   fixture->chunk_state_response = encode_envelope_chunk_state_result(
      test_wire_epoch,
      fixture->outpost->signer_address_hex(),
      sysio::outpost_ethereum_client_detail::chunk_count_for(envelope.size()),
      1,
      static_cast<uint32_t>(envelope.size()) - 1,  // a different envelope, same epoch
      sysio::ETHEREUM_MAX_CHUNK_BYTES);

   fixture->outpost->deliver_outbound_envelope(
      test_wire_epoch, envelope, fc::seconds(test_rpc_deadline_seconds));

   BOOST_CHECK_EQUAL(fixture->discard_calls, 1u);
   check_chunk_call_sequence(fixture->chunk_calls, envelope, test_wire_epoch, 0);
} FC_LOG_AND_RETHROW();

/// A reverting discard means the header is ALREADY clear (the contract's
/// `OPP_ChunkBufferMissing`), which is exactly the state the relay wanted:
/// log it and upload from chunk 0. Only transport failures abandon the tick.
BOOST_AUTO_TEST_CASE(multi_chunk_delivery_treats_a_reverting_discard_as_already_clear) try {
   auto fixture  = create_chunked_delivery_fixture();
   auto envelope = make_chunked_envelope(three_chunk_envelope_bytes);

   fixture->chunk_state_response = encode_envelope_chunk_state_result(
      test_wire_epoch,
      fixture->outpost->signer_address_hex(),
      sysio::outpost_ethereum_client_detail::chunk_count_for(envelope.size()),
      1,
      static_cast<uint32_t>(envelope.size()) - 1,
      sysio::ETHEREUM_MAX_CHUNK_BYTES);
   fixture->discard_failure = fc::network::json_rpc::json_rpc_error(
      contract_revert_rpc_code, "execution reverted: OPP_ChunkBufferMissing", fc::variant{});

   fixture->outpost->deliver_outbound_envelope(
      test_wire_epoch, envelope, fc::seconds(test_rpc_deadline_seconds));

   BOOST_CHECK_EQUAL(fixture->discard_calls, 1u);
   check_chunk_call_sequence(fixture->chunk_calls, envelope, test_wire_epoch, 0);
} FC_LOG_AND_RETHROW();

/// A stale OWN header on a consensus retry is the signature of an epoch that
/// advanced underneath us. One `nextEpochIndex()` read confirms it, and the
/// delivery is abandoned before a single transaction is signed — otherwise the
/// retry pays for up to `totalChunks` late no-ops.
BOOST_AUTO_TEST_CASE(multi_chunk_delivery_skips_when_the_outpost_epoch_advanced) try {
   auto fixture  = create_chunked_delivery_fixture();
   auto envelope = make_chunked_envelope(three_chunk_envelope_bytes);

   // A header epoch differing from the delivery epoch triggers the
   // confirmation read.
   fixture->chunk_state_response = encode_envelope_chunk_state_result(
      test_stale_wire_epoch,
      fixture->outpost->signer_address_hex(),
      sysio::outpost_ethereum_client_detail::chunk_count_for(envelope.size()),
      1,
      static_cast<uint32_t>(envelope.size()),
      sysio::ETHEREUM_MAX_CHUNK_BYTES);
   fixture->next_epoch_index_response = encode_next_epoch_index_result(test_different_wire_epoch);

   const auto tx = fixture->outpost->deliver_outbound_envelope(
      test_wire_epoch, envelope, fc::seconds(test_rpc_deadline_seconds));

   BOOST_CHECK(tx.empty());
   BOOST_CHECK_EQUAL(fixture->next_epoch_reads, 1u);
   BOOST_CHECK_EQUAL(fixture->chunk_calls.size(), 0u);
   BOOST_CHECK_EQUAL(fixture->discard_calls, 0u);
} FC_LOG_AND_RETHROW();

/// The same stale-header shape when the outpost has NOT advanced: the epoch is
/// still deliverable, so the upload proceeds from chunk 0.
BOOST_AUTO_TEST_CASE(multi_chunk_delivery_proceeds_when_the_outpost_epoch_has_not_advanced) try {
   auto fixture  = create_chunked_delivery_fixture();
   auto envelope = make_chunked_envelope(three_chunk_envelope_bytes);

   fixture->chunk_state_response = encode_envelope_chunk_state_result(
      test_stale_wire_epoch,
      fixture->outpost->signer_address_hex(),
      sysio::outpost_ethereum_client_detail::chunk_count_for(envelope.size()),
      1,
      static_cast<uint32_t>(envelope.size()),
      sysio::ETHEREUM_MAX_CHUNK_BYTES);
   fixture->next_epoch_index_response = encode_next_epoch_index_result(test_wire_epoch);

   fixture->outpost->deliver_outbound_envelope(
      test_wire_epoch, envelope, fc::seconds(test_rpc_deadline_seconds));

   BOOST_CHECK_EQUAL(fixture->next_epoch_reads, 1u);
   check_chunk_call_sequence(fixture->chunk_calls, envelope, test_wire_epoch, 0);
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
      .max_gas_limit = fc::uint256{eip_7825_tx_gas_cap},
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

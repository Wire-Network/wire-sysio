#include <gsl-lite/gsl-lite.hpp>

#include <boost/test/unit_test.hpp>
#include <boost/dll.hpp>
#include <boost/process/v1/io.hpp>


#include <atomic>
#include <chrono>
#include <thread>
#include <optional>
#include <set>

#include <fc/crypto/ethereum/ethereum_types.hpp>
#include <fc/crypto/ethereum/ethereum_utils.hpp>
#include <fc/network/ethereum/ethereum_client.hpp>
#include <fc/network/ethereum/ethereum_abi.hpp>
#include <fc/network/ethereum/ethereum_rlp_encoder.hpp>

#include <sysio/chain/types.hpp>
#include <sysio/signature_provider_manager_plugin/signature_provider_manager_plugin.hpp>
#include <fc-test/build_info.hpp>
#include <fc-test/crypto_utils.hpp>

#include <sysio/outpost_ethereum_client_plugin.hpp>

using namespace std::literals;

using namespace fc::crypto;
using namespace fc::crypto::ethereum;
using namespace fc::network::ethereum;

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

BOOST_AUTO_TEST_SUITE(outpost_ethereum_client_plugin)

BOOST_AUTO_TEST_CASE(can_encode_tx_01) try {
   using namespace fc::crypto;

   auto              empty_msg_hash = fc::crypto::ethereum::hash_message("");
   std::stringstream ss;
   for (auto byte : empty_msg_hash) {
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

   auto msg_hash_data = fc::crypto::ethereum::hash_message(actual_unsigned);
   // auto msg_hash_data = fc::crypto::ethereum::hash_message(sample_data_01_raw);
   // auto msg_hash_data = fc::crypto::keccak256_ethereum(actual_unsigned);
   fc::sha256 msg_hash(reinterpret_cast<const char*>(msg_hash_data.data()), msg_hash_data.size());

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
   auto sig = sig_provider->sign(msg_hash);
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

BOOST_AUTO_TEST_SUITE_END()
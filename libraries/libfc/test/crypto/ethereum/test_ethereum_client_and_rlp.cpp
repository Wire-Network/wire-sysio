#include <atomic>
#include <chrono>
#include <thread>
#include <optional>
#include <set>
#include <format>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include <gsl-lite/gsl-lite.hpp>

#include <boost/test/unit_test.hpp>
#include <boost/dll.hpp>
#include <boost/process/v1/spawn.hpp>
#include <boost/process/v1/io.hpp>

#include <fc/crypto/chain_types_reflect.hpp>
#include <fc/crypto/elliptic_ed.hpp>
#include <fc/crypto/elliptic_em.hpp>
#include <fc/crypto/ethereum/ethereum_utils.hpp>
#include <fc/crypto/hex.hpp>
#include <fc/crypto/rand.hpp>
#include <fc/io/fstream.hpp>
#include <fc/io/json.hpp>
#include <fc/crypto/private_key.hpp>
#include <fc/crypto/public_key.hpp>
#include <fc/crypto/sha256.hpp>
#include <fc/crypto/signature_provider.hpp>
#include <fc/crypto/ethereum/ethereum_types.hpp>
#include <fc/network/ethereum/ethereum_abi.hpp>
#include <fc/network/ethereum/ethereum_rlp_encoder.hpp>

namespace sysio {
class signature_provider_manager_plugin;
}

using namespace std::literals;
using namespace fc::crypto;
using namespace fc::crypto::ethereum;
using namespace fc::network::ethereum;

namespace {
/* RLP encoding test data 01 */
std::pair<std::string, std::string> test_str_01{"test123", "c88774657374313233"};

/* RLP vector of encoding tests */
std::vector<std::pair<std::string, std::string>> test_str_pairs{
   test_str_01
};

std::string test_tx_01_sig {"setNumber(uint256)"};
std::vector<std::string> test_tx_01_sig_params {"60"};
std::string test_tx_01_sig_encoded {"3fb5c1cb000000000000000000000000000000000000000000000000000000000000003c"};

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
std::vector<std::uint8_t> test_tx_01_unsigned_result {
   0x02, 0xf8, 0x4e, 0x82, 0x7a, 0x69, 0x0d, 0x84, 0x77, 0x35, 0x94, 0x00,
   0x84, 0x77, 0x37, 0x20, 0x80, 0x83, 0x01, 0x8c, 0x80, 0x94, 0x5f, 0xbd,
   0xb2, 0x31, 0x56, 0x78, 0xaf, 0xec, 0xb3, 0x67, 0xf0, 0x32, 0xd9, 0x3f,
   0x64, 0x2f, 0x64, 0x18, 0x0a, 0xa3, 0x80, 0xa4, 0x3f, 0xb5, 0xc1, 0xcb,
   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3c, 0xc0
 };


[[maybe_unused]] std::string test_tx_01_r = "93166a3ed10a4050dce7261c4ca8bcba16a1731117c453a326a1742c959b33f0";
[[maybe_unused]] std::string test_tx_01_s = "7c17a232cd69ce93f21a30579a2a94309b2d71918043134b4c5df5788078a0e4";
[[maybe_unused]] fc::uint256 test_tx_01_v = 0;

//noinspection SpellCheckingInspection
std::string test_tx_01_result = "02f84e827a690d8477359400847737208083018c80945fbdb2315678afecb367f032d93f642f64180aa380a43fb5c1cb000000000000000000000000000000000000000000000000000000000000003cc0";

}

BOOST_AUTO_TEST_SUITE(rlp_encoder)

BOOST_AUTO_TEST_CASE(can_encode_list_of_strings) try {
   for (auto& [input,expected] : test_str_pairs) {
      auto actual     = rlp::encode_list({rlp::encode_string(input)});
      auto actual_hex = rlp::to_hex(actual, false);
      BOOST_CHECK_EQUAL(actual_hex, expected);
   }
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(can_encode_call_sig) try {
   auto encoded_call_sig = ethereum_contract_call_encode(test_tx_01_sig, test_tx_01_sig_params);
   BOOST_CHECK(encoded_call_sig == test_tx_01_sig_encoded);
} FC_LOG_AND_RETHROW();


BOOST_AUTO_TEST_CASE(can_encode_tx_01) try {
   using namespace fc::crypto;

   auto empty_msg_hash = fc::crypto::ethereum::hash_message("");
   std::stringstream ss;
   for (auto byte : empty_msg_hash) {
      ss << std::hex << std::setfill('0') << std::setw(2)
                << static_cast<unsigned>(byte);
   }
   // auto empty_msg_hash_hex = fc::to_hex(reinterpret_cast<const char*>(empty_msg_hash.data()), empty_msg_hash.size());
   auto empty_msg_hash_hex = ss.str();
   BOOST_CHECK("c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470" == empty_msg_hash_hex);

   auto actual_unsigned     = rlp::encode_eip1559_unsigned_typed(test_tx_01);

   BOOST_CHECK(std::memcmp(actual_unsigned.data(), test_tx_01_unsigned_result.data(), 81) == 0);
   auto actual_unsigned_hex = rlp::to_hex(actual_unsigned, false);
   BOOST_CHECK_EQUAL(actual_unsigned_hex, test_tx_01_result);
} FC_LOG_AND_RETHROW();
BOOST_AUTO_TEST_SUITE_END()
#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <optional>
#include <set>
#include <fc/crypto/hex.hpp>

#include <sysio/outpost_client/ethereum/types.hpp>
#include <sysio/outpost_client/ethereum/rlp_encoder.hpp>

using namespace std::literals;
using namespace sysio::outpost_client;
using namespace sysio::outpost_client::ethereum;

namespace {
std::pair<std::string, std::string> test_str_01{"test123", "c88774657374313233"};

std::vector<std::pair<std::string, std::string>> test_str_pairs{
   test_str_01
};

eip1559_tx test_tx_01{
   .chain_id = 31337,
   .nonce = 11,
   .max_priority_fee_per_gas = 2000000000,
   .max_fee_per_gas = 2000101504,
   .gas_limit = 0x18c80,
   .to = fc::from_hex("5FbDB2315678afecb367f032d93F642f64180aa3"),
   .value = 0,
   .data = fc::from_hex("3fb5c1cb000000000000000000000000000000000000000000000000000000000000003c"),
   .access_list = {}

};
std::string test_tx_01_result =
   "02f84e827a690b8477359400847737208083018c80945fbdb2315678afecb367f032d93f642f64180aa380a43fb5c1cb000000000000000000000000000000000000000000000000000000000000003cc0";

}

BOOST_AUTO_TEST_SUITE(rlp_encoder)

BOOST_AUTO_TEST_CASE(can_encode_list_of_strings) try {
   for (auto [input,expected] : test_str_pairs) {
      auto actual     = rlp::encode_list({rlp::encode_string(input)});
      auto actual_hex = rlp::to_hex(actual, false);
      BOOST_CHECK_EQUAL(actual_hex, expected);
   }
} FC_LOG_AND_RETHROW();


BOOST_AUTO_TEST_CASE(can_encode_tx_01) try {
   auto actual     = rlp::encode_eip1559_unsigned_typed(test_tx_01);
   auto actual_hex = rlp::to_hex(actual, false);
   BOOST_CHECK_EQUAL(actual_hex, test_tx_01_result);
} FC_LOG_AND_RETHROW();
BOOST_AUTO_TEST_SUITE_END()
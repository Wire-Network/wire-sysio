#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <optional>
#include <ranges>
#include <set>
#include <fc/int256.hpp>
#include <fc/crypto/blake2.hpp>
#include <fc/exception/exception.hpp>

#include <sysio/outpost_client/ethereum/rlp_encoder.hpp>

using namespace std::literals;
using namespace sysio::outpost_client::ethereum;

namespace {}

BOOST_AUTO_TEST_SUITE(rlp_encoder)

BOOST_AUTO_TEST_CASE(encode_be_numbers_to_bytes) try {
   std::vector<fc::uint256> values{
      0x01,
      0x0002,
      0x2000,
      0x0000000000000003,
      0x3000000000000000
   };
   // TODO: Write proper tests
   auto encoded_values = values | std::views::transform([](const auto& v) { return rlp::encode_uint(v); })
                         | std::ranges::to<std::vector>();


   BOOST_CHECK(encoded_values.size() == values.size());

   eip1559_tx tx{
      .chain_id = 1,
      .nonce = 1,
      .max_priority_fee_per_gas = 1,
      .max_fee_per_gas = 1,
      .gas_limit = 1,
      .to = {},
      .value = 1,
      .data = {},
      .access_list = {}
   };
} FC_LOG_AND_RETHROW();


BOOST_AUTO_TEST_SUITE_END()
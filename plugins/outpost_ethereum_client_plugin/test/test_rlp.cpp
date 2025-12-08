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

   auto encoded_values = values | std::views::transform([](const auto& v) { return rlp::encode_uint(v); })
                         | std::ranges::to<std::vector<bytes>>();


   BOOST_CHECK(encoded_values.size() == values.size());
} FC_LOG_AND_RETHROW();


BOOST_AUTO_TEST_SUITE_END()
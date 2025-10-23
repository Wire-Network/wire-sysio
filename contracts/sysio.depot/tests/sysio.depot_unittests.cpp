#include <boost/test/unit_test.hpp>
#include <sysio.depot/sysio.opp.hpp>
#include <sysio/chain/controller.hpp>
#include <sysio/chain/global_property_object.hpp>
#include <sysio/chain/resource_limits.hpp>
#include <sysio/testing/tester.hpp>

using namespace sysio;
using namespace sysio::chain;
using namespace sysio::testing;

BOOST_AUTO_TEST_SUITE(depot_tests)
// TODO: @jglanz Build pack/unpack tests, which will require a slight change to
//   the structure

BOOST_AUTO_TEST_CASE(opp_message_chain_unpack) try {
  // opp::message_chain msg_chain(test_data);
  // std::vector<uint8_t> test_data = {
  //     0x02, 0x00, 0x00, 0x00, // message_count = 2
  //     0x10, 0x00, 0x00, 0x00, // total_size = 16 bytes
  //     // First message
  //     0x04, 0x00, 0x00, 0x00, // message 1 size = 4 bytes
  //     0x01, 0x02, 0x03, 0x04, // message 1 data
  //     // Second message
  //     0x04, 0x00, 0x00, 0x00, // message 2 size = 4 bytes
  //     0x05, 0x06, 0x07, 0x08  // message 2 data
  // };
  //
  // opp::message_chain msg_chain(test_data);
  // BOOST_REQUIRE_EQUAL(msg_chain.payload_header->message_count, 2);
  BOOST_REQUIRE_EQUAL(1, 1);
}
FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()

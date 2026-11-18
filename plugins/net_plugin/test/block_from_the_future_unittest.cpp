#include <boost/test/unit_test.hpp>
#include <sysio/net_plugin/net_utils.hpp>

using namespace sysio;
using namespace sysio::net_utils;

namespace {

/// Local clock reading the received blocks are checked against. Falls on a block slot boundary.
const fc::time_point now = fc::time_point::from_iso_string("2026-01-01T00:00:00.000");

/// Duration of one block slot.
constexpr fc::microseconds block_interval = fc::milliseconds(chain::config::block_interval_ms);

/// Header of a block timestamped `offset` from `now`.
chain::block_header header_at(fc::microseconds offset) {
   chain::block_header header;
   header.timestamp = chain::block_timestamp_type{now + offset};
   return header;
}

/// Header of the earliest block that is from the future.
chain::block_header future_header() {
   return header_at(chain::config::allowed_clock_skew + block_interval);
}

} // namespace

BOOST_AUTO_TEST_SUITE(block_from_the_future)

// A node with finalizer keys and no producer rejects a block from the future.
BOOST_AUTO_TEST_CASE(finalizer_node_rejects_future_block) {
   constexpr bool producer_node  = false;
   constexpr bool finalizer_node = true;
   BOOST_CHECK(rejects_block_from_the_future(producer_node, finalizer_node, future_header(), now));
}

// A producer node rejects a block from the future.
BOOST_AUTO_TEST_CASE(producer_node_rejects_future_block) {
   constexpr bool producer_node  = true;
   constexpr bool finalizer_node = false;
   BOOST_CHECK(rejects_block_from_the_future(producer_node, finalizer_node, future_header(), now));
}

// A node without producers or finalizer keys accepts and relays a block from the future.
BOOST_AUTO_TEST_CASE(other_node_accepts_future_block) {
   constexpr bool producer_node  = false;
   constexpr bool finalizer_node = false;
   BOOST_CHECK(!rejects_block_from_the_future(producer_node, finalizer_node, future_header(), now));
}

// A block ahead of the clock by up to the allowed clock skew is accepted.
BOOST_AUTO_TEST_CASE(block_within_clock_skew_accepted) {
   constexpr bool producer_node  = true;
   constexpr bool finalizer_node = true;
   BOOST_CHECK(!rejects_block_from_the_future(producer_node, finalizer_node, header_at(fc::microseconds{}), now));
   BOOST_CHECK(!rejects_block_from_the_future(producer_node, finalizer_node, header_at(block_interval), now));
   BOOST_CHECK(!rejects_block_from_the_future(producer_node, finalizer_node,
                                              header_at(chain::config::allowed_clock_skew), now));
}

// A block behind the clock is accepted.
BOOST_AUTO_TEST_CASE(past_block_accepted) {
   constexpr bool producer_node  = true;
   constexpr bool finalizer_node = true;
   BOOST_CHECK(!rejects_block_from_the_future(producer_node, finalizer_node, header_at(fc::minutes(-1)), now));
}

BOOST_AUTO_TEST_SUITE_END()

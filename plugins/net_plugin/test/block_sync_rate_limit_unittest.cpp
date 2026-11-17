#include <sysio/net_plugin/block_sync_rate_limit.hpp>

#include <boost/test/unit_test.hpp>

#include <limits>

namespace {

using namespace std::chrono_literals;
using sysio::block_sync_rate_limit_reached;

constexpr size_t sync_rate = 20'000; ///< A 20 KB/s cap, in bytes per second.

BOOST_AUTO_TEST_SUITE(block_sync_rate_limit_tests)

/** The cap applies to bytes per full second, including equality and multi-second frames. */
BOOST_AUTO_TEST_CASE(seconds_and_boundaries) {
   BOOST_TEST(!block_sync_rate_limit_reached(sync_rate - 1, 1s, sync_rate));
   BOOST_TEST(block_sync_rate_limit_reached(sync_rate, 1s, sync_rate));
   BOOST_TEST(block_sync_rate_limit_reached(sync_rate + 1, 1s, sync_rate));
   BOOST_TEST(!block_sync_rate_limit_reached(sync_rate * 2 - 1, 2s, sync_rate));
   BOOST_TEST(block_sync_rate_limit_reached(sync_rate * 2, 2s, sync_rate));
}

/** Subsecond comparisons retain precision instead of scaling the measured byte rate down by ten. */
BOOST_AUTO_TEST_CASE(subsecond_intervals) {
   BOOST_TEST(!block_sync_rate_limit_reached(sync_rate / 2 - 1, 500ms, sync_rate));
   BOOST_TEST(block_sync_rate_limit_reached(sync_rate / 2, 500ms, sync_rate));
   BOOST_TEST(block_sync_rate_limit_reached(sync_rate, 500ms, sync_rate));
   BOOST_TEST(block_sync_rate_limit_reached(1, 50us, sync_rate));
   BOOST_TEST(!block_sync_rate_limit_reached(1, 50us + 1ns, sync_rate));
   constexpr size_t gigabyte_per_second = 1'000'000'000;
   BOOST_TEST(block_sync_rate_limit_reached(1, 1ns, gigabyte_per_second));
   BOOST_TEST(!block_sync_rate_limit_reached(1, 2ns, gigabyte_per_second));
}

/** Unlimited connections and the initial block of a frame are never throttled. */
BOOST_AUTO_TEST_CASE(unlimited_and_empty_frames) {
   constexpr size_t largest = std::numeric_limits<size_t>::max();
   BOOST_TEST(!block_sync_rate_limit_reached(largest, 1ns, 0));
   BOOST_TEST(!block_sync_rate_limit_reached(largest, 0ns, 0));
   BOOST_TEST(!block_sync_rate_limit_reached(largest, -1ns, 0));
   BOOST_TEST(!block_sync_rate_limit_reached(0, 1s, sync_rate));
   BOOST_TEST(!block_sync_rate_limit_reached(0, 0ns, sync_rate));
   BOOST_TEST(!block_sync_rate_limit_reached(0, -1ns, sync_rate));
}

/** A frame that has sent bytes waits when the clock has not advanced, without dividing by zero. */
BOOST_AUTO_TEST_CASE(nonpositive_elapsed_time) {
   BOOST_TEST(block_sync_rate_limit_reached(1, 0ns, sync_rate));
   BOOST_TEST(block_sync_rate_limit_reached(1, -1ns, sync_rate));
   BOOST_TEST(block_sync_rate_limit_reached(1, -1s, sync_rate));
}

/** Products at the largest supported cap and elapsed interval must not wrap. */
BOOST_AUTO_TEST_CASE(large_products) {
   constexpr size_t largest = std::numeric_limits<size_t>::max();
   BOOST_TEST(block_sync_rate_limit_reached(largest, 1s, largest));
   BOOST_TEST(!block_sync_rate_limit_reached(largest, 1s + 1ns, largest));
   BOOST_TEST(block_sync_rate_limit_reached(largest / 2 + 1, 500ms, largest));
   BOOST_TEST(!block_sync_rate_limit_reached(largest, std::chrono::nanoseconds::max(), largest));
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace

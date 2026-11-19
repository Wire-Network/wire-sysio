#include <boost/test/unit_test.hpp>
#include <sysio/net_plugin/message_rate_limiter.hpp>

#include <chrono>

using sysio::message_rate_limiter;
using clock_type = message_rate_limiter::clock;

namespace {

/// Burst of the limiters these tests make
constexpr uint32_t test_burst = 3;
/// Refill interval of the limiters these tests make
constexpr clock_type::duration test_interval = std::chrono::seconds(5);
/// Time the tests start at
const clock_type::time_point start = clock_type::time_point{} + std::chrono::hours(1);

/// Take burst messages from limiter at now, each within the limit.
void take_burst(message_rate_limiter& limiter, uint32_t burst, clock_type::time_point now) {
   for (uint32_t n = 0; n < burst; ++n)
      BOOST_REQUIRE(limiter.try_consume(now));
}

} // namespace

BOOST_AUTO_TEST_SUITE(message_rate_limiter_tests)

BOOST_AUTO_TEST_CASE(burst_then_refused) {
   message_rate_limiter limiter(test_burst, test_interval);
   BOOST_CHECK_EQUAL(limiter.available(), test_burst);
   take_burst(limiter, test_burst, start);
   BOOST_CHECK_EQUAL(limiter.available(), 0u);
   BOOST_CHECK(!limiter.try_consume(start));
   BOOST_CHECK(!limiter.try_consume(start + test_interval - std::chrono::nanoseconds(1)));
   BOOST_CHECK_EQUAL(limiter.available(), 0u);
}

BOOST_AUTO_TEST_CASE(one_message_per_interval) {
   message_rate_limiter limiter(test_burst, test_interval);
   take_burst(limiter, test_burst, start);

   // one message returns each interval, and the time short of a whole interval carries to the next
   const clock_type::time_point first = start + test_interval + test_interval / 2;
   BOOST_CHECK(limiter.try_consume(first));
   BOOST_CHECK(!limiter.try_consume(first));
   BOOST_CHECK(limiter.try_consume(start + 2 * test_interval));
   BOOST_CHECK(!limiter.try_consume(start + 2 * test_interval));

   // a refused message takes nothing from what returns next
   BOOST_CHECK(limiter.try_consume(start + 3 * test_interval));
}

BOOST_AUTO_TEST_CASE(refill_stops_at_burst) {
   message_rate_limiter limiter(test_burst, test_interval);
   take_burst(limiter, test_burst, start);

   // a long wait returns no more than burst
   const clock_type::time_point later = start + 100 * test_interval;
   take_burst(limiter, test_burst, later);
   BOOST_CHECK(!limiter.try_consume(later));

   // time spent full gains nothing: after a long idle period, the burst is refilled one interval at a time
   message_rate_limiter idle(test_burst, test_interval);
   take_burst(idle, test_burst, later);
   BOOST_CHECK(!idle.try_consume(later));
   BOOST_CHECK(idle.try_consume(later + test_interval));
   BOOST_CHECK(!idle.try_consume(later + test_interval));
}

BOOST_AUTO_TEST_CASE(earlier_time_returns_nothing) {
   message_rate_limiter limiter(test_burst, test_interval);
   take_burst(limiter, test_burst, start);
   BOOST_CHECK(!limiter.try_consume(start - 10 * test_interval));
   BOOST_CHECK(limiter.try_consume(start + test_interval));
}

BOOST_AUTO_TEST_CASE(reset_fills) {
   message_rate_limiter limiter(test_burst, test_interval);
   take_burst(limiter, test_burst, start);
   limiter.reset();
   BOOST_CHECK_EQUAL(limiter.available(), test_burst);
   take_burst(limiter, test_burst, start);
   BOOST_CHECK(!limiter.try_consume(start));
}

BOOST_AUTO_TEST_SUITE_END()

#include <boost/test/unit_test.hpp>

#include <fc/system_timer.hpp>

#include <chrono>

BOOST_AUTO_TEST_SUITE(system_timer_test_suite)

/// An ordinary point has to survive the trip, at whatever resolution the clock keeps.
BOOST_AUTO_TEST_CASE(from_time_point_round_trips_ordinary_values) {
   const fc::time_point t{fc::microseconds{1'700'000'000'000'000}};   // some time in 2023

   const auto converted = fc::system_clock::from_time_point(t);
   const auto back = std::chrono::duration_cast<std::chrono::microseconds>(converted.time_since_epoch()).count();

   BOOST_CHECK_EQUAL(back, t.time_since_epoch().count());
}

/// The point of the conversion going through fc::time_point::to_system_clock(): this clock's
/// duration is finer than a microsecond on some platforms, so scaling the extremes up directly
/// overflows and wraps. A deadline meant to be unreachable would come back as one already passed,
/// which is a timer that fires at once rather than one that never does.
BOOST_AUTO_TEST_CASE(from_time_point_saturates_at_the_extremes) {
   // Both ends of what the microsecond count can hold, which is what to_system_clock() clamps.
   const auto epoch = fc::system_clock::from_time_point(fc::time_point{});
   const auto latest = fc::system_clock::from_time_point(fc::time_point{fc::microseconds::maximum()});
   const auto earliest = fc::system_clock::from_time_point(fc::time_point{fc::microseconds::minimum()});

   BOOST_CHECK_GT(latest.time_since_epoch().count(), epoch.time_since_epoch().count());
   BOOST_CHECK_LT(earliest.time_since_epoch().count(), epoch.time_since_epoch().count());

   // Saturated rather than merely large: these are the ends of what the clock can hold.
   BOOST_CHECK(latest == fc::system_clock::time_point::max());
   BOOST_CHECK(earliest == fc::system_clock::time_point::min());
}

/// The conversion is what a timer's expires_at() is given, so the extremes have to leave a timer
/// waiting rather than immediately expired.
BOOST_AUTO_TEST_CASE(the_furthest_deadline_is_not_already_expired) {
   BOOST_CHECK_GT(fc::system_clock::from_time_point(fc::time_point::maximum()).time_since_epoch().count(),
                  fc::system_clock::now().time_since_epoch().count());
}

BOOST_AUTO_TEST_SUITE_END()

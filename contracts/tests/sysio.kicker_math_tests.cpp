/** @file sysio.kicker_math_tests.cpp
 * @brief Host arithmetic gates with an independent wide integer oracle.
 */
#include <boost/test/unit_test.hpp>
#include <boost/multiprecision/cpp_int.hpp>
#include "../sysio.kicker/include/sysio.kicker/kicker_math.hpp"
#include <limits>
#include "kicker_test_reference.hpp"
namespace math = sysio::kicker_math;
using kicker_test_reference::wide;
using kicker_test_reference::reference;
BOOST_AUTO_TEST_SUITE(sysio_kicker_math_tests)
/// A literal year at 200 bps pays exactly one fiftieth of principal.
BOOST_AUTO_TEST_CASE(literal_year) {
   constexpr uint64_t principal = 100'000'000'000'000;
   BOOST_CHECK(math::interest(principal, 200, 31'557'600'000'000ULL) == principal / 50);
   BOOST_CHECK_EQUAL(2'000'000'000'000ULL,
      math::gift(principal, 200, 31'557'600'000'000ULL, 1, 1).value());
}
/// Bounds compare the on-chain helper with a wider independent reference, including uncapped long intervals.
BOOST_AUTO_TEST_CASE(overflow_bounds_and_floors) {
   const auto max = math::max_amount;
   const uint64_t elapsed = std::numeric_limits<int64_t>::max();
   const auto interest = math::interest(max, 10000, elapsed);
   const auto correct = wide(max) * 10000 * elapsed / (wide(10'000) * 31'557'600 * 1'000'000);
   BOOST_CHECK(wide(interest) == correct);
   BOOST_CHECK(!math::gift(max, 10000, elapsed, max, 1));
   BOOST_CHECK(!math::gift(max, 10000, math::year_sec * math::micros_per_sec * 2, 1, 1));
   BOOST_CHECK_EQUAL(max, math::gift(max, 10000, math::year_sec * math::micros_per_sec, 1, 1).value());
   BOOST_CHECK_EQUAL(reference(max, 200, elapsed, 1, max), math::gift(max, 200, elapsed, 1, max).value());
   for (uint64_t s : {uint64_t{1}, uint64_t{100'000'000'000'000}, max})
      for (uint64_t dt : {uint64_t{1}, math::day_sec * math::micros_per_sec,
                         math::year_sec * math::micros_per_sec * 1000})
         BOOST_CHECK_EQUAL(reference(s, 200, dt, 1, max), math::gift(s, 200, dt, 1, max).value());
   BOOST_CHECK_EQUAL(0, math::gift(max, 0, elapsed, max, 1).value());
   BOOST_CHECK_EQUAL(0, math::gift(max, 10000, elapsed, 1, 0).value());
}

BOOST_AUTO_TEST_SUITE_END()

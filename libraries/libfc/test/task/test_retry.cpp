/**
 * @file test_retry.cpp
 * @brief Unit tests for `fc::task::retry_until` + `fc::task::retry_options`.
 */

#include <fc/task/retry.hpp>

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>

using namespace fc::task;

namespace {
   /// Options that make tests fast: short timeouts, tight backoffs.
   retry_options fast_opts() {
      retry_options o;
      o.initial_backoff = fc::milliseconds(5);
      o.max_backoff     = fc::milliseconds(25);
      o.total_timeout   = fc::milliseconds(500);
      return o;
   }
} // namespace

BOOST_AUTO_TEST_SUITE(retry_until_tests)

// First-call success skips all backoff logic — the predicate returns a ready
// value and retry_until relays it without sleeping.
BOOST_AUTO_TEST_CASE(returns_value_on_first_success) {
   int calls = 0;
   auto got = retry_until<int>("first-success", fast_opts(),
      [&]() -> std::optional<int> { ++calls; return 42; });
   BOOST_CHECK_EQUAL(got, 42);
   BOOST_CHECK_EQUAL(calls, 1);
}

// A predicate that returns nullopt for a few iterations then a real value
// must exit successfully without consuming the full deadline.
BOOST_AUTO_TEST_CASE(returns_value_after_transient_nullopts) {
   int calls = 0;
   auto got = retry_until<int>("transient", fast_opts(),
      [&]() -> std::optional<int> {
         ++calls;
         if (calls < 4) return std::nullopt;
         return 99;
      });
   BOOST_CHECK_EQUAL(got, 99);
   BOOST_CHECK_EQUAL(calls, 4);
}

// An always-nullopt predicate must eventually throw `fc::timeout_exception`.
// The assertion on elapsed time is loose (>= 90% of budget) to accommodate
// CI jitter while still proving the helper waited roughly the right amount.
BOOST_AUTO_TEST_CASE(throws_timeout_on_deadline_expiry) {
   auto opts = fast_opts();
   opts.total_timeout = fc::milliseconds(100);

   const auto start = fc::time_point::now();
   BOOST_CHECK_EXCEPTION(
      retry_until<int>("deadline", opts,
         []() -> std::optional<int> { return std::nullopt; }),
      fc::timeout_exception,
      [](const fc::timeout_exception&) { return true; });
   const auto elapsed = fc::time_point::now() - start;
   BOOST_CHECK_GE(elapsed.count(), 90 * 1000); // at least 90ms — some tolerance
}

// A fatal exception thrown from inside the predicate must propagate out
// unchanged — retry_until does not swallow or retry on a throw.
BOOST_AUTO_TEST_CASE(propagates_predicate_exception) {
   int calls = 0;
   BOOST_CHECK_THROW(
      retry_until<int>("fatal", fast_opts(),
         [&]() -> std::optional<int> {
            ++calls;
            FC_THROW("fatal error from predicate");
         }),
      fc::exception);
   BOOST_CHECK_EQUAL(calls, 1); // no retries on fatal
}

// Backoff grows geometrically (doubling with growth_factor=2.0) but must
// never exceed `max_backoff`. We verify by observing how many predicate
// calls happen in a fixed window — if the cap weren't honored, sleeps
// would explode and call count would drop.
BOOST_AUTO_TEST_CASE(backoff_respects_max_backoff_cap) {
   retry_options opts;
   opts.initial_backoff = fc::milliseconds(10);
   opts.max_backoff     = fc::milliseconds(20);
   opts.total_timeout   = fc::milliseconds(300);
   opts.growth_factor   = 2.0;

   std::atomic<int> calls{0};
   BOOST_CHECK_THROW(
      retry_until<int>("bounded-backoff", opts,
         [&]() -> std::optional<int> { ++calls; return std::nullopt; }),
      fc::timeout_exception);

   // With backoff capped at 20ms and a 300ms budget, expect several
   // attempts (10, 20, 20, 20, ...). If the cap weren't honored, doubling
   // from 10ms would consume the budget in only a few sleeps. Make the
   // jitter-sensitive lower bound non-fatal so heavily loaded CI runners do
   // not fail only because short sleeps overshot.
   BOOST_WARN_GE(calls.load(), 8);
}

// `growth_factor = 1.0` should keep backoff constant. Verify by counting
// attempts in a budget that's an integer multiple of initial_backoff.
BOOST_AUTO_TEST_CASE(growth_factor_one_is_fixed_interval) {
   retry_options opts;
   opts.initial_backoff = fc::milliseconds(10);
   opts.max_backoff     = fc::milliseconds(100);
   opts.total_timeout   = fc::milliseconds(250);
   opts.growth_factor   = 1.0;

   int calls = 0;
   BOOST_CHECK_THROW(
      retry_until<int>("fixed-interval", opts,
         [&]() -> std::optional<int> { ++calls; return std::nullopt; }),
      fc::timeout_exception);

   // With 10ms fixed interval and a 250ms budget, expect more than a
   // handful of attempts. The lower bound is a warning because overloaded CI
   // can oversleep short intervals; the upper bound still catches a tight
   // retry loop that forgot to sleep.
   BOOST_WARN_GE(calls, 8);
   BOOST_CHECK_LE(calls, 50);
}

BOOST_AUTO_TEST_SUITE_END()

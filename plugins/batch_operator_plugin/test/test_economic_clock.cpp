#include "../src/economic_clock.hpp"

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <vector>

#include <fc/time.hpp>
#include <fc/variant.hpp>
#include <fc/variant_object.hpp>

using namespace sysio::batch_operator_detail;
using mvo = fc::mutable_variant_object;

namespace {

constexpr uint32_t duration_sec = 360;
/// Grid start in unix seconds (an arbitrary fixed instant).
constexpr uint32_t start_sec = 1'800'000'000;
constexpr uint32_t opened_index = 4;

fc::time_point at(uint64_t unix_sec) { return fc::time_point(fc::seconds(static_cast<int64_t>(unix_sec))); }

/// An `economycfg` value as the chain renders it.
fc::variant_object config_row(uint64_t duration = duration_sec) {
   return mvo()(economy::field::epoch_duration_sec, duration)
               (economy::field::start_time, fc::time_point_sec(start_sec));
}

/// An `economystate` value after `index` ticks, the next boundary at `next_sec`.
fc::variant_object state_row(uint32_t index, uint64_t next_sec) {
   return mvo()(economy::field::current_epoch_index, index)
               (economy::field::next_epoch_start, at(next_sec));
}

} // namespace

BOOST_AUTO_TEST_SUITE(economic_clock_tests)

/// Before the first tick the next boundary is the configured start time.
BOOST_AUTO_TEST_CASE(an_unticked_clock_opens_at_its_start_time) {
   const auto clock = decode_economic_clock(config_row(), nullptr);
   BOOST_REQUIRE(clock);
   BOOST_CHECK_EQUAL(0u, clock->current_epoch_index);
   BOOST_CHECK(at(start_sec) == clock->next_boundary);
   BOOST_CHECK_EQUAL(fc::seconds(duration_sec).count(), clock->epoch_duration.count());

   // A state row still at index 0 reads the same way.
   const auto zero_state = state_row(0, start_sec);
   const auto zero_clock = decode_economic_clock(config_row(), &zero_state);
   BOOST_REQUIRE(zero_clock);
   BOOST_CHECK(at(start_sec) == zero_clock->next_boundary);
}

/// After ticks the next boundary is the state's own, not one derived from the start.
BOOST_AUTO_TEST_CASE(a_ticked_clock_opens_at_its_next_epoch_start) {
   const uint64_t next_sec = start_sec + uint64_t{opened_index} * duration_sec;
   const auto state = state_row(opened_index, next_sec);
   const auto clock = decode_economic_clock(config_row(), &state);
   BOOST_REQUIRE(clock);
   BOOST_CHECK_EQUAL(opened_index, clock->current_epoch_index);
   BOOST_CHECK(at(next_sec) == clock->next_boundary);
}

/// A malformed config is not a clock: nothing would be pushed.
BOOST_AUTO_TEST_CASE(a_config_without_a_duration_is_not_a_clock) {
   BOOST_CHECK(!decode_economic_clock(config_row(0), nullptr));
   BOOST_CHECK(!decode_economic_clock(mvo()(economy::field::start_time, fc::time_point_sec(start_sec)), nullptr));
}

/// Nothing is due before the boundary; exactly the next index once it passes.
BOOST_AUTO_TEST_CASE(only_passed_boundaries_are_due) {
   const auto clock = decode_economic_clock(config_row(), nullptr);
   BOOST_REQUIRE(clock);
   BOOST_CHECK(economic_epochs_due(*clock, at(start_sec - 1), economy::ticks_per_poll).empty());
   BOOST_CHECK(economic_epochs_due(*clock, at(start_sec), economy::ticks_per_poll) == std::vector<uint32_t>{1});
   BOOST_CHECK((economic_epochs_due(*clock, at(start_sec + duration_sec), economy::ticks_per_poll)
                == std::vector<uint32_t>{1, 2}));
}

/// A start far in the past catches up in order, a bounded number of epochs per poll.
BOOST_AUTO_TEST_CASE(catch_up_is_ordered_and_bounded_per_poll) {
   const uint64_t next_sec = start_sec + uint64_t{opened_index} * duration_sec;
   const auto state = state_row(opened_index, next_sec);
   const auto clock = decode_economic_clock(config_row(), &state);
   BOOST_REQUIRE(clock);

   const auto due = economic_epochs_due(*clock, at(next_sec + 1000 * duration_sec), economy::ticks_per_poll);
   BOOST_REQUIRE_EQUAL(economy::ticks_per_poll, due.size());
   for (uint32_t i = 0; i < due.size(); ++i) BOOST_CHECK_EQUAL(opened_index + 1 + i, due[i]);
}

BOOST_AUTO_TEST_SUITE_END()

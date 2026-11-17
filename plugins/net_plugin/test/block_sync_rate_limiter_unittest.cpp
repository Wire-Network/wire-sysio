#include <sysio/net_plugin/block_sync_rate_limiter.hpp>

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <cstdint>
#include <limits>

using sysio::block_sync_rate_limiter;
using namespace std::chrono_literals;

namespace {

using time_point = block_sync_rate_limiter::clock::time_point;

constexpr uint64_t limit = 20'000; ///< bytes per second; one byte refills every 50us
const time_point start = time_point{} + 1h;
constexpr int64_t int64_max = std::numeric_limits<int64_t>::max();
constexpr uint64_t uint64_max = std::numeric_limits<uint64_t>::max();

} // namespace

BOOST_AUTO_TEST_SUITE(block_sync_rate_limiter_tests)

BOOST_AUTO_TEST_CASE(starts_full_and_refills_at_the_limit) {
   block_sync_rate_limiter bucket(limit, start);
   BOOST_TEST(!bucket.unlimited());
   BOOST_TEST(bucket.limit_bytes_per_sec() == limit);
   BOOST_TEST(bucket.burst_bytes() == 20'000);
   BOOST_TEST(bucket.allowance() == 20'000);
   BOOST_TEST(bucket.may_send());
   BOOST_TEST(bucket.refill_delay().count() == 0);

   bucket.charge(15'000);
   BOOST_TEST(bucket.allowance() == 5'000);
   BOOST_TEST(bucket.may_send());

   bucket.refill(start + 100ms);
   BOOST_TEST(bucket.allowance() == 7'000);
   bucket.refill(start + 350ms);
   BOOST_TEST(bucket.allowance() == 12'000);
}

BOOST_AUTO_TEST_CASE(zero_limit_is_unlimited) {
   block_sync_rate_limiter bucket(block_sync_rate_limiter::unlimited_rate, start);
   BOOST_TEST(bucket.unlimited());
   BOOST_TEST(bucket.burst_bytes() == 0);
   BOOST_TEST(bucket.may_send());

   bucket.charge(uint64_max);
   BOOST_TEST(bucket.allowance() == 0);
   BOOST_TEST(bucket.may_send());
   BOOST_TEST(bucket.refill_delay().count() == 0);

   bucket.refill(start + 1h);
   BOOST_TEST(bucket.allowance() == 0);
   BOOST_TEST(bucket.may_send());
}

// An allowance of exactly zero holds the next block until refill credits one whole byte.
BOOST_AUTO_TEST_CASE(zero_allowance_holds_until_one_byte_refills) {
   block_sync_rate_limiter bucket(limit, start);
   bucket.charge(20'000);
   BOOST_TEST(bucket.allowance() == 0);
   BOOST_TEST(!bucket.may_send());
   BOOST_TEST(bucket.refill_delay().count() == 50);

   bucket.refill(start + 49us);
   BOOST_TEST(bucket.allowance() == 0);
   BOOST_TEST(!bucket.may_send());
   // 49us carried 980'000 of the 1'000'000 byte-microseconds a byte takes
   BOOST_TEST(bucket.refill_delay().count() == 1);

   bucket.refill(start + 50us);
   BOOST_TEST(bucket.allowance() == 1);
   BOOST_TEST(bucket.may_send());
   BOOST_TEST(bucket.refill_delay().count() == 0);
}

// A block larger than the allowance is sent and leaves it negative; refill_delay() covers the whole deficit.
BOOST_AUTO_TEST_CASE(overdrawn_allowance_waits_for_the_deficit) {
   block_sync_rate_limiter bucket(limit, start);
   bucket.charge(50'000);
   BOOST_TEST(bucket.allowance() == -30'000);
   BOOST_TEST(!bucket.may_send());

   // 30'001 bytes at 20'000 bytes per second
   const std::chrono::microseconds delay = bucket.refill_delay();
   BOOST_TEST(delay.count() == 1'500'050);

   bucket.refill(start + delay - 1us);
   BOOST_TEST(bucket.allowance() == 0);
   BOOST_TEST(!bucket.may_send());
   bucket.refill(start + delay);
   BOOST_TEST(bucket.allowance() == 1);
   BOOST_TEST(bucket.may_send());
}

// A byte that takes a fractional number of microseconds is waited for in whole microseconds, rounded up.
BOOST_AUTO_TEST_CASE(refill_delay_rounds_up) {
   constexpr uint64_t three_per_sec = 3;
   block_sync_rate_limiter bucket(three_per_sec, start);
   BOOST_TEST(bucket.burst_bytes() == 3);
   bucket.charge(3);
   BOOST_TEST(bucket.refill_delay().count() == 333'334);

   bucket.refill(start + 333'333us);
   BOOST_TEST(bucket.allowance() == 0);
   bucket.refill(start + 333'334us);
   BOOST_TEST(bucket.allowance() == 1);
}

// Refills shorter than a byte, or than a microsecond, accumulate rather than round down to nothing.
BOOST_AUTO_TEST_CASE(sub_byte_credit_carries_over) {
   block_sync_rate_limiter bucket(limit, start);
   bucket.charge(20'000);

   // 7us is 0.14 bytes
   time_point now = start;
   for (int i = 0; i < 10'000; ++i) {
      now += 7us;
      bucket.refill(now);
   }
   BOOST_TEST(bucket.allowance() == 1'400);

   bucket.charge(1'400);
   const time_point resumed = now;
   // 999ns is under a microsecond
   for (int i = 0; i < 1'000; ++i) {
      now += 999ns;
      bucket.refill(now);
   }
   // 999us of credit is 19.98 bytes
   BOOST_TEST(bucket.allowance() == 19);
   BOOST_TEST(bucket.refill_delay().count() == 0);
   bucket.charge(19);
   bucket.refill(resumed + 1ms);
   BOOST_TEST(bucket.allowance() == 1);
}

BOOST_AUTO_TEST_CASE(refill_caps_at_the_burst) {
   block_sync_rate_limiter bucket(limit, start);
   bucket.refill(start + 10s);
   BOOST_TEST(bucket.allowance() == bucket.burst_bytes());

   // A full bucket discards credit short of a byte along with the rest.
   bucket.charge(5'000);
   bucket.refill(start + 10s + 250ms + 25us);
   BOOST_TEST(bucket.allowance() == bucket.burst_bytes());
   bucket.charge(20'000);
   bucket.refill(start + 10s + 250ms + 25us + 49us);
   BOOST_TEST(bucket.allowance() == 0);
   bucket.refill(start + 10s + 250ms + 25us + 50us);
   BOOST_TEST(bucket.allowance() == 1);

   // An overdrawn bucket refills from its deficit, still up to the burst.
   block_sync_rate_limiter overdrawn(limit, start);
   overdrawn.charge(30'000);
   BOOST_TEST(overdrawn.allowance() == -10'000);
   overdrawn.refill(start + 1s);
   BOOST_TEST(overdrawn.allowance() == 10'000);
   overdrawn.refill(start + 3s);
   BOOST_TEST(overdrawn.allowance() == 20'000);
}

BOOST_AUTO_TEST_CASE(earlier_time_credits_nothing) {
   block_sync_rate_limiter bucket(limit, start);
   bucket.charge(20'000);
   bucket.refill(start - 1s);
   BOOST_TEST(bucket.allowance() == 0);
   bucket.refill(start);
   BOOST_TEST(bucket.allowance() == 0);
   // the earlier time point did not move the refill time back
   bucket.refill(start + 50us);
   BOOST_TEST(bucket.allowance() == 1);
}

BOOST_AUTO_TEST_CASE(reset_fills_the_bucket) {
   block_sync_rate_limiter bucket(limit, start);
   bucket.charge(50'000);
   bucket.refill(start + 1us); // carries 20'000 byte-microseconds
   bucket.reset(start + 1s);
   BOOST_TEST(bucket.allowance() == 20'000);
   bucket.charge(20'000);
   bucket.refill(start + 1s + 49us);
   BOOST_TEST(bucket.allowance() == 0); // the carried credit was dropped
   bucket.refill(start + 1s + 50us);
   BOOST_TEST(bucket.allowance() == 1);
}

BOOST_AUTO_TEST_CASE(extreme_values_do_not_overflow) {
   // The largest limit: the burst and the deficit saturate at the largest int64_t.
   block_sync_rate_limiter fastest(uint64_max, time_point{});
   BOOST_TEST(fastest.burst_bytes() == int64_max);
   BOOST_TEST(fastest.max_deficit_bytes() == int64_max);
   BOOST_TEST(fastest.allowance() == int64_max);
   fastest.charge(uint64_max);
   BOOST_TEST(fastest.allowance() == -int64_max);
   fastest.charge(uint64_max);
   BOOST_TEST(fastest.allowance() == -int64_max);
   // 2^63 bytes at 2^64 - 1 bytes per second is just over half a second
   BOOST_TEST(fastest.refill_delay().count() == 500'001);
   fastest.refill(time_point::max());
   BOOST_TEST(fastest.allowance() == int64_max);

   // The smallest limit, overdrawn by the largest charge, then refilled over the longest time a time point allows.
   block_sync_rate_limiter slowest(1, time_point{});
   BOOST_TEST(slowest.max_deficit_bytes() == 2);
   slowest.charge(uint64_max);
   BOOST_TEST(slowest.allowance() == -2);
   BOOST_TEST(slowest.refill_delay() == block_sync_rate_limiter::max_refill_delay);
   slowest.refill(time_point::max());
   BOOST_TEST(slowest.allowance() == 1);
   BOOST_TEST(slowest.may_send());
}

// A charge overdraws the bucket by at most max_send_gap of the limit.
BOOST_AUTO_TEST_CASE(charge_overdraws_at_most_max_send_gap_of_the_limit) {
   block_sync_rate_limiter bucket(limit, start);
   BOOST_TEST(bucket.max_deficit_bytes() == 40'000);

   bucket.charge(60'000);
   BOOST_TEST(bucket.allowance() == -40'000);
   bucket.reset(start);
   bucket.charge(60'001);
   BOOST_TEST(bucket.allowance() == -40'000);
   bucket.reset(start);
   bucket.charge(1'000'000);
   BOOST_TEST(bucket.allowance() == -40'000);

   // 40'001 bytes at 20'000 bytes per second: max_send_gap plus the 50us of one byte
   BOOST_TEST(bucket.refill_delay() == block_sync_rate_limiter::max_send_gap + 50us);
}

// However far a charge overdraws the bucket, the wait for the next block is max_send_gap plus the time of one byte at
// most, so never past max_refill_delay.
BOOST_AUTO_TEST_CASE(refill_delay_is_bounded) {
   constexpr uint64_t usec_per_sec = 1'000'000;
   for (const uint64_t rate : {uint64_t{1}, uint64_t{3}, uint64_t{1'000}, limit, uint64_t{1'000'000'000}, uint64_max}) {
      BOOST_TEST_CONTEXT(rate) {
         block_sync_rate_limiter bucket(rate, start);
         bucket.charge(uint64_max);
         // the time of one byte, rounded up to a microsecond
         const std::chrono::microseconds one_byte{usec_per_sec / rate + (usec_per_sec % rate != 0 ? 1 : 0)};
         BOOST_TEST(bucket.refill_delay() <= block_sync_rate_limiter::max_send_gap + one_byte);
         BOOST_TEST(bucket.refill_delay() <= block_sync_rate_limiter::max_refill_delay);
         bucket.refill(start + bucket.refill_delay());
         BOOST_TEST(bucket.may_send());
      }
   }
}

// Blocks larger than max_send_gap of the limit are each charged only that much, so they go one per max_send_gap
// (plus the time of one byte) rather than at the limit.
BOOST_AUTO_TEST_CASE(oversized_blocks_go_one_per_max_send_gap) {
   constexpr uint64_t block_bytes = 100'000; // 5 s at the limit
   block_sync_rate_limiter bucket(limit, start);
   time_point now = start;
   time_point last_send = now;
   for (int i = 0; i < 10; ++i) {
      bucket.refill(now);
      if (!bucket.may_send()) {
         now += bucket.refill_delay();
         bucket.refill(now);
      }
      BOOST_REQUIRE(bucket.may_send());
      if (i > 0) {
         const std::chrono::microseconds gap = std::chrono::duration_cast<std::chrono::microseconds>(now - last_send);
         BOOST_TEST(gap == block_sync_rate_limiter::max_send_gap + 50us);
      }
      last_send = now;
      bucket.charge(block_bytes);
      BOOST_TEST(bucket.allowance() == -bucket.max_deficit_bytes());
   }
}

// Simulates a connection that sends whenever the bucket allows and otherwise waits refill_delay(). Every block is
// sent no sooner than the bucket contract allows, and no later than one microsecond after.
BOOST_AUTO_TEST_CASE(sustained_sending_holds_to_the_limit) {
   constexpr uint64_t usec_per_sec = 1'000'000;
   constexpr uint64_t block_bytes = 7'777;
   block_sync_rate_limiter bucket(limit, start);
   const uint64_t burst = static_cast<uint64_t>(bucket.burst_bytes());
   time_point now = start;
   uint64_t sent = 0;
   for (int i = 0; i < 1'000; ++i) {
      bucket.refill(now);
      bool waited = false;
      if (!bucket.may_send()) {
         now += bucket.refill_delay();
         bucket.refill(now);
         waited = true;
      }
      BOOST_REQUIRE(bucket.may_send());
      const uint64_t elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - start).count();
      // Bytes sent before this block: less than the burst plus the limit over the time since the first.
      BOOST_TEST(sent * usec_per_sec < burst * usec_per_sec + limit * elapsed);
      if (waited) {
         // One microsecond sooner, the allowance had not yet reached one byte.
         BOOST_TEST(limit * (elapsed - 1) < (sent + 1 - burst) * usec_per_sec);
      }
      bucket.charge(block_bytes);
      sent += block_bytes;
      BOOST_TEST(bucket.allowance() > -bucket.max_deficit_bytes()); // under the clamp, so every byte counts
   }
   // The last block waits for the 7'769'223 bytes before it, less the burst, plus one byte: 7'749'224 bytes at 20'000
   // bytes per second.
   BOOST_TEST(std::chrono::duration_cast<std::chrono::microseconds>(now - start).count() == 387'461'200);
}

BOOST_AUTO_TEST_SUITE_END()

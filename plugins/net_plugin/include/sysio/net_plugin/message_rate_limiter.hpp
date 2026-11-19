#pragma once

#include <cassert>
#include <chrono>
#include <cstdint>
#include <utility>

namespace sysio {

/**
 * Token bucket that bounds the rate of the messages of one kind a peer sends.
 *
 * The bucket holds at most burst messages and starts full. Each message takes one, and one returns every
 * refill_interval; a full bucket gains nothing while it waits. A message that finds the bucket empty is over the
 * limit. The caller supplies every time point; the bucket reads no clock.
 */
class message_rate_limiter {
public:
   using clock = std::chrono::steady_clock;

   /// @param burst most messages taken at once, at least one
   /// @param refill_interval time for one message to return to the bucket, more than zero
   message_rate_limiter(uint32_t burst, clock::duration refill_interval)
      : burst_(burst)
      , refill_interval_(refill_interval)
      , available_(burst) {
      assert(burst > 0 && refill_interval > clock::duration::zero());
   }

   /// @return messages that may be taken, as of the last try_consume() or reset()
   uint32_t available() const { return available_; }

   /// Fill the bucket.
   void reset() { available_ = burst_; }

   /// Return to the bucket the messages refilled by now, then take one.
   /// @return true when the message is within the limit; false takes nothing
   bool try_consume(clock::time_point now) {
      refill(now);
      if (available_ == 0)
         return false;
      --available_;
      return true;
   }

private:
   /// Return one message for each refill_interval since the last refill, up to burst. A time point at or before the
   /// last refill returns nothing.
   void refill(clock::time_point now) {
      if (available_ == burst_) {
         last_refill_ = now;
         return;
      }
      if (now <= last_refill_)
         return;
      const clock::rep intervals = (now - last_refill_) / refill_interval_;
      const uint32_t   room      = burst_ - available_;
      if (std::cmp_greater_equal(intervals, room)) {
         available_   = burst_;
         last_refill_ = now;
      } else {
         available_ += static_cast<uint32_t>(intervals);
         last_refill_ += intervals * refill_interval_;
      }
   }

   uint32_t          burst_;
   clock::duration   refill_interval_;
   uint32_t          available_;
   clock::time_point last_refill_; ///< time the last refill counts from
};

} // namespace sysio

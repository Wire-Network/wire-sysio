#pragma once

#include <fc/int128.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>

namespace sysio {

/**
 * Token bucket that holds the sync blocks sent to one peer to a configured rate.
 *
 * The allowance is the number of bytes that may still be sent. Elapsed time refills it at the limit, up to
 * burst_bytes(), and every block sent is charged, so the last block sent may overdraw it. The next block may be sent
 * once the allowance is positive again. A charge never takes the allowance below -max_deficit_bytes(): a block
 * larger than max_send_gap of the limit is sent at one block per max_send_gap rather than at the limit. Credit short
 * of a whole byte carries to the next refill, so no time is lost to rounding.
 *
 * The caller supplies every time point; the bucket reads no clock.
 */
class block_sync_rate_limiter {
public:
   using clock = std::chrono::steady_clock;

   /// Rate limit that means unlimited, as parse_listen_address returns for a listener without one.
   static constexpr uint64_t unlimited_rate = 0;

   /// Time a full bucket lasts at the limit. The most sent at once is this much of the limit plus one block.
   static constexpr std::chrono::microseconds burst_window = std::chrono::seconds{1};

   /// Longest a sender waits between sync blocks, not counting the time of one byte: a charge overdraws the bucket
   /// by at most this much of the limit. Kept under the syncing peer's response timeout, which closes a connection
   /// that sends no block for that long.
   static constexpr std::chrono::microseconds max_send_gap = std::chrono::seconds{2};

   /// Longest refill_delay(): max_send_gap plus the time of one byte at the lowest limit, one byte per second.
   static constexpr std::chrono::microseconds max_refill_delay = max_send_gap + std::chrono::seconds{1};

   /// @param limit_bytes_per_sec rate limit in bytes per second, or unlimited_rate
   /// @param now time the bucket starts full
   block_sync_rate_limiter(uint64_t limit_bytes_per_sec, clock::time_point now)
      : limit_(limit_bytes_per_sec)
      , burst_bytes_(whole_bytes(limit_bytes_per_sec, burst_window))
      , max_deficit_bytes_(whole_bytes(limit_bytes_per_sec, max_send_gap)) {
      reset(now);
   }

   /// @return true when there is no limit: nothing is charged and every block may be sent
   bool unlimited() const { return limit_ == unlimited_rate; }

   /// @return the rate limit in bytes per second, or unlimited_rate
   uint64_t limit_bytes_per_sec() const { return limit_; }

   /// @return bytes that may still be sent; negative once a block overdraws the bucket
   int64_t allowance() const { return allowance_; }

   /// @return the largest allowance: burst_window of the limit, at most the largest int64_t
   int64_t burst_bytes() const { return burst_bytes_; }

   /// @return the most a charge can overdraw the bucket: max_send_gap of the limit, at most the largest int64_t
   int64_t max_deficit_bytes() const { return max_deficit_bytes_; }

   /// Fill the bucket and refill from now on.
   void reset(clock::time_point now) {
      allowance_ = burst_bytes_;
      carry_ = 0;
      last_refill_ = now;
   }

   /// Credit the time since the last refill at the limit, capped at burst_bytes(). A time point at or before the
   /// last refill credits nothing.
   /// @pre now - the last refill time fits clock::duration
   void refill(clock::time_point now) {
      if (unlimited() || now <= last_refill_)
         return;
      const std::chrono::microseconds elapsed =
         std::chrono::duration_cast<std::chrono::microseconds>(now - last_refill_);
      last_refill_ += elapsed; // the sub-microsecond remainder counts toward the next refill
      const fc::uint128 credit = byte_microseconds(limit_, elapsed) + carry_;
      const fc::int128 refilled = fc::int128{allowance_} + static_cast<fc::int128>(credit / usec_per_sec);
      if (refilled >= burst_bytes_) {
         allowance_ = burst_bytes_;
         carry_ = 0;
      } else {
         allowance_ = static_cast<int64_t>(refilled);
         carry_ = static_cast<uint64_t>(credit % usec_per_sec);
      }
   }

   /// Charge a block sent. The allowance may go negative, down to -max_deficit_bytes().
   void charge(uint64_t bytes) {
      if (unlimited())
         return;
      const fc::int128 charged = fc::int128{allowance_} - bytes;
      allowance_ = static_cast<int64_t>(std::max<fc::int128>(charged, -fc::int128{max_deficit_bytes_}));
   }

   /// @return true when a block may be sent: unlimited, or the allowance is positive
   bool may_send() const { return unlimited() || allowance_ > 0; }

   /// @return time until refill makes the allowance positive, zero when a block may be sent now. At most
   ///         max_send_gap plus the time of one byte, rounded up to a microsecond, so at most max_refill_delay.
   std::chrono::microseconds refill_delay() const {
      if (may_send())
         return std::chrono::microseconds{0};
      // Credit that brings the allowance to one byte, less the credit already carried; carry_ < usec_per_sec.
      const fc::uint128 deficit = static_cast<fc::uint128>(1 - fc::int128{allowance_}) * usec_per_sec - carry_;
      return std::chrono::microseconds{static_cast<int64_t>((deficit + limit_ - 1) / limit_)};
   }

private:
   static constexpr uint64_t usec_per_sec = std::chrono::microseconds{std::chrono::seconds{1}}.count();

   /// @return limit times elapsed, in byte-microseconds; the 128-bit product cannot overflow
   static fc::uint128 byte_microseconds(uint64_t limit, std::chrono::microseconds elapsed) {
      return static_cast<fc::uint128>(limit) * static_cast<uint64_t>(elapsed.count());
   }

   /// @return whole bytes the limit allows in elapsed, at most the largest int64_t
   static int64_t whole_bytes(uint64_t limit, std::chrono::microseconds elapsed) {
      const fc::uint128 bytes = byte_microseconds(limit, elapsed) / usec_per_sec;
      return static_cast<int64_t>(std::min<fc::uint128>(bytes, std::numeric_limits<int64_t>::max()));
   }

   uint64_t limit_ = unlimited_rate;
   int64_t burst_bytes_ = 0;
   int64_t max_deficit_bytes_ = 0;
   int64_t allowance_ = 0;
   uint64_t carry_ = 0; ///< credit short of a whole byte, in byte-microseconds
   clock::time_point last_refill_;
};

} // namespace sysio

#pragma once

#include <fc/int128.hpp>

#include <chrono>
#include <cstddef>

namespace sysio {

/** Whether a sync frame has reached its byte-per-second cap before enqueueing another block.
 *  Zero disables the cap, and an empty frame may enqueue its first block. A nonpositive elapsed interval
 *  defers further blocks until time advances. Wide products preserve the comparison for every size_t cap.
 */
inline bool block_sync_rate_limit_reached(size_t bytes, std::chrono::nanoseconds elapsed, size_t rate_limit) {
   if (rate_limit == 0 || bytes == 0)
      return false;
   if (elapsed <= std::chrono::nanoseconds::zero())
      return true;

   constexpr auto nanoseconds_per_second =
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::seconds{1}).count();
   return fc::uint128_t{bytes} * nanoseconds_per_second >=
          fc::uint128_t{rate_limit} * static_cast<fc::uint128_t>(elapsed.count());
}

} // namespace sysio

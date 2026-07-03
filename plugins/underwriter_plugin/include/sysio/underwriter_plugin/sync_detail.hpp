#pragma once
/**
 * @file sync_detail.hpp
 * @brief Pure sync-gate predicate for the underwriter plugin, lifted out of the
 *        `.cpp`-private impl so it is unit-testable without standing up a chain.
 *
 * The underwriter's startup preflight validates depot-side state (opreg
 * registration, chain registry, authex links) via LOCAL table reads. On a
 * cold-booting operator node those reads see mid-replay (possibly genesis)
 * state and fail spuriously, so the plugin must not begin underwriting until
 * the chain plugin reports the node synced. "Synced" here is the same recency
 * notion producer_plugin uses to decide it may produce: the head block time is
 * within a small window of wall-clock now.
 */

#include <fc/time.hpp>

namespace sysio::underwriter_detail {

/**
 * @brief True when the chain head is recent enough to treat the node as synced.
 *
 * While a cold-booting node replays blocks its head time trails `now` by the
 * replay gap; once caught up, the head tracks `now` to within block-interval
 * jitter. The window must exceed that jitter (plus scheduling slack) but stay
 * well under one epoch so underwriting never starts against stale state.
 *
 * @param head_time      The chain head block's timestamp.
 * @param now            Wall-clock now.
 * @param recency_window Maximum head-behind-now gap still considered synced.
 * @return True when `head_time >= now - recency_window`.
 */
inline bool head_is_recent(fc::time_point head_time, fc::time_point now,
                           fc::microseconds recency_window) {
   return head_time >= now - recency_window;
}

} // namespace sysio::underwriter_detail

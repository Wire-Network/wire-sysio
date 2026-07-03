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
#include <fc/variant_object.hpp>

#include <magic_enum/magic_enum.hpp>

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

/**
 * @brief Lifecycle of the deferred startup body, surfaced via the
 *        `/v1/underwriter/...` HTTP endpoints.
 *
 * The endpoints register unconditionally during `plugin_startup` — every
 * `http_plugin` handler registration must happen before the posted HTTP
 * listener goes live, because the beast worker threads read the handler map
 * lock-free — so a request can arrive while the startup body is still gated
 * behind chain sync (or after it failed terminally). The handlers answer with
 * this state until the deferred startup completes, giving operators a
 * queryable surface for why underwriting isn't live. Member spellings ARE the
 * wire `status` values (via `magic_enum::enum_name`).
 */
enum class startup_state : uint8_t {
   waiting_for_sync,   ///< Sync gate not yet armed — the node is still catching up.
   preflight_retrying, ///< Gate armed; preflight incomplete, retrying within the bounded grace.
   preflight_failed,   ///< Preflight still failing after the retry grace (terminal).
   wiring_failed,      ///< Preflight passed but outpost client wiring threw (terminal).
   active              ///< Deferred startup completed — the scan cron is scheduled.
};

/// `detail` carried by the {@link startup_state::preflight_failed} gate payload.
inline constexpr std::string_view preflight_failed_detail =
   "startup preflight failed after sync — depot-side state for this underwriter "
   "is incomplete; underwriting is disabled (see node log)";
/// `detail` carried by the {@link startup_state::wiring_failed} gate payload.
inline constexpr std::string_view wiring_failed_detail =
   "outpost client wiring failed after preflight; underwriting is disabled "
   "(see node log)";

/**
 * @brief The JSON body a `/v1/underwriter/...` endpoint answers with for `state`.
 *
 * Pure (unit-testable without a chain). Shape per state:
 *   - `waiting_for_sync` → `{"status":"waiting_for_sync","head_behind_sec":N}`
 *   - `preflight_failed` / `wiring_failed` → `{"status":...,"detail":...}`
 *   - `active` → `{"status":"active"}` (callers normally serve the real
 *     payload instead once active — the builders stamp the same field).
 *
 * @param state           The current deferred-startup lifecycle state.
 * @param head_behind_sec How far the head trails wall-clock now (seconds);
 *                        consumed only by the `waiting_for_sync` payload.
 * @return The response body as an `fc::variant`.
 */
inline fc::variant startup_gate_payload(startup_state state, int64_t head_behind_sec) {
   fc::mutable_variant_object body;
   body("status", std::string{magic_enum::enum_name(state)});
   if (state == startup_state::waiting_for_sync) {
      body("head_behind_sec", head_behind_sec);
   } else if (state == startup_state::preflight_failed) {
      body("detail", std::string{preflight_failed_detail});
   } else if (state == startup_state::wiring_failed) {
      body("detail", std::string{wiring_failed_detail});
   }
   return fc::variant(std::move(body));
}

} // namespace sysio::underwriter_detail

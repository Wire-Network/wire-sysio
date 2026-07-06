#pragma once
/**
 * @file sync_detail.hpp
 * @brief Pure sync-gate predicate for the underwriter plugin, lifted out of the
 *        `.cpp`-private impl so it is unit-testable without standing up a chain.
 *
 * The underwriter's startup preflight validates depot-side state (opreg
 * registration, chain registry, authex links) via LOCAL table reads. On a
 * cold-booting operator node those reads see mid-sync (possibly genesis)
 * state and fail spuriously, so the plugin must not begin underwriting until
 * the node is synced. "Synced" here means the LAST IRREVERSIBLE block's time
 * is within a small window of wall-clock now — the irreversible state is what
 * the plugin's table reads actually serve (operator daemons run read-mode =
 * irreversible).
 */

#include <fc/time.hpp>
#include <fc/variant_object.hpp>

#include <magic_enum/magic_enum.hpp>

#include <optional>

namespace sysio::underwriter_detail {

/**
 * @brief True when a block time is recent enough to treat the node as synced.
 *
 * While a cold-booting node syncs blocks the tested time trails `now` by the
 * catch-up gap; once caught up, it tracks `now` to within finality-lag
 * jitter. The window must exceed that jitter (plus scheduling slack) but stay
 * well under one epoch so underwriting never starts against stale state.
 *
 * @param head_time      The block timestamp to test — the caller's sync
 *                       criterion (the underwriter feeds the LAST IRREVERSIBLE
 *                       block's time, since that is the state its reads serve).
 * @param now            Wall-clock now.
 * @param recency_window Maximum behind-now gap still considered synced.
 * @return True when `head_time >= now - recency_window`.
 */
inline bool head_is_recent(fc::time_point head_time, fc::time_point now,
                           fc::microseconds recency_window) {
   return head_time >= now - recency_window;
}

/// JSON field keys shared by the gate payloads below, the post-startup
/// response builders in `underwriter_plugin.cpp`, and their tests — one
/// spelling authority instead of per-translation-unit string literals.
namespace field {
   inline constexpr auto status          = "status";
   inline constexpr auto head_behind_sec = "head_behind_sec";
   inline constexpr auto lib_behind_sec  = "lib_behind_sec";
   inline constexpr auto detail          = "detail";
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
   startup_failed,     ///< The deferred startup body threw past the specific
                       ///< failure paths above (terminal). Exists so an escaping
                       ///< exception is contained as a diagnosable gate state
                       ///< instead of unwinding the app executor — which would
                       ///< tear down the whole node — with the gate still
                       ///< claiming `waiting_for_sync`.
   active              ///< Deferred startup completed — the scan cron is scheduled.
};

/// The wire `status` spelling of {@link startup_state::active} — shared by the
/// post-startup response builders so the value has one authority beside the
/// enum whose member spellings define the wire format.
inline constexpr std::string_view active_status = magic_enum::enum_name(startup_state::active);

/// `detail` carried by the {@link startup_state::preflight_failed} gate payload.
inline constexpr std::string_view preflight_failed_detail =
   "startup preflight failed after sync — depot-side state for this underwriter "
   "is incomplete; underwriting is disabled (see node log)";
/// `detail` carried by the {@link startup_state::wiring_failed} gate payload.
inline constexpr std::string_view wiring_failed_detail =
   "outpost client wiring failed after preflight; underwriting is disabled "
   "(see node log)";
/// `detail` carried by the {@link startup_state::startup_failed} gate payload.
inline constexpr std::string_view startup_failed_detail =
   "deferred startup failed unexpectedly; underwriting is disabled "
   "(see node log)";

/**
 * @brief The JSON body a `/v1/underwriter/...` endpoint answers with for `state`.
 *
 * Pure (unit-testable without a chain). Shape per state:
 *   - `waiting_for_sync` → `{"status":"waiting_for_sync","head_behind_sec":N,
 *     "lib_behind_sec":M}` — the gate's criterion is the IRREVERSIBLE state
 *     (`lib_behind_sec`); the head gap is reported alongside so a
 *     head-current-but-finality-stalled node is distinguishable from one
 *     still catching up. `lib_behind_sec` is -1 until an irreversible root
 *     exists (matching the startup wait log).
 *   - `preflight_retrying` → `{"status":"preflight_retrying"}`
 *   - `preflight_failed` / `wiring_failed` / `startup_failed` →
 *     `{"status":...,"detail":...}`
 *   - `active` → `{"status":"active"}` (callers normally serve the real
 *     payload instead once active — the builders stamp the same field).
 *
 * @param state       The current deferred-startup lifecycle state.
 * @param head_behind How far the head trails wall-clock now; consumed only by
 *                    the `waiting_for_sync` payload (emitted in whole seconds).
 * @param lib_behind  How far the last-irreversible block trails wall-clock
 *                    now, or empty while no irreversible root exists yet
 *                    (emitted as -1, matching the startup wait log); consumed
 *                    only by the `waiting_for_sync` payload.
 * @return The response body as an `fc::variant`.
 */
inline fc::variant startup_gate_payload(startup_state state, fc::microseconds head_behind,
                                        std::optional<fc::microseconds> lib_behind) {
   fc::mutable_variant_object body;
   body(field::status, std::string{magic_enum::enum_name(state)});
   if (state == startup_state::waiting_for_sync) {
      body(field::head_behind_sec, head_behind.to_seconds());
      body(field::lib_behind_sec, lib_behind ? lib_behind->to_seconds() : -1);
   } else if (state == startup_state::preflight_failed) {
      body(field::detail, std::string{preflight_failed_detail});
   } else if (state == startup_state::wiring_failed) {
      body(field::detail, std::string{wiring_failed_detail});
   } else if (state == startup_state::startup_failed) {
      body(field::detail, std::string{startup_failed_detail});
   }
   return fc::variant(std::move(body));
}

} // namespace sysio::underwriter_detail

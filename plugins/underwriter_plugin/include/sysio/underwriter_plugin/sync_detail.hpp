#pragma once
/**
 * @file sync_detail.hpp
 * @brief Pure gate-lifecycle machinery for the underwriter plugin — the deferred-
 *        startup states and the `/v1/underwriter/*` gate payloads — lifted out of
 *        the `.cpp`-private impl so they are unit-testable without standing up a
 *        chain.
 *
 * The underwriter's startup preflight validates depot-side state (opreg
 * registration, chain registry, authex links) via LOCAL table reads. On a
 * cold-booting operator node those reads see mid-sync (possibly genesis)
 * state and fail spuriously, so the plugin must not begin underwriting until
 * the node is synced. The sync predicate itself is `controller::is_synced()`
 * (LIB recency, woken by the `irreversible_block` channel), shared by every
 * operator-daemon plugin; this header carries only the underwriter's OWN gate
 * lifecycle surface.
 */

#include <fc/time.hpp>
#include <fc/variant_object.hpp>

#include <magic_enum/magic_enum.hpp>

#include <optional>

namespace sysio::underwriter_detail {

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
                       ///< exception is contained as a diagnosable gate state —
                       ///< stored and logged BEFORE the fail-fast shutdown — instead
                       ///< of unwinding the app executor mid-task with the gate
                       ///< still claiming `waiting_for_sync`.
   active              ///< Deferred startup completed — the scan cron is scheduled.
};

/**
 * @brief True for the terminal deferred-startup FAILURE states — the states the
 *        uniform fail-fast policy shuts the node down on (see
 *        `underwriter_plugin.cpp`'s `quit_if_startup_failed_terminally`).
 *
 * `active` is terminal success, not failure; `preflight_retrying` is transient by
 * definition (bounded by the retry grace). Kept beside the enum so a new state
 * cannot be added without deciding its fail-fast classification here.
 *
 * @param s The deferred-startup lifecycle state to classify.
 * @return True when `s` is a terminal failure state.
 */
inline constexpr bool is_terminal_failure(startup_state s) {
   return s == startup_state::preflight_failed || s == startup_state::wiring_failed ||
          s == startup_state::startup_failed;
}

/// The wire `status` spelling of {@link startup_state::active} — shared by the
/// post-startup response builders so the value has one authority beside the
/// enum whose member spellings define the wire format.
inline constexpr std::string_view active_status = magic_enum::enum_name(startup_state::active);
// Compile-time wire-format pin: renaming the enum member would silently change
// the HTTP `status` value every consumer switches on.
static_assert(active_status == "active");

/// `detail` carried by the {@link startup_state::preflight_failed} gate payload.
inline constexpr std::string_view preflight_failed_detail =
   "startup preflight failed after sync — depot-side state for this underwriter "
   "is incomplete; the node is shutting down (fail-fast; see node log)";
/// `detail` carried by the {@link startup_state::wiring_failed} gate payload.
inline constexpr std::string_view wiring_failed_detail =
   "outpost client wiring failed after preflight; the node is shutting down "
   "(fail-fast; see node log)";
/// `detail` carried by the {@link startup_state::startup_failed} gate payload.
inline constexpr std::string_view startup_failed_detail =
   "deferred startup failed unexpectedly; the node is shutting down "
   "(fail-fast; see node log)";

/**
 * @brief The JSON body a `/v1/underwriter/...` endpoint answers with for `state`.
 *
 * Pure (unit-testable without a chain). Shape per state:
 *   - `waiting_for_sync` → `{"status":"waiting_for_sync","head_behind_sec":N
 *     [,"lib_behind_sec":M]}` — the gate's criterion is the IRREVERSIBLE
 *     state (`lib_behind_sec`); the head gap is reported alongside so a
 *     head-current-but-finality-stalled node is distinguishable from one
 *     still catching up. `lib_behind_sec` is ABSENT until an irreversible
 *     root exists: an in-band sentinel would collide with real negative
 *     gaps (clock skew) after whole-second truncation.
 *   - `preflight_retrying` → `{"status":"preflight_retrying"}`
 *   - `preflight_failed` / `wiring_failed` / `startup_failed` →
 *     `{"status":...,"detail":...}`
 *   - `active` → `{"status":"active"}` (callers normally serve the real
 *     payload instead once active — the builders stamp the same field).
 *
 * @param state       The current deferred-startup lifecycle state.
 * @param head_behind How far the head trails wall-clock now; consumed only by
 *                    the `waiting_for_sync` payload (emitted in whole seconds;
 *                    may be negative under clock skew).
 * @param lib_behind  How far the last-irreversible block trails wall-clock
 *                    now, or empty while no irreversible root exists yet (the
 *                    key is then omitted); consumed only by the
 *                    `waiting_for_sync` payload.
 * @return The response body as an `fc::variant`.
 */
inline fc::variant startup_gate_payload(startup_state state, fc::microseconds head_behind,
                                        std::optional<fc::microseconds> lib_behind) {
   fc::mutable_variant_object body;
   body(field::status, std::string{magic_enum::enum_name(state)});
   if (state == startup_state::waiting_for_sync) {
      body(field::head_behind_sec, head_behind.to_seconds());
      if (lib_behind) {
         body(field::lib_behind_sec, lib_behind->to_seconds());
      }
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

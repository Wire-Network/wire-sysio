#pragma once

#include <string_view>

/**
 * @file
 * Depot-side helpers for the `sysio.opreg::operators[].status` field.
 *
 * Both `batch_operator_plugin` and `underwriter_plugin` poll their own
 * status row each tick to decide whether to keep relaying. The string
 * spellings come from the protobuf enum (`OPERATOR_STATUS_*`), so we
 * pin them in one place and let both plugins consume them via this
 * header — no plugin dependency, no duplicated string literals.
 */
namespace sysio::depot::opreg_status {

/// Protobuf enum spellings as they surface through the ABI serializer
/// when the `operators` table row is decoded. Keep in lockstep with
/// `OperatorStatus` in `libraries/opp/proto/types.proto`.
inline constexpr std::string_view active     = "OPERATOR_STATUS_ACTIVE";
inline constexpr std::string_view slashed    = "OPERATOR_STATUS_SLASHED";
inline constexpr std::string_view terminated = "OPERATOR_STATUS_TERMINATED";

/**
 * Map a status string to an `is_active` flag for the relay loop.
 *
 * Callers pass the previous `is_active` so the helper can preserve it
 * for transient / unknown statuses (`OPERATOR_STATUS_STANDBY`,
 * `OPERATOR_STATUS_UNKNOWN`, an empty string from a stale read, etc.).
 * That avoids spurious flips when the row is momentarily unavailable
 * — only `ACTIVE` and the terminal `SLASHED` / `TERMINATED` states
 * actually toggle the flag.
 *
 * @param status        status string read from the operators row.
 * @param previous      `is_active` value from the prior tick.
 * @return              true iff the operator should keep relaying.
 */
inline bool compute_is_active(std::string_view status, bool previous) noexcept {
   if (status == active)                            return true;
   if (status == slashed || status == terminated)   return false;
   return previous;
}

} // namespace sysio::depot::opreg_status

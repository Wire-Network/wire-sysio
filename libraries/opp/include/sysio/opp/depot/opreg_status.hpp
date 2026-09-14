#pragma once

#include <string_view>

#include <sysio/opp/types/types.pb.h>

/**
 * @file
 * Depot-side helper for the `sysio.opreg::operators[].status` field.
 *
 * Both `batch_operator_plugin` and `underwriter_plugin` poll their own status
 * row each tick to decide whether to keep relaying. The spelling set IS the
 * protobuf `OperatorStatus` enum, so the string is parsed through the generated
 * descriptor rather than compared against copies of its names — a proto rename
 * reaches this decision through the compiler and the parse, never as a silent
 * fall-through.
 */
namespace sysio::opp::depot::opreg_status {

/**
 * Map a status string to an `is_active` flag for the relay loop.
 *
 * Callers pass the previous `is_active` so the helper can preserve it for the
 * transient states (`WARMUP`, `COOLDOWN`), `UNKNOWN`, and any spelling that
 * fails to parse (an empty string from a stale read). That avoids spurious
 * flips when the row is momentarily unavailable — only `ACTIVE` and the
 * terminal `TERMINATED` / `SLASHED` states actually toggle the flag.
 *
 * @param status   status string read from the operators row.
 * @param previous `is_active` value from the prior tick.
 * @return         true iff the operator should keep relaying.
 */
inline bool compute_is_active(std::string_view status, bool previous) {
   types::OperatorStatus parsed{};
   if (!types::OperatorStatus_Parse(status, &parsed)) return previous;

   switch (parsed) {
      case types::OPERATOR_STATUS_ACTIVE:     return true;
      case types::OPERATOR_STATUS_TERMINATED:
      case types::OPERATOR_STATUS_SLASHED:    return false;
      default:                                return previous;  // WARMUP / COOLDOWN / UNKNOWN
   }
}

} // namespace sysio::opp::depot::opreg_status

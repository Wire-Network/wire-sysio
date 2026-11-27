#pragma once

#include <string_view>

#include <sysio/opp/types/types.pb.h>

/**
 * @file
 * Depot-side helper for the `sysio.opreg::operators[].status` field.
 *
 * The batch-operator relay polls its own status
 * row each tick to decide whether to keep pushing (`compute_is_active`) and
 * whether to keep its relay jobs running (`compute_delivers`). The spelling set
 * IS the protobuf `OperatorStatus` enum, so the string is parsed through the
 * generated descriptor rather than compared against copies of its names — a
 * proto rename reaches these decisions through the compiler and the parse,
 * never as a silent fall-through.
 */
namespace sysio::opp::depot::opreg_status {

/**
 * Map a status string to the relay's `is_active` flag, which gates its
 * `chkcons` and depot crank pushes.
 *
 * Callers pass the previous `is_active` so the helper can preserve it for the
 * transient states (`WARMUP`, `COOLDOWN`), `UNKNOWN`, and any spelling that
 * fails to parse (an empty string from a stale read). That avoids spurious
 * flips when the row is momentarily unavailable — only `ACTIVE` and the
 * terminal `TERMINATED` / `SLASHED` states actually toggle the flag.
 *
 * @param status   status string read from the operators row.
 * @param previous `is_active` value from the prior tick.
 * @return         true iff the operator should keep pushing.
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

/**
 * Whether `sysio.msgch::deliver` accepts a delivery from an operator with this
 * status, which gates the relay jobs: only `ACTIVE` passes its status check.
 * `UNKNOWN` (registered without its minimum collateral, or fallen below it),
 * `WARMUP`, `COOLDOWN` and the terminal `TERMINATED` / `SLASHED` are refused.
 *
 * A spelling that fails to parse keeps `previous`, as for `compute_is_active`.
 *
 * @param status   status string read from the operators row.
 * @param previous decision from the prior tick.
 * @return         true iff `deliver` accepts the status.
 */
inline bool compute_delivers(std::string_view status, bool previous) {
   types::OperatorStatus parsed{};
   if (!types::OperatorStatus_Parse(status, &parsed)) return previous;
   return parsed == types::OPERATOR_STATUS_ACTIVE;
}

} // namespace sysio::opp::depot::opreg_status

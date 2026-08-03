#pragma once
/**
 * @file name_ops.hpp
 * @brief Non-throwing parse of a cross-chain account string into a `sysio::name`.
 *
 * Companion to `safe_ops.hpp` for the OPP inbound-consensus dispatch path. Where
 * `safe_ops.hpp` is deliberately dependency-free so its predicates can be unit
 * tested on the native host (no CDT `name` on the include path), this header
 * constructs a CDT `name` and therefore pulls in `<sysio/name.hpp>`; it is
 * included only from WASM contract translation units.
 *
 * The foot-gun it closes: constructing `name{std::string_view}` from an
 * operator-relayed cross-chain account string. CDT's `name` constructor
 * `check(false, ...)`-aborts on a string longer than 13 characters, one holding a
 * character outside ".12345abcdefghijklmnopqrstuvwxyz", or one whose 13th symbol
 * exceeds the 4-bit final slot. An abort inside a handler reached from
 * `sysio.msgch::deliver -> evalcons -> dispatch` rolls back the consensus-tipping
 * delivery and stalls epoch advancement chain-wide
 * (`feedback_opp_handlers_never_throw.md`). Every dispatch handler that turns a
 * `WireAccount.name` (or a CHAIN_KIND_WIRE `ChainAddress`) into a `name` routes
 * through `parse_wire_account_name`, so validation and construction are
 * inseparable and live in one auditable place.
 */

#include <sysio/name.hpp>
#include <sysio.opp.common/safe_ops.hpp>   // is_valid_name_string
#include <optional>
#include <string_view>

namespace sysio::opp::safe {

/// Parse an inbound account string into a validated `sysio::name`, or
/// `std::nullopt` when the string is empty or not a canonical account name.
///
/// Validation is delegated to `is_valid_name_string`, which mirrors CDT
/// `basic_name`'s charset, length, and final-symbol rules exactly, so the returned
/// `name{s}` is guaranteed to construct without aborting — in particular a
/// legitimate 13-byte name is accepted where a naive `size() > 12` cap would
/// wrongly reject it. Callers on the OPP dispatch path MUST treat `std::nullopt`
/// as "drop this message" and `return`, never as a reason to `check()`-abort.
///
/// @param s the candidate account string (no leading/trailing trimming).
/// @return the constructed `name` iff `s` is a nonempty canonical account name.
inline std::optional<sysio::name> parse_wire_account_name(std::string_view s) {
   if (s.empty() || !is_valid_name_string(s)) return std::nullopt;
   return sysio::name{s};
}

} // namespace sysio::opp::safe

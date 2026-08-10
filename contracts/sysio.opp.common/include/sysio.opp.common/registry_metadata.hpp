#pragma once
/**
 * @file registry_metadata.hpp
 * @brief Byte bounds for the human-readable metadata every depot registry row carries.
 *
 * The depot's three system-owned registries -- `sysio.chains::chains`,
 * `sysio.tokens::tokens`, and `sysio.reserv::reserves` -- each persist two
 * free-form strings alongside their typed columns: a short display LABEL
 * (`Chain.name`, `Token.symbol_name`, `Reserve.name`) and a longer
 * DESCRIPTION. Both land in chain state rather than transient action data.
 *
 * Those rows bill to `ram_payer = "sysio"` -- the shared system pool -- so an
 * unbounded string is not merely cosmetic: a privileged operator, a compromised
 * privileged account, or a faulty governance/admin workflow can make each unique
 * registry key consume up to the KV/action ceiling instead of a
 * business-appropriate size, inflating state and making the registries expensive
 * to inspect. Every registration action bounds both strings before `emplace`.
 *
 * The limits live here, once, so the three registries cannot drift apart, and
 * so raising a bound is a single reviewed edit rather than three. The precedent
 * for bounding a persisted contract string is `sysio.token::issue`'s
 * `memo.size() <= 256`; `description_max_bytes` matches it deliberately.
 */

#include <sysio/check.hpp>

#include <cstddef>
#include <string>
#include <string_view>

namespace sysio::opp::registry {

/// Maximum byte length of a registry row's short display label --
/// `Chain.name`, `Token.symbol_name`, `Reserve.name`.
///
/// The bound exists to stop a row consuming up to the KV/action ceiling of
/// system-paid state; it is NOT a house style for terse names. Real labels run
/// well past ticker length -- a reserve names its full leg
/// ("ETHEREUM-ETH/WIRE unlinked-creator reserve" is 42 bytes) -- so the bound
/// is set with room for descriptive names rather than at the current longest.
/// A label that has to be shortened to satisfy this is a sign the bound is
/// wrong, not the label.
inline constexpr std::size_t label_max_bytes = 128;

/// Maximum byte length of a registry row's free-form description. Matches the
/// established `sysio.token::issue` memo bound.
inline constexpr std::size_t description_max_bytes = 256;

/**
 * @brief Bound the two system-paid metadata strings of a registry row.
 *
 * Call before the row is emplaced/modified, so an oversized string never
 * reaches chain state.
 *
 * @param label       The row's short display label (`name` / `symbol_name`).
 * @param description The row's free-form description.
 * @param context     Contract-scoped message prefix, e.g. `"sysio.tokens"`.
 */
inline void check_metadata(std::string_view label,
                           std::string_view description,
                           std::string_view context) {
   // Build the message only on failure -- `sysio::check(bool, const std::string&)`
   // would otherwise construct it on every passing call.
   if (label.size() > label_max_bytes) {
      sysio::check(false, std::string(context) + ": label exceeds "
                          + std::to_string(label_max_bytes) + " bytes");
   }
   if (description.size() > description_max_bytes) {
      sysio::check(false, std::string(context) + ": description exceeds "
                          + std::to_string(description_max_bytes) + " bytes");
   }
}

/**
 * @brief Non-throwing counterpart of `check_metadata`, for OPP inbound dispatch handlers.
 *
 * A handler reachable from `sysio.msgch::deliver -> evalcons -> dispatch` MUST NOT abort
 * on operator-relayed data: a `check()` there rolls back the consensus-tipping delivery
 * and stalls epoch advancement chain-wide (`feedback_opp_handlers_never_throw`). Such a
 * handler asks this instead, and routes an over-bound row into its own reject/refund path.
 *
 * @return true when either string is over its bound.
 */
inline bool metadata_exceeds_bounds(std::string_view label, std::string_view description) {
   return label.size() > label_max_bytes || description.size() > description_max_bytes;
}

/**
 * @brief The label a handler stores on a row it rejected for over-bound metadata.
 *
 * The rejected strings are NOT truncated onto the row. Truncation would have to cut at a
 * byte offset rather than a UTF-8 code-point boundary (splitting a multi-byte character), and
 * the salvaged text buys nothing: nothing reads a tombstone's metadata — the reclaim path
 * overwrites every field — and the creator's original strings are preserved in the inbound
 * OPP envelope artifact regardless. A short fixed marker states plainly that the row carries
 * no metadata BECAUSE it was rejected, rather than leaving a blank a reader has to interpret.
 */
inline constexpr std::string_view rejected_label = "<rejected>";

} // namespace sysio::opp::registry

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
/// `Chain.name`, `Token.symbol_name`, `Reserve.name`. These are ticker- or
/// title-sized ("Wire", "Ethereum", "wire-depot"), never prose.
inline constexpr std::size_t label_max_bytes = 32;

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

/// Clamp a label for storage on a reject-path tombstone row. The tombstone still lands in
/// `sysio`-billed state, so an over-bound string must be truncated rather than stored
/// verbatim — otherwise rejecting an oversized registration would persist exactly the
/// state the bound exists to prevent.
inline std::string truncate_label(std::string label) {
   if (label.size() > label_max_bytes) label.resize(label_max_bytes);
   return label;
}

/// Clamp a description for storage on a reject-path tombstone row. See `truncate_label`.
inline std::string truncate_description(std::string description) {
   if (description.size() > description_max_bytes) description.resize(description_max_bytes);
   return description;
}

} // namespace sysio::opp::registry

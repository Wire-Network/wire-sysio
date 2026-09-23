#pragma once
/**
 * @file registry_codes.hpp
 * @brief The spelling guard every depot registry writer applies to a `slug_name` code.
 *
 * A `slug_name` is carried on the wire and in chain state as a packed `uint64`, but its
 * JSON carrier is the canonical STRING spelling. Not every 64-bit value has one -- `7`
 * does not -- and the raw `slug_name{uint64}` constructor does not check, by design,
 * because the OPP dispatch surfaces must never throw on operator-relayed data.
 *
 * `fc::slug_name::to_variant` is TOTAL -- it renders `to_string()` without asserting
 * canonicality, because a throwing read path turns one bad row into a failure of every
 * scan over it. So an uncanonical code does not fail loudly, it renders MISLEADINGLY:
 * `to_string()` reads only bits 0-47 and stops at the first zero symbol, so
 * `pack("ETH") | 1<<63` renders "ETH" and re-parses to a different value.
 *
 * Hence this guard. A registered code stays canonical, and distinct raw values never
 * alias onto one spelling. A depot registry has no erase action, so a bad row is
 * permanent -- and now silent. Registry writers are privileged top-level actions, so
 * unlike the OPP dispatch surfaces they CAN refuse: `sysio::check` reverts one admin
 * transaction and writes nothing, which is what lets a registry LOOKUP stand in for a
 * canonicality check.
 *
 * Mirrors the shape of `registry_metadata.hpp`: a throwing helper for the admin writers,
 * while a never-throw dispatch handler asks `slug_name::is_canonical()` directly and
 * routes the failure into its own drop/refund path.
 */

#include <sysio/check.hpp>
#include <sysio/slug_name.hpp>

#include <initializer_list>
#include <string>
#include <string_view>

namespace sysio::opp::registry {

/**
 * @brief Refuse a code with no canonical string spelling before it reaches chain state.
 *
 * Call before the row is emplaced/modified, alongside `check_metadata`.
 *
 * The zero code passes deliberately: it spells as `""`, which is a valid literal that
 * packs back to zero, so it round-trips like any other code. Whether an EMPTY code
 * belongs in a given registry row is that registry's own question -- this guard is only
 * about values that do not survive the round trip.
 *
 * @param codes   The row's `slug_name` columns.
 * @param context Contract-scoped message prefix, e.g. `"sysio.tokens"`.
 */
inline void check_codes(std::initializer_list<sysio::slug_name> codes, std::string_view context) {
   for (const sysio::slug_name code : codes) {
      // Build the message only on failure -- `sysio::check(bool, const std::string&)`
      // would otherwise construct it on every passing call.
      if (!code.is_canonical()) {
         sysio::check(false, std::string(context) + ": code " + std::to_string(code.value)
                             + " has no canonical slug_name spelling");
      }
   }
}

} // namespace sysio::opp::registry

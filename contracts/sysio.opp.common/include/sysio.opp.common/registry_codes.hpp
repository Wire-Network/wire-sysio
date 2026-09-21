#pragma once
/**
 * @file registry_codes.hpp
 * @brief The spelling guard every depot registry writer applies to a `slug_name` code.
 *
 * A `slug_name` is carried on the wire and in chain state as a packed `uint64`, but its
 * JSON carrier is the canonical STRING spelling: `fc::slug_name`'s `to_variant` renders
 * `to_string()` and asserts the value round-trips. Not every 64-bit value has such a
 * spelling -- `7` does not -- and the raw `slug_name{uint64}` constructor does not check,
 * by design, because the OPP dispatch surfaces must never throw on operator-relayed data.
 *
 * The consequence is asymmetric: a code with no spelling can be WRITTEN into a registry
 * row, and every later attempt to RENDER that row throws. A depot registry has no erase
 * action, so such a row is permanent, and the readers that trip over it are not the writer
 * -- `get_table_rows` falls back to hex per cell, while the underwriter plugin's scan
 * reaches an unconditional `get_object()` and drops its entire cycle.
 *
 * Registry writers are privileged, top-level actions rather than dispatch handlers, so
 * unlike the OPP surfaces they CAN refuse: `sysio::check` here reverts one admin
 * transaction and writes nothing. That is the whole point of this guard -- it keeps the
 * "a registered code is a renderable code" invariant true by construction, which is what
 * lets every downstream registry LOOKUP stand in for a spelling check.
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
 * packs back to zero, so it renders and round-trips like any other code. Whether an
 * EMPTY code belongs in a given registry row is that registry's own question -- this
 * guard is only about values that cannot be rendered at all.
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

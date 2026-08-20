#pragma once

#include <string>
#include <string_view>

/**
 * @file
 * Depot-side view of the `sysio.chains::chains` registry row.
 *
 * Both `batch_operator_plugin` and `underwriter_plugin` read the same rows to
 * discover the chains they serve and the remote contracts they talk to. The
 * field spellings and the one non-obvious decoding rule live here so the two
 * daemons cannot drift apart — no plugin dependency, no duplicated literals.
 *
 * Keep in lockstep with `chain_row` in
 * `contracts/sysio.chains/include/sysio.chains/sysio.chains.hpp`.
 */
namespace sysio::depot::chains {

/// Account and table the registry lives on.
inline constexpr auto account      = "sysio.chains";
inline constexpr auto table_chains = "chains";

/// Field names on a `chain_row` as they surface through the ABI serializer.
namespace field {
   inline constexpr auto code              = "code";              // {value: uint64} slug_name
   inline constexpr auto kind              = "kind";              // ChainKind enum (string spelling)
   inline constexpr auto external_chain_id = "external_chain_id"; // uint32
   inline constexpr auto is_depot          = "is_depot";          // bool — the single WIRE-self row
   inline constexpr auto active            = "active";            // bool
   inline constexpr auto outpost           = "outpost";           // nested outpost_addrs struct

   /// Field names on the nested `outpost` struct. `sysio.chains` validates the
   /// set against the row's kind before it is stored, so a non-empty value here
   /// is already well-formed for that chain; a reader only has to check presence.
   namespace outpost_addr {
      inline constexpr auto opp_addr               = "opp_addr";
      inline constexpr auto opp_inbound_addr       = "opp_inbound_addr";
      inline constexpr auto operator_registry_addr = "operator_registry_addr";
      inline constexpr auto source_deposit_addr    = "source_deposit_addr";
   }
}

/**
 * @brief Resolve the address of one remote role, applying the single-program rule.
 *
 * An EVM outpost deploys a separate contract per role, so each role field names
 * its own address. An SVM outpost is ONE program serving every role, so the
 * registry stores it in `opp_addr` and `sysio.chains` REQUIRES the role fields
 * to be empty. A reader that wants a specific role therefore falls back to
 * `opp_addr` whenever the role field is empty, and both deployment shapes are
 * handled without branching on `ChainKind` at every call site.
 *
 * @param role_addr Role-specific field from the row's `outpost` struct.
 * @param opp_addr  The row's `opp_addr` field.
 * @return The role's address, or empty when the row is not configured yet.
 */
inline std::string resolve_role_addr(std::string_view role_addr, std::string_view opp_addr) {
   return role_addr.empty() ? std::string{opp_addr} : std::string{role_addr};
}

} // namespace sysio::depot::chains

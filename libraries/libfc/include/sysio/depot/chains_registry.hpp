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
 * @brief Resolve the address of one remote role for a chain's deployment shape.
 *
 * An SVM outpost is ONE program serving every role: the registry stores it in
 * `opp_addr` and `sysio.chains` REQUIRES the role fields to be empty, so every
 * role resolves to `opp_addr`. An EVM outpost deploys a SEPARATE contract per
 * role, so a role resolves to its own field and NOTHING else.
 *
 * The EVM case must not fall back to `opp_addr`. `setoutpost` accepts a partial
 * EVM set — a row may legitimately carry the OPP address before its
 * OperatorRegistry is deployed — and substituting `opp_addr` there would yield a
 * plausible, non-empty, WRONG address: it passes any is-it-configured check and
 * then sends commits or verification reads to the OPP contract. An unset EVM
 * role stays empty so the caller fails closed and waits for `setoutpost`.
 *
 * @param role_addr      Role-specific field from the row's `outpost` struct.
 * @param opp_addr       The row's `opp_addr` field.
 * @param single_program True when one program serves every role (SVM).
 * @return The role's address, or empty when this role is not configured yet.
 */
inline std::string resolve_role_addr(std::string_view role_addr,
                                     std::string_view opp_addr,
                                     bool             single_program) {
   return single_program ? std::string{opp_addr} : std::string{role_addr};
}

} // namespace sysio::depot::chains

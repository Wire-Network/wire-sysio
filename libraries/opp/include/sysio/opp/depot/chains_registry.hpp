#pragma once

/**
 * @file
 * Depot-side view of the `sysio.chains::chains` registry row.
 *
 * The batch-operator relay reads these rows to discover its served chains and
 * remote contracts. The shared field spellings keep registry clients
 * consistent without plugin dependencies.
 *
 * Keep in lockstep with `chain_row` in
 * `contracts/sysio.chains/include/sysio.chains/sysio.chains.hpp`.
 */
namespace sysio::opp::depot::chains {

/// Account and table the registry lives on.
inline constexpr auto account      = "sysio.chains";
inline constexpr auto table_chains = "chains";

/// Field names on a `chain_row` as they surface through the ABI serializer.
namespace field {
   inline constexpr auto code              = "code";              // slug_name (canonical string)
   inline constexpr auto kind              = "kind";              // ChainKind enum (string spelling)
   inline constexpr auto external_chain_id = "external_chain_id"; // uint32
   inline constexpr auto is_depot          = "is_depot";          // bool — the single WIRE-self row
   inline constexpr auto active            = "active";            // bool
   inline constexpr auto outpost           = "outpost";           // nested outpost_addrs struct

   /// Field names on the nested `outpost` struct. `sysio.chains` validates the
   /// set against the row's kind before it is stored, so a non-empty value here
   /// is already well-formed for that chain; a reader only has to check presence.
   namespace outpost_addr {
      inline constexpr auto opp_addr         = "opp_addr";
      inline constexpr auto opp_inbound_addr = "opp_inbound_addr";
   }
}

} // namespace sysio::opp::depot::chains

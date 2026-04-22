#pragma once

// =============================================================================
// Read-only mirror of sysio.opreg table types, exposed for cross-contract
// readers (notably sysio.system's emissions). No [[sysio::table]] attribute
// on these structs -- they MUST NOT emit ABI entries on the consumer's side.
//
// INVARIANT: field order and types here MUST track the canonical definitions
// in sysio.opreg.hpp. A mismatch deserializes garbage bytes at runtime with
// no compile-time signal.
//
// When adding / reordering / renaming a field on the canonical operator_entry,
// update this file in the SAME commit. The two definitions live in the same
// directory so reviewers see them side by side.
// =============================================================================

#include <sysio/kv_table.hpp>
#include <sysio/name.hpp>
#include <sysio/opp/types/types.pb.hpp>

#include <cstdint>
#include <vector>

namespace sysio::opreg::readonly {

struct stake_entry {
   sysio::opp::types::ChainAddress chain_addr;
   sysio::opp::types::TokenAmount  amount;
   uint64_t                        timestamp_ms = 0;

   SYSLIB_SERIALIZE(stake_entry, (chain_addr)(amount)(timestamp_ms))
};

struct operator_key {
   uint64_t account;
   SYSLIB_SERIALIZE(operator_key, (account))
};

struct operator_entry {
   sysio::name                        account;
   sysio::opp::types::OperatorType    type;
   sysio::opp::types::OperatorStatus  status;
   bool                               is_bootstrapped = false;
   std::vector<stake_entry>           stakes;
   uint64_t                           registered_at   = 0;
   uint64_t                           available_at    = 0;
   uint64_t                           slashed_at      = 0;
   uint64_t                           terminated_at   = 0;

   uint64_t by_type()   const { return static_cast<uint64_t>(type);   }
   uint64_t by_status() const { return static_cast<uint64_t>(status); }

   SYSLIB_SERIALIZE(operator_entry,
      (account)(type)(status)(is_bootstrapped)(stakes)
      (registered_at)(available_at)(slashed_at)(terminated_at))
};

using operators_t = sysio::kv::table<"operators"_n, operator_key, operator_entry,
   sysio::kv::index<"bytype"_n,
      sysio::const_mem_fun<operator_entry, uint64_t, &operator_entry::by_type>>,
   sysio::kv::index<"bystatus"_n,
      sysio::const_mem_fun<operator_entry, uint64_t, &operator_entry::by_status>>
>;

} // namespace sysio::opreg::readonly

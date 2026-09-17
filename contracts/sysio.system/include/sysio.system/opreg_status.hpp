#pragma once

#include <sysio/name.hpp>
#include <sysio/opp/types/types.pb.hpp>
#include <sysio.opreg/sysio.opreg.hpp>

#include <optional>

// Shared opreg-status predicate for sysio.system. Kept in its own header --
// rather than as a system_contract member or in emissions.hpp -- because it
// depends on the OPP protobuf types (types.pb.hpp). emissions.hpp is
// deliberately protobuf-free so proto-free downstream contracts (notably
// sysio.roa) can include it, and sysio.system.hpp is included by many other
// contracts; pulling the proto dependency into either would widen their
// include surface. Only the two translation units that need the predicate --
// reward distribution (emissions.cpp) and producer scheduling (ranking.cpp) --
// include this header, and both already compile with the OPP include dirs.

namespace sysiosystem {

/// Well-known sysio.opreg (operator registry) account. Single definition shared
/// by the translation units that read the operator roster cross-contract.
namespace opreg_refs {
   constexpr sysio::name account = "sysio.opreg"_n;
}

/// Return `account`'s operator row when it has the requested role and ACTIVE status.
///
/// Any other status (WARMUP / COOLDOWN / UNKNOWN / SLASHED / TERMINATED), a
/// type mismatch, or a missing operator row returns nullopt. This is the
/// single lookup for live role standing; callers that need more than a boolean
/// reuse the returned row instead of reading sysio.opreg again.
/// Requiring the exact `expected_type` prevents an account collateralized for
/// one role (e.g. a batch operator, backed by req_batchop_collat) from
/// satisfying the gate for a different role (e.g. producer, backed by
/// req_prod_collat) merely because its status is ACTIVE.
///
/// @param account       operator account to check.
/// @param expected_type operator role the caller requires.
/// @return the matching ACTIVE operator row, or nullopt.
inline std::optional<sysio::opreg::operator_entry>
find_active_operator( const sysio::name& account,
                      sysio::opp::types::OperatorType expected_type ) {
   sysio::opreg::operators_t ops( opreg_refs::account );
   const auto key = sysio::opreg::operator_key{ account.value };
   if( !ops.contains( key ) )
      return std::nullopt;
   const auto op = ops.get( key );
   if( op.status != sysio::opp::types::OperatorStatus::OPERATOR_STATUS_ACTIVE
       || op.type != expected_type ) {
      return std::nullopt;
   }
   return op;
}

/// Return whether `account` has the requested role and ACTIVE status.
///
/// @param account       operator account to check.
/// @param expected_type operator role the caller requires.
/// @return true iff {@link find_active_operator} returns a row.
inline bool is_op_active( const sysio::name& account,
                          sysio::opp::types::OperatorType expected_type ) {
   return find_active_operator( account, expected_type ).has_value();
}

} // namespace sysiosystem

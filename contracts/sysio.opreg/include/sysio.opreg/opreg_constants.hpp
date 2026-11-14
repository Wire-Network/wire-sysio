#pragma once

/// Names of the sysio.opreg operator roster, shared by the contract and the host code that reads it. Include only
/// headers that both CDT and the host provide.
namespace sysio::opreg_names {

/// Account the contract is deployed on.
inline constexpr auto account = "sysio.opreg";

/// Table of the operators, keyed by account.
inline constexpr auto table_operators = "operators";

/// Secondary index of `operators` by operator type.
inline constexpr auto index_bytype = "bytype";

/// Secondary index of `operators` by operator status.
inline constexpr auto index_bystatus = "bystatus";

/// Field names of an `operators` row.
namespace field {

/// The operator's status.
inline constexpr auto status = "status";

} // namespace field

} // namespace sysio::opreg_names

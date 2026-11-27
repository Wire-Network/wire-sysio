#pragma once

#include <cstdint>

/// Names and enumerations of the sysio.opreg operator roster and chain memberships, shared by the contract and the
/// host code that reads them. Include only headers that both CDT and the host provide.
namespace sysio::opreg_names {

/// Account the contract is deployed on.
inline constexpr auto account = "sysio.opreg";

/// Table of the operators, keyed by account.
inline constexpr auto table_operators = "operators";

/// Secondary index of `operators` by operator type.
inline constexpr auto index_bytype = "bytype";

/// Secondary index of `operators` by operator status.
inline constexpr auto index_bystatus = "bystatus";

/// Table of batch operators' chain memberships, keyed by chain then account.
inline constexpr auto table_servchains = "servchains";

/// Secondary index of `servchains` by account.
inline constexpr auto index_byaccount = "byaccount";

/// Field names of an `operators` or `servchains` row.
namespace field {

/// The operator's status, or its membership's status on a chain.
inline constexpr auto status = "status";

/// The operator's account.
inline constexpr auto account = "account";

/// The membership's outpost chain code.
inline constexpr auto chain_code = "chain_code";

} // namespace field

/// A batch operator's standing on one outpost chain (`servchains::status`). Only SERVING draws new seats there.
enum class chain_service_status : uint8_t {
   SERVING,   ///< draws new seats on the chain
   LEFT,      ///< left by the operator; the seats it holds still owe their duty
   EVICTED,   ///< removed by the chain's miss rails; its deliveries on the chain are refused
};

} // namespace sysio::opreg_names

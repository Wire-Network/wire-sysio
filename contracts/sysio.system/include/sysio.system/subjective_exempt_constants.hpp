#pragma once

/// Names of the sysio.system list of accounts exempt from subjective CPU billing, shared by the contract and the host
/// code that reads its table. Include only headers that both CDT and the host provide.
namespace sysiosystem::subjective_exemption {

/// Table of the exempt accounts, keyed by account.
inline constexpr auto table_exempt = "subjexempt";

} // namespace sysiosystem::subjective_exemption

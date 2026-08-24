#pragma once

#include <cstdint>

namespace sysio::protocol::snapshot_attestation {

/// Contract error code emitted when a provider's local tuple conflicts with a final record.
inline constexpr uint64_t disagreement_error_code = 9001;

/// Name of the provider vote action.
inline constexpr auto action_votesnaphash = "votesnaphash";

/// Name of the permissionless pending-vote evaluation action.
inline constexpr auto action_evalsnapvote = "evalsnapvote";

/// Name of the final snapshot-attestation record table.
inline constexpr auto table_snaprecords = "snaprecords";

/// Name of the pending snapshot-vote table.
inline constexpr auto table_snapvotes = "snapvotes";

/// ABI field identifiers used by host-side table readers.
namespace field {

/// Chain block that included the final attestation record.
inline constexpr auto attested_at_block = "attested_at_block";

/// Final or pending tuple block identifier.
inline constexpr auto block_id = "block_id";

/// Final or pending tuple block height.
inline constexpr auto block_num = "block_num";

/// Pending-vote primary identifier.
inline constexpr auto id = "id";

/// Active-roster version bound to a pending vote.
inline constexpr auto roster_version = "roster_version";

/// Final or pending snapshot root hash.
inline constexpr auto snapshot_hash = "snapshot_hash";

} // namespace field

} // namespace sysio::protocol::snapshot_attestation

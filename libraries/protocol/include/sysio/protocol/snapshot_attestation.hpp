#pragma once

#include <cstdint>

namespace sysio::protocol::snapshot_attestation {

/// Contract error code emitted when a provider's local tuple conflicts with a final record.
inline constexpr uint64_t disagreement_error_code = 9001;

/// Name of the provider vote action.
inline constexpr auto action_votesnaphash = "votesnaphash";

/// Name of the final snapshot-attestation record table.
inline constexpr auto table_snaprecords = "snaprecords";

/// ABI struct stored by the final snapshot-attestation record table.
inline constexpr auto type_snap_record = "snap_record";

/// ABI primary-index kind required by the block-number keyed record table.
inline constexpr auto index_type_i64 = "i64";

/// ABI primitive type identifiers used by the final record schema.
namespace abi_type {

/// ABI spelling for a 32-byte checksum.
inline constexpr auto checksum256 = "checksum256";

/// ABI spelling for a 32-bit unsigned integer.
inline constexpr auto uint32 = "uint32";

/// ABI spelling for a 64-bit unsigned integer.
inline constexpr auto uint64 = "uint64";

} // namespace abi_type

/// ABI field identifiers used by host-side table readers.
namespace field {

/// Chain block that included the final attestation record.
inline constexpr auto attested_at_block = "attested_at_block";

/// Final tuple block identifier.
inline constexpr auto block_id = "block_id";

/// Final tuple block height.
inline constexpr auto block_num = "block_num";

/// Final snapshot root hash.
inline constexpr auto snapshot_hash = "snapshot_hash";

} // namespace field

} // namespace sysio::protocol::snapshot_attestation

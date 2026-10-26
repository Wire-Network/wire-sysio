#pragma once

#include <cstdint>

/// Snapshot-attestation constants shared by the sysio.system contract and the host code that reads its tables and
/// pushes its actions. Include only headers that both CDT and the host build provide.
namespace sysiosystem::snapshot_attestation {

/// Maximum number of producers that may participate in snapshot attestation.
inline constexpr uint32_t maximum_providers = 30;

/// Production default and largest supported distance between attested snapshot heights.
inline constexpr uint32_t default_block_spacing = 25'000;

/// Smallest supported distance between attested snapshot heights.
inline constexpr uint32_t minimum_block_spacing = 4;

/// Largest supported distance between attested snapshot heights.
inline constexpr uint32_t maximum_block_spacing = default_block_spacing;

/** Return whether a snapshot-attestation cadence is within the supported bounds. */
constexpr bool is_valid_block_spacing(uint32_t block_spacing) {
   return block_spacing >= minimum_block_spacing && block_spacing <= maximum_block_spacing;
}

/** Return whether a block height belongs to the supplied snapshot-attestation cadence. */
constexpr bool is_scheduled_block(uint32_t block_num, uint32_t block_spacing) {
   return is_valid_block_spacing(block_spacing) && block_num > 0
          && block_num % block_spacing == 0;
}

/** Return the finalized-block grace window associated with a valid cadence. */
constexpr uint32_t snapshot_attestation_grace_blocks(uint32_t block_spacing) {
   return block_spacing / 2;
}

/// Contract error code emitted when a provider's local tuple conflicts with a final record.
inline constexpr uint64_t disagreement_error_code = 9001;

/// Name of the provider vote action.
inline constexpr auto action_votesnaphash = "votesnaphash";

/// Name of the provider-registration action.
inline constexpr auto action_regsnapprov = "regsnapprov";

/// Name of the snapshot-attestation configuration action.
inline constexpr auto action_setsnapcfg = "setsnpcfg";

/// Name of the final snapshot-attestation record table.
inline constexpr auto table_snaprecords = "snaprecords";

/// Name of the governance-owned snapshot-attestation configuration table.
inline constexpr auto table_snapconfig = "snapconfig";

/// ABI struct stored by the final snapshot-attestation record table.
inline constexpr auto type_snap_record = "snap_record";

/// ABI struct stored by the snapshot-attestation configuration table.
inline constexpr auto type_snap_config = "snap_config";

/// ABI primary-index kind required by the block-number keyed record table.
inline constexpr auto index_type_i64 = "i64";

/// ABI primitive type identifiers used by the final record schema.
namespace abi_type {

/// ABI spelling for an Antelope account or identifier name.
inline constexpr auto name = "name";

/// ABI spelling for a 32-byte checksum.
inline constexpr auto checksum256 = "checksum256";

/// ABI spelling for a 32-bit unsigned integer.
inline constexpr auto uint32 = "uint32";

/// ABI spelling for a 64-bit unsigned integer.
inline constexpr auto uint64 = "uint64";

} // namespace abi_type

/// ABI field identifiers used by host-side table readers.
namespace field {

/// Primary-key field used by a global configuration row.
inline constexpr auto name = "name";

/// Fixed number of distinct producer votes required to attest a tuple.
inline constexpr auto min_providers = "min_providers";

/// Immutable distance between eligible snapshot-attestation heights.
inline constexpr auto block_spacing = "block_spacing";

/// Producer account associated with a snapshot provider.
inline constexpr auto producer = "producer";

/// Snapshot-provider account authorized to submit a vote.
inline constexpr auto snap_account = "snap_account";

/// Chain block that included the final attestation record.
inline constexpr auto attested_at_block = "attested_at_block";

/// Final tuple block identifier.
inline constexpr auto block_id = "block_id";

/// Final tuple block height.
inline constexpr auto block_num = "block_num";

/// Final snapshot root hash.
inline constexpr auto snapshot_hash = "snapshot_hash";

} // namespace field

} // namespace sysiosystem::snapshot_attestation

#pragma once

#include <sysio/producer_plugin/snapshot_attestation_recovery.hpp>

#include <cstddef>
#include <string>
#include <utility>

namespace sysio::snapshot_attestation_test {

/// Deterministic provider identity shared by recovery tests.
inline constexpr auto provider_account_name = "snapprovider";

/// Deterministic recovery cursor shared by recovery tests.
inline constexpr uint32_t pending_vote_cursor = 42;

/// Number of hexadecimal characters in the deterministic digest fixtures.
inline constexpr std::size_t fixture_digest_character_count = 64;

/// Fill character used by the default deterministic chain id.
inline constexpr char default_chain_id_fill = 'a';

/// Fill character used by the deterministic snapshot block id.
inline constexpr char snapshot_block_id_fill = '1';

/// Block number used by deterministic snapshot metadata.
inline constexpr uint32_t snapshot_block_num = 1;

/// Filename used by deterministic snapshot metadata.
inline constexpr auto snapshot_filename = "snapshot-0000000001.bin";

/// Fill character used to derive the deterministic snapshot hash.
inline constexpr char snapshot_hash_payload_fill = '2';

/// Construct a deterministic chain id for recovery-sidecar tests.
inline chain::chain_id_type make_chain_id(char fill = default_chain_id_fill) {
   return chain::chain_id_type(std::string(fixture_digest_character_count, fill));
}

/// Construct deterministic local snapshot metadata for recovery-sidecar tests.
inline chain::snapshot_scheduler::snapshot_information make_snapshot_information() {
   return {
      chain::block_id_type(std::string(fixture_digest_character_count, snapshot_block_id_fill)),
      snapshot_block_num,
      chain::block_timestamp_type(),
      chain::chain_snapshot_header::current_version,
      snapshot_filename,
      fc::crypto::blake3(std::string(fixture_digest_character_count, snapshot_hash_payload_fill)),
   };
}

/// Construct a populated recovery state with an optional durable quarantine latch.
inline snapshot_attestation_recovery_state make_recovery_state(
   chain::chain_id_type chain_id = make_chain_id(), bool disagreement_detected = false) {
   return {
      snapshot_attestation_recovery_schema_version,
      std::move(chain_id),
      chain::account_name(provider_account_name),
      make_snapshot_information(),
      pending_vote_cursor,
      disagreement_detected,
   };
}

} // namespace sysio::snapshot_attestation_test

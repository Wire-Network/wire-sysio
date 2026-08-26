#pragma once

#include <sysio/chain_plugin/chain_plugin.hpp>
#include <sysio/protocol/snapshot_attestation.hpp>

#include <fc/crypto/blake3.hpp>
#include <fc/variant.hpp>

#include <algorithm>

namespace sysio::snapshot_api {

/**
 * Return whether a decoded record irreversibly attests the expected snapshot tuple.
 *
 * Malformed, mismatched, and still-reversible rows fail closed.
 */
inline bool is_servable_snapshot_attestation(
   const fc::variant& row,
   uint32_t expected_block_num,
   const chain::block_id_type& expected_block_id,
   const fc::crypto::blake3& expected_snapshot_hash,
   uint32_t last_irreversible_block_num) {
   if (!row.is_object()) {
      return false;
   }

   try {
      const auto& record = row.get_object();
      return record[protocol::snapshot_attestation::field::block_num].as_uint64()
                == expected_block_num
             && record[protocol::snapshot_attestation::field::attested_at_block].as_uint64()
                   <= last_irreversible_block_num
             && snapshot_attestation_record_matches(
                   expected_block_id, expected_snapshot_hash,
                   record[protocol::snapshot_attestation::field::block_id]
                      .as<chain::block_id_type>(),
                   record[protocol::snapshot_attestation::field::snapshot_hash].as_string());
   } catch (...) {
      return false;
   }
}

/** Find the newest attestation-cadence entry in an ascending block-number catalog. */
template <typename Catalog>
auto find_latest_scheduled_snapshot(const Catalog& catalog) {
   return std::find_if(catalog.rbegin(), catalog.rend(), [](const auto& entry) {
      return protocol::snapshot_attestation::is_scheduled_block(entry.first);
   });
}

/** Return whether an entry may be served explicitly: manual, or scheduled and attested. */
template <typename Snapshot, typename IsAttested>
bool is_snapshot_servable(uint32_t block_num, const Snapshot& snapshot, IsAttested&& is_attested) {
   return !protocol::snapshot_attestation::is_scheduled_block(block_num)
          || is_attested(snapshot);
}

} // namespace sysio::snapshot_api

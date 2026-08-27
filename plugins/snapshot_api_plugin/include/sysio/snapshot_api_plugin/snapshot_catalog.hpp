#pragma once

#include <sysio/chain_plugin/chain_plugin.hpp>
#include <sysio/protocol/snapshot_attestation.hpp>

#include <fc/crypto/blake3.hpp>
#include <fc/variant.hpp>

#include <limits>
#include <optional>

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

/** Decode a snapshot-attestation block number without trusting the ABI row shape or width. */
inline std::optional<uint32_t> snapshot_attestation_block_num(const fc::variant& row) {
   try {
      const uint64_t raw_block_num =
         row.get_object()[protocol::snapshot_attestation::field::block_num].as_uint64();
      if (raw_block_num > std::numeric_limits<uint32_t>::max()) {
         return std::nullopt;
      }
      return static_cast<uint32_t>(raw_block_num);
   } catch (...) {
      return std::nullopt;
   }
}

/** Return whether one decoded row makes its matching local catalog entry servable. */
template <typename Catalog, typename IsAvailable>
bool is_servable_catalog_snapshot_attestation(const Catalog& catalog,
                                              const fc::variant& row,
                                              uint32_t last_irreversible_block_num,
                                              IsAvailable&& is_available) {
   const auto block_num = snapshot_attestation_block_num(row);
   if (!block_num || !protocol::snapshot_attestation::is_scheduled_block(*block_num)) {
      return false;
   }

   const auto entry = catalog.find(*block_num);
   return entry != catalog.end()
          && is_available(entry->second)
          && is_servable_snapshot_attestation(
             row, *block_num, entry->second.block_id, entry->second.root_hash,
             last_irreversible_block_num);
}

/**
 * Find the first locally available catalog entry matched by newest-first irreversible attestations.
 *
 * Malformed rows, rows without a local catalog entry, unavailable files, and tuple mismatches are
 * skipped so callers can continue with an older attestation page.
 */
template <typename Catalog, typename IsAvailable>
auto find_latest_servable_scheduled_snapshot(const Catalog& catalog,
                                             const fc::variants& newest_first_attestations,
                                             uint32_t last_irreversible_block_num,
   IsAvailable&& is_available) {
   for (const auto& row : newest_first_attestations) {
      if (!is_servable_catalog_snapshot_attestation(
             catalog, row, last_irreversible_block_num, is_available)) {
         continue;
      }
      return catalog.find(*snapshot_attestation_block_num(row));
   }
   return catalog.end();
}

/** Return whether an entry may be served explicitly: manual, or scheduled and attested. */
template <typename Snapshot, typename IsAttested>
bool is_snapshot_servable(uint32_t block_num, const Snapshot& snapshot, IsAttested&& is_attested) {
   return !protocol::snapshot_attestation::is_scheduled_block(block_num)
          || is_attested(snapshot);
}

} // namespace sysio::snapshot_api

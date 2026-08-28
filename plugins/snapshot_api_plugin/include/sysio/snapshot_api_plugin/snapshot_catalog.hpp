#pragma once

#include <sysio/chain_plugin/chain_plugin.hpp>
#include <sysio/protocol/snapshot_attestation.hpp>

#include <fc/crypto/blake3.hpp>
#include <fc/io/json.hpp>
#include <fc/variant.hpp>

#include <limits>
#include <optional>

namespace sysio::snapshot_api {

/// Maximum number of attestation records inspected in one reverse discovery query.
inline constexpr uint32_t snapshot_attestation_discovery_page_size = 50;

/** Outcome of attempting to discover the latest servable scheduled snapshot. */
enum class snapshot_discovery_status {
   found,
   not_found,
   unavailable,
};

/** A discovery outcome and the snapshot present only for a successful lookup. */
template <typename Snapshot>
struct snapshot_discovery_result {
   snapshot_discovery_status status = snapshot_discovery_status::not_found;
   std::optional<Snapshot> snapshot;
};

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
 * Discover the newest locally available scheduled snapshot through bounded reverse table pages.
 *
 * The reader must apply the supplied C++-only row filter while servicing each page. An exhausted
 * scan is a genuine absence; a failed read, expired deadline, or inconsistent filtered row is a
 * temporary discovery failure.
 */
template <typename Catalog, typename IsAvailable, typename IsServableAttestation,
          typename ReadPage, typename DeadlineReached>
auto discover_latest_servable_scheduled_snapshot(const Catalog& catalog,
                                                 IsAvailable&& is_available,
                                                 IsServableAttestation&& is_servable_attestation,
                                                 ReadPage&& read_page,
                                                 DeadlineReached&& deadline_reached)
   -> snapshot_discovery_result<typename Catalog::mapped_type> {
   using result_type = snapshot_discovery_result<typename Catalog::mapped_type>;

   std::optional<uint32_t> oldest_available_scheduled;
   std::optional<uint32_t> newest_available_scheduled;
   for (const auto& [block_num, entry] : catalog) {
      if (!protocol::snapshot_attestation::is_scheduled_block(block_num)
          || !is_available(entry)) {
         continue;
      }
      if (!oldest_available_scheduled) {
         oldest_available_scheduled = block_num;
      }
      newest_available_scheduled = block_num;
   }
   if (!oldest_available_scheduled || !newest_available_scheduled) {
      return result_type{snapshot_discovery_status::not_found, std::nullopt};
   }

   auto params = make_snapshot_attestation_record_query(*oldest_available_scheduled);
   params.limit = snapshot_attestation_discovery_page_size;
   params.reverse = true;
   params.upper_bound = fc::json::to_string(
      fc::mutable_variant_object()
      (protocol::snapshot_attestation::field::block_num,
       static_cast<uint64_t>(*newest_available_scheduled) + 1),
      fc::time_point::maximum());
   params.filter = [&catalog, &is_servable_attestation](const fc::variant& row) {
      return is_servable_attestation(catalog, row);
   };

   while (true) {
      if (deadline_reached()) {
         return result_type{snapshot_discovery_status::unavailable, std::nullopt};
      }

      const auto page = read_page(params);
      if (!page) {
         return result_type{snapshot_discovery_status::unavailable, std::nullopt};
      }
      if (!page->rows.empty()) {
         const auto block_num = snapshot_attestation_block_num(page->rows.front());
         if (!block_num) {
            return result_type{snapshot_discovery_status::unavailable, std::nullopt};
         }
         const auto entry = catalog.find(*block_num);
         if (entry == catalog.end()) {
            return result_type{snapshot_discovery_status::unavailable, std::nullopt};
         }
         return result_type{snapshot_discovery_status::found, entry->second};
      }
      if (!page->more || page->next_key.empty()) {
         return result_type{snapshot_discovery_status::not_found, std::nullopt};
      }
      params.upper_bound = page->next_key;
   }
}

/** Return whether an entry may be served explicitly: manual, or scheduled and attested. */
template <typename Snapshot, typename IsAttested>
bool is_snapshot_servable(uint32_t block_num, const Snapshot& snapshot, IsAttested&& is_attested) {
   return !protocol::snapshot_attestation::is_scheduled_block(block_num)
          || is_attested(snapshot);
}

} // namespace sysio::snapshot_api

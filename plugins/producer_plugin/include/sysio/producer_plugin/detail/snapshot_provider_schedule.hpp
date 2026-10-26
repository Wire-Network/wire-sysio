#pragma once

#include <sysio/chain/snapshot_scheduler.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace sysio::producer_plugin_detail {

/** Description reserved for the snapshot provider's automatically managed request. */
inline constexpr auto snapshot_provider_schedule_description = "snapshot-provider auto";

/** Outcome of reconciling the provider-owned recurring snapshot request. */
enum class snapshot_provider_schedule_disposition {
   /** Snapshot attestation is disabled and no provider-owned request remains. */
   disabled,
   /** An existing provider-owned request already matches the configured cadence. */
   reused_automatic,
   /** An operator-created request exactly matches and is reused without taking ownership. */
   reused_manual,
   /** A new provider-owned request was created for the configured cadence. */
   created_automatic,
};

/** Result of reconciling the provider-owned recurring request with authoritative configuration. */
struct snapshot_provider_schedule_reconciliation {
   /** Enabled cadence, or no value when snapshot attestation is disabled. */
   std::optional<uint32_t>                  block_spacing;
   /** How the active request, if any, was obtained. */
   snapshot_provider_schedule_disposition  disposition =
      snapshot_provider_schedule_disposition::disabled;
   /** Active recurring request identifier, when reconciliation leaves one in use. */
   std::optional<uint32_t>                  request_id;
   /** Identifiers of stale provider-owned requests removed during reconciliation. */
   std::vector<uint32_t>                    removed_request_ids;
};

/**
 * Reconcile the reserved provider request while preserving every operator-created schedule.
 *
 * A disabled cadence removes reserved requests. An enabled cadence retains one exact reserved
 * request, adopts an exact manual recurrence when one already exists, or creates a new reserved
 * request. The returned disposition and identifiers let the plugin update its vote cache and log
 * only state changes.
 */
inline snapshot_provider_schedule_reconciliation reconcile_snapshot_provider_schedule(
   chain::snapshot_scheduler& scheduler, std::optional<uint32_t> desired_block_spacing) {
   snapshot_provider_schedule_reconciliation result;

   const auto existing_requests = scheduler.get_snapshot_requests().snapshot_requests;
   std::optional<uint32_t> matching_automatic_request;
   for (const auto& request : existing_requests) {
      if (request.snapshot_description != snapshot_provider_schedule_description) {
         continue;
      }

      const bool matches = desired_block_spacing
                           && request.block_spacing == *desired_block_spacing
                           && request.start_block_num == *desired_block_spacing
                           && request.end_block_num == std::numeric_limits<uint32_t>::max();
      if (matches && !matching_automatic_request) {
         matching_automatic_request = request.snapshot_request_id;
         continue;
      }

      scheduler.unschedule_snapshot(request.snapshot_request_id);
      result.removed_request_ids.push_back(request.snapshot_request_id);
   }

   if (!desired_block_spacing) {
      return result;
   }

   result.block_spacing = desired_block_spacing;
   if (matching_automatic_request) {
      result.disposition = snapshot_provider_schedule_disposition::reused_automatic;
      result.request_id = matching_automatic_request;
      return result;
   }

   const auto block_spacing = *desired_block_spacing;
   const auto maximum_block_num = std::numeric_limits<uint32_t>::max();
   if (const auto existing = scheduler.find_snapshot_request(
          block_spacing, block_spacing, maximum_block_num)) {
      result.disposition = snapshot_provider_schedule_disposition::reused_manual;
      result.request_id = existing;
      return result;
   }

   chain::snapshot_scheduler::snapshot_request_information request;
   request.block_spacing = block_spacing;
   request.start_block_num = block_spacing;
   request.end_block_num = maximum_block_num;
   request.snapshot_description = snapshot_provider_schedule_description;
   const auto scheduled = scheduler.schedule_snapshot(request, {});
   result.disposition = snapshot_provider_schedule_disposition::created_automatic;
   result.request_id = scheduled.snapshot_request_id;
   return result;
}

} // namespace sysio::producer_plugin_detail

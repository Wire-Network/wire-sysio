#pragma once

#include <sysio/protocol/snapshot_attestation.hpp>

#include <algorithm>

namespace sysio::snapshot_api {

/** Find the newest attestation-cadence entry in an ascending block-number catalog. */
template <typename Catalog>
auto find_latest_scheduled_snapshot(const Catalog& catalog) {
   return std::find_if(catalog.rbegin(), catalog.rend(), [](const auto& entry) {
      return protocol::snapshot_attestation::is_scheduled_block(entry.first);
   });
}

} // namespace sysio::snapshot_api

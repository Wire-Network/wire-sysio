#pragma once

#include <fc/variant.hpp>
#include <optional>
#include <string>
#include <cstdint>

namespace sysio::beacon_chain_detail {

/// The field name in beacon chain queue API responses that holds the estimated processing timestamp (Unix seconds).
inline constexpr auto epa_field = "estimated_processed_at";

/// Extract a named field from an fc::variant object.
/// Returns empty optional if expected_obj is not an object or does not contain expected_field.
std::optional<fc::variant> get_field_from_object(const fc::variant& expected_obj,
                                                  const std::string& expected_field);

/// Extract the queue wait time in seconds (from now) for the given queue branch from a beacon chain queues response.
/// Throws sysio::chain::plugin_config_exception if required fields are absent or malformed.
std::optional<uint64_t> get_queue_length(const fc::variant& queues, const std::string& queue_branch);

/// Convert an APY fraction (e.g. 0.05 for 5%) to basis points (e.g. 500).
/// Uses a small epsilon for floating-point robustness when the result should be a whole number.
inline uint64_t apy_fraction_to_bps(double apr_fraction) {
   return static_cast<uint64_t>(apr_fraction * 10000.0 + 1e-12);
}

} // namespace sysio::beacon_chain_detail

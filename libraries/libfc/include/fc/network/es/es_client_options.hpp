#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace fc::network::es {

// Delivery defaults shared by every `_bulk` producer (es_sink, status_monitor_plugin). Batching cadence
// and queue depth are the producer's own concern; only the request-level values live here.

/// NDJSON bulk-request body cap, in bytes.
inline constexpr uint32_t es_default_max_batch_bytes = 1024 * 1024;
/// Single-document cap, in bytes; a producer drops and counts a larger document.
inline constexpr uint32_t es_default_max_doc_bytes = 256 * 1024;
/// ADDITIONAL delivery attempts after the first (total attempts = max_retries + 1, so four per batch at this
/// default). Each attempt is bounded by es_default_request_timeout_ms, with the capped backoff in between.
inline constexpr uint32_t es_default_max_retries = 3;
/// Initial retry backoff; doubles per attempt, capped at es_max_retry_backoff_ms.
inline constexpr uint32_t es_default_retry_backoff_ms = 250;
/// Connection-establishment budget for one delivery attempt, in milliseconds.
inline constexpr uint32_t es_default_connect_timeout_ms = 5000;
/// Per-attempt budget for headers, body read, idle, and total, in milliseconds.
inline constexpr uint32_t es_default_request_timeout_ms = 10000;
/// Hard ceiling on max_batch_bytes; bounds producer memory even with an absurd config.
inline constexpr uint32_t es_max_batch_bytes_ceiling = 16u * 1024 * 1024;
/// Ceiling on the doubled retry backoff between attempts, in milliseconds.
inline constexpr uint32_t es_max_retry_backoff_ms = 2'000;

/// Endpoint and request-level settings of one OpenSearch/Elasticsearch target. Validated and normalized by
/// es_client::validate(); a producer keeps the returned copy.
struct es_client_options {
   /// base URL, e.g. "https://opensearch.example.com" (required; a trailing '/' is stripped by validate())
   std::string url;
   std::string index;                                           ///< target index or write alias (required non-empty)
   std::optional<std::string> username;                         ///< optional HTTP basic auth user
   std::optional<std::string> password;                         ///< required iff username is set
   uint32_t max_batch_bytes = es_default_max_batch_bytes;       ///< see es_default_max_batch_bytes
   uint32_t max_doc_bytes = es_default_max_doc_bytes;           ///< see es_default_max_doc_bytes
   uint32_t max_retries = es_default_max_retries;               ///< see es_default_max_retries
   uint32_t retry_backoff_ms = es_default_retry_backoff_ms;     ///< see es_default_retry_backoff_ms
   uint32_t connect_timeout_ms = es_default_connect_timeout_ms; ///< see es_default_connect_timeout_ms
   uint32_t request_timeout_ms = es_default_request_timeout_ms; ///< see es_default_request_timeout_ms
};

} // namespace fc::network::es

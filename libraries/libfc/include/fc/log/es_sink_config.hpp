#pragma once

/**
 * @file
 * es_sink's configuration and its batching defaults; the request-level defaults are
 * fc::network::es::es_default_*.
 */

#include <cstdint>
#include <fc/network/es/es_client_options.hpp>
#include <optional>
#include <string>

namespace fc {
namespace sink {
/// Defaults for es_sink's batching. The request-level defaults (byte caps, retries, timeouts) are
/// fc::network::es::es_default_* in fc/network/es/es_client_options.hpp, shared with every _bulk producer.
/// Worst-case buffered memory is roughly (max_pending_batches + 2) * max_batch_bytes -- the pending
/// batch, the queued batches, and one batch in flight.
inline constexpr uint32_t default_es_batch_size = 100; ///< documents per bulk request
/// Interval flush cadence for a partially-filled batch.
inline constexpr uint32_t default_es_flush_interval_ms = 1000;
/// Delivery-queue bound, in batches; a full queue drops the newest batch.
inline constexpr uint32_t default_es_max_pending_batches = 8;
/// Stall budget on the BLOCK-PRODUCTION thread, not a background wait: SIGHUP
/// posts through the priority_queue_executor's single-threaded read_write
/// queue -- the same queue block production uses -- and handlers run to
/// completion, so the sink destructor's drain (which fires inside the
/// handle_sighup() fan-out, e.g. on every logrotate) delays the next
/// production handler by up to this budget plus the ~50 ms cancel-poll join.
/// Sized well under the 500 ms block interval; against a dead endpoint the
/// drain always times out, so the worst case recurs on every SIGHUP.
inline constexpr uint32_t default_es_shutdown_flush_timeout_ms = 100;

/// Ships log documents to an OpenSearch/Elasticsearch _bulk endpoint from a
/// dedicated worker thread. Document shape is owned by the sink's formatter
/// (fc::log::json_formatter with the fc::log::es_default_layout template by
/// default); identity fields (env/app/principal/logStream/...) ride the
/// formatter's extra_fields. This struct configures endpoint, batching, and
/// delivery only; the delivery fields are handed to fc::network::es::es_client.
struct es_sink_config {
   /// base URL, e.g. "https://elasticsearch.example.com" (required; trailing '/' stripped)
   std::string url;
   std::string index;                   ///< target index or write alias (required non-empty)
   std::optional<std::string> username; ///< optional HTTP basic auth user
   std::optional<std::string> password; ///< required iff username is set
   uint32_t batch_size = default_es_batch_size;
   uint32_t max_batch_bytes = network::es::es_default_max_batch_bytes;
   uint32_t max_doc_bytes = network::es::es_default_max_doc_bytes;
   uint32_t flush_interval_ms = default_es_flush_interval_ms;
   uint32_t max_pending_batches = default_es_max_pending_batches;
   uint32_t max_retries = network::es::es_default_max_retries;
   uint32_t retry_backoff_ms = network::es::es_default_retry_backoff_ms;
   uint32_t connect_timeout_ms = network::es::es_default_connect_timeout_ms;
   uint32_t request_timeout_ms = network::es::es_default_request_timeout_ms;
   uint32_t shutdown_flush_timeout_ms = default_es_shutdown_flush_timeout_ms;
};
} // namespace sink
} // namespace fc

#include <fc/reflect/reflect.hpp>
// clang-format off
FC_REFLECT(fc::sink::es_sink_config,
           (url)(index)(username)(password)
           (batch_size)(max_batch_bytes)(max_doc_bytes)
           (flush_interval_ms)(max_pending_batches)
           (max_retries)(retry_backoff_ms)
           (connect_timeout_ms)(request_timeout_ms)
           (shutdown_flush_timeout_ms))
// clang-format on

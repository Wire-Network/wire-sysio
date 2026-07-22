/**
 *  @file
 *  @copyright defined in LICENSE.txt
 */
#pragma once

#include <fc/static_variant.hpp>
#include <fc/time.hpp>
#include <fc/variant.hpp>
#include <fc/exception/exception.hpp>
#include <fc/network/url.hpp>
#include <fc/crypto/blake3.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>

namespace fc {

/** Current phase of a streamed HTTP file download. */
enum class http_file_download_phase {
   connecting,
   sending_request,
   waiting_for_response,
   downloading,
   complete,
};

/** Periodic status for a streamed HTTP file download. */
struct http_file_download_status {
   /// Current request or transfer phase.
   http_file_download_phase phase;
   /// Decoded response-body bytes persisted so far.
   uint64_t downloaded_bytes;
   /// Expected response-body bytes when the peer supplied Content-Length.
   std::optional<uint64_t> total_bytes;
   /// Time elapsed since the download request started.
   microseconds elapsed;
};

/** Resource limits and status reporting for a streamed HTTP file download. */
struct http_file_download_options {
   /// Maximum accepted response-body size in bytes.
   uint64_t max_response_body_bytes;
   /// Free bytes that must remain on the destination filesystem.
   uint64_t min_free_disk_space_bytes;
   /// Retry a failed request once when it used a cached connection. Only enable for idempotent requests.
   bool retry_failed_reused_connection = false;
   /// Receive phase transitions and periodic transfer status. The callback must not throw.
   std::function<void(const http_file_download_status&)> status_callback;
};

class http_client {
   public:
      http_client();
      ~http_client();

      variant post_sync(const url& dest, const variant& payload, const time_point& deadline = time_point::maximum());

      template<typename T>
      variant post_sync(const url& dest, const T& payload, const time_point& deadline = time_point::maximum()) {
         variant payload_v;
         to_variant(payload, payload_v);
         return post_sync(dest, payload_v, deadline);
      }

      /**
       * Download a binary POST response using explicit resource limits.
       *
       * The response is written to a temporary sibling of @p output and renamed only after
       * the complete bounded body has been persisted successfully.
       */
      void post_to_file(const url& dest,
                        const variant& payload,
                        const std::filesystem::path& output,
                        const http_file_download_options& options);

      /**
       * Set a predicate used to cancel synchronous HTTP operations.
       *
       * The predicate is polled while resolver and socket operations are pending. Returning true
       * cancels the active operation so callers can interrupt otherwise unbounded requests.
       */
      void set_cancel_check(std::function<bool()> cancel_check);

      void add_cert(const std::string& cert_pem_string);
      void set_verify_peers(bool enabled);

private:
   std::unique_ptr<class http_client_impl> _my;
};

}

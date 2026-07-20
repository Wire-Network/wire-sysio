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

namespace fc {

/** Resource and deadline limits for a streamed HTTP file download. */
struct http_file_download_options {
   /// Maximum time allowed to establish the connection.
   microseconds connect_timeout;
   /// Maximum time allowed for each of the request-write and response-header phases.
   microseconds response_header_timeout;
   /// Maximum time allowed without decoded response-body progress.
   microseconds idle_read_timeout;
   /// Maximum time allowed for the complete request and download.
   microseconds total_timeout;
   /// Maximum accepted response-body size in bytes.
   uint64_t max_response_body_bytes;
   /// Free bytes that must remain on the destination filesystem.
   uint64_t min_free_disk_space_bytes;
   /// Retry a failed request once when it used a cached connection. Only enable for idempotent requests.
   bool retry_failed_reused_connection = false;
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
       * Download a binary POST response using explicit phase deadlines and resource limits.
       *
       * The response is written to a temporary sibling of @p output and renamed only after
       * the complete bounded body has been persisted successfully.
       */
      void post_to_file(const url& dest,
                        const variant& payload,
                        const std::filesystem::path& output,
                        const http_file_download_options& options);

      void add_cert(const std::string& cert_pem_string);
      void set_verify_peers(bool enabled);

private:
   std::unique_ptr<class http_client_impl> _my;
};

}

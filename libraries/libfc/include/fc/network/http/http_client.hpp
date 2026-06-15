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

#include <filesystem>

namespace fc {

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

      /// Download binary response from a POST request to a file.
      /// Uses a temp file during download and renames on completion.
      /// Clean up on failure.
      void post_to_file(const url& dest,
                        const variant& payload,
                        const std::filesystem::path& output,
                        const time_point& deadline = time_point::maximum());

      void add_cert(const std::string& cert_pem_string);
      void set_verify_peers(bool enabled);

private:
   std::unique_ptr<class http_client_impl> _my;
};

}
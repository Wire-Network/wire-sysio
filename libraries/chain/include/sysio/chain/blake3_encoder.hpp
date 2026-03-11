#pragma once

#include <fc/crypto/blake3.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace sysio { namespace chain {

/**
 * Incremental BLAKE3 hasher backed by LLVM's bundled implementation.
 *
 * Usage:
 *   blake3_encoder enc;
 *   enc.write(data, len);
 *   fc::crypto::blake3 hash = enc.result();
 */
class blake3_encoder {
public:
   blake3_encoder();
   ~blake3_encoder();

   blake3_encoder(blake3_encoder&&) noexcept;
   blake3_encoder& operator=(blake3_encoder&&) noexcept;

   void write(const char* d, size_t len);
   void write(const uint8_t* d, size_t len) { write(reinterpret_cast<const char*>(d), len); }
   void put(char c) { write(&c, 1); }
   void reset();
   fc::crypto::blake3 result();

   /// One-shot convenience: hash a contiguous buffer.
   static fc::crypto::blake3 hash(const char* data, size_t len);
   static fc::crypto::blake3 hash(const uint8_t* data, size_t len) {
      return hash(reinterpret_cast<const char*>(data), len);
   }

private:
   struct impl;
   std::unique_ptr<impl> my;
};

}} // namespace sysio::chain

#include <sysio/chain/blake3_encoder.hpp>
#include <llvm-c/blake3.h>

#include <cstring>

namespace sysio { namespace chain {

struct blake3_encoder::impl {
   static constexpr size_t buf_size = 64 * 1024; // 64 KB

   llvm_blake3_hasher hasher;
   char   buf[buf_size];
   size_t pos = 0;

   impl() { llvm_blake3_hasher_init(&hasher); }

   void write(const char* d, size_t len) {
      // Large write: flush buffer, then hash directly (avoids double-buffering
      // when called from hashing_streambuf which already sends 1 MB chunks).
      if(len >= buf_size) {
         flush();
         llvm_blake3_hasher_update(&hasher, d, len);
         return;
      }
      while(len > 0) {
         const size_t space = buf_size - pos;
         const size_t chunk = std::min(len, space);
         std::memcpy(buf + pos, d, chunk);
         pos += chunk;
         d += chunk;
         len -= chunk;
         if(pos == buf_size)
            flush();
      }
   }

   void flush() {
      if(pos > 0) {
         llvm_blake3_hasher_update(&hasher, buf, pos);
         pos = 0;
      }
   }
};

blake3_encoder::blake3_encoder() : my(std::make_unique<impl>()) {}
blake3_encoder::~blake3_encoder() = default;
blake3_encoder::blake3_encoder(blake3_encoder&&) noexcept = default;
blake3_encoder& blake3_encoder::operator=(blake3_encoder&&) noexcept = default;

void blake3_encoder::write(const char* d, size_t len) {
   my->write(d, len);
}

void blake3_encoder::reset() {
   my->pos = 0;
   llvm_blake3_hasher_init(&my->hasher);
}

fc::crypto::blake3 blake3_encoder::result() {
   my->flush();
   fc::crypto::blake3 h;
   llvm_blake3_hasher_finalize(&my->hasher, h.data(), h.data_size());
   return h;
}

fc::crypto::blake3 blake3_encoder::hash(const char* data, size_t len) {
   blake3_encoder enc;
   enc.write(data, len);
   return enc.result();
}

}} // namespace sysio::chain

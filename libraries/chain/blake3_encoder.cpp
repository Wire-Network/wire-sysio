#include <sysio/chain/blake3_encoder.hpp>
#include <llvm-c/blake3.h>

namespace sysio { namespace chain {

struct blake3_encoder::impl {
   llvm_blake3_hasher hasher;

   impl() { llvm_blake3_hasher_init(&hasher); }
};

blake3_encoder::blake3_encoder() : my(std::make_unique<impl>()) {}
blake3_encoder::~blake3_encoder() = default;
blake3_encoder::blake3_encoder(blake3_encoder&&) noexcept = default;
blake3_encoder& blake3_encoder::operator=(blake3_encoder&&) noexcept = default;

void blake3_encoder::write(const char* d, size_t len) {
   llvm_blake3_hasher_update(&my->hasher, d, len);
}

void blake3_encoder::reset() {
   llvm_blake3_hasher_init(&my->hasher);
}

fc::crypto::blake3 blake3_encoder::result() {
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

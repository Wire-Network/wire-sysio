#include <fc/crypto/elliptic_ed.hpp>

namespace fc { namespace crypto { namespace ed {

   // Ensure libsodium is initialized exactly once
   static void sodium_init_guard() {
      // capture the return value into a variable to avoid compiler warnings
      int init_result = sodium_init();

      if (init_result < 0)
         FC_THROW_EXCEPTION(exception, "Failed to initialize libsodium");
   }

   // Derive public key from 64-byte secret key
   public_key_shim private_key_shim::get_public_key() const {
      sodium_init_guard();
      public_key_shim pub;
      // libsodium stores seed+pub in first half of SK, or you can derive:
      if (crypto_sign_ed25519_sk_to_pk(pub._data.data(), _data.data()) != 0) {
         FC_THROW_EXCEPTION(exception, "ed25519: failed to derive public key");
      }
      return pub;
   }

   // Sign a 32‑byte digest (treat digest as the “message”)
   signature_shim private_key_shim::sign(const sha256& digest, bool require_canonical) const {
      // 1) Ensure libsodium is initialized
      sodium_init_guard();

      // 2) Hex‑encode the 32‑byte digest as ASCII (lower‑case)
      const std::string hex = fc::to_hex(digest.data(), digest.data_size());

      // 3) Detached‑sign that ASCII hex payload
      unsigned char sigbuf[crypto_sign_BYTES];
      unsigned long long siglen = 0;
      if (crypto_sign_detached(
            sigbuf, &siglen,
            reinterpret_cast<const unsigned char*>(hex.data()),
            hex.size(),
            _data.data()
         ) != 0
         || siglen != crypto_sign_BYTES)
      {
         FC_THROW_EXCEPTION(exception, "Failed to create ED25519 signature");
      }

      // 4) Pack into your signature_shim
      signature_shim out;
      // zero‑pad entire buffer then copy
      memset(out._data.data(), 0, out.size);
      memcpy(out._data.data(), sigbuf, crypto_sign_BYTES);
      return out;
   }

   // Verify a 32‑byte digest signature
   bool signature_shim::verify(const sha256& digest, const public_key_shim& pub) const {
      sodium_init_guard();

      // 1) Convert raw digest to ASCII hex, added due to Phantom wallet limitations which doesn't allow signing raw binary data. Guard rails so users don't unknowingly sign malicious transactions.
      const std::string hex = fc::to_hex(digest.data(), digest.data_size());

      // 2) Verify signature on hex payload
      return crypto_sign_verify_detached(
         _data.data(),
         reinterpret_cast<const unsigned char*>(hex.data()),
         hex.size(),
         pub._data.data()
      ) == 0; // returns 0 on success
   }

   // Sign raw bytes directly (for Solana transaction signing)
   signature_shim private_key_shim::sign_raw(const unsigned char* data, size_t len) const {
      sodium_init_guard();

      unsigned char sigbuf[crypto_sign_BYTES];
      unsigned long long siglen = 0;
      if (crypto_sign_detached(
            sigbuf, &siglen,
            data,
            len,
            _data.data()
         ) != 0
         || siglen != crypto_sign_BYTES)
      {
         FC_THROW_EXCEPTION(exception, "Failed to create ED25519 signature");
      }

      signature_shim out;
      memset(out._data.data(), 0, out.size);
      memcpy(out._data.data(), sigbuf, crypto_sign_BYTES);
      return out;
   }

   // stub out ECDH for the visitor
   sha512 private_key_shim::generate_shared_secret(const public_key_shim&) const {
      FC_THROW_EXCEPTION(exception, "ED25519 shared_secret not supported");
   }
}}} // fc::crypto::ed

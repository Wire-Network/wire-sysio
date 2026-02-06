#include <algorithm>
#include <ethash/keccak.hpp>
#include <fc-lite/traits.hpp>
#include <fc/crypto/hex.hpp>
#include <fc/crypto/solana/solana_crypto_utils.hpp>

namespace fc::crypto::solana {
//=============================================================================
// pubkey implementation
//=============================================================================

std::string solana_public_key::to_base58() const {
   return fc::to_base58(reinterpret_cast<const char*>(data.data()), data.size(), fc::yield_function_t{});
}

solana_public_key solana_public_key::from_base58(const std::string& str) {
   auto bytes = fc::from_base58(str);
   FC_ASSERT(bytes.size() == SIZE, "Invalid Solana pubkey length: expected {}, got {}", SIZE, bytes.size());
   solana_public_key result;
   std::memcpy(result.data.data(), bytes.data(), SIZE);
   return result;
}

solana_public_key solana_public_key::from_public_key(const fc::crypto::public_key& pk) {
   FC_ASSERT(pk.contains<fc::crypto::ed::public_key_shim>(), "Public key must be ED25519 type for Solana");
   return from_ed_public_key(pk.get<fc::crypto::ed::public_key_shim>());
}

solana_public_key solana_public_key::from_ed_public_key(const fc::crypto::ed::public_key_shim& pk) {
   solana_public_key result;
   static_assert(sizeof(pk._data) == SIZE, "ED25519 public key size mismatch");
   std::copy(pk._data.begin(), pk._data.end(), result.data.begin());
   return result;
}

bool solana_public_key::is_zero() const {
   for (auto b : data) {
      if (b != 0)
         return false;
   }
   return true;
}

//=============================================================================
// signature implementation
//=============================================================================

std::string solana_signature::to_base58() const {
   return fc::to_base58(reinterpret_cast<const char*>(data.data()), data.size(), fc::yield_function_t{});
}

solana_signature solana_signature::from_base58(const std::string& str) {
   auto bytes = fc::from_base58(str);
   FC_ASSERT(bytes.size() == SIZE, "Invalid Solana signature length: expected {}, got {}", SIZE, bytes.size());
   solana_signature result;
   std::memcpy(result.data.data(), bytes.data(), SIZE);
   return result;
}

solana_signature solana_signature::from_ed_signature(const fc::crypto::ed::signature_shim& sig) {
   solana_signature result;
   static_assert(sizeof(sig._data) == SIZE, "ED25519 signature size mismatch");
   std::ranges::copy(sig._data, result.data.begin());
   return result;
}

bool is_on_curve(const solana_public_key& address) {
   // Solana checks if the point is on the Ed25519 curve (can be decompressed).
   // This is different from libsodium's crypto_core_ed25519_is_valid_point which
   // ALSO checks if the point is in the prime-order subgroup (torsion-free).
   //
   // For PDA derivation, we only need to check if the point is on the curve,
   // not if it's in the main subgroup. A point that is on the curve but in a
   // small subgroup (torsion point) should still be rejected as a valid PDA
   // because it IS on the curve.
   //
   // We implement the same check as Solana's curve25519_dalek:
   // Try to decompress the compressed Edwards Y point.
   //
   // Ed25519 curve: -x² + y² = 1 + d*x²*y² where d = -121665/121666
   // Compressed format: 32 bytes where first 255 bits = Y, last bit = sign of X
   //
   // To check if on curve:
   // 1. Extract Y (clear the sign bit)
   // 2. Compute u = y² - 1
   // 3. Compute v = d*y² + 1
   // 4. Compute x² = u/v = u * v^(p-2) mod p
   // 5. Check if x² is a quadratic residue (has a square root)
   //
   // We use the Legendre symbol: x² is a QR iff (x²)^((p-1)/2) ≡ 1 (mod p)

   // Field prime p = 2^255 - 19
   // d = -121665/121666 mod p
   // We need to work in the finite field F_p

   // For simplicity and correctness, we'll use a different approach:
   // Use libsodium's ge25519_frombytes which does the decompression check.
   // This is an internal function but we can access it via the public API.

   // Actually, we can use crypto_scalarmult_ed25519_noclamp to test.
   // If we multiply the point by scalar 1, it should work iff the point is on the curve.
   // But this might not work for points not in the main subgroup.

   // The safest approach: implement the curve check directly using big integer math.
   // But for now, let's use a workaround: try to use the point in an operation.

   // Alternative: crypto_core_ed25519_sub with identity should work
   // Actually, let's just implement using OpenSSL BIGNUM for the curve check.

   // For now, let's use a simple heuristic that matches Solana's behavior:
   // Try decompression by computing x² and checking if it's a quadratic residue.

   // We'll use libsodium's internal API via a workaround.
   // crypto_core_ed25519_add with the identity point (all zeros) should work
   // if the point is on the curve.

   // Actually, crypto_core_ed25519_from_hash takes arbitrary 64 bytes and maps
   // them to a valid curve point. We can't use that.

   // Let me implement the proper check using OpenSSL BN.

   // Copy the compressed point bytes
   std::array<uint8_t, 32> compressed = address.data;

   // Extract Y (clear the sign bit which is the MSB of the last byte)
   // The sign bit is used during full decompression but not needed for curve check
   compressed[31] &= 0x7F;

   // Field prime p = 2^255 - 19
   // d = -121665/121666 mod p
   // d_bytes (little-endian) = a3785913ca4deb75abd841414d0a700098e879777940c78c73fe6f2bee6c0352

   // Convert Y to BIGNUM (little-endian to big-endian for OpenSSL)
   BIGNUM* y = BN_new();
   BIGNUM* y_sq = BN_new();
   BIGNUM* u = BN_new();
   BIGNUM* v = BN_new();
   BIGNUM* p = BN_new();
   BIGNUM* d = BN_new();
   BIGNUM* x_sq = BN_new();
   BIGNUM* exp = BN_new();
   BIGNUM* legendre = BN_new();
   BIGNUM* one = BN_new();
   BIGNUM* neg_one = BN_new();
   BN_CTX* ctx = BN_CTX_new();

   // Set p = 2^255 - 19
   BN_one(p);
   BN_lshift(p, p, 255);
   BN_sub_word(p, 19);

   // Convert Y from little-endian to BIGNUM
   // OpenSSL expects big-endian, so we need to reverse
   std::array<uint8_t, 32> y_be;
   for (size_t i = 0; i < 32; i++) {
      y_be[i] = compressed[31 - i];
   }
   BN_bin2bn(y_be.data(), 32, y);

   // Check if Y >= p (invalid)
   if (BN_cmp(y, p) >= 0) {
      BN_free(y);
      BN_free(y_sq);
      BN_free(u);
      BN_free(v);
      BN_free(p);
      BN_free(d);
      BN_free(x_sq);
      BN_free(exp);
      BN_free(legendre);
      BN_free(one);
      BN_free(neg_one);
      BN_CTX_free(ctx);
      return false; // Y out of range means not on curve
   }

   // Compute y² mod p
   BN_mod_sqr(y_sq, y, p, ctx);

   // u = y² - 1 mod p
   BN_one(one);
   BN_mod_sub(u, y_sq, one, p, ctx);

   // d = -121665/121666 mod p
   // = 37095705934669439343138083508754565189542113879843219016388785533085940283555
   // In big-endian hex: 52036cee2b6ffe738cc740797779e89800700a4d4141d8ab75eb4dca135978a3
   const uint8_t d_be[] = {0x52, 0x03, 0x6c, 0xee, 0x2b, 0x6f, 0xfe, 0x73, 0x8c, 0xc7, 0x40,
                           0x79, 0x77, 0x79, 0xe8, 0x98, 0x00, 0x70, 0x0a, 0x4d, 0x41, 0x41,
                           0xd8, 0xab, 0x75, 0xeb, 0x4d, 0xca, 0x13, 0x59, 0x78, 0xa3};
   BN_bin2bn(d_be, 32, d);

   // v = d*y² + 1 mod p
   BN_mod_mul(v, d, y_sq, p, ctx);
   BN_mod_add(v, v, one, p, ctx);

   // x² = u * v^(-1) mod p = u * v^(p-2) mod p (Fermat's little theorem)
   BN_copy(exp, p);
   BN_sub_word(exp, 2);
   BN_mod_exp(x_sq, v, exp, p, ctx);
   BN_mod_mul(x_sq, x_sq, u, p, ctx);

   // Check if x² is a quadratic residue using Legendre symbol
   // Legendre symbol = x²^((p-1)/2) mod p
   // If result is 0 or 1, it's a QR (on curve). If result is p-1, it's not.
   BN_copy(exp, p);
   BN_sub_word(exp, 1);
   BN_rshift1(exp, exp); // (p-1)/2
   BN_mod_exp(legendre, x_sq, exp, p, ctx);

   // neg_one = p - 1
   BN_copy(neg_one, p);
   BN_sub_word(neg_one, 1);

   // If Legendre symbol is -1 (p-1), x² is not a QR, point is not on curve
   bool on_curve = (BN_cmp(legendre, neg_one) != 0);

   BN_free(y);
   BN_free(y_sq);
   BN_free(u);
   BN_free(v);
   BN_free(p);
   BN_free(d);
   BN_free(x_sq);
   BN_free(exp);
   BN_free(legendre);
   BN_free(one);
   BN_free(neg_one);
   BN_CTX_free(ctx);

   return on_curve;
}

} // namespace fc::crypto::solana

#include "ed25519/vectors.hpp"

#include <boost/test/unit_test.hpp>
#include <fc/crypto/elliptic_ed.hpp>
#include <fc/crypto/hex.hpp>

BOOST_AUTO_TEST_SUITE(ed25519_tests)

/// Pin the shared raw verifier's acceptance rules with externally published and constructed vectors.
BOOST_AUTO_TEST_CASE(frozen_verification_rules) {
   for (const auto& vector : ed25519_test_vectors::cases) {
      BOOST_TEST_CONTEXT(vector.name) {
         const auto sig = fc::from_hex(vector.signature);
         const auto pub = fc::from_hex(vector.public_key);
         const auto msg = fc::from_hex(vector.message);
         BOOST_CHECK_EQUAL(fc::crypto::ed::verify_detached(std::as_bytes(std::span{sig}),
                                                          std::as_bytes(std::span{msg}),
                                                          std::as_bytes(std::span{pub})), vector.valid);
         if (sig.size() != crypto_sign_BYTES || pub.size() != crypto_sign_PUBLICKEYBYTES)
            continue; // The legacy signature shim always stores fixed-size signature/key arrays.
         fc::crypto::ed::public_key_shim key;
         fc::crypto::ed::signature_shim signature;
         std::copy(pub.begin(), pub.end(), key._data.begin());
         std::copy(pub.begin(), pub.end(), signature._data.begin());
         std::copy(sig.begin(), sig.end(), signature._data.begin() + crypto_sign_PUBLICKEYBYTES);
         BOOST_CHECK_EQUAL(signature.verify_solana(reinterpret_cast<const uint8_t*>(msg.data()), msg.size(), key), vector.valid);
      }
   }
}

/// Reject short/long buffers in the shared helper before passing their pointers to libsodium.
BOOST_AUTO_TEST_CASE(invalid_buffer_lengths) {
   const auto key = fc::crypto::ed::private_key_shim::generate();
   const auto public_key = key.get_public_key();
   const auto signature = key.sign_raw(nullptr, 0);
   std::array<std::byte, crypto_sign_BYTES + 1> sig{};
   std::array<std::byte, crypto_sign_PUBLICKEYBYTES + 1> pub{};
   const auto signature_bytes = std::as_bytes(std::span{signature._data}.subspan(crypto_sign_PUBLICKEYBYTES));
   const auto key_bytes = std::as_bytes(std::span{public_key._data});
   std::copy(signature_bytes.begin(), signature_bytes.end(), sig.begin());
   std::copy(key_bytes.begin(), key_bytes.end(), pub.begin());
   const auto valid_sig = std::span{sig}.first(crypto_sign_BYTES);
   const auto valid_pub = std::span{pub}.first(crypto_sign_PUBLICKEYBYTES);
   BOOST_REQUIRE(fc::crypto::ed::verify_detached(valid_sig, {}, valid_pub));
   for (const size_t size : {size_t{0}, size_t{crypto_sign_BYTES - 1}, sig.size()})
      BOOST_CHECK(!fc::crypto::ed::verify_detached(std::span{sig}.first(size), {}, valid_pub));
   for (const size_t size : {size_t{0}, size_t{crypto_sign_PUBLICKEYBYTES - 1}, pub.size()})
      BOOST_CHECK(!fc::crypto::ed::verify_detached(valid_sig, {}, std::span{pub}.first(size)));
   BOOST_CHECK(!fc::crypto::ed::verify_detached({}, {}, {}));
}

/// Reusing the raw verifier must preserve transaction/recover_key's existing ASCII-hex digest convention.
BOOST_AUTO_TEST_CASE(digest_recovery_preserves_hex_encoding) {
   constexpr auto message = "Ed25519 transaction digest compatibility";
   const auto digest = fc::sha256::hash(message);
   const auto key = fc::crypto::ed::private_key_shim::generate();
   const auto pub = key.get_public_key();
   const auto signature = key.sign_sha256(digest);
   BOOST_CHECK(signature.verify(digest, pub));
   BOOST_CHECK(signature.recover(digest) == pub);
   const auto hex = fc::to_hex(digest.data(), digest.data_size());
   BOOST_CHECK(signature.verify_solana(reinterpret_cast<const uint8_t*>(hex.data()), hex.size(), pub));
   BOOST_CHECK(!signature.verify_solana(reinterpret_cast<const uint8_t*>(digest.data()), digest.data_size(), pub));
   const auto raw = key.sign_raw(reinterpret_cast<const uint8_t*>(digest.data()), digest.data_size());
   BOOST_CHECK(!raw.verify(digest, pub));
   BOOST_CHECK_THROW(raw.recover(digest), fc::assert_exception);
}

BOOST_AUTO_TEST_SUITE_END()

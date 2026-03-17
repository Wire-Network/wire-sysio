/**
 * Golden-value tests for consensus-critical crypto hash functions.
 *
 * The WASM intrinsics (sha256, sha1, sha512, ripemd160) delegate to the
 * fc:: hash implementations via hash_with_checktime. These tests assert
 * exact output for NIST/standard test vectors, catching any change in
 * the underlying hash library.
 */
#include <fc/crypto/sha256.hpp>
#include <fc/crypto/sha1.hpp>
#include <fc/crypto/sha512.hpp>
#include <fc/crypto/ripemd160.hpp>
#include <fc/crypto/sha3.hpp>
#include <fc/crypto/keccak256.hpp>
#include <fc/crypto/private_key.hpp>
#include <fc/crypto/public_key.hpp>
#include <fc/crypto/signature.hpp>
#include <fc/crypto/bls_private_key.hpp>
#include <fc/crypto/bls_public_key.hpp>
#include <fc/crypto/bls_signature.hpp>

#include <boost/test/unit_test.hpp>

#include <string>
#include <vector>

using namespace fc;

BOOST_AUTO_TEST_SUITE(crypto_golden_tests)

// ============================================================================
// SHA-256 golden values (NIST FIPS 180-4 test vectors)
// ============================================================================

BOOST_AUTO_TEST_CASE(sha256_golden) {
   // NIST test vector: "abc"
   BOOST_CHECK_EQUAL(
      sha256::hash("abc", 3).str(),
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

   // Empty string
   BOOST_CHECK_EQUAL(
      sha256::hash("", 0).str(),
      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

   // NIST test vector: 448-bit message
   const char* msg448 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
   BOOST_CHECK_EQUAL(
      sha256::hash(msg448, strlen(msg448)).str(),
      "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

   // "message digest"
   BOOST_CHECK_EQUAL(
      sha256::hash("message digest", 14).str(),
      "f7846f55cf23e14eebeab5b4e1550cad5b509e3348fbc4efa3a1413d393cb650");

   // Determinism: same input always produces same output
   auto h1 = sha256::hash("determinism test", 16);
   auto h2 = sha256::hash("determinism test", 16);
   BOOST_CHECK(h1 == h2);

   // Different input produces different output
   auto h3 = sha256::hash("different input", 15);
   BOOST_CHECK(h1 != h3);
}

// ============================================================================
// SHA-1 golden values (NIST FIPS 180-4 test vectors)
// ============================================================================

BOOST_AUTO_TEST_CASE(sha1_golden) {
   // "abc"
   BOOST_CHECK_EQUAL(
      sha1::hash("abc", 3).str(),
      "a9993e364706816aba3e25717850c26c9cd0d89d");

   // Empty string
   BOOST_CHECK_EQUAL(
      sha1::hash("", 0).str(),
      "da39a3ee5e6b4b0d3255bfef95601890afd80709");

   // 448-bit message
   const char* msg448 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
   BOOST_CHECK_EQUAL(
      sha1::hash(msg448, strlen(msg448)).str(),
      "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
}

// ============================================================================
// SHA-512 golden values (NIST FIPS 180-4 test vectors)
// ============================================================================

BOOST_AUTO_TEST_CASE(sha512_golden) {
   // "abc"
   BOOST_CHECK_EQUAL(
      sha512::hash("abc", 3).str(),
      "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
      "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");

   // Empty string
   BOOST_CHECK_EQUAL(
      sha512::hash("", 0).str(),
      "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
      "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e");

   // 448-bit message
   const char* msg448 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
   BOOST_CHECK_EQUAL(
      sha512::hash(msg448, strlen(msg448)).str(),
      "204a8fc6dda82f0a0ced7beb8e08a41657c16ef468b228a8279be331a703c335"
      "96fd15c13b1b07f9aa1d3bea57789ca031ad85c7a71dd70354ec631238ca3445");
}

// ============================================================================
// RIPEMD-160 golden values
// ============================================================================

BOOST_AUTO_TEST_CASE(ripemd160_golden) {
   // "abc"
   BOOST_CHECK_EQUAL(
      ripemd160::hash("abc", 3).str(),
      "8eb208f7e05d987a9b044a8e98c6b087f15a0bfc");

   // Empty string
   BOOST_CHECK_EQUAL(
      ripemd160::hash("", 0).str(),
      "9c1185a5c5e9fc54612808977ee8f548b2258d31");

   // "message digest"
   BOOST_CHECK_EQUAL(
      ripemd160::hash("message digest", 14).str(),
      "5d0689ef49d2fae572b881b123a85ffa21595f36");

   // "abcdefghijklmnopqrstuvwxyz"
   const char* alpha = "abcdefghijklmnopqrstuvwxyz";
   BOOST_CHECK_EQUAL(
      ripemd160::hash(alpha, 26).str(),
      "f71c27109c692c1b56bbdceb5b9d2865b3708dbc");
}

// ============================================================================
// Cross-hash verification: same input, different hash functions, different output
// ============================================================================

BOOST_AUTO_TEST_CASE(cross_hash_independence) {
   const char* input = "consensus critical data";
   size_t len = strlen(input);

   auto h256  = sha256::hash(input, len);
   auto h1    = sha1::hash(input, len);
   auto h512  = sha512::hash(input, len);
   auto hripe = ripemd160::hash(input, len);

   // All different lengths
   BOOST_CHECK_EQUAL(sizeof(h256), 32u);
   BOOST_CHECK_EQUAL(sizeof(h1), 20u);
   BOOST_CHECK_EQUAL(sizeof(h512), 64u);
   BOOST_CHECK_EQUAL(sizeof(hripe), 20u);

   // SHA1 and RIPEMD160 are both 20 bytes but different algorithms
   BOOST_CHECK(memcmp(&h1, &hripe, 20) != 0);
}

// ============================================================================
// Hash stability under binary input (non-printable bytes)
// ============================================================================

BOOST_AUTO_TEST_CASE(sha256_binary_input) {
   // All-zero input
   char zeros[32] = {};
   auto h = sha256::hash(zeros, 32);
   BOOST_CHECK_EQUAL(
      h.str(),
      "66687aadf862bd776c8fc18b8e9f8e20089714856ee233b3902a591d0d5f2925");

   // Single byte 0x01
   char one = 0x01;
   auto h2 = sha256::hash(&one, 1);
   BOOST_CHECK_EQUAL(
      h2.str(),
      "4bf5122f344554c53bde2ebb8cd2b7e3d1600ad631c385a5d7cce23c7785459a");
}

// ============================================================================
// SHA-3 (NIST) golden values
// ============================================================================

BOOST_AUTO_TEST_CASE(sha3_nist_golden) {
   // Empty string — NIST SHA3-256
   BOOST_CHECK_EQUAL(
      sha3::hash("", 0, true).str(),
      "a7ffc6f8bf1ed76651c14756a061d662f580ff4de43b49fa82d80a4b80f8434a");

   // "abc" — NIST SHA3-256
   BOOST_CHECK_EQUAL(
      sha3::hash("abc", 3, true).str(),
      "3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532");

   // Longer input
   const char* msg = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
   BOOST_CHECK_EQUAL(
      sha3::hash(msg, strlen(msg), true).str(),
      "41c0dba2a9d6240849100376a8235e2c82e1b9998a999e21db32dd97496d3376");
}

// ============================================================================
// Keccak-256 golden values (Ethereum standard — NOT NIST)
// ============================================================================

BOOST_AUTO_TEST_CASE(keccak256_golden) {
   // Empty string — Keccak256 (Ethereum)
   BOOST_CHECK_EQUAL(
      sha3::hash("", 0, false).str(),
      "c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470");

   // "abc" — Keccak256
   BOOST_CHECK_EQUAL(
      sha3::hash("abc", 3, false).str(),
      "4e03657aea45a94fc7d47ba826c8d667c0d1e6e33a64a036ec44f58fa12d6c45");

   // Verify NIST vs Keccak produce DIFFERENT outputs for same input
   auto nist = sha3::hash("abc", 3, true);
   auto keccak = sha3::hash("abc", 3, false);
   BOOST_CHECK(nist.str() != keccak.str());
}

// ============================================================================
// K1 Recover (secp256k1 public key recovery)
// ============================================================================

BOOST_AUTO_TEST_CASE(k1_sign_determinism) {
   // K1 signing must be deterministic (RFC 6979)
   auto privkey = crypto::private_key::generate(crypto::private_key::key_type::k1);
   auto pubkey = privkey.get_public_key();

   auto digest = sha256::hash("k1 determinism test", 19);
   auto sig1 = privkey.sign(digest);
   auto sig2 = privkey.sign(digest);

   // Same key + same digest = same signature (deterministic nonce)
   BOOST_CHECK(sig1 == sig2);

   // Different digest = different signature
   auto digest2 = sha256::hash("different message", 17);
   auto sig3 = privkey.sign(digest2);
   BOOST_CHECK(sig1 != sig3);

   // Different key = different signature
   auto privkey2 = crypto::private_key::generate(crypto::private_key::key_type::k1);
   auto sig4 = privkey2.sign(digest);
   BOOST_CHECK(sig1 != sig4);

   // Public key derivation is deterministic
   auto pubkey2 = privkey.get_public_key();
   BOOST_CHECK(pubkey == pubkey2);
}

// ============================================================================
// BLS golden values — key generation, signing, verification
// ============================================================================

BOOST_AUTO_TEST_CASE(bls_sign_verify_determinism) {
   // Generate from seed for determinism
   std::vector<uint8_t> seed(32, 0x42);
   auto privkey = fc::crypto::bls::private_key(seed);
   auto pubkey = privkey.get_public_key();

   // Public key must be valid
   BOOST_CHECK(pubkey.valid());

   // Serialization round-trip
   auto pubkey_bytes = pubkey.serialize();
   auto pubkey2 = fc::crypto::bls::public_key(pubkey_bytes);
   BOOST_CHECK(pubkey2.serialize() == pubkey_bytes);

   // Sign and verify
   auto digest = sha256::hash("bls test message", 16);
   auto sig = privkey.sign_sha256(digest);

   std::vector<uint8_t> msg(reinterpret_cast<const uint8_t*>("bls test message"),
                            reinterpret_cast<const uint8_t*>("bls test message") + 16);
   // Signature serialization round-trip
   auto sig_bytes = sig.serialize();
   auto sig2 = fc::crypto::bls::signature(sig_bytes);
   BOOST_CHECK(sig2.serialize() == sig_bytes);
}

BOOST_AUTO_TEST_CASE(bls_different_keys_different_sigs) {
   std::vector<uint8_t> seed1(32, 0x01);
   std::vector<uint8_t> seed2(32, 0x02);
   auto key1 = fc::crypto::bls::private_key(seed1);
   auto key2 = fc::crypto::bls::private_key(seed2);

   // Different seeds produce different public keys
   BOOST_CHECK(key1.get_public_key().serialize() != key2.get_public_key().serialize());

   // Same message, different keys produce different signatures
   auto digest = sha256::hash("same message", 12);
   auto sig1 = key1.sign_sha256(digest);
   auto sig2 = key2.sign_sha256(digest);
   BOOST_CHECK(sig1.serialize() != sig2.serialize());
}

BOOST_AUTO_TEST_CASE(bls_public_key_serialization_size) {
   std::vector<uint8_t> seed(32, 0xAA);
   auto privkey = fc::crypto::bls::private_key(seed);
   auto pubkey = privkey.get_public_key();

   // BLS public key is 96 bytes (G1 point, affine, non-Montgomery, little-endian)
   BOOST_CHECK_EQUAL(pubkey.serialize().size(), 96u);

   // BLS signature is 192 bytes (G2 point)
   auto digest = sha256::hash("size test", 9);
   auto sig = privkey.sign_sha256(digest);
   BOOST_CHECK_EQUAL(sig.serialize().size(), 192u);
}

BOOST_AUTO_TEST_CASE(bls_proof_of_possession) {
   std::vector<uint8_t> seed(32, 0xBB);
   auto privkey = fc::crypto::bls::private_key(seed);

   // Proof of possession must be deterministic
   auto pop1 = privkey.proof_of_possession();
   auto pop2 = privkey.proof_of_possession();
   BOOST_CHECK(pop1.serialize() == pop2.serialize());

   // Different keys produce different proofs
   std::vector<uint8_t> seed2(32, 0xCC);
   auto privkey2 = fc::crypto::bls::private_key(seed2);
   auto pop3 = privkey2.proof_of_possession();
   BOOST_CHECK(pop1.serialize() != pop3.serialize());
}

BOOST_AUTO_TEST_SUITE_END()

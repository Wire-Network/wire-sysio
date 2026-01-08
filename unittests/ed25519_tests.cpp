#include <boost/test/unit_test.hpp>
#include <fc/crypto/base58.hpp>      // fc::from_base58
#include <fc/crypto/elliptic_ed.hpp> // public_key_shim, signature_shim, private_key_shim
#include <fc/crypto/sha256.hpp>      // fc::sha256::hash
#include <fc/crypto/private_key.hpp>
#include <fc/crypto/public_key.hpp>
#include <fc/io/raw.hpp>             // fc::raw::pack / unpack
#include <fc/crypto/hex.hpp>         // fc::to_hex
#include <sodium.h>                  // libsodium

using namespace fc::crypto::ed;

BOOST_AUTO_TEST_SUITE(ed25519_tests)

// Test 0: Phantom‐generated signature on raw "hello" via libsodium verify
static constexpr std::array<uint8_t,5> raw_message = {{ 'h','e','l','l','o' }};
static constexpr char solana_address[] = "8gyK1dDhSBchQdf3T5tDXrJXawdwv4wKb45QsLaiGsar";
static const std::array<uint8_t,64> signature_bytes = {{
    74,255,192,241,191,244,80,184,215,164,43,194,26,179,32,164,
    110,135,210,106,200,228,225,233,156,209,147,209,145,245,200,143,
    84,32,210,211,67,65,210,24,75,130,216,212,163,238,143,76,
    224,207,40,35,81,191,236,144,248,197,69,81,233,35,141,12
}};

BOOST_AUTO_TEST_CASE(ed25519_phantom_test) {
    try {
        // 1) Decode Solana address → 32-byte raw Ed25519 public key
        auto decoded = fc::from_base58(solana_address);
        BOOST_REQUIRE_MESSAGE(
            decoded.size() == crypto_sign_PUBLICKEYBYTES,
            "Base58 decoded length = " << decoded.size()
            << ", expected " << crypto_sign_PUBLICKEYBYTES
        );

        // 2) Load into public_key_shim and validate
        public_key_shim pubkey;
        std::copy_n(
            reinterpret_cast<const unsigned char*>(decoded.data()),
            crypto_sign_PUBLICKEYBYTES,
            pubkey._data.data()
        );
        BOOST_REQUIRE_MESSAGE(
            pubkey.valid(),
            "public_key_shim.valid() returned false; key = "
            << fc::to_hex(
                   reinterpret_cast<const char*>(pubkey._data.data()),
                   crypto_sign_PUBLICKEYBYTES
               )
        );

        // 3) Directly verify the 64-byte signature on "hello" with libsodium
        int rc = crypto_sign_verify_detached(
            signature_bytes.data(),
            raw_message.data(),
            raw_message.size(),
            pubkey._data.data()
        );
        BOOST_CHECK_MESSAGE(
            rc == 0,
            "crypto_sign_verify_detached failed (rc=" << rc << ")"
        );
    } FC_LOG_AND_RETHROW()
}

// Test 1: Generate a fresh keypair, sign SHA-256("hello"), verify via shim
BOOST_AUTO_TEST_CASE(hello_dynamic_key_signature_test) {
    try {
        // 1) Initialize libsodium
        static bool inited = (sodium_init() >= 0);
        BOOST_REQUIRE_MESSAGE(inited, "sodium_init() failed");

        // 2) Derive keypair from random seed
        unsigned char seed[crypto_sign_SEEDBYTES];
        randombytes_buf(seed, sizeof(seed));
        unsigned char pk_raw[crypto_sign_PUBLICKEYBYTES], sk_raw[crypto_sign_SECRETKEYBYTES];
        int rc = crypto_sign_seed_keypair(pk_raw, sk_raw, seed);
        BOOST_REQUIRE_MESSAGE(rc == 0, "crypto_sign_seed_keypair failed (rc=" << rc << ")");

        // 3) Copy raw key bytes into our shim types
        private_key_shim sk;
        std::copy_n(sk_raw, crypto_sign_SECRETKEYBYTES, sk._data.data());
        public_key_shim  pk;
        std::copy_n(pk_raw, crypto_sign_PUBLICKEYBYTES, pk._data.data());
        BOOST_CHECK_MESSAGE(pk.valid(), "Derived public_key_shim.valid() returned false");

        // 4) Hash the string "hello"
        auto digest = fc::sha256::hash(std::string("hello"));
        BOOST_TEST_MESSAGE("SHA-256(\"hello\") = " << digest);

        // 5) Sign the digest
        signature_shim sig = sk.sign(digest, /*require_canonical=*/false);

        // 6) Verify the signature using our shim
        bool ok = sig.verify(digest, pk);
        BOOST_CHECK_MESSAGE(ok, "signature_shim.verify() returned false");
    } FC_LOG_AND_RETHROW()
}

// Test 2: sign/verify round-trip on a SHA-256 digest via shim.sign/verify
BOOST_AUTO_TEST_CASE(sign_verify_sha256_roundtrip) {
    try {
        // 1) Initialize libsodium
        BOOST_REQUIRE_MESSAGE(sodium_init() >= 0, "sodium_init() failed");

        // 2) Generate a keypair
        unsigned char seed[crypto_sign_SEEDBYTES];
        randombytes_buf(seed, sizeof(seed));
        unsigned char pk_raw[crypto_sign_PUBLICKEYBYTES], sk_raw[crypto_sign_SECRETKEYBYTES];
        int rc = crypto_sign_seed_keypair(pk_raw, sk_raw, seed);
        BOOST_REQUIRE_MESSAGE(rc == 0, "Keypair generation failed (rc=" << rc << ")");

        // 3) Load into shim objects
        private_key_shim sk;
        std::copy_n(sk_raw, crypto_sign_SECRETKEYBYTES, sk._data.data());
        public_key_shim pk = sk.get_public_key();
        BOOST_REQUIRE_MESSAGE(pk.valid(), "public_key_shim.valid() returned false");

        // 4) Hash an arbitrary message
        const std::string payload = "roundtrip test";
        auto digest = fc::sha256::hash(payload);

        // 5) Sign & 6) Verify
        signature_shim sig = sk.sign(digest, false);
        bool ok = sig.verify(digest, pk);
        BOOST_CHECK_MESSAGE(ok, "sign/verify round-trip failed; digest=" << digest);
    } FC_LOG_AND_RETHROW()
}

// Test 3: Shared-secret symmetry via X25519 (generate_shared_secret)
BOOST_AUTO_TEST_CASE(shared_secret_symmetry) {
    try {
        // 1) Initialize libsodium
        BOOST_REQUIRE_MESSAGE(sodium_init() >= 0, "sodium_init() failed");

        // 2) Create two independent seeds & keypairs
        unsigned char s1[crypto_sign_SEEDBYTES], s2[crypto_sign_SEEDBYTES];
        randombytes_buf(s1,sizeof(s1)); randombytes_buf(s2,sizeof(s2));

        unsigned char pk1_raw[crypto_sign_PUBLICKEYBYTES], sk1_raw[crypto_sign_SECRETKEYBYTES];
        unsigned char pk2_raw[crypto_sign_PUBLICKEYBYTES], sk2_raw[crypto_sign_SECRETKEYBYTES];

        int rc1 = crypto_sign_seed_keypair(pk1_raw, sk1_raw, s1);
        BOOST_REQUIRE_MESSAGE(rc1 == 0, "keypair1 generation failed (rc=" << rc1 << ")");
        int rc2 = crypto_sign_seed_keypair(pk2_raw, sk2_raw, s2);
        BOOST_REQUIRE_MESSAGE(rc2 == 0, "keypair2 generation failed (rc=" << rc2 << ")");

        // 3) Load into shim types
        private_key_shim sk1, sk2;
        public_key_shim  pk1, pk2;
        std::copy_n(sk1_raw,crypto_sign_SECRETKEYBYTES,sk1._data.data());
        std::copy_n(sk2_raw,crypto_sign_SECRETKEYBYTES,sk2._data.data());
        std::copy_n(pk1_raw,crypto_sign_PUBLICKEYBYTES,pk1._data.data());
        std::copy_n(pk2_raw,crypto_sign_PUBLICKEYBYTES,pk2._data.data());

        // 4) ED25519 shared-secret is currently unsupported → should throw
        BOOST_CHECK_THROW(
                sk1.generate_shared_secret(pk2),
                fc::exception
            );

       // verify to/from string
       fc::crypto::public_key pubkey1(pk1);
       std::string pk1_str = pubkey1.to_string({});
       dlog("pk1_str = ${k}", ("k", pk1_str));
       fc::crypto::public_key pubkey2(pk1_str);
       std::string pk2_str = pubkey2.to_string({});
       BOOST_TEST(pk1_str == pk2_str);
       BOOST_TEST(pk1_str.starts_with("PUB_ED_"));

    } FC_LOG_AND_RETHROW()
}

// Test 4: signature_shim.recover() must throw (unsupported for ED25519)
BOOST_AUTO_TEST_CASE(signature_recover_throws) {
    try {
        signature_shim sig;
        fc::sha256 dummy;
        // Recover is unsupported—should always throw
        BOOST_CHECK_THROW(sig.recover(dummy, true), fc::exception);
    } FC_LOG_AND_RETHROW()
}

// Test 5: pack/unpack public_key_shim preserves all bytes
BOOST_AUTO_TEST_CASE(pack_unpack_public_key) {
    try {
        // 1) Prepare a dummy public_key_shim (32 bytes of 0xAB)
        public_key_shim orig;
        std::fill_n(orig._data.data(), crypto_sign_PUBLICKEYBYTES, 0xAB);

        // 2) Pack to binary, then unpack
        auto blob = fc::raw::pack(orig);
        auto got  = fc::raw::unpack<public_key_shim>(blob);

        // 3) Compare raw buffers
        BOOST_CHECK_MESSAGE(
            memcmp(orig._data.data(), got._data.data(), crypto_sign_PUBLICKEYBYTES) == 0,
            "public_key_shim pack/unpack mismatch"
        );
    } FC_LOG_AND_RETHROW()
}

// Test 6: pack/unpack signature_shim preserves 64 bytes + zero-padding
BOOST_AUTO_TEST_CASE(pack_unpack_signature) {
    try {
        // 1) Prepare dummy signature_shim (64x 0x5A + pad=0)
        signature_shim orig;
        std::fill_n(orig._data.data(), crypto_sign_BYTES, 0x5A);
        orig._data[crypto_sign_BYTES] = 0;

        // 2) Pack: Use fc standard packing/unpacking
        auto blob = fc::raw::pack(orig);
        BOOST_CHECK_MESSAGE(
            blob.size() == crypto_sign_BYTES,
            "blob.size()=" << blob.size()
            << ", expected=" << crypto_sign_BYTES
        );

        // 3) Unpack back → got
        auto got = fc::raw::unpack<signature_shim>(blob);

        // 4) Verify the 64 data bytes match
        BOOST_CHECK_MESSAGE(
            memcmp(orig._data.data(), got._data.data(), crypto_sign_BYTES) == 0,
            "signature bytes mismatch after unpack"
        );
        // 5) The implicit pad byte (index 64) should still be zero
        BOOST_CHECK_EQUAL(got._data[crypto_sign_BYTES], 0u);
    } FC_LOG_AND_RETHROW()
}

// Test 7: padding persistence through multiple pack/unpack loops
BOOST_AUTO_TEST_CASE(signature_padding_persistence) {
    try {
        // 1) Prepare dummy signature_shim + pad=0
        signature_shim orig;
        std::fill_n(orig._data.data(), crypto_sign_BYTES, 0xA5);
        orig._data[crypto_sign_BYTES] = 0;

        // 2) Pack/unpack twice
        auto b1 = fc::raw::pack(orig);
        auto u1 = fc::raw::unpack<signature_shim>(b1);
        auto b2 = fc::raw::pack(u1);
        auto u2 = fc::raw::unpack<signature_shim>(b2);

        // 3) Pad must remain zero each time
        BOOST_CHECK_EQUAL(u1._data[crypto_sign_BYTES], 0u);
        BOOST_CHECK_EQUAL(u2._data[crypto_sign_BYTES], 0u);

        // 4) Inner 64 bytes must remain unchanged
        BOOST_CHECK_MESSAGE(
            memcmp(orig._data.data(), u2._data.data(), crypto_sign_BYTES) == 0,
            "signature bytes corrupted after pack/unpack loops"
        );
    } FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_SUITE_END()

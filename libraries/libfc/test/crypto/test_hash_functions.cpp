#include <boost/test/unit_test.hpp>

#include <fc/crypto/hex.hpp>
#include <fc/crypto/sha1.hpp>
#include <fc/crypto/sha224.hpp>
#include <fc/crypto/sha256.hpp>
#include <fc/crypto/sha3.hpp>
#include <fc/crypto/sha512.hpp>
#include <fc/crypto/ripemd160.hpp>
#include <fc/crypto/keccak256.hpp>
#include <fc/crypto/blake3.hpp>
#include <fc/io/json.hpp>
#include <fc/reflect/json_stream.hpp>
#include <fc/utility.hpp>

using namespace fc;

namespace {
   // Round-trip exercise for FC_SERIALIZE_AS_STRING-trait hash types.  Verifies that:
   //   - Construction from a hex string round-trips through str() / to_string().
   //   - The static T::from_string(string_view) factory matches construction.
   //   - The trait-routed fc::to_variant + fc::from_variant round-trip preserves the value
   //     and the variant's payload is the expected hex form.
   //   - fc::to_json_string emits the hex inside JSON quotes.
   template<typename Hash>
   void check_hash_string_roundtrip(std::string_view hex) {
      const Hash h{ hex };
      BOOST_CHECK_EQUAL(h.str(),       hex);
      BOOST_CHECK_EQUAL(h.to_string(), hex);

      const Hash h2 = Hash::from_string(hex);
      BOOST_CHECK(h == h2);

      fc::variant v;
      fc::to_variant(h, v);
      BOOST_CHECK_EQUAL(v.as_string(), hex);

      Hash h3;
      fc::from_variant(v, h3);
      BOOST_CHECK(h == h3);

      const std::string expected_json = std::string{ "\"" } + std::string{ hex } + "\"";
      BOOST_CHECK_EQUAL(fc::to_json_string(h), expected_json);
   }
}

BOOST_AUTO_TEST_SUITE(hash_functions)
BOOST_AUTO_TEST_CASE(sha3) try {

   using test_sha3 = std::tuple<std::string, std::string>;
   const std::vector<test_sha3> tests {
      //test
      {
         "",
         "a7ffc6f8bf1ed76651c14756a061d662f580ff4de43b49fa82d80a4b80f8434a",
      },

      //test
      {
         "abc",
         "3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532",
      },

      //test
      {
         "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
         "41c0dba2a9d6240849100376a8235e2c82e1b9998a999e21db32dd97496d3376",
      }
   };

   for(const auto& test : tests) {
      BOOST_CHECK_EQUAL(fc::sha3::hash(std::get<0>(test), true).str(), std::get<1>(test));
   }

} FC_LOG_AND_RETHROW();


BOOST_AUTO_TEST_CASE(keccak256) try {

   using test_keccak256 = std::tuple<std::string, std::string>;
   const std::vector<test_keccak256> tests {
      //test
      {
         "",
         "c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470",
      },

      //test
      {
         "abc",
         "4e03657aea45a94fc7d47ba826c8d667c0d1e6e33a64a036ec44f58fa12d6c45",
      },

      //test
      {
         "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
         "45d3b367a6904e6e8d502ee04999a7c27647f91fa845d456525fd352ae3d7371",
      }
   };

   for(const auto& test : tests) {
      BOOST_CHECK_EQUAL(fc::sha3::hash(std::get<0>(test), false).str(), std::get<1>(test));
   }

} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(sha1_string_roundtrip) try {
   // 20 bytes / 40 hex chars
   check_hash_string_roundtrip<fc::sha1>("0123456789abcdef0123456789abcdef01234567");
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(sha224_string_roundtrip) try {
   // 28 bytes / 56 hex chars
   check_hash_string_roundtrip<fc::sha224>(
      "0123456789abcdef0123456789abcdef0123456789abcdef01234567");
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(sha256_string_roundtrip) try {
   // 32 bytes / 64 hex chars
   check_hash_string_roundtrip<fc::sha256>(
      "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(sha512_string_roundtrip) try {
   // 64 bytes / 128 hex chars
   check_hash_string_roundtrip<fc::sha512>(
      "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
      "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210");
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(sha3_string_roundtrip) try {
   // 32 bytes / 64 hex chars
   check_hash_string_roundtrip<fc::sha3>(
      "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(ripemd160_string_roundtrip) try {
   // 20 bytes / 40 hex chars
   check_hash_string_roundtrip<fc::ripemd160>("0123456789abcdef0123456789abcdef01234567");
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(keccak256_string_roundtrip) try {
   // 32 bytes / 64 hex chars
   check_hash_string_roundtrip<fc::crypto::keccak256>(
      "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(blake3_string_roundtrip) try {
   // 32 bytes / 64 hex chars
   check_hash_string_roundtrip<fc::crypto::blake3>(
      "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_SUITE_END()
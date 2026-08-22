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

#include <random>
#include <string>
#include <tuple>
#include <vector>

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

      // Odd-length hex is malformed input: from_string (and therefore the trait-routed
      // fc::from_variant) must reject it rather than zero-extend the trailing nibble.
      const std::string odd{ hex.substr(0, hex.size() - 1) };
      BOOST_CHECK_THROW(Hash::from_string(odd), fc::assert_exception);
      Hash h4;
      BOOST_CHECK_THROW(fc::from_variant(fc::variant{ odd }, h4), fc::assert_exception);
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
      },

      // ERC-20 Transfer event topic -- a recognizable Ethereum constant that
      // any drift in the Keccak-256 padding domain separator would corrupt.
      {
         "Transfer(address,address,uint256)",
         "ddf252ad1be2c89b69c2b068fc378daa952ba7f163c4a11628f55a4df523b3ef",
      },
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

/// Sanity-check that fc::crypto::keccak256 (the ethash-backed wrapper used by
/// EM signature recovery and Ethereum tooling) returns the expected bytes for
/// standard vectors. Catches wiring bugs in the wrapper independent of ethash
/// itself.
BOOST_AUTO_TEST_CASE(keccak256_class_golden) try {

   using test_keccak256_class = std::tuple<std::string, std::string>;
   const std::vector<test_keccak256_class> tests {
      {
         "",
         "c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470",
      },
      {
         "abc",
         "4e03657aea45a94fc7d47ba826c8d667c0d1e6e33a64a036ec44f58fa12d6c45",
      },
      {
         "Transfer(address,address,uint256)",
         "ddf252ad1be2c89b69c2b068fc378daa952ba7f163c4a11628f55a4df523b3ef",
      },
   };

   for(const auto& test : tests) {
      BOOST_CHECK_EQUAL(fc::crypto::keccak256::hash(std::get<0>(test)).str(),
                        std::get<1>(test));
   }

} FC_LOG_AND_RETHROW();

/// Cross-impl agreement between fc::sha3 (hand-rolled, on the consensus path
/// via the WASM sha3() host intrinsic) and fc::crypto::keccak256 (ethash-
/// backed, on the consensus path via EM signature recovery). Both must
/// produce bit-identical Keccak-256 output for every input or chains carrying
/// EM-signed transactions alongside contracts that hash via sysio::keccak()
/// will desynchronize.
///
/// Inputs cover the Keccak-256 sponge rate boundary (1088 bits / 136 bytes)
/// and a multi-block buffer to exercise the absorbing phase across rounds.
///
/// This test is one leg of a three-way pin: the keccak256 case anchors
/// fc::sha3 against published vectors, keccak256_class_golden anchors
/// fc::crypto::keccak256 against the same vectors, and this case asserts the
/// two impls agree across additional inputs. A bug that shifted both impls
/// identically off-spec would be caught by the golden cases, not this one --
/// they are complementary, not redundant.
BOOST_AUTO_TEST_CASE(ethash_fc_sha3_cross_impl_agreement) try {

   const std::vector<std::string> corpus = {
      "",
      "a",
      "abc",
      "Transfer(address,address,uint256)",
      std::string(135, 'A'),   // rate boundary minus one byte
      std::string(136, 'A'),   // exactly one Keccak-256 rate
      std::string(137, 'A'),   // rate plus one byte (forces a second block)
      std::string(1024, 'Z'),  // multi-block input
   };

   for(const auto& msg : corpus) {
      BOOST_CHECK_EQUAL(fc::sha3::hash(msg, false).str(),
                        fc::crypto::keccak256::hash(msg).str());
   }

   // Large deterministic buffer beyond the small vector corpus, to ensure the
   // agreement holds at sizes that absorb many sponge blocks.
   std::mt19937_64 rng{0xC0FFEEULL};
   std::string big(100 * 1024, '\0');
   for(auto& c : big) c = static_cast<char>(rng());
   BOOST_CHECK_EQUAL(fc::sha3::hash(big, false).str(),
                     fc::crypto::keccak256::hash(big).str());

} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_SUITE_END()
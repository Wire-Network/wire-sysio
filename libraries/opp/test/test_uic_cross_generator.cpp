#include <boost/test/unit_test.hpp>

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <fc/crypto/private_key.hpp>
#include <fc/crypto/hex.hpp>
#include <fc/io/raw.hpp>
#include <sysio/opp/attestations/attestations.pb.h>
#include <zpp_bits.h>

// The host protobuf and CDT zpp generators intentionally emit the same C++
// namespace. Rename only the generated CDT model in this characterization TU
// so both real generator outputs can be compared directly without wrappers.
#define sysio cdt_sysio
#include <sysio/opp/attestations/attestations.pb.hpp>
#undef sysio

namespace {

using host_uic = sysio::opp::attestations::UnderwriteIntentCommit;
using cdt_uic = cdt_sysio::opp::attestations::UnderwriteIntentCommit;

struct uic_golden_vector {
   std::string_view label;
   std::string_view vector_id;
   std::string_view schema_revision;
   std::string_view record_checksum_hex;
   sysio::opp::types::ChainKind host_kind;
   cdt_sysio::opp::types::ChainKind cdt_kind;
   std::string_view address_hex;
   std::string_view public_key;
   std::string_view blanked_hex;
   std::string_view signing_digest_hex;
   std::string_view signature_hex;
   std::string_view full_hex;
   std::string_view full_sha256_hex;
};

constexpr std::string_view uic_schema_revision =
   "wire-opp-attestations-underwrite-intent-commit-v1-fields-1-2-3-5-6-7-8";

/** Hash the deliberately simple, language-neutral sealed-record envelope. */
std::string sealed_record_checksum(const uic_golden_vector& vector) {
   const auto record = std::string{"wire291-uic-vector-record-v1\nvector_id="}
      + std::string{vector.vector_id}
      + "\nschema_revision=" + std::string{vector.schema_revision}
      + "\nfull_hex=" + std::string{vector.full_hex}
      + "\nfull_sha256=" + std::string{vector.full_sha256_hex} + "\n";
   return fc::sha256::hash(record.data(), record.size()).str();
}

/** Paired load-bearing encodings produced while signing one host UIC. */
struct uic_encodings {
   std::vector<char> blanked; ///< Serialized UIC with an empty signature.
   std::vector<char> full;    ///< Serialized UIC with its signature populated.
};

/** Serialize a host protobuf UIC with its current signature bytes. */
std::vector<char> serialize_host_uic(const host_uic& uic) {
   std::string bytes;
   BOOST_REQUIRE(uic.SerializeToString(&bytes));
   return {bytes.begin(), bytes.end()};
}

/** Decode an immutable lowercase hexadecimal protocol fixture. */
std::vector<char> chars_from_hex(std::string_view hex) {
   BOOST_REQUIRE_EQUAL(hex.size() % 2, 0u);
   std::vector<char> bytes(hex.size() / 2);
   BOOST_REQUIRE_EQUAL(fc::from_hex(hex, bytes.data(), bytes.size()), bytes.size());
   return bytes;
}

/** Construct and encode a golden UIC directly with the generated CDT model. */
uic_encodings encode_cdt_uic(const uic_golden_vector& vector) {
   cdt_uic uic;
   uic.uw_account.name = "underwriter";
   uic.uw_ext_chain_addr.kind = vector.cdt_kind;
   uic.uw_ext_chain_addr.address = chars_from_hex(vector.address_hex);
   uic.uw_request_id = 300;
   uic.token_code = 301;
   uic.chain_code = 302;
   uic.reserve_code = 303;

   std::vector<char> blanked;
   auto blanked_out = zpp::bits::out{blanked, zpp::bits::no_size{}};
   BOOST_REQUIRE(blanked_out(uic) == zpp::bits::errc{});

   uic.signature = chars_from_hex(vector.signature_hex);
   std::vector<char> full;
   auto full_out = zpp::bits::out{full, zpp::bits::no_size{}};
   BOOST_REQUIRE(full_out(uic) == zpp::bits::errc{});
   return {std::move(blanked), std::move(full)};
}

/** Decode host bytes with CDT zpp and return its blanked and full re-encodings. */
uic_encodings reencode_with_cdt(const std::vector<char>& full_bytes) {
   cdt_uic decoded;
   auto in = zpp::bits::in{
      std::span{full_bytes.data(), full_bytes.size()}, zpp::bits::no_size{}};
   BOOST_REQUIRE(in(decoded) == zpp::bits::errc{});

   const auto signature = decoded.signature;
   decoded.signature.clear();
   std::vector<char> blanked;
   auto blanked_out = zpp::bits::out{blanked, zpp::bits::no_size{}};
   BOOST_REQUIRE(blanked_out(decoded) == zpp::bits::errc{});

   decoded.signature = signature;
   std::vector<char> full;
   auto full_out = zpp::bits::out{full, zpp::bits::no_size{}};
   BOOST_REQUIRE(full_out(decoded) == zpp::bits::errc{});
   return {std::move(blanked), std::move(full)};
}

/** Construct one exact non-default nested shape used by the underwriter daemon. */
host_uic production_uic(sysio::opp::types::ChainKind kind,
                        size_t caller_address_size,
                        char caller_address_byte = '\x01',
                        uint64_t request_id = 42,
                        uint64_t token_code = 11,
                        uint64_t chain_code = 12,
                        uint64_t reserve_code = 13) {
   host_uic uic;
   uic.mutable_uw_account()->set_name("underwriter");
   uic.mutable_uw_ext_chain_addr()->set_kind(kind);
   uic.mutable_uw_ext_chain_addr()->set_address(
      std::string(caller_address_size, caller_address_byte));
   uic.set_uw_request_id(request_id);
   uic.set_token_code(token_code);
   uic.set_chain_code(chain_code);
   uic.set_reserve_code(reserve_code);
   return uic;
}

/** Sign one host-generated UIC and capture the host blanked/full bytes. */
uic_encodings sign_host_uic(host_uic uic,
                            fc::crypto::private_key::key_type key_type) {
   uic.clear_signature();
   auto blanked = serialize_host_uic(uic);
   const auto digest = fc::sha256::hash(blanked.data(), blanked.size());
   const auto packed = fc::raw::pack(
      fc::crypto::private_key::generate(key_type).sign(digest));
   uic.set_signature(packed.data(), packed.size());
   return {std::move(blanked), serialize_host_uic(uic)};
}

/** Assert the expected host/CDT relationship for both load-bearing byte sequences. */
void check_cross_generator_shape(std::string_view label,
                                 const uic_encodings& host,
                                 bool blanked_equal,
                                 bool full_equal) {
   const auto cdt = reencode_with_cdt(host.full);
   BOOST_TEST_CONTEXT(label) {
      BOOST_CHECK_EQUAL(cdt.blanked == host.blanked, blanked_equal);
      BOOST_CHECK_EQUAL(cdt.full == host.full, full_equal);
   }
}

} // namespace

BOOST_AUTO_TEST_SUITE(opp_specs)

// This is the pre-enforcement WIRE-291 compatibility gate. The daemon hashes
// host-protobuf bytes while the depot hashes CDT zpp bytes, and strict depot
// canonicality additionally compares the complete signed payload. Every
// production signature variant must therefore agree on both byte sequences.
BOOST_AUTO_TEST_CASE(uic_host_and_cdt_generators_agree_on_production_shapes) {
   using key_type = fc::crypto::private_key::key_type;
   const std::array variants{
      key_type::k1,
      key_type::r1,
      key_type::em,
      key_type::ed,
   };

   for (const auto variant : variants) {
      for (const auto [kind, caller_address_size] : std::array{
              std::pair{sysio::opp::types::CHAIN_KIND_EVM, size_t{20}},
              std::pair{sysio::opp::types::CHAIN_KIND_SVM, size_t{32}},
           }) {
         const auto host = sign_host_uic(
            production_uic(kind, caller_address_size), variant);
         check_cross_generator_shape(
            "production signature/outpost variant", host, true, true);
      }
   }
}

// These sealed records are the protocol contract shared by the host/CDT,
// Solidity, and Rust production encoders. The generated TypeScript model is a
// convenience/reference materializer only. Construction and decode/re-encode
// are deliberately tested as separate properties.
BOOST_AUTO_TEST_CASE(uic_host_and_cdt_match_fixed_outpost_vectors) {
   const std::array vectors{
      uic_golden_vector{
         "EVM", "wire291-uic-evm-v1", uic_schema_revision,
         "69391877d7b4a8d053c5ab81fb88a2fed91edd4a550ea8135b52fbc4a275db3d",
         sysio::opp::types::CHAIN_KIND_EVM,
         cdt_sysio::opp::types::CHAIN_KIND_EVM,
         "9965507d1a55bcc2695c58ba16fb37d819b0a4dc",
         "PUB_K1_8mycTeevEWPgQhjMM9QcLyGmpc93pLQrhD9hJVmMiAP9WH6jd8",
         "0a0d0a0b756e6465727772697465721218080212149965507d1a55bcc2695c58b"
         "a16fb37d819b0a4dc18ac0230ad0238ae0240af02",
         "22734feb95901bb659e762238405ca8c9872e93b14a77d0a264de642c89f3c1b",
         "0020dca5027647a8282688d535cc783057ece6a950450c63cefafa007ac95688e"
         "e2608525965898277231aee8ee079065a8da3cd886a34e2326ae6143b074be7"
         "87fb",
         "0a0d0a0b756e6465727772697465721218080212149965507d1a55bcc2695c58b"
         "a16fb37d819b0a4dc18ac022a420020dca5027647a8282688d535cc783057ece"
         "6a950450c63cefafa007ac95688ee2608525965898277231aee8ee079065a8da3"
         "cd886a34e2326ae6143b074be787fb30ad0238ae0240af02",
         "b4c4a61bd106dfcd64372481d5678bfc86a9fdadf18e942b4e5599f6f57b6e10"},
      uic_golden_vector{
         "SVM", "wire291-uic-svm-v1", uic_schema_revision,
         "cecd8797cec145808bdb4dd6dbc29fdb3b950e320f3ee5e7a1ab1713843c9a93",
         sysio::opp::types::CHAIN_KIND_SVM,
         cdt_sysio::opp::types::CHAIN_KIND_SVM,
         "17cb79fb2b4120f2b1ec65e4198d6e08b28e813feb01e4a400839b85e18080ce",
         "PUB_ED_B7EZQq7AV8FnEEvEZUiFDPbfUWNE12XZv6wvn9ubFUJV",
         "0a0d0a0b756e64657277726974657212240803122017cb79fb2b4120f2b1ec65"
         "e4198d6e08b28e813feb01e4a400839b85e18080ce18ac0230ad0238ae0240af"
         "02",
         "4bcefe9b2968297dae12cb4ef6d9a7aeebcc9300170164a65d33d62f1678440c",
         "04962daf636c40b2b7236987387b2d04b2cafb7dd1da8711ae48afb0e3d3baad"
         "12311911aecac80f2a05cb4f364a35fe1437c74aeee5b7d51b448ecc758e374"
         "091b0a6817ce27ea8df7adc5491f116c0248d7470a819e22d241e5768151371"
         "be0e",
         "0a0d0a0b756e64657277726974657212240803122017cb79fb2b4120f2b1ec65"
         "e4198d6e08b28e813feb01e4a400839b85e18080ce18ac022a6104962daf636c"
         "40b2b7236987387b2d04b2cafb7dd1da8711ae48afb0e3d3baad12311911aec"
         "ac80f2a05cb4f364a35fe1437c74aeee5b7d51b448ecc758e374091b0a6817c"
         "e27ea8df7adc5491f116c0248d7470a819e22d241e5768151371be0e30ad023"
         "8ae0240af02",
         "a099702eec067e6daefa7129a03048294b695fc8e60d858b3719584e2d47b1c4"},
   };

   for (const auto& vector : vectors) {
      BOOST_TEST_CONTEXT(vector.label) {
         BOOST_CHECK_EQUAL(vector.schema_revision, uic_schema_revision);
         BOOST_CHECK_EQUAL(sealed_record_checksum(vector),
                           vector.record_checksum_hex);
         const auto expected_blanked = chars_from_hex(vector.blanked_hex);
         const auto expected_signature = chars_from_hex(vector.signature_hex);
         const auto expected_full = chars_from_hex(vector.full_hex);
         const auto expected_address = chars_from_hex(vector.address_hex);

         auto unsigned_host = production_uic(
            vector.host_kind, expected_address.size(), 0,
            300, 301, 302, 303);
         unsigned_host.mutable_uw_ext_chain_addr()->set_address(
            expected_address.data(), expected_address.size());
         const auto host_blanked = serialize_host_uic(unsigned_host);
         BOOST_CHECK_EQUAL_COLLECTIONS(host_blanked.begin(), host_blanked.end(),
                                       expected_blanked.begin(), expected_blanked.end());
         const auto digest = fc::sha256::hash(host_blanked.data(), host_blanked.size());
         BOOST_CHECK_EQUAL(digest.str(), vector.signing_digest_hex);

         const auto expected_public_key = fc::crypto::public_key::from_string(
            std::string{vector.public_key});
         const auto signature = fc::raw::unpack<fc::crypto::signature>(expected_signature);
         BOOST_CHECK(fc::crypto::public_key::recover(signature, digest)
                     == expected_public_key);

         unsigned_host.set_signature(expected_signature.data(), expected_signature.size());
         const auto host_full = serialize_host_uic(unsigned_host);
         BOOST_CHECK_EQUAL_COLLECTIONS(host_full.begin(), host_full.end(),
                                       expected_full.begin(), expected_full.end());
         BOOST_CHECK_EQUAL(fc::sha256::hash(host_full.data(), host_full.size()).str(),
                           vector.full_sha256_hex);

         const auto cdt = encode_cdt_uic(vector);
         BOOST_CHECK_EQUAL_COLLECTIONS(cdt.blanked.begin(), cdt.blanked.end(),
                                       expected_blanked.begin(), expected_blanked.end());
         BOOST_CHECK_EQUAL_COLLECTIONS(cdt.full.begin(), cdt.full.end(),
                                       expected_full.begin(), expected_full.end());
         const auto reencoded = reencode_with_cdt(expected_full);
         BOOST_CHECK(reencoded.blanked == expected_blanked);
         BOOST_CHECK(reencoded.full == expected_full);
      }
   }
}

// Proto3-default shapes are characterized explicitly. The absent/default
// nested-address forms are not valid UIC construction shapes because host and
// CDT serialization diverge. A nonzero EVM kind with an empty address happens
// to agree between host and CDT, but is not a four-encoder production shape:
// the generated outpost codecs emit the empty bytes field. The daemon therefore
// supplies both a nonzero kind and the concrete authenticated caller address.
// A zero top-level scalar also diverges and is invalid at this boundary.
BOOST_AUTO_TEST_CASE(uic_host_and_cdt_generators_characterize_default_shapes) {
   using key_type = fc::crypto::private_key::key_type;

   auto absent_nested = production_uic(
      sysio::opp::types::CHAIN_KIND_EVM, 20);
   absent_nested.clear_uw_ext_chain_addr();
   check_cross_generator_shape(
      "absent uw_ext_chain_addr",
      sign_host_uic(std::move(absent_nested), key_type::k1), false, false);

   auto default_nested = production_uic(
      sysio::opp::types::CHAIN_KIND_EVM, 20);
   default_nested.mutable_uw_ext_chain_addr()->set_kind(
      sysio::opp::types::CHAIN_KIND_UNKNOWN);
   check_cross_generator_shape(
      "present default-valued uw_ext_chain_addr",
      sign_host_uic(std::move(default_nested), key_type::k1), false, false);

   auto evm_with_empty_address = production_uic(
      sysio::opp::types::CHAIN_KIND_EVM, 20);
   evm_with_empty_address.mutable_uw_ext_chain_addr()->clear_address();
   check_cross_generator_shape(
      "EVM kind with empty caller address",
      sign_host_uic(std::move(evm_with_empty_address), key_type::k1), true, true);

   auto zero_scalar = production_uic(
      sysio::opp::types::CHAIN_KIND_EVM, 20);
   zero_scalar.set_token_code(0);
   check_cross_generator_shape(
      "zero top-level scalar",
      sign_host_uic(std::move(zero_scalar), key_type::k1), false, false);
}

BOOST_AUTO_TEST_SUITE_END()

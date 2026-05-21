/**
 * Unit tests for the AWS KMS signature provider parser.
 *
 * These cases cover only the offline parse path -- `parse_kms_spec` does no
 * network I/O and constructs no `KMSClient`, so the suite runs without AWS
 * credentials and without an internet connection. The one end-to-end signing
 * test in this file (`kms_live_sign_round_trip`) is gated on the
 * `KMS_LIVE_SPEC` / `KMS_LIVE_PUBKEY` environment variables and is skipped
 * unless both are set.
 */

#include <boost/test/unit_test.hpp>

#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/types.hpp>

#include <fc/crypto/elliptic_em.hpp>
#include <fc/crypto/keccak256.hpp>
#include <fc/crypto/key_serdes.hpp>
#include <fc/crypto/private_key.hpp>
#include <fc/crypto/public_key.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <aws/kms/KMSErrors.h>

#include "../src/kms_signature_provider.hpp"

using sysio::sigprov::kms::kms_key_ref;
using sysio::sigprov::kms::parse_kms_spec;

namespace {

/**
 * Test helper: encode a 64-byte (r||s) compact signature as ASN.1 DER.
 *
 *   30 <total-len> 02 <r-len> <r-bytes> 02 <s-len> <s-bytes>
 *
 * `<r-bytes>` and `<s-bytes>` strip leading zero bytes; if the resulting
 * MSB is set, a single 0x00 is prepended (ASN.1 INTEGER is signed). This
 * matches what KMS / OpenSSL emit, and lets us round-trip through the
 * production `der_to_compact` decoder without involving AWS.
 */
std::vector<unsigned char> compact_to_der(const std::array<unsigned char, 64>& compact) {
   const auto encode_int = [](std::span<const unsigned char> bytes) {
      // Strip leading zero bytes (ASN.1 INTEGER must be minimal).
      std::size_t start = 0;
      while (start + 1 < bytes.size() && bytes[start] == 0) {
         ++start;
      }
      std::vector<unsigned char> out;
      out.reserve(bytes.size() - start + 1);
      // Pad with 0x00 if MSB is set so the INTEGER stays positive.
      if ((bytes[start] & 0x80) != 0) {
         out.push_back(0x00);
      }
      out.insert(out.end(), bytes.begin() + start, bytes.end());
      return out;
   };

   const auto r_int = encode_int(std::span<const unsigned char>(compact.data(), 32));
   const auto s_int = encode_int(std::span<const unsigned char>(compact.data() + 32, 32));

   const std::size_t inner_len = 2 + r_int.size() + 2 + s_int.size();
   std::vector<unsigned char> der;
   der.reserve(2 + inner_len);
   der.push_back(0x30);
   der.push_back(static_cast<unsigned char>(inner_len));
   der.push_back(0x02);
   der.push_back(static_cast<unsigned char>(r_int.size()));
   der.insert(der.end(), r_int.begin(), r_int.end());
   der.push_back(0x02);
   der.push_back(static_cast<unsigned char>(s_int.size()));
   der.insert(der.end(), s_int.begin(), s_int.end());
   return der;
}

/**
 * Test helper: wrap a 65-byte uncompressed secp256k1 EC point (0x04 || X || Y)
 * in a DER X.509 SubjectPublicKeyInfo, byte-for-byte identical to what AWS KMS
 * `GetPublicKey` returns for an `ECC_SECG_P256K1` signing key. Lets us exercise
 * the production `spki_der_to_public_key` decoder without calling AWS.
 *
 * The 23-byte prefix is fixed because every field length is small enough for
 * single-byte DER length encoding and the algorithm identifiers are constant:
 *
 *   SEQUENCE (0x56) {
 *      SEQUENCE (0x10) { OID id-ecPublicKey, OID secp256k1 }
 *      BIT STRING (0x42) { 00 <65-byte point> }
 *   }
 */
std::vector<unsigned char> point_to_spki_der(
   const fc::em::public_key_data_uncompressed& point) {
   static constexpr std::array<unsigned char, 23> spki_prefix = {
      0x30, 0x56, 0x30, 0x10, 0x06, 0x07, 0x2A, 0x86,
      0x48, 0xCE, 0x3D, 0x02, 0x01, 0x06, 0x05, 0x2B,
      0x81, 0x04, 0x00, 0x0A, 0x03, 0x42, 0x00};
   std::vector<unsigned char> der(spki_prefix.begin(), spki_prefix.end());
   for (const char c : point) {
      der.push_back(static_cast<unsigned char>(c));
   }
   return der;
}

/// Curve order N for secp256k1 (big-endian). Used to construct deliberately
/// high-S signatures from a known low-S form by computing s' = N - s.
constexpr std::array<unsigned char, 32> secp256k1_curve_order = {
   0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
   0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE,
   0xBA, 0xAE, 0xDC, 0xE6, 0xAF, 0x48, 0xA0, 0x3B,
   0xBF, 0xD2, 0x5E, 0x8C, 0xD0, 0x36, 0x41, 0x41,
};

/// Big-endian 256-bit subtraction `s_high = N - s_low`. Operates on the
/// `s` half (bytes 32..63) of a compact signature.
void make_high_s(std::array<unsigned char, 64>& compact) {
   int borrow = 0;
   for (int i = 31; i >= 0; --i) {
      int diff = static_cast<int>(secp256k1_curve_order[i])
               - static_cast<int>(compact[32 + i])
               - borrow;
      if (diff < 0) {
         diff += 256;
         borrow = 1;
      } else {
         borrow = 0;
      }
      compact[32 + i] = static_cast<unsigned char>(diff);
   }
}

/// Take the first 64 bytes (r || s) of a 65-byte compact_signature.
std::array<unsigned char, 64> drop_v(const fc::em::compact_signature& sig) {
   std::array<unsigned char, 64> rs{};
   std::copy_n(sig.begin(), 64, rs.begin());
   return rs;
}

} // anonymous namespace

BOOST_AUTO_TEST_SUITE(kms_signature_provider_tests)

BOOST_AUTO_TEST_CASE(parse_kms_spec_arn_key) {
   const auto ref = parse_kms_spec("arn:aws:kms:us-east-1:111122223333:key/1234abcd-12ab-34cd-56ef-1234567890ab");
   BOOST_CHECK_EQUAL(ref.region, "us-east-1");
   BOOST_CHECK_EQUAL(ref.key_id, "key/1234abcd-12ab-34cd-56ef-1234567890ab");
}

BOOST_AUTO_TEST_CASE(parse_kms_spec_arn_alias) {
   const auto ref = parse_kms_spec("arn:aws:kms:eu-west-2:111122223333:alias/wire-cranker-eth-01");
   BOOST_CHECK_EQUAL(ref.region, "eu-west-2");
   BOOST_CHECK_EQUAL(ref.key_id, "alias/wire-cranker-eth-01");
}

BOOST_AUTO_TEST_CASE(parse_kms_spec_shorthand_uuid) {
   const auto ref = parse_kms_spec("us-east-1:1234abcd-12ab-34cd-56ef-1234567890ab");
   BOOST_CHECK_EQUAL(ref.region, "us-east-1");
   BOOST_CHECK_EQUAL(ref.key_id, "1234abcd-12ab-34cd-56ef-1234567890ab");
}

BOOST_AUTO_TEST_CASE(parse_kms_spec_shorthand_alias) {
   const auto ref = parse_kms_spec("us-east-1:alias/wire-cranker-eth-01");
   BOOST_CHECK_EQUAL(ref.region, "us-east-1");
   BOOST_CHECK_EQUAL(ref.key_id, "alias/wire-cranker-eth-01");
}

BOOST_AUTO_TEST_CASE(parse_kms_spec_rejects_empty) {
   BOOST_CHECK_THROW(parse_kms_spec(""), sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(parse_kms_spec_rejects_no_region_no_arn) {
   // Bare key id -- ambiguous about which region the key lives in. Must throw.
   BOOST_CHECK_THROW(parse_kms_spec("1234abcd-12ab-34cd-56ef-1234567890ab"),
                     sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(parse_kms_spec_rejects_arn_missing_tail) {
   // ARN truncated before the key/alias portion (trailing colon, empty tail).
   BOOST_CHECK_THROW(parse_kms_spec("arn:aws:kms:us-east-1:111122223333:"),
                     sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(parse_kms_spec_rejects_arn_empty_account) {
   // A stray colon collapsed the account-id segment to empty. The six segment
   // count still holds, so this is caught by the explicit account check, not
   // the segment-count assertion.
   BOOST_CHECK_THROW(parse_kms_spec("arn:aws:kms:us-east-1::key/abc"),
                     sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(parse_kms_spec_rejects_arn_empty_region) {
   // Likewise for an empty region segment.
   BOOST_CHECK_THROW(parse_kms_spec("arn:aws:kms::111122223333:key/abc"),
                     sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(parse_kms_spec_rejects_arn_bad_tail_prefix) {
   // ARN tail that doesn't start with `key/` or `alias/` -- KMS rejects this
   // server-side, but we can fail loud at parse time.
   BOOST_CHECK_THROW(parse_kms_spec("arn:aws:kms:us-east-1:111122223333:foo/bar"),
                     sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(parse_kms_spec_rejects_arn_bare_alias_prefix) {
   // `alias/` with no name attached.
   BOOST_CHECK_THROW(parse_kms_spec("arn:aws:kms:us-east-1:111122223333:alias/"),
                     sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(parse_kms_spec_rejects_arn_too_few_segments) {
   // Missing both account and tail.
   BOOST_CHECK_THROW(parse_kms_spec("arn:aws:kms:us-east-1"),
                     sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(parse_kms_spec_rejects_china_partition) {
   // `aws-cn` partition is out of scope. Must fail loudly at parse time
   // rather than mis-parsing region as "arn" and failing later at first sign.
   BOOST_CHECK_THROW(parse_kms_spec("arn:aws-cn:kms:cn-north-1:111122223333:key/abc"),
                     sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(parse_kms_spec_rejects_govcloud_partition) {
   // `aws-us-gov` partition is likewise out of scope.
   BOOST_CHECK_THROW(parse_kms_spec("arn:aws-us-gov:kms:us-gov-west-1:111122223333:key/abc"),
                     sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(parse_kms_spec_rejects_uppercase_arn) {
   // A mis-cased ARN is not the supported lowercase `arn:aws:kms:` form;
   // recognised case-insensitively as an ARN so it fails loudly here.
   BOOST_CHECK_THROW(parse_kms_spec("ARN:AWS:KMS:us-east-1:111122223333:key/abc"),
                     sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(parse_kms_spec_rejects_typoed_service) {
   // `ksm` instead of `kms` -- would otherwise mis-parse region as "arn".
   BOOST_CHECK_THROW(parse_kms_spec("arn:aws:ksm:us-east-1:111122223333:key/abc"),
                     sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(parse_kms_spec_rejects_bare_arn_prefix) {
   // Just `arn:` with nothing after it is still an ARN-shaped spec, not
   // shorthand with a region literally named "arn".
   BOOST_CHECK_THROW(parse_kms_spec("arn:"), sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(parse_kms_spec_rejects_shorthand_empty_region) {
   // Leading colon means no region.
   BOOST_CHECK_THROW(parse_kms_spec(":alias/wire-cranker-eth-01"),
                     sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(parse_kms_spec_rejects_shorthand_empty_key_id) {
   // Trailing colon means no key id.
   BOOST_CHECK_THROW(parse_kms_spec("us-east-1:"),
                     sysio::chain::plugin_config_exception);
}

// ---------------------------------------------------------------------------
// DER -> raw + low-S + v-recovery helpers
//
// These tests synthesise the inputs that AWS KMS would normally produce by
// signing locally with `fc::em::private_key::sign_compact` and re-encoding
// the resulting (r, s) as ASN.1 DER via `compact_to_der` above. No AWS
// credentials, no network, no `KMSClient`.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(der_to_compact_round_trips_random_signature) {
   // ECDSA `r` is uniform over [1, N-1]; ~half of generated signatures will
   // have the high bit of `r` set, exercising the leading-zero pad path in
   // both `compact_to_der` and `der_to_compact`. Drive several rounds so we
   // hit both branches deterministically over the suite's lifetime.
   for (int i = 0; i < 8; ++i) {
      const auto priv   = fc::em::private_key::generate();
      const auto digest = fc::crypto::keccak256::hash(std::string{"round-trip"});
      const auto sig    = priv.sign_compact(digest);
      const auto rs     = drop_v(sig);

      const auto der     = compact_to_der(rs);
      const auto decoded = sysio::sigprov::kms::der_to_compact(std::span<const unsigned char>(der));
      BOOST_CHECK(decoded == rs);
   }
}

BOOST_AUTO_TEST_CASE(der_to_compact_rejects_garbage) {
   const std::array<unsigned char, 4> garbage{0xde, 0xad, 0xbe, 0xef};
   BOOST_CHECK_THROW(sysio::sigprov::kms::der_to_compact(std::span<const unsigned char>(garbage)),
                     sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(normalise_low_s_passes_through_already_low) {
   // libsecp256k1's signing primitive always returns canonical (low-S) form,
   // so the freshly produced signature must not be touched.
   const auto priv   = fc::em::private_key::generate();
   const auto digest = fc::crypto::keccak256::hash(std::string{"already-low"});
   const auto sig    = priv.sign_compact(digest);

   auto rs = drop_v(sig);
   const auto rs_orig = rs;
   const bool was_high = sysio::sigprov::kms::normalise_low_s(rs);
   BOOST_CHECK(!was_high);
   BOOST_CHECK(rs == rs_orig);
}

BOOST_AUTO_TEST_CASE(normalise_low_s_flips_high_to_low) {
   // Construct an artificial high-S sig by setting s := N - s on a known
   // low-S signature. The normaliser must flip it back identically.
   const auto priv   = fc::em::private_key::generate();
   const auto digest = fc::crypto::keccak256::hash(std::string{"flip-to-low"});
   const auto sig    = priv.sign_compact(digest);
   const auto rs_low = drop_v(sig);

   auto rs_high = rs_low;
   make_high_s(rs_high);
   BOOST_REQUIRE(rs_high != rs_low); // sanity: high-S really differs

   const bool was_high = sysio::sigprov::kms::normalise_low_s(rs_high);
   BOOST_CHECK(was_high);
   BOOST_CHECK(rs_high == rs_low);
}

BOOST_AUTO_TEST_CASE(recover_v_finds_correct_parity) {
   const auto priv   = fc::em::private_key::generate();
   const auto pub    = priv.get_public_key();
   const auto digest = fc::crypto::keccak256::hash(std::string{"recover-v"});
   const auto sig    = priv.sign_compact(digest);

   const auto rs_only      = drop_v(sig);
   const unsigned char orig_v = static_cast<unsigned char>(sig[64] - 27);
   const unsigned char recovered_v =
      sysio::sigprov::kms::recover_v(rs_only, digest.to_uint8_span(), pub);
   BOOST_CHECK_EQUAL(static_cast<int>(recovered_v), static_cast<int>(orig_v));
}

BOOST_AUTO_TEST_CASE(recover_v_throws_on_wrong_pubkey) {
   const auto priv      = fc::em::private_key::generate();
   const auto wrong_pub = fc::em::private_key::generate().get_public_key();
   const auto digest    = fc::crypto::keccak256::hash(std::string{"wrong-pub"});
   const auto sig       = priv.sign_compact(digest);

   const auto rs_only = drop_v(sig);
   BOOST_CHECK_THROW(sysio::sigprov::kms::recover_v(rs_only, digest.to_uint8_span(), wrong_pub),
                     sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(der_to_eth_signature_end_to_end_matches_local_sign) {
   // Full pipeline: pretend KMS handed us back a DER encoding of a known
   // low-S signature, then check the helper reconstructs the exact 65-byte
   // form we would have got by signing locally.
   const auto priv   = fc::em::private_key::generate();
   const auto pub    = priv.get_public_key();
   const auto digest = fc::crypto::keccak256::hash(std::string{"end-to-end"});
   const auto local  = priv.sign_compact(digest);

   const auto rs  = drop_v(local);
   const auto der = compact_to_der(rs);
   const auto out = sysio::sigprov::kms::der_to_eth_signature(
      std::span<const unsigned char>(der), digest.to_uint8_span(), pub);

   BOOST_CHECK(out == local);
}

BOOST_AUTO_TEST_CASE(der_to_eth_signature_normalises_high_s_input) {
   // KMS does not enforce low-S, so a real-world DER blob may have high-S.
   // The helper must normalise and still recover a valid v.
   const auto priv   = fc::em::private_key::generate();
   const auto pub    = priv.get_public_key();
   const auto digest = fc::crypto::keccak256::hash(std::string{"high-s-from-kms"});
   const auto local  = priv.sign_compact(digest);

   auto rs = drop_v(local);
   make_high_s(rs);
   const auto der_high = compact_to_der(rs);
   const auto out      = sysio::sigprov::kms::der_to_eth_signature(
      std::span<const unsigned char>(der_high), digest.to_uint8_span(), pub);

   // Output must be canonical (low-S) and match the locally signed form.
   BOOST_CHECK(out == local);
}

// ---------------------------------------------------------------------------
// spki_der_to_public_key -- KMS public-key pinning decoder
//
// These tests synthesise the X.509 SubjectPublicKeyInfo that AWS KMS
// `GetPublicKey` returns by wrapping a locally generated EC point with
// `point_to_spki_der` above. No AWS credentials, no network.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(spki_der_to_public_key_round_trips) {
   // A well-formed secp256k1 SPKI must decode back to the exact key it wraps.
   for (int i = 0; i < 4; ++i) {
      const auto priv = fc::em::private_key::generate();
      const auto pub  = priv.get_public_key();

      const auto der     = point_to_spki_der(pub.serialize_uncompressed());
      const auto decoded = sysio::sigprov::kms::spki_der_to_public_key(
         std::span<const unsigned char>(der));
      BOOST_CHECK(decoded == pub);
   }
}

BOOST_AUTO_TEST_CASE(spki_der_to_public_key_rejects_garbage) {
   // Non-DER bytes must fail the structural walk, not be silently accepted.
   const std::array<unsigned char, 4> garbage{0xde, 0xad, 0xbe, 0xef};
   BOOST_CHECK_THROW(
      sysio::sigprov::kms::spki_der_to_public_key(std::span<const unsigned char>(garbage)),
      sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(spki_der_to_public_key_rejects_wrong_curve) {
   // Flip the final byte of the curve OID (0x0A -> 0x07): the structure stays
   // valid DER but the named curve is no longer secp256k1. An operator who
   // pointed the spec at a non-secp256k1 EC key must get a loud parse error.
   const auto priv = fc::em::private_key::generate();
   auto der = point_to_spki_der(priv.get_public_key().serialize_uncompressed());
   der[19] = 0x07; // index 19 = last octet of the secp256k1 curve OID body
   BOOST_CHECK_THROW(
      sysio::sigprov::kms::spki_der_to_public_key(std::span<const unsigned char>(der)),
      sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(spki_der_to_public_key_rejects_truncated) {
   // A buffer shorter than its declared DER lengths must fail, not over-read.
   const auto priv = fc::em::private_key::generate();
   auto der = point_to_spki_der(priv.get_public_key().serialize_uncompressed());
   der.resize(der.size() - 10); // chop the tail of the EC point
   BOOST_CHECK_THROW(
      sysio::sigprov::kms::spki_der_to_public_key(std::span<const unsigned char>(der)),
      sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(spki_der_to_public_key_rejects_off_curve_point) {
   // Structurally valid SPKI whose 65-byte point (0x04 followed by all zeros)
   // is not on the secp256k1 curve. libsecp256k1 rejects it; the helper must
   // surface that as the module's standard plugin_config_exception.
   fc::em::public_key_data_uncompressed off_curve{};
   off_curve[0] = 0x04;
   const auto der = point_to_spki_der(off_curve);
   BOOST_CHECK_THROW(
      sysio::sigprov::kms::spki_der_to_public_key(std::span<const unsigned char>(der)),
      sysio::chain::plugin_config_exception);
}

// ---------------------------------------------------------------------------
// KMSClient cache + AWS SDK lifecycle
//
// `get_kms_client` is offline: it only touches the AWS SDK to construct
// `Aws::KMS::KMSClient`, which does not resolve credentials or open
// connections at construction time. These tests therefore run without AWS
// creds.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(get_kms_client_caches_per_region) {
   const auto a1 = sysio::sigprov::kms::get_kms_client("us-east-1");
   const auto a2 = sysio::sigprov::kms::get_kms_client("us-east-1");
   BOOST_REQUIRE(a1);
   BOOST_REQUIRE(a2);
   BOOST_CHECK_EQUAL(a1.get(), a2.get());
}

BOOST_AUTO_TEST_CASE(get_kms_client_distinct_per_region) {
   const auto east = sysio::sigprov::kms::get_kms_client("us-east-1");
   const auto west = sysio::sigprov::kms::get_kms_client("eu-west-2");
   BOOST_REQUIRE(east);
   BOOST_REQUIRE(west);
   BOOST_CHECK_NE(east.get(), west.get());
}

BOOST_AUTO_TEST_CASE(get_kms_client_rejects_empty_region) {
   BOOST_CHECK_THROW(sysio::sigprov::kms::get_kms_client(""),
                     sysio::chain::plugin_config_exception);
}

// ---------------------------------------------------------------------------
// make_kms_signature_provider
//
// Construction is offline (creates a closure capturing a cached KMSClient
// and the expected pubkey). Invocation issues a real KMS::Sign request and
// is therefore covered by env-gated live tests, not this suite.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(make_kms_signature_provider_returns_callable_for_ethereum) {
   const auto chain_pub =
      fc::crypto::private_key::generate(fc::crypto::private_key::key_type::em).get_public_key();

   const auto kms = sysio::sigprov::kms::make_kms_signature_provider(
      kms_key_ref{"us-east-1", "alias/wire-cranker-eth-01"},
      fc::crypto::chain_key_type_ethereum,
      chain_pub);

   // Both the signing closure and the startup probe must be set; we deliberately
   // invoke neither here (either would hit live KMS).
   BOOST_CHECK(static_cast<bool>(kms.sign));
   BOOST_CHECK(static_cast<bool>(kms.warm_up));
}

BOOST_AUTO_TEST_CASE(make_kms_signature_provider_rejects_wire_k1) {
   // Wire K1 (`chain_key_type_wire`) is also secp256k1, but its public-key
   // and signature shapes differ from Ethereum's, so it is not yet supported
   // -- fail loud rather than sign with the wrong format.
   const auto chain_pub =
      fc::crypto::private_key::generate(fc::crypto::private_key::key_type::k1).get_public_key();

   BOOST_CHECK_THROW(
      sysio::sigprov::kms::make_kms_signature_provider(
         kms_key_ref{"us-east-1", "alias/wire-k1"},
         fc::crypto::chain_key_type_wire,
         chain_pub),
      sysio::chain::pending_impl_exception);
}

BOOST_AUTO_TEST_CASE(make_kms_signature_provider_rejects_solana) {
   // Solana keys are Ed25519 -- KMS does not support that signing algorithm.
   const auto chain_pub =
      fc::crypto::private_key::generate(fc::crypto::private_key::key_type::ed).get_public_key();

   BOOST_CHECK_THROW(
      sysio::sigprov::kms::make_kms_signature_provider(
         kms_key_ref{"us-east-1", "alias/test"},
         fc::crypto::chain_key_type_solana,
         chain_pub),
      sysio::chain::pending_impl_exception);
}

// ---------------------------------------------------------------------------
// Live KMS round-trip -- env-gated, skipped in default CI.
//
// Set both env vars to exercise this case against a real AWS KMS key:
//   KMS_LIVE_SPEC    body of a `KMS:` spec, e.g. `us-east-1:alias/wire-ci-test`
//                    or `arn:aws:kms:us-east-1:111122223333:alias/wire-ci-test`
//   KMS_LIVE_PUBKEY  uncompressed secp256k1 public key hex (0x04 || X || Y),
//                    matching the public key held by the KMS key
//
// The case will:
//   1. Build a real `make_kms_signature_provider` closure.
//   2. Sign a fixed deterministic digest (so the test is replayable).
//   3. Recover the public key from the resulting Ethereum compact signature.
//   4. Assert the recovered key matches `KMS_LIVE_PUBKEY` byte-for-byte.
//
// Requires AWS credentials in the runner's environment (env, ~/.aws/, IRSA,
// or IMDS) with `kms:Sign` on the target key. KMS::Sign is billable, so this
// case is opt-in only.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(kms_live_sign_round_trip) {
   const auto* spec_env = std::getenv("KMS_LIVE_SPEC");
   const auto* pub_env  = std::getenv("KMS_LIVE_PUBKEY");
   if (!spec_env || !pub_env || *spec_env == '\0' || *pub_env == '\0') {
      BOOST_TEST_MESSAGE("KMS_LIVE_SPEC / KMS_LIVE_PUBKEY not set -- skipping live KMS test");
      return;
   }

   const std::string pub_hex{pub_env};
   const auto chain_pub =
      fc::crypto::from_native_string_to_public_key<fc::crypto::chain_key_type_ethereum>(pub_hex);
   const auto em_expected = chain_pub.get<fc::em::public_key_shim>().unwrapped();

   const auto ref = sysio::sigprov::kms::parse_kms_spec(spec_env);
   const auto kms = sysio::sigprov::kms::make_kms_signature_provider(
      ref, fc::crypto::chain_key_type_ethereum, chain_pub);

   // Exercise the startup probe: a GetPublicKey call plus the public-key pin,
   // with no signing. It must pass before we attempt a (billable) Sign -- and a
   // passing probe pre-pins the closure.
   BOOST_CHECK_NO_THROW(kms.warm_up());

   // Deterministic digest so the test is replayable and the AWS account audit
   // log entries are recognisable across runs.
   const auto keccak = fc::crypto::keccak256::hash(std::string{"wire-sysio kms live test 2026"});

   // chain::digest_type is fc::sha256 -- a 32-byte holder. We copy the
   // keccak256 bytes verbatim because KMS treats the message as opaque
   // when MessageType=DIGEST.
   sysio::chain::digest_type chain_digest;
   std::memcpy(chain_digest.data(), keccak.data(), 32);

   const auto sig = kms.sign(chain_digest);

   const auto& em_sig    = sig.get<fc::em::signature_shim>();
   const auto  recovered = em_sig.recover_eth(keccak).unwrapped();
   BOOST_CHECK(recovered == em_expected);
}

BOOST_AUTO_TEST_CASE(make_kms_signature_provider_rejects_pubkey_variant_mismatch) {
   // Caller declared key_type=ethereum but handed us a non-em pubkey. This
   // catches a misconfigured spec where `<key-type>` and `<public-key>`
   // disagree on the underlying curve.
   const auto wire_pub =
      fc::crypto::private_key::generate(fc::crypto::private_key::key_type::k1).get_public_key();

   BOOST_CHECK_THROW(
      sysio::sigprov::kms::make_kms_signature_provider(
         kms_key_ref{"us-east-1", "alias/test"},
         fc::crypto::chain_key_type_ethereum,
         wire_pub),
      sysio::chain::plugin_config_exception);
}

// ---------------------------------------------------------------------------
// throw_kms_error -- transient vs permanent classification
//
// `throw_kms_error` maps an AWS error onto two exception types using the SDK's
// own retryability classification. Constructing an `AWSError` is offline -- it
// is a plain value type, so these tests need no SDK init and no network.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(throw_kms_error_maps_retryable_to_transient) {
   // A throttling error is retryable -- the caller should back off and retry.
   const Aws::Client::AWSError<Aws::KMS::KMSErrors> err(
      Aws::KMS::KMSErrors::THROTTLING, "ThrottlingException", "Rate exceeded",
      /* isRetryable */ true);
   BOOST_CHECK_THROW(sysio::sigprov::kms::throw_kms_error("Sign", "alias/x", err),
                     sysio::chain::signing_transient_exception);
}

BOOST_AUTO_TEST_CASE(throw_kms_error_maps_non_retryable_to_config) {
   // Access-denied is permanent -- retrying will not help, the IAM grant is
   // missing. It must surface as a config exception, not a transient one.
   const Aws::Client::AWSError<Aws::KMS::KMSErrors> err(
      Aws::KMS::KMSErrors::ACCESS_DENIED, "AccessDeniedException",
      "not authorized to perform kms:Sign", /* isRetryable */ false);
   BOOST_CHECK_THROW(sysio::sigprov::kms::throw_kms_error("Sign", "alias/x", err),
                     sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(throw_kms_error_transient_is_not_a_config_exception) {
   // The transient and permanent classes must be siblings, not parent/child,
   // so a handler that catches only `plugin_config_exception` cannot silently
   // swallow a retryable error and defeat the caller's retry/backoff logic.
   const Aws::Client::AWSError<Aws::KMS::KMSErrors> err(
      Aws::KMS::KMSErrors::K_M_S_INTERNAL, "KMSInternalException",
      "internal service error", /* isRetryable */ true);
   bool caught_transient = false;
   try {
      sysio::sigprov::kms::throw_kms_error("GetPublicKey", "alias/x", err);
   } catch (const sysio::chain::plugin_config_exception&) {
      BOOST_FAIL("transient KMS error was caught as plugin_config_exception");
   } catch (const sysio::chain::signing_transient_exception&) {
      caught_transient = true;
   }
   BOOST_CHECK(caught_transient);
}

BOOST_AUTO_TEST_CASE(throw_kms_error_message_carries_enum_name) {
   // Regression guard for the magic_enum range. KMS service-specific error
   // codes (Aws::KMS::KMSErrors) begin at 129; magic_enum's default ceiling is
   // 128, so without MAGIC_ENUM_RANGE_MAX raised the enum-name field of the
   // diagnostic is blank for every KMS-specific error. DISABLED has value 141,
   // squarely in the formerly-dead range.
   const Aws::Client::AWSError<Aws::KMS::KMSErrors> err(
      Aws::KMS::KMSErrors::DISABLED, "DisabledException", "Key is disabled",
      /* isRetryable */ false);
   try {
      sysio::sigprov::kms::throw_kms_error("Sign", "alias/x", err);
      BOOST_FAIL("throw_kms_error did not throw");
   } catch (const fc::exception& e) {
      // "DISABLED" is the magic_enum::enum_name spelling. The wire name
      // ("DisabledException") and the human message ("Key is disabled") use
      // mixed / lower case, so the all-caps form appears only if enum_name
      // resolved -- asserting on it specifically pins the range fix.
      const auto detail = e.to_detail_string();
      BOOST_CHECK_MESSAGE(detail.find("DISABLED") != std::string::npos,
                          "KMS diagnostic is missing the KMSErrors enum name: " << detail);
   }
}

BOOST_AUTO_TEST_SUITE_END()

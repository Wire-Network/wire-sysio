/**
 * Unit tests for the AWS KMS signature provider parser.
 *
 * These cases cover only the offline parse path — `parse_kms_spec` does no
 * network I/O and constructs no `KMSClient`, so the suite runs without AWS
 * credentials and without an internet connection. End-to-end signing tests
 * live in a separate suite (gated behind `KMS_LIVE_TEST`, see
 * `KMS_SIGNING_DESIGN.md` §7).
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
   // Bare key id — ambiguous about which region the key lives in. Must throw.
   BOOST_CHECK_THROW(parse_kms_spec("1234abcd-12ab-34cd-56ef-1234567890ab"),
                     sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(parse_kms_spec_rejects_arn_missing_tail) {
   // ARN truncated before the key/alias portion (trailing colon, empty tail).
   BOOST_CHECK_THROW(parse_kms_spec("arn:aws:kms:us-east-1:111122223333:"),
                     sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(parse_kms_spec_rejects_arn_bad_tail_prefix) {
   // ARN tail that doesn't start with `key/` or `alias/` — KMS rejects this
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
// DER → raw + low-S + v-recovery helpers
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

   const auto sign = sysio::sigprov::kms::make_kms_signature_provider(
      kms_key_ref{"us-east-1", "alias/wire-cranker-eth-01"},
      fc::crypto::chain_key_type_ethereum,
      chain_pub);

   // Closure must be set; we deliberately do not invoke it (would hit live KMS).
   BOOST_CHECK(static_cast<bool>(sign));
}

BOOST_AUTO_TEST_CASE(make_kms_signature_provider_rejects_wire_k1) {
   // Wire K1 (`chain_key_type_wire`) is also secp256k1, but its public-key
   // and signature shapes differ from Ethereum's. Goal #2 of the design note
   // tracks adding it; until then, fail loud.
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
   // Solana keys are Ed25519 — KMS does not support that signing algorithm.
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
// Live KMS round-trip — env-gated, skipped in default CI.
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
      BOOST_TEST_MESSAGE("KMS_LIVE_SPEC / KMS_LIVE_PUBKEY not set — skipping live KMS test");
      return;
   }

   const std::string pub_hex{pub_env};
   const auto chain_pub =
      fc::crypto::from_native_string_to_public_key<fc::crypto::chain_key_type_ethereum>(pub_hex);
   const auto em_expected = chain_pub.get<fc::em::public_key_shim>().unwrapped();

   const auto ref  = sysio::sigprov::kms::parse_kms_spec(spec_env);
   const auto sign = sysio::sigprov::kms::make_kms_signature_provider(
      ref, fc::crypto::chain_key_type_ethereum, chain_pub);

   // Deterministic digest so the test is replayable and the AWS account audit
   // log entries are recognisable across runs.
   const auto keccak = fc::crypto::keccak256::hash(std::string{"wire-sysio kms live test 2026"});

   // chain::digest_type is fc::sha256 — a 32-byte holder. We copy the
   // keccak256 bytes verbatim because KMS treats the message as opaque
   // when MessageType=DIGEST.
   sysio::chain::digest_type chain_digest;
   std::memcpy(chain_digest.data(), keccak.data(), 32);

   const auto sig = sign(chain_digest);

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

BOOST_AUTO_TEST_SUITE_END()

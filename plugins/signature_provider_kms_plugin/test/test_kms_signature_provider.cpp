/**
 * Unit tests for the AWS KMS signature provider.
 *
 * Every case runs offline, without AWS credentials. The signing closure is driven through `local_key_kms_client`, a
 * `KMSClient` double that answers `GetPublicKey` / `Sign` from a local key. The one test against real KMS
 * (`kms_live_sign_round_trip`) is gated on the `KMS_LIVE_SPEC` / `KMS_LIVE_PUBKEY` environment variables and is
 * skipped unless both are set.
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
#include <tuple>
#include <vector>

#include <aws/core/auth/AWSCredentials.h>
#include <aws/core/utils/Array.h>
#include <aws/kms/KMSClient.h>
#include <aws/kms/KMSErrors.h>
#include <aws/kms/model/GetPublicKeyRequest.h>
#include <aws/kms/model/GetPublicKeyResult.h>
#include <aws/kms/model/MessageType.h>
#include <aws/kms/model/SignRequest.h>
#include <aws/kms/model/SignResult.h>
#include <aws/kms/model/SigningAlgorithmSpec.h>

#include <fc/crypto/elliptic.hpp>
#include <fc/crypto/sha256.hpp>
#include <fc/crypto/signature.hpp>
#include <fc/crypto/signature_provider.hpp>
#include <fc/crypto/signer.hpp>

#include <memory>
#include <optional>

#include <sysio/signature_provider_aws/aws_common.hpp>
#include <sysio/signature_provider_aws/test/env_fixtures.hpp>
#include <sysio/signature_provider_kms_plugin/kms_signature_provider.hpp>

using sysio::sigprov::kms::kms_key_ref;
using sysio::sigprov::kms::parse_kms_spec;
namespace sigaws = sysio::sigprov::aws;

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

/// Take the 64 bytes (r || s) after the header of a 65-byte K1 compact_signature.
std::array<unsigned char, 64> drop_k1_header(const fc::ecc::compact_signature& sig) {
   std::array<unsigned char, 64> rs{};
   std::copy_n(sig.begin() + 1, 64, rs.begin());
   return rs;
}

/// The `fc::crypto::public_key` a WIRE K1 spec names for `key`.
fc::crypto::public_key wire_k1_public_key(const fc::ecc::private_key& key) {
   return fc::crypto::public_key{
      fc::crypto::public_key::storage_type{fc::ecc::public_key_shim{key.get_public_key().serialize()}}};
}

/// The `fc::crypto::public_key` an Ethereum spec names for the same secp256k1 `key`.
fc::crypto::public_key ethereum_public_key(const fc::ecc::private_key& key) {
   return fc::crypto::public_key{
      fc::crypto::public_key::storage_type{fc::em::public_key_shim{key.get_public_key().serialize()}}};
}

/// `KMSClient` double answering from a local secp256k1 key the way KMS does: `GetPublicKey` returns the key's
/// SubjectPublicKeyInfo, `Sign` returns a DER signature over the message with no recovery id (high-S when
/// `high_s` is set, which KMS may emit). Records every call so tests can assert what reached "KMS".
class local_key_kms_client : public Aws::KMS::KMSClient {
public:
   /// Static credentials keep construction off the default provider chain, whose instance-metadata probe is slow.
   explicit local_key_kms_client(fc::ecc::private_key key)
      : Aws::KMS::KMSClient(Aws::Auth::AWSCredentials{"test-access-key", "test-secret-key"},
                            sigaws::make_client_configuration("us-east-1"))
      , _key(std::move(key)) {}

   Aws::KMS::Model::GetPublicKeyOutcome
   GetPublicKey(const Aws::KMS::Model::GetPublicKeyRequest& request) const override {
      ++get_public_key_calls;
      last_get_public_key_key_id = request.GetKeyId();
      if (fail_next_get_public_key) {
         fail_next_get_public_key = false;
         return Aws::KMS::KMSError{Aws::Client::AWSError<Aws::KMS::KMSErrors>(
            Aws::KMS::KMSErrors::THROTTLING, "ThrottlingException", "Rate exceeded", /* isRetryable */ true)};
      }
      const fc::em::public_key point{_key.get_public_key().serialize()};
      const std::vector<unsigned char> spki = point_to_spki_der(point.serialize_uncompressed());
      Aws::KMS::Model::GetPublicKeyResult result;
      result.SetPublicKey(Aws::Utils::ByteBuffer{spki.data(), spki.size()});
      return result;
   }

   Aws::KMS::Model::SignOutcome Sign(const Aws::KMS::Model::SignRequest& request) const override {
      ++sign_calls;
      last_sign_key_id  = request.GetKeyId();
      last_message_type = request.GetMessageType();
      last_algorithm    = request.GetSigningAlgorithm();
      const Aws::Utils::CryptoBuffer& message = request.GetMessage();
      last_message.assign(message.GetUnderlyingData(), message.GetUnderlyingData() + message.GetLength());
      if (sign_error) {
         return Aws::KMS::KMSError{*sign_error};
      }

      std::vector<unsigned char> der = sign_reply.value_or(std::vector<unsigned char>{});
      if (!sign_reply) {
         fc::sha256 digest;
         BOOST_REQUIRE_EQUAL(last_message.size(), digest.data_size());
         std::memcpy(digest.data(), last_message.data(), last_message.size());
         std::array<unsigned char, 64> rs = drop_k1_header(sign_with.value_or(_key).sign_compact(digest));
         if (high_s) {
            make_high_s(rs);
         }
         der = compact_to_der(rs);
      }
      Aws::KMS::Model::SignResult result;
      result.SetSignature(Aws::Utils::ByteBuffer{der.data(), der.size()});
      return result;
   }

   bool         high_s                   = false;
   mutable bool fail_next_get_public_key = false;
   /// `Sign` signs with this key instead of the one `GetPublicKey` reports, as after an alias is re-pointed.
   std::optional<fc::ecc::private_key> sign_with;
   /// `Sign` fails with this error.
   std::optional<Aws::Client::AWSError<Aws::KMS::KMSErrors>> sign_error;
   /// `Sign` returns these bytes as the signature.
   std::optional<std::vector<unsigned char>> sign_reply;

   mutable int                                   get_public_key_calls = 0;
   mutable int                                   sign_calls           = 0;
   mutable Aws::KMS::Model::MessageType          last_message_type    = Aws::KMS::Model::MessageType::NOT_SET;
   mutable Aws::KMS::Model::SigningAlgorithmSpec last_algorithm = Aws::KMS::Model::SigningAlgorithmSpec::NOT_SET;
   mutable std::vector<unsigned char>            last_message;
   mutable Aws::String                           last_get_public_key_key_id;
   mutable Aws::String                           last_sign_key_id;

private:
   fc::ecc::private_key _key;
};

/// Wrap a 32-byte Ed25519 public key in the 44-byte DER SubjectPublicKeyInfo KMS `GetPublicKey` returns for an
/// `ECC_NIST_EDWARDS25519` key (RFC 8410): SEQUENCE { SEQUENCE { OID 1.3.101.112 }, BIT STRING { 00 <key> } }.
std::vector<unsigned char> ed25519_to_spki_der(const fc::crypto::ed::public_key_shim& pub) {
   static constexpr std::array<unsigned char, 12> spki_prefix = {
      0x30, 0x2A, 0x30, 0x05, 0x06, 0x03, 0x2B, 0x65, 0x70, 0x03, 0x21, 0x00};
   std::vector<unsigned char> der(spki_prefix.begin(), spki_prefix.end());
   der.insert(der.end(), pub._data.begin(), pub._data.end());
   return der;
}

/// The detached 64-byte signature of an `ed::signature_shim` (it follows the embedded public key).
std::vector<unsigned char> detached(const fc::crypto::ed::signature_shim& sig) {
   return {sig._data.begin() + fc::crypto::ed::public_key_shim::size, sig._data.end()};
}

/// `KMSClient` double for an `ECC_NIST_EDWARDS25519` key: `GetPublicKey` returns the key's SPKI, `Sign` returns the
/// detached Ed25519 signature of the raw message, as KMS does for `ED25519_SHA_512`. Records every call.
class local_ed25519_kms_client : public Aws::KMS::KMSClient {
public:
   explicit local_ed25519_kms_client(fc::crypto::ed::private_key_shim key)
      : Aws::KMS::KMSClient(Aws::Auth::AWSCredentials{"test-access-key", "test-secret-key"},
                            sigaws::make_client_configuration("us-east-1"))
      , _key(key) {}

   Aws::KMS::Model::GetPublicKeyOutcome
   GetPublicKey(const Aws::KMS::Model::GetPublicKeyRequest& request) const override {
      ++get_public_key_calls;
      last_get_public_key_key_id = request.GetKeyId();
      const std::vector<unsigned char> spki = spki_reply.value_or(ed25519_to_spki_der(_key.get_public_key()));
      Aws::KMS::Model::GetPublicKeyResult result;
      result.SetPublicKey(Aws::Utils::ByteBuffer{spki.data(), spki.size()});
      return result;
   }

   Aws::KMS::Model::SignOutcome Sign(const Aws::KMS::Model::SignRequest& request) const override {
      ++sign_calls;
      last_sign_key_id  = request.GetKeyId();
      last_message_type = request.GetMessageType();
      last_algorithm    = request.GetSigningAlgorithm();
      const Aws::Utils::CryptoBuffer& message = request.GetMessage();
      last_message.assign(message.GetUnderlyingData(), message.GetUnderlyingData() + message.GetLength());
      if (sign_error) {
         return Aws::KMS::KMSError{*sign_error};
      }

      const fc::crypto::ed::private_key_shim& signer = sign_with.value_or(_key);
      const std::vector<unsigned char> sig =
         sign_reply.value_or(detached(signer.sign_raw(last_message.data(), last_message.size())));
      Aws::KMS::Model::SignResult result;
      result.SetSignature(Aws::Utils::ByteBuffer{sig.data(), sig.size()});
      return result;
   }

   /// `GetPublicKey` returns these bytes instead of the key's SPKI.
   std::optional<std::vector<unsigned char>> spki_reply;
   /// `Sign` signs with this key instead, as after an alias is re-pointed.
   std::optional<fc::crypto::ed::private_key_shim> sign_with;
   /// `Sign` returns these bytes as the signature.
   std::optional<std::vector<unsigned char>> sign_reply;
   /// `Sign` fails with this error.
   std::optional<Aws::Client::AWSError<Aws::KMS::KMSErrors>> sign_error;

   mutable int                                   get_public_key_calls = 0;
   mutable int                                   sign_calls           = 0;
   mutable Aws::KMS::Model::MessageType          last_message_type    = Aws::KMS::Model::MessageType::NOT_SET;
   mutable Aws::KMS::Model::SigningAlgorithmSpec last_algorithm = Aws::KMS::Model::SigningAlgorithmSpec::NOT_SET;
   mutable std::vector<unsigned char>            last_message;
   mutable Aws::String                           last_get_public_key_key_id;
   mutable Aws::String                           last_sign_key_id;

private:
   fc::crypto::ed::private_key_shim _key;
};

/// A fresh Ed25519 key, as the local key behind `local_ed25519_kms_client`.
fc::crypto::ed::private_key_shim generate_ed25519_key() {
   return fc::crypto::private_key::generate(fc::crypto::private_key::key_type::ed)
      .get<fc::crypto::ed::private_key_shim>();
}

/// The `fc::crypto::public_key` a Solana spec names for `key`.
fc::crypto::public_key solana_public_key(const fc::crypto::ed::private_key_shim& key) {
   return fc::crypto::public_key{fc::crypto::public_key::storage_type{key.get_public_key()}};
}

} // anonymous namespace

BOOST_AUTO_TEST_SUITE(kms_signature_provider_tests)

BOOST_AUTO_TEST_CASE(parse_kms_spec_arn_key) {
   // A key ARN must round-trip into `key_id` whole. AWS KMS accepts a key ARN
   // as `KeyId` but rejects the bare `key/<uuid>` tail, and keeping the full
   // ARN preserves the account id.
   constexpr auto arn =
      "arn:aws:kms:us-east-1:111122223333:key/1234abcd-12ab-34cd-56ef-1234567890ab";
   const auto ref = parse_kms_spec(arn);
   BOOST_CHECK_EQUAL(ref.region, "us-east-1");
   BOOST_CHECK_EQUAL(ref.key_id, arn);
}

BOOST_AUTO_TEST_CASE(parse_kms_spec_arn_alias) {
   // An alias ARN must likewise round-trip whole: stripping it to the bare
   // `alias/<name>` would drop the account id and could resolve a same-named
   // alias in the caller's own account.
   constexpr auto arn = "arn:aws:kms:eu-west-2:111122223333:alias/wire-cranker-eth-01";
   const auto ref = parse_kms_spec(arn);
   BOOST_CHECK_EQUAL(ref.region, "eu-west-2");
   BOOST_CHECK_EQUAL(ref.key_id, arn);
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

BOOST_AUTO_TEST_CASE(parse_kms_spec_regionless_uuid) {
   // No colon anywhere = the region-less form: `region` parses empty and the client region resolves from
   // the environment (env vars -> shared config -> IMDS on AWS compute) when the provider is created.
   const auto ref = parse_kms_spec("1234abcd-12ab-34cd-56ef-1234567890ab");
   BOOST_CHECK_EQUAL(ref.region, "");
   BOOST_CHECK_EQUAL(ref.key_id, "1234abcd-12ab-34cd-56ef-1234567890ab");
}

BOOST_AUTO_TEST_CASE(parse_kms_spec_regionless_alias) {
   const auto ref = parse_kms_spec("alias/wire-cranker-eth-01");
   BOOST_CHECK_EQUAL(ref.region, "");
   BOOST_CHECK_EQUAL(ref.key_id, "alias/wire-cranker-eth-01");
}

BOOST_AUTO_TEST_CASE(parse_kms_spec_regionless_multi_region_key) {
   // `mrk-` multi-Region key ids are two hyphen-separated segments -- never region-shaped.
   const auto ref = parse_kms_spec("mrk-1234abcd12ab34cd56ef1234567890ab");
   BOOST_CHECK_EQUAL(ref.region, "");
   BOOST_CHECK_EQUAL(ref.key_id, "mrk-1234abcd12ab34cd56ef1234567890ab");
}

BOOST_AUTO_TEST_CASE(parse_kms_spec_rejects_bare_region) {
   // A bare token shaped like a region is the likely "region without key id" typo, not a plausible KeyId --
   // fail at parse time with the fix rather than handing a region code to KMS.
   BOOST_CHECK_THROW(parse_kms_spec("us-east-1"), sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(parse_kms_spec_rejects_colon_with_non_region_lead) {
   // Key ids and aliases can never contain ':', so a colon-bearing spec whose leading token is not shaped
   // like a region has no valid reading -- neither an explicit-region form nor a region-less one.
   BOOST_CHECK_THROW(parse_kms_spec("alias/wire-cranker-eth-01:junk"),
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

BOOST_AUTO_TEST_CASE(der_to_compact_round_trips_known_answer_vectors) {
   // Deterministic coverage of the ASN.1 INTEGER pad/strip branches in both
   // `compact_to_der` (the test encoder) and `der_to_compact` (production).
   //
   // The previous version generated random signatures and leaned on the ~50%
   // odds that a given `r` has its high bit set to exercise the leading-zero
   // pad path. A run that happened to miss it on every iteration (~1/256 for
   // eight rounds) would silently drop that coverage. These fixed (r, s)
   // vectors pin each branch directly. Every component is a valid secp256k1
   // scalar in [1, n-1] -- the top byte is at most 0x80, well under n's leading
   // 0xFF -- so libsecp256k1's DER decoder preserves it byte-for-byte across the
   // round trip (it does not normalise low-S, so a deliberately high-S `s`
   // survives unchanged too).
   const auto check_round_trip = [](const std::array<unsigned char, 64>& rs) {
      const auto der     = compact_to_der(rs);
      const auto decoded = sysio::sigprov::kms::der_to_compact(std::span<const unsigned char>(der));
      BOOST_CHECK(decoded == rs);
   };

   // Build a 64-byte r||s from the most-significant and least-significant byte
   // of each 32-byte half (all interior bytes zero). The MSB selects which
   // branch the encoder takes; the non-zero LSB keeps the scalar in [1, n-1].
   const auto make_rs = [](unsigned char r_msb, unsigned char r_lsb,
                           unsigned char s_msb, unsigned char s_lsb) {
      std::array<unsigned char, 64> rs{};
      rs[0]  = r_msb;
      rs[31] = r_lsb;
      rs[32] = s_msb;
      rs[63] = s_lsb;
      return rs;
   };

   // r high bit set  -> encoder must PREPEND a 0x00 so the INTEGER stays positive.
   check_round_trip(make_rs(0x80, 0x01, /* s normal */ 0x7F, 0x01));
   // r top byte zero -> encoder must STRIP the leading 0x00 (minimal INTEGER).
   check_round_trip(make_rs(0x00, 0x42, /* s normal */ 0x7F, 0x01));
   // Same two branches exercised on the s INTEGER (s = 0x80.. is also high-S).
   check_round_trip(make_rs(/* r normal */ 0x7F, 0x01, 0x80, 0x01));
   check_round_trip(make_rs(/* r normal */ 0x7F, 0x01, 0x00, 0x42));
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

BOOST_AUTO_TEST_CASE(der_to_wire_k1_signature_matches_local_sign) {
   // libsecp256k1 signs low-S, so the K1 packing of a DER blob must reproduce the local K1 signature byte for byte,
   // header included.
   const fc::ecc::private_key      priv   = fc::ecc::private_key::generate();
   const fc::em::public_key        pub{priv.get_public_key().serialize()};
   const fc::sha256                digest = fc::sha256::hash(std::string{"wire-k1-end-to-end"});
   const fc::ecc::compact_signature local = priv.sign_compact(digest);

   const std::vector<unsigned char> der = compact_to_der(drop_k1_header(local));
   const fc::ecc::compact_signature out = sysio::sigprov::kms::der_to_wire_k1_signature(
      std::span<const unsigned char>(der), digest.to_uint8_span(), pub);

   BOOST_CHECK(out == local);
   BOOST_CHECK(fc::ecc::public_key::recover(out, digest) == priv.get_public_key());
}

BOOST_AUTO_TEST_CASE(der_to_wire_k1_signature_normalises_high_s_input) {
   const fc::ecc::private_key      priv   = fc::ecc::private_key::generate();
   const fc::em::public_key        pub{priv.get_public_key().serialize()};
   const fc::sha256                digest = fc::sha256::hash(std::string{"wire-k1-high-s"});
   const fc::ecc::compact_signature local = priv.sign_compact(digest);

   std::array<unsigned char, 64> rs = drop_k1_header(local);
   make_high_s(rs);
   const std::vector<unsigned char> der = compact_to_der(rs);
   const fc::ecc::compact_signature out = sysio::sigprov::kms::der_to_wire_k1_signature(
      std::span<const unsigned char>(der), digest.to_uint8_span(), pub);

   BOOST_CHECK(out == local);
}

BOOST_AUTO_TEST_CASE(der_to_wire_k1_signature_throws_on_wrong_pubkey) {
   const fc::ecc::private_key      priv      = fc::ecc::private_key::generate();
   const fc::em::public_key        wrong_pub{fc::ecc::private_key::generate().get_public_key().serialize()};
   const fc::sha256                digest    = fc::sha256::hash(std::string{"wire-k1-wrong-pub"});
   const std::vector<unsigned char> der      = compact_to_der(drop_k1_header(priv.sign_compact(digest)));

   BOOST_CHECK_THROW(sysio::sigprov::kms::der_to_wire_k1_signature(std::span<const unsigned char>(der),
                                                                   digest.to_uint8_span(), wrong_pub),
                     sysio::chain::plugin_config_exception);
}

// ---------------------------------------------------------------------------
// spki_der_to_public_key -- KMS public-key pinning decoder
//
// These tests synthesise the X.509 SubjectPublicKeyInfo that AWS KMS
// `GetPublicKey` returns by wrapping a locally generated EC point with
// `point_to_spki_der` above. No AWS credentials, no network.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(spki_der_to_public_key_round_trips) {
   // Known-answer vector: the secp256k1 generator point G is a fixed,
   // externally verifiable public key (SEC 2, every secp256k1 reference). A
   // well-formed SPKI wrapping it must decode back to exactly that key. The
   // previous version round-tripped four randomly generated keys, which never
   // pinned a specific answer and re-rolled the input on every run.
   //
   // 0x04 || G_x || G_y, big-endian:
   static constexpr std::array<unsigned char,
                               std::tuple_size_v<fc::em::public_key_data_uncompressed>> generator = {
      0x04,
      0x79, 0xBE, 0x66, 0x7E, 0xF9, 0xDC, 0xBB, 0xAC, // G_x
      0x55, 0xA0, 0x62, 0x95, 0xCE, 0x87, 0x0B, 0x07,
      0x02, 0x9B, 0xFC, 0xDB, 0x2D, 0xCE, 0x28, 0xD9,
      0x59, 0xF2, 0x81, 0x5B, 0x16, 0xF8, 0x17, 0x98,
      0x48, 0x3A, 0xDA, 0x77, 0x26, 0xA3, 0xC4, 0x65, // G_y
      0x5D, 0xA4, 0xFB, 0xFC, 0x0E, 0x11, 0x08, 0xA8,
      0xFD, 0x17, 0xB4, 0x48, 0xA6, 0x85, 0x54, 0x19,
      0x9C, 0x47, 0xD0, 0x8F, 0xFB, 0x10, 0xD4, 0xB8};

   fc::em::public_key_data_uncompressed g{};
   std::ranges::copy(generator, g.begin());
   const auto expected = fc::em::public_key{g}; // validates G is on-curve

   const auto der     = point_to_spki_der(g);
   const auto decoded = sysio::sigprov::kms::spki_der_to_public_key(
      std::span<const unsigned char>(der));
   BOOST_CHECK(decoded == expected);
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

BOOST_AUTO_TEST_CASE(spki_der_to_public_key_rejects_wrong_algorithm_oid) {
   // Flip the final octet of the algorithm OID (id-ecPublicKey, bytes 6..12 of
   // the prefix): the DER stays structurally valid but the algorithm is no
   // longer id-ecPublicKey. A KMS key that is, say, RSA or EdDSA would land
   // here -- the parser must reject it rather than mis-read a non-EC key as a
   // secp256k1 point. The existing wrong-curve test flips the *curve* OID; this
   // exercises the distinct id-ecPublicKey check.
   const auto priv = fc::em::private_key::generate();
   auto der = point_to_spki_der(priv.get_public_key().serialize_uncompressed());
   der[12] = 0x02; // index 12 = last octet of the id-ecPublicKey OID body
   BOOST_CHECK_THROW(
      sysio::sigprov::kms::spki_der_to_public_key(std::span<const unsigned char>(der)),
      sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(spki_der_to_public_key_rejects_trailing_bytes) {
   // Bytes after the outer SubjectPublicKeyInfo SEQUENCE. DER is a canonical
   // encoding with no trailing slack, so a buffer longer than the structure it
   // declares is malformed and must be rejected, not silently truncated.
   const auto priv = fc::em::private_key::generate();
   auto der = point_to_spki_der(priv.get_public_key().serialize_uncompressed());
   der.push_back(0x00); // one stray byte past the declared outer length
   BOOST_CHECK_THROW(
      sysio::sigprov::kms::spki_der_to_public_key(std::span<const unsigned char>(der)),
      sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(spki_der_to_public_key_rejects_nonzero_unused_bits) {
   // The subjectPublicKey BIT STRING begins with an "unused bits" octet that
   // must be 0x00 for a byte-aligned EC point. A non-zero count would mean the
   // final byte of the point is partial -- nonsensical for a 65-byte key -- so
   // it must be rejected rather than have the point silently mis-aligned.
   const auto priv = fc::em::private_key::generate();
   auto der = point_to_spki_der(priv.get_public_key().serialize_uncompressed());
   der[22] = 0x01; // index 22 = the BIT STRING's leading unused-bit count
   BOOST_CHECK_THROW(
      sysio::sigprov::kms::spki_der_to_public_key(std::span<const unsigned char>(der)),
      sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(spki_der_to_ed25519_public_key_decodes_rfc8410_example) {
   // RFC 8410 section 10.1's example SubjectPublicKeyInfo:
   // MCowBQYDK2VwAyEAGb9ECWmEzf6FQbrBZ9w7lshQhqowtrbLDFw4rXAxZuE=
   static constexpr std::array<unsigned char, 44> spki = {
      0x30, 0x2A, 0x30, 0x05, 0x06, 0x03, 0x2B, 0x65, 0x70, 0x03, 0x21, 0x00, 0x19, 0xBF, 0x44,
      0x09, 0x69, 0x84, 0xCD, 0xFE, 0x85, 0x41, 0xBA, 0xC1, 0x67, 0xDC, 0x3B, 0x96, 0xC8, 0x50,
      0x86, 0xAA, 0x30, 0xB6, 0xB6, 0xCB, 0x0C, 0x5C, 0x38, 0xAD, 0x70, 0x31, 0x66, 0xE1};
   const fc::crypto::ed::public_key_shim decoded =
      sysio::sigprov::kms::spki_der_to_ed25519_public_key(std::span<const unsigned char>(spki));
   BOOST_CHECK(std::ranges::equal(decoded._data, std::span<const unsigned char>(spki).subspan(12)));
}

BOOST_AUTO_TEST_CASE(spki_der_to_ed25519_public_key_rejects_other_shapes) {
   const fc::crypto::ed::public_key_shim pub = generate_ed25519_key().get_public_key();
   const auto rejects = [](const std::vector<unsigned char>& der) {
      BOOST_CHECK_THROW(sysio::sigprov::kms::spki_der_to_ed25519_public_key(std::span<const unsigned char>(der)),
                        sysio::chain::plugin_config_exception);
   };

   // A secp256k1 key.
   rejects(point_to_spki_der(fc::em::private_key::generate().get_public_key().serialize_uncompressed()));

   // Another algorithm OID (X25519, 1.3.101.110).
   std::vector<unsigned char> x25519 = ed25519_to_spki_der(pub);
   x25519[8] = 0x6E;
   rejects(x25519);

   // Parameters after the OID, which RFC 8410 forbids: AlgorithmIdentifier grows by a 2-byte NULL.
   std::vector<unsigned char> with_params = ed25519_to_spki_der(pub);
   with_params[1] += 2;
   with_params[3] += 2;
   with_params.insert(with_params.begin() + 9, {0x05, 0x00});
   rejects(with_params);

   // A 31-byte key.
   std::vector<unsigned char> short_key = ed25519_to_spki_der(pub);
   short_key.pop_back();
   short_key[1] -= 1;
   short_key[10] -= 1;
   rejects(short_key);

   // Trailing bytes.
   std::vector<unsigned char> trailing = ed25519_to_spki_der(pub);
   trailing.push_back(0x00);
   rejects(trailing);
}

BOOST_AUTO_TEST_CASE(spki_der_to_public_key_rejects_ed25519) {
   const std::vector<unsigned char> der = ed25519_to_spki_der(generate_ed25519_key().get_public_key());
   BOOST_CHECK_THROW(sysio::sigprov::kms::spki_der_to_public_key(std::span<const unsigned char>(der)),
                     sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(spki_der_to_public_key_pinning_rejects_mismatched_key) {
   // Public-key pinning -- the security-relevant branch -- exercised offline.
   //
   // verify_kms_pubkey() decodes the SubjectPublicKeyInfo that KMS
   // `GetPublicKey` returns and asserts it equals the operator-pinned key
   // (`kms_pubkey == state.expected_em_pubkey`); a mismatch is turned into
   // plugin_config_exception so a spec that pins the WRONG <public-key> is
   // rejected before any billable Sign. `kms_signer_rejects_mismatched_pin_before_sign`
   // drives verify_kms_pubkey itself through a client double; the key-equality
   // semantics that assertion relies on are exercised here directly: a key
   // decoded from one KMS SPKI must compare equal to itself and unequal to any
   // other key. `spki_der_to_public_key` is the exact decode the pin check runs.
   const auto priv_a = fc::em::private_key::generate();
   const auto priv_b = fc::em::private_key::generate();
   const auto pub_a  = priv_a.get_public_key();
   const auto pub_b  = priv_b.get_public_key();
   BOOST_REQUIRE(!(pub_a == pub_b)); // sanity: two independent keys must differ

   // KMS hands back key A's SPKI; decode it the way verify_kms_pubkey does.
   const auto der_a   = point_to_spki_der(pub_a.serialize_uncompressed());
   const auto kms_pub = sysio::sigprov::kms::spki_der_to_public_key(
      std::span<const unsigned char>(der_a));

   // Pin matches the key KMS holds -> assertion holds, pin would pass.
   BOOST_CHECK(kms_pub == pub_a);
   // Pin names a different key -> assertion fails; this is the branch
   // verify_kms_pubkey converts into plugin_config_exception.
   BOOST_CHECK(!(kms_pub == pub_b));
}

// ---------------------------------------------------------------------------
// KMSClient cache + AWS SDK lifecycle
//
// `get_kms_client` is offline: it only touches the AWS SDK to construct
// `Aws::KMS::KMSClient`, which does not resolve credentials or open
// connections at construction time. These tests therefore run without AWS
// creds. The empty-region cases pin the resolution environment (env vars,
// shared config, IMDS off) via the shared fixtures so they stay offline and
// deterministic regardless of the host's AWS setup; the resolution chain
// itself is unit-tested in the signature_provider_aws library's own suite.
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

BOOST_AUTO_TEST_CASE(get_kms_client_empty_region_resolves_from_env) {
   // A region-less spec resolves the client region from the environment and is cached under the RESOLVED
   // region, so the equivalent explicit-region lookup shares the client.
   const sigaws::test::scoped_env_var default_region{sigaws::env_default_region, nullptr};
   const sigaws::test::scoped_env_var region{sigaws::env_region, "eu-north-1"};
   const auto resolved      = sysio::sigprov::kms::get_kms_client("");
   const auto explicit_look = sysio::sigprov::kms::get_kms_client("eu-north-1");
   BOOST_REQUIRE(resolved);
   BOOST_CHECK_EQUAL(resolved.get(), explicit_look.get());
}

BOOST_AUTO_TEST_CASE(get_kms_client_empty_region_unresolvable_throws) {
   // With nothing to resolve from (no env, no shared config, IMDS disabled), a region-less lookup must
   // throw -- never silently default to a region the operator didn't choose.
   const sigaws::test::scoped_unresolvable_region_env pinned;
   BOOST_CHECK_THROW(sysio::sigprov::kms::get_kms_client(""),
                     sysio::chain::plugin_config_exception);
}

// ---------------------------------------------------------------------------
// make_kms_signature_provider
//
// Construction is offline (creates a closure capturing a cached KMSClient
// and the expected pubkey). These cases build the closures without calling
// them; the `kms_signer_*` cases below call them through a client double.
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

BOOST_AUTO_TEST_CASE(make_kms_signature_provider_regionless_resolves_from_env) {
   // A region-less ref constructs end-to-end: the client region resolves from the environment at
   // construction (pinned here so the case stays offline and deterministic), and the closures come back set
   // exactly as in the explicit-region case. Neither closure is invoked -- either would hit live KMS.
   const sigaws::test::scoped_env_var default_region{sigaws::env_default_region, nullptr};
   const sigaws::test::scoped_env_var region{sigaws::env_region, "eu-north-1"};

   const auto chain_pub =
      fc::crypto::private_key::generate(fc::crypto::private_key::key_type::em).get_public_key();

   const auto kms = sysio::sigprov::kms::make_kms_signature_provider(
      kms_key_ref{{}, "alias/wire-cranker-eth-01"},
      fc::crypto::chain_key_type_ethereum,
      chain_pub);

   BOOST_CHECK(static_cast<bool>(kms.sign));
   BOOST_CHECK(static_cast<bool>(kms.warm_up));
}

BOOST_AUTO_TEST_CASE(make_kms_signature_provider_returns_callable_for_wire_k1) {
   const fc::crypto::public_key chain_pub =
      fc::crypto::private_key::generate(fc::crypto::private_key::key_type::k1).get_public_key();

   const sysio::sigprov::kms::kms_signer kms = sysio::sigprov::kms::make_kms_signature_provider(
      kms_key_ref{"us-east-1", "alias/wire-k1"}, fc::crypto::chain_key_type_wire, chain_pub);

   BOOST_CHECK(static_cast<bool>(kms.sign));
   BOOST_CHECK(static_cast<bool>(kms.warm_up));
}

BOOST_AUTO_TEST_CASE(make_kms_signature_provider_rejects_wire_r1) {
   // KMS holds secp256k1 keys here; a WIRE R1 (P-256) key has no signing path yet.
   const fc::crypto::public_key chain_pub =
      fc::crypto::private_key::generate(fc::crypto::private_key::key_type::r1).get_public_key();

   BOOST_CHECK_THROW(sysio::sigprov::kms::make_kms_signature_provider(
                        kms_key_ref{"us-east-1", "alias/wire-r1"}, fc::crypto::chain_key_type_wire, chain_pub),
                     sysio::chain::pending_impl_exception);
}

BOOST_AUTO_TEST_CASE(make_kms_signature_provider_rejects_null_client) {
   const fc::ecc::private_key priv = fc::ecc::private_key::generate();
   BOOST_CHECK_THROW(sysio::sigprov::kms::make_kms_signature_provider(
                        nullptr, "alias/wire-k1", fc::crypto::chain_key_type_wire, wire_k1_public_key(priv)),
                     sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(make_kms_signature_provider_rejects_wire_non_k1_and_bls) {
   // A WIRE spec carrying an Ethereum-variant key, and a BLS key type, have no KMS signing path.
   const fc::ecc::private_key   priv   = fc::ecc::private_key::generate();
   const fc::crypto::public_key em_pub = ethereum_public_key(priv);
   BOOST_CHECK_THROW(sysio::sigprov::kms::make_kms_signature_provider(
                        kms_key_ref{"us-east-1", "alias/x"}, fc::crypto::chain_key_type_wire, em_pub),
                     sysio::chain::pending_impl_exception);

   const fc::crypto::public_key bls_pub =
      fc::crypto::private_key::generate(fc::crypto::private_key::key_type::bls).get_public_key();
   BOOST_CHECK_THROW(sysio::sigprov::kms::make_kms_signature_provider(
                        kms_key_ref{"us-east-1", "alias/x"}, fc::crypto::chain_key_type_wire_bls, bls_pub),
                     sysio::chain::pending_impl_exception);
}

BOOST_AUTO_TEST_CASE(make_kms_signature_provider_validates_key_before_region) {
   // With no region resolvable, an unsupported key must still report the key, not the region.
   const sigaws::test::scoped_unresolvable_region_env pinned;
   const fc::crypto::public_key r1_pub =
      fc::crypto::private_key::generate(fc::crypto::private_key::key_type::r1).get_public_key();
   BOOST_CHECK_THROW(sysio::sigprov::kms::make_kms_signature_provider(
                        kms_key_ref{{}, "alias/x"}, fc::crypto::chain_key_type_wire, r1_pub),
                     sysio::chain::pending_impl_exception);
}

BOOST_AUTO_TEST_CASE(make_kms_signature_provider_returns_raw_signer_for_solana) {
   const sysio::sigprov::kms::kms_signer kms = sysio::sigprov::kms::make_kms_signature_provider(
      kms_key_ref{"us-east-1", "alias/sol"}, fc::crypto::chain_key_type_solana,
      solana_public_key(generate_ed25519_key()));

   BOOST_CHECK(static_cast<bool>(kms.sign));
   BOOST_CHECK(static_cast<bool>(kms.sign_raw));
   BOOST_CHECK(static_cast<bool>(kms.warm_up));

   // Only an Ed25519 key signs raw messages.
   const fc::ecc::private_key            k1  = fc::ecc::private_key::generate();
   const sysio::sigprov::kms::kms_signer eth = sysio::sigprov::kms::make_kms_signature_provider(
      kms_key_ref{"us-east-1", "alias/eth"}, fc::crypto::chain_key_type_ethereum, ethereum_public_key(k1));
   BOOST_CHECK(!static_cast<bool>(eth.sign_raw));
}

BOOST_AUTO_TEST_CASE(make_kms_signature_provider_rejects_solana_with_non_ed_key) {
   const fc::ecc::private_key k1 = fc::ecc::private_key::generate();
   BOOST_CHECK_THROW(sysio::sigprov::kms::make_kms_signature_provider(
                        kms_key_ref{"us-east-1", "alias/sol"}, fc::crypto::chain_key_type_solana,
                        wire_k1_public_key(k1)),
                     sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(make_kms_signature_provider_rejects_wire_ed25519) {
   // An Ed25519 key under a WIRE spec has no KMS signing path.
   BOOST_CHECK_THROW(sysio::sigprov::kms::make_kms_signature_provider(
                        kms_key_ref{"us-east-1", "alias/x"}, fc::crypto::chain_key_type_wire,
                        solana_public_key(generate_ed25519_key())),
                     sysio::chain::pending_impl_exception);
}

// ---------------------------------------------------------------------------
// Signing closure driven through `local_key_kms_client`: pinning, the Sign
// request, and the packing of the returned DER, without AWS.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(kms_signer_signs_wire_k1) {
   for (const bool high_s : {false, true}) {
      BOOST_TEST_CONTEXT("high_s=" << high_s) {
         const fc::ecc::private_key                  priv   = fc::ecc::private_key::generate();
         const std::shared_ptr<local_key_kms_client> client = std::make_shared<local_key_kms_client>(priv);
         client->high_s                                     = high_s;
         const fc::crypto::public_key expected              = wire_k1_public_key(priv);

         const sysio::sigprov::kms::kms_signer kms = sysio::sigprov::kms::make_kms_signature_provider(
            client, "alias/wire-k1", fc::crypto::chain_key_type_wire, expected);
         BOOST_CHECK_NO_THROW(kms.warm_up());

         const fc::sha256            digest = fc::sha256::hash(std::string{"wire-k1-trx-digest"});
         const fc::crypto::signature sig    = kms.sign(digest);

         BOOST_REQUIRE(sig.contains<fc::ecc::signature_shim>());
         BOOST_CHECK(fc::crypto::public_key::recover(sig, digest) == expected);
         // libsecp256k1 signs deterministically, so the KMS path must reproduce the local K1 signature exactly.
         BOOST_CHECK(sig.get<fc::ecc::signature_shim>().serialize() == priv.sign_compact(digest));

         BOOST_CHECK_EQUAL(client->last_get_public_key_key_id, "alias/wire-k1");
         BOOST_CHECK_EQUAL(client->last_sign_key_id, "alias/wire-k1");
         BOOST_CHECK(client->last_message_type == Aws::KMS::Model::MessageType::DIGEST);
         BOOST_CHECK(client->last_algorithm == Aws::KMS::Model::SigningAlgorithmSpec::ECDSA_SHA_256);
         BOOST_CHECK(std::ranges::equal(client->last_message, digest.to_uint8_span()));
         // warm_up pinned the key, so the sign did not fetch it again.
         BOOST_CHECK_EQUAL(client->get_public_key_calls, 1);
         BOOST_CHECK_EQUAL(client->sign_calls, 1);
      }
   }
}

BOOST_AUTO_TEST_CASE(kms_signer_signs_ethereum_through_eth_client_signer) {
   // The Ethereum caller hands the closure a keccak digest and self-verifies the first signature.
   const fc::ecc::private_key                  priv     = fc::ecc::private_key::generate();
   const std::shared_ptr<local_key_kms_client> client   = std::make_shared<local_key_kms_client>(priv);
   const fc::crypto::public_key                expected = ethereum_public_key(priv);

   const sysio::sigprov::kms::kms_signer kms = sysio::sigprov::kms::make_kms_signature_provider(
      client, "alias/eth", fc::crypto::chain_key_type_ethereum, expected);
   const fc::crypto::signature_provider_t provider{fc::crypto::chain_kind_ethereum, fc::crypto::chain_key_type_ethereum,
                                                   "kms-eth", expected, std::nullopt, kms.sign};

   const std::string                  payload = "rlp-encoded-ethereum-transaction";
   const std::span<const uint8_t>     bytes{reinterpret_cast<const uint8_t*>(payload.data()), payload.size()};
   const fc::crypto::eth_client_signer signer{provider};
   const fc::crypto::signature        sig = signer.sign(bytes);

   BOOST_REQUIRE(sig.contains<fc::em::signature_shim>());
   BOOST_CHECK(signer.recover(sig, bytes) == expected);
   BOOST_CHECK(provider.self_verified->load());
   // No warm_up: the first sign pinned the key.
   BOOST_CHECK_EQUAL(client->get_public_key_calls, 1);
   BOOST_CHECK_EQUAL(client->sign_calls, 1);
}

BOOST_AUTO_TEST_CASE(kms_signer_rejects_mismatched_pin_before_sign) {
   const fc::ecc::private_key                  held   = fc::ecc::private_key::generate();
   const fc::ecc::private_key                  pinned = fc::ecc::private_key::generate();
   const std::shared_ptr<local_key_kms_client> client = std::make_shared<local_key_kms_client>(held);

   const sysio::sigprov::kms::kms_signer kms = sysio::sigprov::kms::make_kms_signature_provider(
      client, "alias/wire-k1", fc::crypto::chain_key_type_wire, wire_k1_public_key(pinned));

   BOOST_CHECK_THROW(kms.warm_up(), sysio::chain::plugin_config_exception);
   BOOST_CHECK_THROW(kms.sign(fc::sha256::hash(std::string{"never-signed"})), sysio::chain::plugin_config_exception);
   // A failed pin is not cached: each attempt fetches the key again, and no Sign is ever sent.
   BOOST_CHECK_EQUAL(client->get_public_key_calls, 2);
   BOOST_CHECK_EQUAL(client->sign_calls, 0);
}

BOOST_AUTO_TEST_CASE(kms_signer_retries_pin_after_transient_failure) {
   const fc::ecc::private_key                  priv     = fc::ecc::private_key::generate();
   const std::shared_ptr<local_key_kms_client> client   = std::make_shared<local_key_kms_client>(priv);
   const fc::crypto::public_key                expected = wire_k1_public_key(priv);
   client->fail_next_get_public_key                     = true;

   const sysio::sigprov::kms::kms_signer kms = sysio::sigprov::kms::make_kms_signature_provider(
      client, "alias/wire-k1", fc::crypto::chain_key_type_wire, expected);

   BOOST_CHECK_THROW(kms.warm_up(), sysio::chain::signing_transient_exception);
   const fc::sha256 digest = fc::sha256::hash(std::string{"after-throttle"});
   BOOST_CHECK(fc::crypto::public_key::recover(kms.sign(digest), digest) == expected);
   BOOST_CHECK_EQUAL(client->get_public_key_calls, 2);
   BOOST_CHECK_EQUAL(client->sign_calls, 1);
}

BOOST_AUTO_TEST_CASE(kms_signer_classifies_sign_errors) {
   const fc::ecc::private_key                  priv   = fc::ecc::private_key::generate();
   const std::shared_ptr<local_key_kms_client> client = std::make_shared<local_key_kms_client>(priv);
   const sysio::sigprov::kms::kms_signer       kms    = sysio::sigprov::kms::make_kms_signature_provider(
      client, "alias/wire-k1", fc::crypto::chain_key_type_wire, wire_k1_public_key(priv));
   const fc::sha256 digest = fc::sha256::hash(std::string{"sign-errors"});

   client->sign_error = Aws::Client::AWSError<Aws::KMS::KMSErrors>(
      Aws::KMS::KMSErrors::THROTTLING, "ThrottlingException", "Rate exceeded", /* isRetryable */ true);
   BOOST_CHECK_THROW(kms.sign(digest), sysio::chain::signing_transient_exception);

   client->sign_error = Aws::Client::AWSError<Aws::KMS::KMSErrors>(
      Aws::KMS::KMSErrors::DISABLED, "DisabledException", "Key is disabled", /* isRetryable */ false);
   BOOST_CHECK_THROW(kms.sign(digest), sysio::chain::plugin_config_exception);
   BOOST_CHECK_EQUAL(client->sign_calls, 2);
}

BOOST_AUTO_TEST_CASE(kms_signer_rejects_malformed_der) {
   const fc::ecc::private_key                  priv   = fc::ecc::private_key::generate();
   const std::shared_ptr<local_key_kms_client> client = std::make_shared<local_key_kms_client>(priv);
   client->sign_reply                                 = std::vector<unsigned char>{0xde, 0xad, 0xbe, 0xef};
   const sysio::sigprov::kms::kms_signer kms          = sysio::sigprov::kms::make_kms_signature_provider(
      client, "alias/wire-k1", fc::crypto::chain_key_type_wire, wire_k1_public_key(priv));

   BOOST_CHECK_THROW(kms.sign(fc::sha256::hash(std::string{"bad-der"})), sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(kms_signer_signs_solana_through_sol_client_signer) {
   const fc::crypto::ed::private_key_shim         key      = generate_ed25519_key();
   const std::shared_ptr<local_ed25519_kms_client> client   = std::make_shared<local_ed25519_kms_client>(key);
   const fc::crypto::public_key                    expected = solana_public_key(key);

   const sysio::sigprov::kms::kms_signer kms = sysio::sigprov::kms::make_kms_signature_provider(
      client, "alias/sol", fc::crypto::chain_key_type_solana, expected);
   BOOST_CHECK_NO_THROW(kms.warm_up());
   const fc::crypto::signature_provider_t provider{fc::crypto::chain_kind_solana, fc::crypto::chain_key_type_solana,
                                                   "kms-sol", expected, std::nullopt, kms.sign, kms.sign_raw};

   const std::vector<uint8_t>        message(1232, 0xA5); // the largest Solana transaction
   const fc::crypto::sol_client_signer signer{provider};
   const fc::crypto::signature       sig = signer.sign(message);

   BOOST_REQUIRE(sig.contains<fc::crypto::ed::signature_shim>());
   BOOST_CHECK(signer.verify(sig, message));
   BOOST_CHECK(provider.self_verified->load());
   // Ed25519 is deterministic, so KMS's signature equals the local key's.
   BOOST_CHECK(sig.get<fc::crypto::ed::signature_shim>()._data == key.sign_raw(message.data(), message.size())._data);

   BOOST_CHECK_EQUAL(client->last_get_public_key_key_id, "alias/sol");
   BOOST_CHECK_EQUAL(client->last_sign_key_id, "alias/sol");
   BOOST_CHECK(client->last_message_type == Aws::KMS::Model::MessageType::RAW);
   BOOST_CHECK(client->last_algorithm == Aws::KMS::Model::SigningAlgorithmSpec::ED25519_SHA_512);
   BOOST_CHECK(std::ranges::equal(client->last_message, message));
   BOOST_CHECK_EQUAL(client->get_public_key_calls, 1);
   BOOST_CHECK_EQUAL(client->sign_calls, 1);
}

BOOST_AUTO_TEST_CASE(kms_signer_ed25519_digest_sign_matches_local_key) {
   // `sign` signs a digest's hex, as a local Ed25519 key's `sign_sha256` does.
   const fc::crypto::ed::private_key_shim         key    = generate_ed25519_key();
   const std::shared_ptr<local_ed25519_kms_client> client = std::make_shared<local_ed25519_kms_client>(key);
   const sysio::sigprov::kms::kms_signer kms = sysio::sigprov::kms::make_kms_signature_provider(
      client, "alias/sol", fc::crypto::chain_key_type_solana, solana_public_key(key));

   const fc::sha256            digest = fc::sha256::hash(std::string{"ed25519-digest"});
   const fc::crypto::signature sig    = kms.sign(digest);
   BOOST_REQUIRE(sig.contains<fc::crypto::ed::signature_shim>());
   BOOST_CHECK(sig.get<fc::crypto::ed::signature_shim>()._data == key.sign_sha256(digest)._data);
}

BOOST_AUTO_TEST_CASE(kms_signer_ed25519_rejects_mismatched_pin_before_sign) {
   const std::shared_ptr<local_ed25519_kms_client> client =
      std::make_shared<local_ed25519_kms_client>(generate_ed25519_key());
   const sysio::sigprov::kms::kms_signer kms = sysio::sigprov::kms::make_kms_signature_provider(
      client, "alias/sol", fc::crypto::chain_key_type_solana, solana_public_key(generate_ed25519_key()));

   BOOST_CHECK_THROW(kms.warm_up(), sysio::chain::plugin_config_exception);
   const std::vector<uint8_t> message{1, 2, 3};
   BOOST_CHECK_THROW(kms.sign_raw(message), sysio::chain::plugin_config_exception);
   BOOST_CHECK_EQUAL(client->sign_calls, 0);
}

BOOST_AUTO_TEST_CASE(kms_signer_ed25519_rejects_a_secp256k1_kms_key) {
   // The spec names a Solana key, but the KMS key is secp256k1.
   const fc::crypto::ed::private_key_shim         key    = generate_ed25519_key();
   const std::shared_ptr<local_ed25519_kms_client> client = std::make_shared<local_ed25519_kms_client>(key);
   client->spki_reply = point_to_spki_der(fc::em::private_key::generate().get_public_key().serialize_uncompressed());
   const sysio::sigprov::kms::kms_signer kms = sysio::sigprov::kms::make_kms_signature_provider(
      client, "alias/sol", fc::crypto::chain_key_type_solana, solana_public_key(key));

   BOOST_CHECK_THROW(kms.warm_up(), sysio::chain::plugin_config_exception);
}

BOOST_AUTO_TEST_CASE(kms_signer_ed25519_rejects_bad_signatures) {
   const fc::crypto::ed::private_key_shim         key    = generate_ed25519_key();
   const std::shared_ptr<local_ed25519_kms_client> client = std::make_shared<local_ed25519_kms_client>(key);
   const sysio::sigprov::kms::kms_signer kms = sysio::sigprov::kms::make_kms_signature_provider(
      client, "alias/sol", fc::crypto::chain_key_type_solana, solana_public_key(key));
   const std::vector<uint8_t> message{1, 2, 3};

   // Signed by another key after the pin passed.
   client->sign_with = generate_ed25519_key();
   BOOST_CHECK_THROW(kms.sign_raw(message), sysio::chain::plugin_config_exception);
   client->sign_with.reset();

   // Not 64 bytes.
   client->sign_reply = std::vector<unsigned char>(63, 0x01);
   BOOST_CHECK_THROW(kms.sign_raw(message), sysio::chain::plugin_config_exception);
   BOOST_CHECK_EQUAL(client->sign_calls, 2);
}

BOOST_AUTO_TEST_CASE(kms_signer_ed25519_rejects_oversized_message_before_sign) {
   const fc::crypto::ed::private_key_shim         key    = generate_ed25519_key();
   const std::shared_ptr<local_ed25519_kms_client> client = std::make_shared<local_ed25519_kms_client>(key);
   const sysio::sigprov::kms::kms_signer kms = sysio::sigprov::kms::make_kms_signature_provider(
      client, "alias/sol", fc::crypto::chain_key_type_solana, solana_public_key(key));

   BOOST_CHECK_NO_THROW(kms.sign_raw(std::vector<uint8_t>(4096, 0x01)));
   BOOST_CHECK_THROW(kms.sign_raw(std::vector<uint8_t>(4097, 0x01)), fc::invalid_arg_exception);
   BOOST_CHECK_EQUAL(client->sign_calls, 1);
}

BOOST_AUTO_TEST_CASE(kms_signer_ed25519_rejects_empty_or_oversized_message_before_any_kms_call) {
   const fc::crypto::ed::private_key_shim         key    = generate_ed25519_key();
   const std::shared_ptr<local_ed25519_kms_client> client = std::make_shared<local_ed25519_kms_client>(key);
   const sysio::sigprov::kms::kms_signer kms = sysio::sigprov::kms::make_kms_signature_provider(
      client, "alias/sol", fc::crypto::chain_key_type_solana, solana_public_key(key));

   BOOST_CHECK_THROW(kms.sign_raw(std::vector<uint8_t>{}), fc::invalid_arg_exception);
   BOOST_CHECK_THROW(kms.sign_raw(std::vector<uint8_t>(4097, 0x01)), fc::invalid_arg_exception);
   BOOST_CHECK_EQUAL(client->get_public_key_calls, 0);
   BOOST_CHECK_EQUAL(client->sign_calls, 0);
}

BOOST_AUTO_TEST_CASE(kms_signer_ed25519_classifies_sign_errors) {
   const fc::crypto::ed::private_key_shim         key    = generate_ed25519_key();
   const std::shared_ptr<local_ed25519_kms_client> client = std::make_shared<local_ed25519_kms_client>(key);
   const sysio::sigprov::kms::kms_signer kms = sysio::sigprov::kms::make_kms_signature_provider(
      client, "alias/sol", fc::crypto::chain_key_type_solana, solana_public_key(key));
   const std::vector<uint8_t> message{1, 2, 3};

   client->sign_error = Aws::Client::AWSError<Aws::KMS::KMSErrors>(
      Aws::KMS::KMSErrors::THROTTLING, "ThrottlingException", "Rate exceeded", /* isRetryable */ true);
   BOOST_CHECK_THROW(kms.sign_raw(message), sysio::chain::signing_transient_exception);

   client->sign_error = Aws::Client::AWSError<Aws::KMS::KMSErrors>(
      Aws::KMS::KMSErrors::DISABLED, "DisabledException", "Key is disabled", /* isRetryable */ false);
   BOOST_CHECK_THROW(kms.sign_raw(message), sysio::chain::plugin_config_exception);
   BOOST_CHECK_EQUAL(client->sign_calls, 2);
}

BOOST_AUTO_TEST_CASE(kms_signer_rejects_signature_from_another_key_after_pinning) {
   // The pin passed, then the alias was re-pointed: every signature is still checked against the pinned key.
   const fc::ecc::private_key                  priv   = fc::ecc::private_key::generate();
   const std::shared_ptr<local_key_kms_client> client = std::make_shared<local_key_kms_client>(priv);
   const sysio::sigprov::kms::kms_signer       kms    = sysio::sigprov::kms::make_kms_signature_provider(
      client, "alias/wire-k1", fc::crypto::chain_key_type_wire, wire_k1_public_key(priv));
   BOOST_CHECK_NO_THROW(kms.warm_up());

   client->sign_with = fc::ecc::private_key::generate();
   BOOST_CHECK_THROW(kms.sign(fc::sha256::hash(std::string{"re-pointed"})), sysio::chain::plugin_config_exception);
   BOOST_CHECK_EQUAL(client->sign_calls, 1);
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
//   5. Repeat with a WIRE K1 spec for the same key, recovering the K1 key.
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

   // The same KMS key backs a WIRE K1 spec: sign a sha256 digest and recover the K1 key from it.
   const fc::crypto::public_key k1_pub{
      fc::crypto::public_key::storage_type{fc::ecc::public_key_shim{em_expected.serialize()}}};
   const sysio::sigprov::kms::kms_signer k1 =
      sysio::sigprov::kms::make_kms_signature_provider(ref, fc::crypto::chain_key_type_wire, k1_pub);
   BOOST_CHECK_NO_THROW(k1.warm_up());
   const fc::sha256 wire_digest = fc::sha256::hash(std::string{"wire-sysio kms live k1 test 2026"});
   BOOST_CHECK(fc::crypto::public_key::recover(k1.sign(wire_digest), wire_digest) == k1_pub);
}

// ---------------------------------------------------------------------------
// Live KMS Ed25519 round-trip -- env-gated, skipped in default CI.
//
//   KMS_LIVE_ED25519_SPEC    body of a `KMS:` spec for an ECC_NIST_EDWARDS25519 key
//   KMS_LIVE_ED25519_PUBKEY  the key's Solana (base58) public key
//
// Signs one fixed message through `sol_client_signer` and verifies it locally. One billable Sign call.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(kms_live_ed25519_sign_round_trip) {
   const char* spec_env = std::getenv("KMS_LIVE_ED25519_SPEC");
   const char* pub_env  = std::getenv("KMS_LIVE_ED25519_PUBKEY");
   if (!spec_env || !pub_env || *spec_env == '\0' || *pub_env == '\0') {
      BOOST_TEST_MESSAGE("KMS_LIVE_ED25519_SPEC / KMS_LIVE_ED25519_PUBKEY not set -- skipping live KMS Ed25519 test");
      return;
   }

   const fc::crypto::public_key pub =
      fc::crypto::from_native_string_to_public_key<fc::crypto::chain_key_type_solana>(std::string{pub_env});
   const sysio::sigprov::kms::kms_signer kms = sysio::sigprov::kms::make_kms_signature_provider(
      sysio::sigprov::kms::parse_kms_spec(spec_env), fc::crypto::chain_key_type_solana, pub);
   BOOST_CHECK_NO_THROW(kms.warm_up());

   const fc::crypto::signature_provider_t provider{fc::crypto::chain_kind_solana, fc::crypto::chain_key_type_solana,
                                                   "kms-live-sol", pub, std::nullopt, kms.sign, kms.sign_raw};
   const std::string                   text = "wire-sysio kms live ed25519 test 2026";
   const std::span<const uint8_t>      message{reinterpret_cast<const uint8_t*>(text.data()), text.size()};
   const fc::crypto::sol_client_signer signer{provider};
   BOOST_CHECK(signer.verify(signer.sign(message), message));
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

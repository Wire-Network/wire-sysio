#include "kms_signature_provider.hpp"

#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/types.hpp>

#include <fc/exception/exception.hpp>
#include <fc/string.hpp>

#include <aws/core/Aws.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/utils/Array.h>
#include <aws/kms/KMSClient.h>
#include <aws/kms/KMSErrors.h>
#include <aws/kms/model/GetPublicKeyRequest.h>
#include <aws/kms/model/SignRequest.h>
#include <aws/kms/model/MessageType.h>
#include <aws/kms/model/SigningAlgorithmSpec.h>

#include <fc/log/logger.hpp>
#include <magic_enum/magic_enum.hpp>

#include <secp256k1.h>
#include <secp256k1_recovery.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <memory>
#include <mutex>
#include <span>

namespace sysio::sigprov::kms {

namespace {

/// Anchor for ARN detection. ARNs always start with `arn:aws:kms:` (no
/// regional suffix on the partition for kms — `arn:aws-cn:kms:` and
/// `arn:aws-us-gov:kms:` are not currently in scope; revisit if a partition
/// other than `aws` becomes a deployment target).
constexpr std::string_view kms_arn_prefix = "arn:aws:kms:";

/// Case-insensitive lead-in shared by every ARN. A spec that begins with this
/// but does not match `kms_arn_prefix` is a malformed or out-of-scope ARN —
/// never the shorthand `<region>:<key-id>` form — and must fail loudly rather
/// than fall through to the shorthand parser. See `parse_kms_spec`.
constexpr std::string_view arn_lead_in = "arn:";

/// Number of colon-separated segments in a well-formed KMS ARN:
/// `arn`, `aws`, `kms`, `<region>`, `<account>`, `(key|alias)/<id>`.
constexpr std::size_t kms_arn_segment_count = 6;

/// Indices into the split ARN.
constexpr std::size_t arn_idx_partition = 1;
constexpr std::size_t arn_idx_service   = 2;
constexpr std::size_t arn_idx_region    = 3;
constexpr std::size_t arn_idx_tail      = 5;

/// Tail prefixes the KMS API accepts for the `KeyId` field.
constexpr std::string_view tail_prefix_key   = "key/";
constexpr std::string_view tail_prefix_alias = "alias/";

/// Process-wide secp256k1 context used by the DER / low-S helpers. Created
/// lazily on first use; libsecp256k1 contexts are thread-safe for the
/// signing-verification operations we use here. Lives separate from libfc's
/// internal context (`fc::em::detail::_get_context`) because that one is
/// not exposed across translation units.
const secp256k1_context* kms_secp_ctx() {
   static secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
   return ctx;
}

/// Ethereum's `v` offset: per Yellow-Paper Appendix F, the recovery byte is
/// `27 + recovery_id`. EIP-155 introduces a chain-id-tagged form for txs,
/// but the raw signing path used by the cranker / outpost client uses the
/// pre-EIP-155 (27/28) form.
constexpr unsigned char eth_v_offset = 27;

/// Process-wide AWS SDK lifecycle. Constructed lazily on first KMS access,
/// destroyed at static destruction (after the client cache, since the cache
/// is touched by `get_kms_client` *after* this lifecycle, making it the
/// younger Meyers singleton; younger statics are destroyed first). The
/// safe because the application object owns the plugin and is destroyed
/// before atexit static teardown; do not hand a KMS-backed `sign_fn` to an
/// owner that outlives the application.
struct aws_sdk_lifecycle {
   static aws_sdk_lifecycle& instance() {
      static aws_sdk_lifecycle s;
      return s;
   }
private:
   aws_sdk_lifecycle()  { Aws::InitAPI(_options); }
   ~aws_sdk_lifecycle() { Aws::ShutdownAPI(_options); }
   Aws::SDKOptions _options{};
};

/// Per-closure state for a KMS-backed signer. Captured by `std::shared_ptr`
/// so that `std::function` copies remain cheap and the same `KMSClient` /
/// expected pubkey are shared across copies of the closure.
///
/// A user-provided constructor is required because `std::once_flag` is neither
/// copyable nor movable, which rules out the aggregate / designated-initializer
/// construction the struct would otherwise allow.
struct kms_signer_state {
   kms_signer_state(std::shared_ptr<Aws::KMS::KMSClient> client_,
                    std::string                         key_id_,
                    fc::em::public_key                  expected_em_pubkey_)
      : client(std::move(client_))
      , key_id(std::move(key_id_))
      , expected_em_pubkey(std::move(expected_em_pubkey_)) {}

   std::shared_ptr<Aws::KMS::KMSClient> client;
   std::string                          key_id;
   /// secp256k1 uncompressed public key the spec pinned. Verified once against
   /// the live KMS key by `verify_kms_pubkey`, then used by `recover_v` to
   /// discriminate between recovery_id 0 and 1.
   fc::em::public_key                   expected_em_pubkey;
   /// One-shot guard for the GetPublicKey pinning check. The check runs on the
   /// first `Sign`; `std::call_once` re-runs it only if it threw — e.g. a
   /// transient GetPublicKey API error — and never again once it has passed.
   std::once_flag                       pin_once;
};

/// Translate an AWS KMS error outcome into a fc::exception with a stable
/// shape. The error-type enum name is the most actionable signal (e.g.
/// `AccessDenied`, `KeyUnavailable`, `Throttling`); the message and HTTP
/// status round out the diagnostic.
[[noreturn]] void throw_kms_error(std::string_view op, std::string_view key_id,
                                  const Aws::Client::AWSError<Aws::KMS::KMSErrors>& err) {
   FC_THROW_EXCEPTION(chain::plugin_config_exception,
                      "AWS KMS {} for key \"{}\" failed: {} (status {}, {}): {}",
                      op, key_id,
                      magic_enum::enum_name(err.GetErrorType()),
                      static_cast<int>(err.GetResponseCode()),
                      std::string{err.GetExceptionName().c_str()},
                      std::string{err.GetMessage().c_str()});
}

/// Per-region cache of KMS clients. Lock once on lookup; the SDK's HTTP
/// pool inside the client is itself thread-safe, so multiple sign closures
/// sharing a client may submit `Sign` requests concurrently.
struct kms_client_cache {
   std::mutex                                                      mutex;
   std::map<std::string, std::shared_ptr<Aws::KMS::KMSClient>>     by_region;
};

kms_client_cache& kms_clients() {
   static kms_client_cache c;
   return c;
}

/// Case-insensitive ASCII prefix test. ARN partitions and services are
/// lowercase by convention, but an operator may paste a mis-cased
/// `ARN:AWS:KMS:...`; we still want to recognise it as an ARN so it fails
/// loudly in `parse_kms_spec` rather than being mistaken for the shorthand
/// `<region>:<key-id>` form.
bool starts_with_ci(std::string_view s, std::string_view prefix) {
   if (s.size() < prefix.size())
      return false;
   return std::equal(prefix.begin(), prefix.end(), s.begin(),
                     [](unsigned char a, unsigned char b) {
                        return std::tolower(a) == std::tolower(b);
                     });
}

// ---------------------------------------------------------------------------
// X.509 SubjectPublicKeyInfo (DER) decoding for KMS public-key pinning.
//
// AWS KMS `GetPublicKey` returns the key as a DER-encoded SubjectPublicKeyInfo
// (RFC 5280 §4.1). We walk just enough of that structure to verify the key is
// secp256k1 and to lift out the raw EC point.
// ---------------------------------------------------------------------------

/// ASN.1 DER universal tags that appear inside an EC SubjectPublicKeyInfo.
constexpr unsigned char der_tag_sequence   = 0x30;
constexpr unsigned char der_tag_oid        = 0x06;
constexpr unsigned char der_tag_bit_string = 0x03;

/// DER length encoding: when the high bit of the leading length octet is set,
/// the low 7 bits give the number of subsequent big-endian length octets.
constexpr unsigned char der_length_long_form_bit = 0x80;
constexpr unsigned char der_length_value_mask    = 0x7F;

/// DER OBJECT IDENTIFIER bodies (the content of the OID TLV — tag and length
/// stripped) for the two OIDs that a secp256k1 SPKI must carry:
/// `1.2.840.10045.2.1` id-ecPublicKey and `1.3.132.0.10` secp256k1.
constexpr std::array<unsigned char, 7> oid_ec_public_key = {
   0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01};
constexpr std::array<unsigned char, 5> oid_secp256k1 = {
   0x2B, 0x81, 0x04, 0x00, 0x0A};

/// An uncompressed secp256k1 EC point is `0x04 || X[32] || Y[32]`.
constexpr unsigned char ec_point_uncompressed_prefix = 0x04;
constexpr std::size_t   ec_point_uncompressed_len    = 65;

/// Minimal ASN.1 DER reader over a byte span. DER is a canonical, unambiguous
/// encoding, so a structural tag-length-value walk is a genuine parse — not a
/// heuristic. Every malformed input raises `plugin_config_exception`.
struct der_reader {
   std::span<const unsigned char> buf;
   std::size_t                    pos = 0;

   /// True once every byte of `buf` has been consumed.
   bool at_end() const { return pos >= buf.size(); }

   /// One decoded tag-length-value triple. `content` views into the reader's
   /// underlying buffer (no copy is made).
   struct element {
      unsigned char                  tag;
      std::span<const unsigned char> content;
   };

   /// Read the next TLV element and advance past it.
   element next() {
      SYS_ASSERT(pos + 2 <= buf.size(), chain::plugin_config_exception,
                 "Malformed KMS public-key DER: truncated tag/length header");
      const unsigned char tag = buf[pos++];

      std::size_t len = buf[pos++];
      if ((len & der_length_long_form_bit) != 0) {
         const std::size_t len_octets = len & der_length_value_mask;
         SYS_ASSERT(len_octets >= 1 && len_octets <= sizeof(std::size_t),
                    chain::plugin_config_exception,
                    "Malformed KMS public-key DER: unsupported {}-octet length field",
                    len_octets);
         SYS_ASSERT(pos + len_octets <= buf.size(), chain::plugin_config_exception,
                    "Malformed KMS public-key DER: truncated long-form length");
         len = 0;
         for (std::size_t i = 0; i < len_octets; ++i) {
            len = (len << 8) | buf[pos++];
         }
      }
      // `buf.size() - pos` cannot underflow: `pos <= buf.size()` is an
      // invariant here, and the subtraction form avoids `pos + len` wrapping
      // on a maliciously large long-form length.
      SYS_ASSERT(len <= buf.size() - pos, chain::plugin_config_exception,
                 "Malformed KMS public-key DER: element body of {} bytes overruns buffer",
                 len);

      const auto content = buf.subspan(pos, len);
      pos += len;
      return {tag, content};
   }
};

/// Walk a DER X.509 SubjectPublicKeyInfo and return its raw uncompressed
/// secp256k1 EC point (`0x04 || X || Y`). Verifies the algorithm is
/// `id-ecPublicKey` over the `secp256k1` named curve, so an operator who
/// pointed the spec at an RSA key, a P-256 key, etc. gets a precise error
/// instead of a downstream surprise.
std::array<unsigned char, ec_point_uncompressed_len>
parse_spki_ec_point(std::span<const unsigned char> spki_der) {
   der_reader top{spki_der};
   const auto spki = top.next();
   SYS_ASSERT(spki.tag == der_tag_sequence, chain::plugin_config_exception,
              "KMS public-key DER: expected outer SEQUENCE, got tag {:#x}",
              static_cast<unsigned>(spki.tag));
   SYS_ASSERT(top.at_end(), chain::plugin_config_exception,
              "KMS public-key DER: unexpected trailing bytes after SubjectPublicKeyInfo");

   // SubjectPublicKeyInfo ::= SEQUENCE { algorithm AlgorithmIdentifier,
   //                                     subjectPublicKey BIT STRING }
   der_reader body{spki.content};
   const auto algorithm  = body.next();
   const auto subject_pk = body.next();
   SYS_ASSERT(algorithm.tag == der_tag_sequence, chain::plugin_config_exception,
              "KMS public-key DER: expected AlgorithmIdentifier SEQUENCE, got tag {:#x}",
              static_cast<unsigned>(algorithm.tag));
   SYS_ASSERT(subject_pk.tag == der_tag_bit_string, chain::plugin_config_exception,
              "KMS public-key DER: expected subjectPublicKey BIT STRING, got tag {:#x}",
              static_cast<unsigned>(subject_pk.tag));
   SYS_ASSERT(body.at_end(), chain::plugin_config_exception,
              "KMS public-key DER: unexpected trailing bytes inside SubjectPublicKeyInfo");

   // AlgorithmIdentifier ::= SEQUENCE { algorithm OID, parameters (curve OID) }
   der_reader alg{algorithm.content};
   const auto algo_oid  = alg.next();
   const auto curve_oid = alg.next();
   SYS_ASSERT(algo_oid.tag == der_tag_oid && curve_oid.tag == der_tag_oid,
              chain::plugin_config_exception,
              "KMS public-key DER: AlgorithmIdentifier is not a pair of OBJECT IDENTIFIERs");
   SYS_ASSERT(std::ranges::equal(algo_oid.content, oid_ec_public_key),
              chain::plugin_config_exception,
              "KMS public-key DER: algorithm is not id-ecPublicKey — the KMS key is not an "
              "elliptic-curve key");
   SYS_ASSERT(std::ranges::equal(curve_oid.content, oid_secp256k1),
              chain::plugin_config_exception,
              "KMS public-key DER: EC curve is not secp256k1 — the KMS key must be created "
              "with key spec ECC_SECG_P256K1");

   // subjectPublicKey BIT STRING: a leading "unused bits" octet (0 for a
   // byte-aligned key) followed by the EC point itself.
   SYS_ASSERT(!subject_pk.content.empty() && subject_pk.content.front() == 0x00,
              chain::plugin_config_exception,
              "KMS public-key DER: subjectPublicKey BIT STRING has a non-zero unused-bit count");
   const auto point = subject_pk.content.subspan(1);
   SYS_ASSERT(point.size() == ec_point_uncompressed_len, chain::plugin_config_exception,
              "KMS public-key DER: EC point is {} bytes, expected {} (uncompressed 0x04 form)",
              point.size(), ec_point_uncompressed_len);
   SYS_ASSERT(point.front() == ec_point_uncompressed_prefix, chain::plugin_config_exception,
              "KMS public-key DER: EC point is not in uncompressed (0x04) form");

   std::array<unsigned char, ec_point_uncompressed_len> out{};
   std::ranges::copy(point, out.begin());
   return out;
}

/// Public-key pinning check. Fetch the KMS key's public key with the free,
/// non-billable `GetPublicKey` API, decode its SubjectPublicKeyInfo, and assert
/// it matches the key the operator pinned in the spec. On mismatch this throws
/// `plugin_config_exception` early — before any billable `Sign` — with a
/// message that names the misconfiguration directly. Invoked exactly once per
/// closure through `kms_signer_state::pin_once`.
void verify_kms_pubkey(kms_signer_state& state) {
   Aws::KMS::Model::GetPublicKeyRequest req;
   req.SetKeyId(Aws::String{state.key_id.c_str()});

   auto outcome = state.client->GetPublicKey(req);
   if (!outcome.IsSuccess()) {
      throw_kms_error("GetPublicKey", state.key_id, outcome.GetError());
   }

   const auto& spki_buf = outcome.GetResult().GetPublicKey();
   const std::span<const unsigned char> spki_der{
      spki_buf.GetUnderlyingData(), spki_buf.GetLength()};

   const auto kms_pubkey = spki_der_to_public_key(spki_der);
   SYS_ASSERT(kms_pubkey == state.expected_em_pubkey, chain::plugin_config_exception,
              "AWS KMS key \"{}\" holds a public key that does not match the public key "
              "pinned in the signature-provider spec. Correct the spec's <public-key> to the "
              "key this KMS key actually holds, or point the spec at the intended KMS key.",
              state.key_id);
}

/// Run the public-key pinning check exactly once for `state`. Both the first
/// `Sign` and the opt-in startup probe funnel through here, so `std::call_once`
/// guarantees a single GetPublicKey round-trip — and retries it only if it
/// threw (e.g. a transient GetPublicKey API error).
void ensure_kms_pubkey_pinned(kms_signer_state& state) {
   std::call_once(state.pin_once, [&] { verify_kms_pubkey(state); });
}

} // namespace

kms_key_ref parse_kms_spec(std::string_view spec_data) {
   SYS_ASSERT(!spec_data.empty(), chain::plugin_config_exception,
              "KMS spec body is empty; expected an ARN or '<region>:<key-id-or-alias>'");

   if (spec_data.starts_with(kms_arn_prefix)) {
      // Full ARN form. Capture exactly `kms_arn_segment_count` parts so any
      // stray colons inside the trailing `key/<id>` segment stay glued to it
      // (KMS key ids are uuids, no colons today, but aliases are operator-
      // chosen and we should not silently truncate).
      auto parts = fc::split(std::string{spec_data}, ':', kms_arn_segment_count);
      SYS_ASSERT(parts.size() == kms_arn_segment_count, chain::plugin_config_exception,
                 "Malformed KMS ARN \"{}\": expected {} colon-separated segments, got {}",
                 spec_data, kms_arn_segment_count, parts.size());

      const auto& region = parts[arn_idx_region];
      const auto& tail   = parts[arn_idx_tail];

      SYS_ASSERT(!region.empty(), chain::plugin_config_exception,
                 "KMS ARN \"{}\" has empty region segment", spec_data);
      SYS_ASSERT(tail.starts_with(tail_prefix_key) || tail.starts_with(tail_prefix_alias),
                 chain::plugin_config_exception,
                 "KMS ARN tail must start with 'key/' or 'alias/', got \"{}\" in \"{}\"",
                 tail, spec_data);
      // Reject bare prefixes: `key/` or `alias/` with nothing after them.
      SYS_ASSERT(tail.size() > tail_prefix_key.size() &&
                 (!tail.starts_with(tail_prefix_alias) || tail.size() > tail_prefix_alias.size()),
                 chain::plugin_config_exception,
                 "KMS ARN tail \"{}\" has empty key/alias name", tail);

      return kms_key_ref{region, tail};
   }

   // A spec that begins with `arn:` (any casing) but did not match the
   // supported `arn:aws:kms:` form above is a malformed or out-of-scope ARN,
   // never shorthand. Falling through to the `<region>:<key-id>` parser below
   // would split on the first colon and silently yield region="arn"; AWS then
   // rejects that only at first sign — with an opaque `InvalidRegion`/endpoint
   // error, after a billable attempt. Fail loudly here instead, naming the
   // offending partition and service. Non-`aws` partitions (`aws-cn`,
   // `aws-us-gov`) are deliberately out of scope; this is the boundary that
   // enforces it. A mis-cased `ARN:AWS:KMS:...` and a typo'd service
   // (`arn:aws:ksm:...`) land here too — the message points at the canonical
   // form in every case.
   if (starts_with_ci(spec_data, arn_lead_in)) {
      const auto parts = fc::split(std::string{spec_data}, ':', kms_arn_segment_count);
      std::string partition, service;
      if (parts.size() > arn_idx_partition)
         partition = parts[arn_idx_partition];
      if (parts.size() > arn_idx_service)
         service = parts[arn_idx_service];
      FC_THROW_EXCEPTION(chain::plugin_config_exception,
                         "Unsupported KMS ARN \"{}\": only the 'arn:aws:kms:' partition/service "
                         "is supported (got partition \"{}\", service \"{}\"). Non-'aws' "
                         "partitions such as 'aws-cn' and 'aws-us-gov' are out of scope.",
                         spec_data, partition, service);
   }

   // Shorthand `<region>:<key-id-or-alias>`. We only split on the first colon
   // so aliases that themselves contain colons round-trip unchanged (KMS
   // alias names are operator-chosen, and while AWS docs disallow colons in
   // alias names today, the parser should not be the layer that depends on
   // that constraint).
   const auto colon = spec_data.find(':');
   SYS_ASSERT(colon != std::string_view::npos,
              chain::plugin_config_exception,
              "KMS spec \"{}\" must include a region: expected '<region>:<key-id-or-alias>' "
              "or a full 'arn:aws:kms:...' ARN", spec_data);
   SYS_ASSERT(colon > 0, chain::plugin_config_exception,
              "KMS spec \"{}\" has empty region", spec_data);
   SYS_ASSERT(colon + 1 < spec_data.size(), chain::plugin_config_exception,
              "KMS spec \"{}\" has empty key id", spec_data);

   return kms_key_ref{
      std::string{spec_data.substr(0, colon)},
      std::string{spec_data.substr(colon + 1)},
   };
}

std::array<unsigned char, 64> der_to_compact(std::span<const unsigned char> der) {
   secp256k1_ecdsa_signature parsed{};
   SYS_ASSERT(secp256k1_ecdsa_signature_parse_der(kms_secp_ctx(), &parsed, der.data(), der.size()) == 1,
              chain::plugin_config_exception,
              "KMS returned a malformed DER ECDSA signature ({} bytes)", der.size());

   std::array<unsigned char, 64> compact{};
   const auto serialised = secp256k1_ecdsa_signature_serialize_compact(
      kms_secp_ctx(), compact.data(), &parsed);
   SYS_ASSERT(serialised == 1, chain::plugin_config_exception,
              "Failed to serialise DER signature to compact (r||s) form");
   return compact;
}

bool normalise_low_s(std::array<unsigned char, 64>& compact) {
   secp256k1_ecdsa_signature parsed{};
   SYS_ASSERT(secp256k1_ecdsa_signature_parse_compact(kms_secp_ctx(), &parsed, compact.data()) == 1,
              chain::plugin_config_exception,
              "Cannot normalise an invalid compact ECDSA signature");

   secp256k1_ecdsa_signature normalised{};
   // libsecp256k1 returns 1 if it actually flipped the signature, 0 if input
   // was already in canonical (low-S) form.
   const bool was_high =
      secp256k1_ecdsa_signature_normalize(kms_secp_ctx(), &normalised, &parsed) == 1;

   secp256k1_ecdsa_signature_serialize_compact(
      kms_secp_ctx(), compact.data(), &normalised);
   return was_high;
}

unsigned char recover_v(const std::array<unsigned char, 64>& compact,
                        std::span<const std::uint8_t, 32>    digest,
                        const fc::em::public_key&            expected) {
   for (unsigned char rec_id = 0; rec_id < 2; ++rec_id) {
      fc::em::compact_signature trial{};
      std::ranges::copy(compact, trial.begin());
      trial[64] = static_cast<unsigned char>(eth_v_offset + rec_id);

      try {
         auto recovered = fc::em::public_key::recover(trial, digest.data(),
                                                      /* check_canonical */ false);
         if (recovered == expected) {
            return rec_id;
         }
      } catch (const fc::exception&) {
         // Recovery failed for this parity; try the other one.
      }
   }
   FC_THROW_EXCEPTION(chain::plugin_config_exception,
                      "Could not recover v: signature does not match expected public key");
}

fc::em::compact_signature der_to_eth_signature(
   std::span<const unsigned char>      der,
   std::span<const std::uint8_t, 32>   digest,
   const fc::em::public_key&           expected_pubkey) {
   auto compact = der_to_compact(der);
   normalise_low_s(compact);
   const auto rec_id = recover_v(compact, digest, expected_pubkey);

   fc::em::compact_signature out{};
   std::ranges::copy(compact, out.begin());
   out[64] = static_cast<unsigned char>(eth_v_offset + rec_id);
   return out;
}

fc::em::public_key spki_der_to_public_key(std::span<const unsigned char> spki_der) {
   const auto point = parse_spki_ec_point(spki_der);

   // `public_key_data_uncompressed` is std::array<char, 65>; the EC point is
   // std::array<unsigned char, 65>. The element-wise copy is a value-preserving
   // unsigned-char -> char conversion (bit pattern unchanged).
   fc::em::public_key_data_uncompressed uncompressed{};
   std::ranges::copy(point, uncompressed.begin());

   // The fc::em::public_key constructor re-validates the point on the curve via
   // libsecp256k1 and raises an fc assertion on a bad point. Translate that to
   // the module's standard exception type so every failure here is uniform.
   try {
      return fc::em::public_key{uncompressed};
   } catch (const fc::exception& e) {
      FC_THROW_EXCEPTION(chain::plugin_config_exception,
                         "KMS public-key DER carries an EC point that is not a valid "
                         "secp256k1 public key: {}", e.to_detail_string());
   }
}

kms_signer make_kms_signature_provider(const kms_key_ref&             ref,
                                       fc::crypto::chain_key_type_t   key_type,
                                       const fc::crypto::public_key&  expected_pubkey) {
   // v1 only supports secp256k1 keys held as Ethereum-flavoured public keys
   // (ECC_SECG_P256K1 in KMS, signed with ECDSA_SHA_256). Wire K1 (sysio's
   // own secp256k1 path), R1, BLS, Ed25519, and webauthn use different key
   // shapes or curves and need separate plumbing — see KMS_SIGNING_DESIGN.md
   // §2 non-goals for the contract.
   SYS_ASSERT(key_type == fc::crypto::chain_key_type_ethereum,
              chain::pending_impl_exception,
              "KMS signing currently supports only chain_key_type_ethereum, got {}",
              fc::crypto::chain_key_type_reflector::to_string(key_type));

   SYS_ASSERT(expected_pubkey.contains<fc::em::public_key_shim>(),
              chain::plugin_config_exception,
              "KMS spec key_type={} does not match the public key variant "
              "(expected em::public_key_shim)",
              fc::crypto::chain_key_type_reflector::to_string(key_type));

   const auto& shim = expected_pubkey.get<fc::em::public_key_shim>();

   auto state = std::make_shared<kms_signer_state>(
      get_kms_client(ref.region), ref.key_id, shim.unwrapped());

   fc::crypto::sign_fn sign = [state](const chain::digest_type& digest) -> chain::signature_type {
      // Public-key pinning. Before the first — and only the first — billable
      // Sign, fetch the KMS key's own public key with the free GetPublicKey
      // API and assert it matches the key pinned in the spec. This turns the
      // common "wrong <public-key> in the spec" mistake into a fast, direct
      // error instead of an opaque recovery failure that would otherwise
      // surface only after a paid Sign. If the opt-in startup probe already
      // ran the check, this is a no-op — both paths share `state->pin_once`
      // through `ensure_kms_pubkey_pinned`.
      ensure_kms_pubkey_pinned(*state);

      // Build a Sign request. MessageType=DIGEST tells KMS the 32 bytes are
      // already a hash; otherwise it would re-hash with SHA-256 and break
      // any chain that hashes with anything other than SHA-256.
      Aws::KMS::Model::SignRequest req;
      req.SetKeyId(Aws::String{state->key_id.c_str()});
      req.SetMessage(Aws::Utils::ByteBuffer{
         digest.to_uint8_span().data(),
         digest.to_uint8_span().size()});
      req.SetMessageType(Aws::KMS::Model::MessageType::DIGEST);
      req.SetSigningAlgorithm(Aws::KMS::Model::SigningAlgorithmSpec::ECDSA_SHA_256);

      auto outcome = state->client->Sign(req);
      if (!outcome.IsSuccess()) {
         throw_kms_error("Sign", state->key_id, outcome.GetError());
      }

      const auto& der_buf = outcome.GetResult().GetSignature();
      const std::span<const unsigned char> der{
         der_buf.GetUnderlyingData(), der_buf.GetLength()};

      const auto compact = der_to_eth_signature(
         der, digest.to_uint8_span(), state->expected_em_pubkey);

      return fc::crypto::signature(
         fc::crypto::signature::storage_type{fc::em::signature_shim{compact}});
   };

   // Startup probe: runs the same one-shot pinning check as the first Sign,
   // but issues only the free GetPublicKey — no billable Sign. An opt-in
   // plugin_startup() calls this so a missing credential, bad region, absent
   // IAM grant, or wrong pinned key fails loudly at boot instead of deep in
   // production. It shares `state` (hence `pin_once`) with `sign`, so enabling
   // the probe never doubles the check.
   std::function<void()> warm_up = [state] { ensure_kms_pubkey_pinned(*state); };

   return kms_signer{.sign = std::move(sign), .warm_up = std::move(warm_up)};
}

std::shared_ptr<Aws::KMS::KMSClient> get_kms_client(const std::string& region) {
   SYS_ASSERT(!region.empty(), chain::plugin_config_exception,
              "get_kms_client: region must not be empty");

   // Force the lifecycle singleton's construction *before* we touch the
   // cache, so its destructor (Aws::ShutdownAPI) runs *after* the cache's
   // destructor (which clears all KMSClient shared_ptrs). Static-init order
   // is the order of first-touch within the same TU; hitting the lifecycle
   // first here pins it as the older static.
   (void)aws_sdk_lifecycle::instance();

   auto& c = kms_clients();
   std::scoped_lock lock{c.mutex};
   auto& slot = c.by_region[region];
   if (!slot) {
      Aws::Client::ClientConfiguration cfg;
      cfg.region = Aws::String{region.c_str()};
      slot = std::make_shared<Aws::KMS::KMSClient>(cfg);
   }
   return slot;
}

} // namespace sysio::sigprov::kms

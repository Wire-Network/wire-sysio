#include <sysio/signature_provider_manager_plugin/kms/kms_signature_provider.hpp>

#include <sysio/signature_provider_manager_plugin/aws/aws_common.hpp>

#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/types.hpp>

#include <fc/crypto/ethereum/ethereum_utils.hpp>
#include <fc/exception/exception.hpp>
#include <fc/string.hpp>

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

#include <array>
#include <memory>
#include <mutex>
#include <span>

// KMSErrors codes begin at 129 (SERVICE_EXTENSION_START_RANGE + 1) and run to
// 176, above magic_enum's default ceiling of 127, so enum_name() returns "" for
// them without a wider range. Scope the widening to this one enum rather than the
// global MAGIC_ENUM_RANGE_MAX macro, which would affect every enum in the TU
// (incl. transitively-included chain enums) and risk an ODR mismatch with other
// TUs at the default range.
template<>
struct magic_enum::customize::enum_range<Aws::KMS::KMSErrors> {
   static constexpr int min = 0;
   static constexpr int max = 256;
};

namespace sysio::sigprov::kms {

namespace {

/// Anchor for ARN detection. ARNs always start with `arn:aws:kms:` (no
/// regional suffix on the partition for kms -- `arn:aws-cn:kms:` and
/// `arn:aws-us-gov:kms:` are not currently in scope; revisit if a partition
/// other than `aws` becomes a deployment target). The service-agnostic ARN
/// pieces (`arn:` lead-in, segment count / indices, the case-insensitive
/// prefix test) are shared with the sibling sub-libraries via
/// `sysio::sigprov::aws` (aws_common.hpp).
constexpr std::string_view kms_arn_prefix = "arn:aws:kms:";

/// Tail prefixes the KMS API accepts for the `KeyId` field.
constexpr std::string_view tail_prefix_key   = "key/";
constexpr std::string_view tail_prefix_alias = "alias/";

/// Process-wide secp256k1 context used by the DER / low-S helpers. Created
/// lazily on first use; libsecp256k1 contexts are thread-safe for the
/// signing-verification operations we use here. Lives separate from libfc's
/// internal context (`fc::em::detail::_get_context`) because that one is
/// not exposed across translation units. It is created with
/// `SECP256K1_CONTEXT_NONE` -- no precomputation tables -- so this second
/// long-lived context costs only a few hundred bytes, not the few hundred KiB
/// a precomputed context would.
const secp256k1_context* kms_secp_ctx() {
   static secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
   return ctx;
}

/// Per-closure state for a KMS-backed signer. Captured by `std::shared_ptr`
/// so that `std::function` copies remain cheap and the same `KMSClient` /
/// expected pubkey are shared across copies of the closure.
///
/// A user-provided constructor is required because `std::mutex` is neither
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
   /// Guard for the GetPublicKey pinning check. The check runs on the first
   /// `Sign` (or the startup probe) and never again once it has passed.
   ///
   /// A `std::mutex` + flag is used deliberately, NOT `std::once_flag`:
   /// `verify_kms_pubkey` throws on a permanent misconfiguration or a transient
   /// API error, and on glibc an exception unwinding through `std::call_once`'s
   /// `pthread_once` aborts the process instead of propagating. With the mutex
   /// the exception propagates cleanly -- so a permanent error fails loudly and
   /// a transient one can be retried -- and `pinned` is set only after success.
   std::mutex                           pin_mutex;
   bool                                 pinned = false;
};

// ---------------------------------------------------------------------------
// X.509 SubjectPublicKeyInfo (DER) decoding for KMS public-key pinning.
//
// AWS KMS `GetPublicKey` returns the key as a DER-encoded SubjectPublicKeyInfo
// (RFC 5280 section 4.1). We walk just enough of that structure to verify the key is
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

/// DER OBJECT IDENTIFIER bodies (the content of the OID TLV -- tag and length
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
/// encoding, so a structural tag-length-value walk is a genuine parse -- not a
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
   [[nodiscard]] element next() {
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
              "KMS public-key DER: algorithm is not id-ecPublicKey -- the KMS key is not an "
              "elliptic-curve key");
   SYS_ASSERT(std::ranges::equal(curve_oid.content, oid_secp256k1),
              chain::plugin_config_exception,
              "KMS public-key DER: EC curve is not secp256k1 -- the KMS key must be created "
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
/// `plugin_config_exception` early -- before any billable `Sign` -- with a
/// message that names the misconfiguration directly. Invoked at most once per
/// closure through `ensure_kms_pubkey_pinned`.
void verify_kms_pubkey(kms_signer_state& state) {
   Aws::KMS::Model::GetPublicKeyRequest req;
   req.SetKeyId(Aws::String{state.key_id});

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

/// Run the public-key pinning check at most once per `state`. Both the first
/// `Sign` and the opt-in startup probe funnel through here. A successful check
/// sets `pinned` so subsequent calls are a cheap no-op; a throwing check (a
/// permanent misconfiguration or a transient API error) leaves `pinned` false
/// so the next call retries -- and, crucially, the exception propagates to the
/// caller rather than aborting the process (see `pin_mutex`). The mutex
/// serialises concurrent first-signs so only one GetPublicKey round-trip runs.
void ensure_kms_pubkey_pinned(kms_signer_state& state) {
   std::scoped_lock lock(state.pin_mutex);
   if (!state.pinned) {
      verify_kms_pubkey(state);   // throws on failure; `pinned` stays false -> retried
      state.pinned = true;
   }
}

} // namespace

[[noreturn]] void throw_kms_error(std::string_view op, std::string_view key_id,
                                  const Aws::Client::AWSError<Aws::KMS::KMSErrors>& err) {
   // Classification (ShouldRetry -> transient vs permanent) and message shape
   // live in the shared `sysio::sigprov::aws::throw_aws_error`; see its doc
   // for the full rationale. This wrapper contributes the KMS service label
   // and the "key" resource noun.
   sigprov::aws::throw_aws_error("KMS", op, "key", key_id, err);
}

kms_key_ref parse_kms_spec(std::string_view spec_data) {
   SYS_ASSERT(!spec_data.empty(), chain::plugin_config_exception,
              "KMS spec body is empty; expected an ARN or '<region>:<key-id-or-alias>'");

   if (spec_data.starts_with(kms_arn_prefix)) {
      // Full ARN form. Split into exactly `aws::arn_segment_count` parts so
      // any stray colons inside the trailing `key/<id>` / `alias/<name>`
      // segment stay glued to it (KMS key ids are uuids with no colons today,
      // but aliases are operator-chosen and must not be silently truncated).
      // The split is only for *validation* below -- the value handed to KMS is
      // the unmodified ARN.
      auto parts = fc::split(spec_data, ':', sigprov::aws::arn_segment_count);
      SYS_ASSERT(parts.size() == sigprov::aws::arn_segment_count, chain::plugin_config_exception,
                 "Malformed KMS ARN \"{}\": expected {} colon-separated segments, got {}",
                 spec_data, sigprov::aws::arn_segment_count, parts.size());

      const auto& region  = parts[sigprov::aws::arn_idx_region];
      const auto& account = parts[sigprov::aws::arn_idx_account];
      const auto& tail    = parts[sigprov::aws::arn_idx_tail];

      // `arn`, `aws`, and `kms` are guaranteed non-empty and correct by the
      // `kms_arn_prefix` match above. The region, account, and tail segments
      // are operator-supplied; an empty one means a stray colon collapsed two
      // segments (e.g. `arn:aws:kms:us-east-1::key/x`), producing a malformed
      // ARN. Reject that here with a precise message rather than after a
      // billable Sign against a bad endpoint.
      SYS_ASSERT(!region.empty(), chain::plugin_config_exception,
                 "KMS ARN \"{}\" has empty region segment", spec_data);
      SYS_ASSERT(!account.empty(), chain::plugin_config_exception,
                 "KMS ARN \"{}\" has empty account-id segment", spec_data);
      SYS_ASSERT(tail.starts_with(tail_prefix_key) || tail.starts_with(tail_prefix_alias),
                 chain::plugin_config_exception,
                 "KMS ARN tail must start with 'key/' or 'alias/', got \"{}\" in \"{}\"",
                 tail, spec_data);
      // Reject bare prefixes: `key/` or `alias/` with nothing after them. The
      // assertion above guarantees `tail` starts with exactly one of the two,
      // so the prefix actually present determines how much to strip.
      const auto name = tail.starts_with(tail_prefix_key)
                           ? tail.substr(tail_prefix_key.size())
                           : tail.substr(tail_prefix_alias.size());
      SYS_ASSERT(!name.empty(), chain::plugin_config_exception,
                 "KMS ARN tail \"{}\" has empty key/alias name", tail);

      // Hand KMS the full ARN, not the `key/<id>` / `alias/<name>` tail. AWS
      // KMS accepts a bare key id, a key ARN, an alias name, or an alias ARN
      // as `KeyId` -- but NOT the bare `key/<uuid>` tail. Passing the whole
      // ARN both keeps key ARNs valid and preserves the account id: an alias
      // ARN stripped to `alias/<name>` would resolve against the *caller's*
      // account and could silently bind a same-named alias for a different
      // key. `region` is still taken from the ARN to build the regional
      // client; it matches the region embedded in the ARN we pass through.
      return kms_key_ref{region, std::string{spec_data}};
   }

   // A spec that begins with `arn:` (any casing) but did not match the
   // supported `arn:aws:kms:` form above is a malformed or out-of-scope ARN,
   // never shorthand. Falling through to the `<region>:<key-id>` parser below
   // would split on the first colon and silently yield region="arn"; AWS then
   // rejects that only at first sign -- with an opaque `InvalidRegion`/endpoint
   // error, after a billable attempt. Fail loudly here instead, naming the
   // offending partition and service. Non-`aws` partitions (`aws-cn`,
   // `aws-us-gov`) are deliberately out of scope; this is the boundary that
   // enforces it. A mis-cased `ARN:AWS:KMS:...` and a typo'd service
   // (`arn:aws:ksm:...`) land here too -- the message points at the canonical
   // form in every case.
   if (sigprov::aws::starts_with_ci(spec_data, sigprov::aws::arn_lead_in)) {
      const auto parts = fc::split(spec_data, ':', sigprov::aws::arn_segment_count);
      std::string partition, service;
      if (parts.size() > sigprov::aws::arn_idx_partition)
         partition = parts[sigprov::aws::arn_idx_partition];
      if (parts.size() > sigprov::aws::arn_idx_service)
         service = parts[sigprov::aws::arn_idx_service];
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
   // ECDSA recovery ids span {0, 1, 2, 3}. Ids 2 and 3 only arise when the
   // signature's `r` had to be reduced modulo the curve order `n` because the
   // ephemeral point's x-coordinate exceeded `n` -- a ~2^-128 event that no
   // compliant ECDSA implementation, AWS KMS included, ever emits. The
   // recoverable set is therefore exactly {0, 1}. Do not widen this bound
   // speculatively: a genuine id of 2/3 would mean KMS returned a
   // non-canonical signature, which is a defect to surface, not to absorb.
   for (unsigned char rec_id = 0; rec_id < 2; ++rec_id) {
      fc::em::compact_signature trial{};
      std::ranges::copy(compact, trial.begin());
      trial[64] = static_cast<unsigned char>(fc::crypto::ethereum::v_offset + rec_id);

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
   out[64] = static_cast<unsigned char>(fc::crypto::ethereum::v_offset + rec_id);
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
   // shapes or curves and need separate plumbing, so they are out of scope
   // for v1.
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
      // Public-key pinning. Before the first -- and only the first -- billable
      // Sign, fetch the KMS key's own public key with the free GetPublicKey
      // API and assert it matches the key pinned in the spec. This turns the
      // common "wrong <public-key> in the spec" mistake into a fast, direct
      // error instead of an opaque recovery failure that would otherwise
      // surface only after a paid Sign. If the opt-in startup probe already
      // ran the check, this is a no-op -- both paths share `state`'s pinning
      // guard through `ensure_kms_pubkey_pinned`.
      ensure_kms_pubkey_pinned(*state);

      // Build a Sign request. MessageType=DIGEST tells KMS the 32 bytes are
      // already a hash; otherwise it would re-hash with SHA-256 and break
      // any chain that hashes with anything other than SHA-256.
      Aws::KMS::Model::SignRequest req;
      req.SetKeyId(Aws::String{state->key_id});
      req.SetMessage(Aws::Utils::ByteBuffer{
         digest.to_uint8_span().data(),
         digest.to_uint8_span().size()});
      req.SetMessageType(Aws::KMS::Model::MessageType::DIGEST);

      // SigningAlgorithm: KMS's `ECC_SECG_P256K1` key spec (the curve Ethereum
      // and Bitcoin use) supports exactly one signing-algorithm value -- this
      // one. This block is the single authoritative explanation of the
      // DIGEST/ECDSA_SHA_256 wire-format choice; the header doc for
      // make_kms_signature_provider defers here rather than restating it.
      // The name is misleading: with `MessageType=DIGEST` set above, KMS
      // does NOT apply SHA-256 to the input. It treats the 32 bytes as an
      // already-hashed value and signs them via raw ECDSA over secp256k1; the
      // "SHA_256" portion of the spec only constrains the input length to 32
      // bytes (the SHA-256 output size), which Ethereum's Keccak-256 digest
      // also satisfies. ECDSA's math is hash-agnostic -- it reduces the
      // 32-byte digest modulo the curve order and emits (r, s) -- so the
      // signature KMS produces over `keccak256(rlp(tx))` is byte-identical to
      // what a local secp256k1 signer would produce over the same 32 bytes.
      // `recover_v` below recovers the signer from each signature and rejects any
      // that does not match the pinned key, so a deviation is caught on every
      // sign; the once-per-provider self-verify in `em_sign_keccak` is a separate,
      // structural check that the keccak digest reached the closure intact.
      req.SetSigningAlgorithm(Aws::KMS::Model::SigningAlgorithmSpec::ECDSA_SHA_256);

      auto outcome = state->client->Sign(req);
      if (!outcome.IsSuccess()) {
         throw_kms_error("Sign", state->key_id, outcome.GetError());
      }

      const auto& der_buf = outcome.GetResult().GetSignature();
      const std::span<const unsigned char> der{
         der_buf.GetUnderlyingData(), der_buf.GetLength()};

      // `der_to_eth_signature` -> `recover_v` recovers the signer to pick the
      // recovery id AND throws if it does not match the pinned key. That runs on
      // every sign and is the per-signature guarantee that KMS signed with the
      // expected key -- keep it. `em_sign_keccak` (the caller) recovers once more,
      // but only on the first sign per provider (gated by
      // `signature_provider_t::self_verified`): a one-time structural check that
      // the keccak digest survived the `sha256`-typed closure boundary. So the
      // first sign does two recoveries and every later sign one; this is the
      // external-chain submission path, not a hot loop, so neither cost matters.
      const auto compact = der_to_eth_signature(
         der, digest.to_uint8_span(), state->expected_em_pubkey);

      return fc::crypto::signature(
         fc::crypto::signature::storage_type{fc::em::signature_shim{compact}});
   };

   // Startup probe: runs the same one-shot pinning check as the first Sign,
   // but issues only the free GetPublicKey -- no billable Sign. An opt-in
   // plugin_startup() calls this so a missing credential, bad region, absent
   // IAM grant, or wrong pinned key fails loudly at boot instead of deep in
   // production. It shares `state` (hence the pinning guard) with `sign`, so
   // enabling the probe never doubles the check.
   std::function<void()> warm_up = [state] { ensure_kms_pubkey_pinned(*state); };

   return kms_signer{.sign = std::move(sign), .warm_up = std::move(warm_up)};
}

std::shared_ptr<Aws::KMS::KMSClient> get_kms_client(const std::string& region) {
   SYS_ASSERT(!region.empty(), chain::plugin_config_exception,
              "get_kms_client: region must not be empty");

   // Function-local static, constructed on first use. Its constructor runs
   // `ensure_aws_sdk_initialized()`, pinning the SDK lifecycle singleton as
   // the older static so Aws::ShutdownAPI runs only after this cache has
   // released its KMSClient shared_ptrs.
   static sigprov::aws::region_client_cache<Aws::KMS::KMSClient> cache;
   return cache.get(region);
}

sysio::provider_spec_result create_kms_provider(
   fc::crypto::chain_key_type_t  key_type,
   const fc::crypto::public_key& expected_pub,
   std::string_view              spec_data) {
   // Parse the `KMS:`-spec body, build the signer (key-type / pubkey pairing
   // is validated up front; no KMS network I/O), and package the two closures
   // into the shape the plugin's registry expects. The `warm_up` callback
   // becomes the generic `startup_probe`, which the plugin runs from
   // `plugin_startup()` when its opt-in startup-probe flag is enabled.
   auto ref = parse_kms_spec(spec_data);
   auto kms = make_kms_signature_provider(ref, key_type, expected_pub);
   return {.signer        = std::move(kms.sign),
           .private_key   = std::nullopt,
           .startup_probe = std::move(kms.warm_up)};
}

} // namespace sysio::sigprov::kms

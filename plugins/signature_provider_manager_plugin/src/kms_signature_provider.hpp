#pragma once

/**
 * AWS KMS-backed signature provider — plugin-private header.
 *
 * The KMS provider extends the existing `KEY:` / `KIOD:` spec grammar with a
 * third form, `KMS:<key-ref>`, where the signing key never leaves AWS.
 *
 * This header is consumed only by the plugin's own translation units and by
 * its tests; it is intentionally not installed under `include/`.
 */

#include <fc/crypto/chain_types_reflect.hpp>
#include <fc/crypto/elliptic_em.hpp>
#include <fc/crypto/keccak256.hpp>
#include <fc/crypto/public_key.hpp>
#include <fc/crypto/signature_provider.hpp>

#include <array>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

/**
 * Forward declarations for the AWS SDK types named in this header's function
 * signatures. The full `<aws/kms/...>` headers pull in a large dependency
 * tree; declaring these types opaquely keeps that tree out of every
 * translation unit that includes this header only for `parse_kms_spec` /
 * `make_kms_signature_provider` — notably `signature_provider_manager_plugin.cpp`,
 * which uses no AWS type at all. The full AWS headers are included only where
 * the complete types are actually needed: `kms_signature_provider.cpp` and the
 * plugin's test translation unit.
 *
 * `KMSClient` is named only as `std::shared_ptr<KMSClient>` and `AWSError`
 * only behind a reference, so an incomplete type suffices for the declarations
 * below. This couples the header to the AWS SDK's (very stable) declaration of
 * these names; the SDK ships no forward-declaration header of its own.
 */
namespace Aws {
namespace KMS {
   class KMSClient;
   enum class KMSErrors;
} // namespace KMS
namespace Client {
   template<typename ERROR_TYPE> class AWSError;
} // namespace Client
} // namespace Aws

namespace sysio::sigprov::kms {

/**
 * @brief Parsed KMS key reference.
 *
 * `key_id` is whatever follows the resolved region — either the bare key id /
 * alias as the operator typed it (`<region>:<key-id-or-alias>` form), or the
 * `key/<uuid>` / `alias/<name>` tail of an ARN. AWS KMS accepts both shapes
 * in the `KeyId` field of `Sign` / `GetPublicKey`, so we hand it through
 * verbatim and let the SDK validate it.
 */
struct kms_key_ref {
   std::string region;
   std::string key_id;
};

/**
 * @brief Parse the body of a `KMS:` provider spec (everything after `KMS:`).
 *
 * Accepted forms:
 *   - Full ARN: `arn:aws:kms:<region>:<account>:(key|alias)/<id>`
 *     Region is taken from the ARN's region segment; `key_id` is the trailing
 *     `key/<id>` or `alias/<name>` portion.
 *   - Shorthand: `<region>:<key-id-or-alias>`
 *     Region is the leading token; everything after the first `:` is `key_id`.
 *
 * Region must always be present in the spec. We do not silently fall back to
 * `AWS_REGION` env or shared-config lookups because misconfiguration there is
 * harder to diagnose than a parse error here.
 *
 * @param spec_data the spec body, e.g. `us-east-1:alias/wire-cranker-eth-01`
 * @throws sysio::chain::plugin_config_exception if the form is empty,
 *         malformed, or omits region
 * @return parsed `kms_key_ref` ready to hand to the AWS SDK
 */
kms_key_ref parse_kms_spec(std::string_view spec_data);

/**
 * @brief Decode a DER-encoded ECDSA signature into 64-byte compact `r || s`.
 *
 * AWS KMS returns ECDSA signatures in ASN.1 DER (`30 ll 02 lr <r> 02 ls <s>`),
 * with `r` and `s` zero-padded by one byte when their MSB is set. This helper
 * normalises any such variation to a fixed-width 32-byte big-endian r/s pair.
 *
 * @param der DER-encoded signature bytes (typically 70-72 bytes)
 * @throws sysio::chain::plugin_config_exception on malformed DER
 * @return 64 bytes `[ r[32] | s[32] ]`
 */
std::array<unsigned char, 64> der_to_compact(std::span<const unsigned char> der);

/**
 * @brief Force `s` into the low half of the secp256k1 curve order (EIP-2).
 *
 * Ethereum (EIP-2) and Bitcoin (BIP-62) reject signatures with `s` in the upper
 * half of the curve order to remove malleability. KMS does not enforce low-S,
 * so we normalise on the way out.
 *
 * @param compact 64-byte `[ r | s ]` to be normalised in place
 * @return true if `s` was high and got flipped, false if it was already low
 */
bool normalise_low_s(std::array<unsigned char, 64>& compact);

/**
 * @brief Find the `recovery_id ∈ {0, 1}` that recovers `expected` from `compact`.
 *
 * KMS does not return a recovery byte. We derive it locally by trying both
 * parities, recovering the candidate public key, and comparing to the pubkey
 * pinned in the spec.
 *
 * The digest is whatever 32 bytes were actually signed by the underlying
 * ECDSA primitive. For Ethereum that is `keccak-256(rlp(tx))`; for any
 * other secp256k1 path it is whatever the caller hashed. The recovery
 * routine treats the bytes as opaque, so the helper takes a generic
 * fixed-size span rather than a typed digest wrapper.
 *
 * @param compact 64-byte `[ r | s ]` (must already be low-S)
 * @param digest the 32-byte digest the signature was produced over
 * @param expected the public key the signer is supposed to control
 * @throws sysio::chain::plugin_config_exception if neither parity recovers `expected`
 * @return `recovery_id` ∈ {0, 1} (Ethereum's `v` byte is `27 + recovery_id`)
 */
unsigned char recover_v(const std::array<unsigned char, 64>& compact,
                        std::span<const std::uint8_t, 32>    digest,
                        const fc::em::public_key&            expected);

/**
 * @brief End-to-end: DER → low-S compact + recovered v → 65-byte Ethereum sig.
 *
 * Wraps `der_to_compact` + `normalise_low_s` + `recover_v` and packs the
 * result as `[ r | s | (27 + recovery_id) ]`, the format consumed by
 * `fc::em::compact_signature` and ethereum-style transaction signing paths.
 *
 * @param der DER-encoded ECDSA signature returned from KMS::Sign
 * @param digest 32-byte digest that was passed to KMS as `MessageType=DIGEST`
 * @param expected_pubkey pubkey from the provider spec, validated against `KMSClient::GetPublicKey`
 * @return ready-to-use 65-byte recoverable signature
 */
fc::em::compact_signature der_to_eth_signature(
   std::span<const unsigned char>      der,
   std::span<const std::uint8_t, 32>   digest,
   const fc::em::public_key&           expected_pubkey);

/**
 * @brief Decode an X.509 SubjectPublicKeyInfo (DER) into a secp256k1 public key.
 *
 * AWS KMS `GetPublicKey` returns the public key as a DER-encoded
 * SubjectPublicKeyInfo (RFC 5280 §4.1): an outer `SEQUENCE` wrapping an
 * `AlgorithmIdentifier` and a `BIT STRING`. This helper walks that structure,
 * verifies the algorithm is `id-ecPublicKey` over the `secp256k1` named curve,
 * and lifts the trailing uncompressed `0x04 || X || Y` point into an
 * `fc::em::public_key`. It backs the KMS public-key pinning check.
 *
 * Because DER is a canonical encoding, the walk is an exact parse rather than a
 * heuristic: anything that is not a well-formed secp256k1 SPKI is rejected.
 *
 * @param spki_der DER bytes of the SubjectPublicKeyInfo (88 bytes for a
 *        well-formed uncompressed secp256k1 key)
 * @throws sysio::chain::plugin_config_exception if the DER is malformed, the
 *         algorithm or curve is not secp256k1, or the EC point is not a valid
 *         uncompressed secp256k1 public key
 * @return the decoded public key
 */
fc::em::public_key spki_der_to_public_key(std::span<const unsigned char> spki_der);

/**
 * @brief Get (or lazily create) a process-wide `KMSClient` for `region`.
 *
 * The first call from the process triggers `Aws::InitAPI`; the SDK is shut
 * down at static destruction (last-on-construct, first-off-destruct relative
 * to this cache, so any `KMSClient` shared from here is destroyed before
 * shutdown). Threadsafe; closures may keep the returned `shared_ptr` for
 * their lifetime, and multiple specs in the same region share a single
 * client (KMS API requests are stateless and the SDK's HTTP pool is
 * thread-safe).
 *
 * Construction is offline: no credential resolution, no network. Credentials
 * are looked up via the standard AWS provider chain on the first KMS API
 * call, not here.
 *
 * @param region AWS region (e.g. `us-east-1`)
 * @return shared `KMSClient` configured for `region`
 */
std::shared_ptr<Aws::KMS::KMSClient> get_kms_client(const std::string& region);

/**
 * @brief Translate a failed AWS KMS API outcome into an fc exception, split by
 *        whether the failure is transient.
 *
 * The AWS SDK classifies every deserialised error as retryable or not — the
 * same classification its own retry strategy uses. `throw_kms_error` maps that
 * split onto two distinct exception types so a caller can react correctly:
 *
 *   - Transient (throttling, `KMSInternal`, dependency / network timeouts,
 *     service-unavailable) -> `sysio::chain::signing_transient_exception`. The
 *     operation may be retried with backoff; the credentials and key are fine.
 *   - Permanent (access denied, key not found, disabled key, invalid state,
 *     bad parameters) -> `sysio::chain::plugin_config_exception`. Retrying will
 *     not help — the operator must fix credentials, IAM, region, or the spec.
 *
 * The two types are siblings, not parent and child, so a handler that catches
 * only `plugin_config_exception` will not silently swallow a retryable error.
 *
 * @param op short label for the failed operation (e.g. "Sign", "GetPublicKey")
 * @param key_id the KMS key id / alias / ARN tail the call targeted
 * @param err the failed outcome's AWS error
 * @throws sysio::chain::signing_transient_exception if `err` is retryable
 * @throws sysio::chain::plugin_config_exception otherwise
 */
[[noreturn]] void throw_kms_error(std::string_view op, std::string_view key_id,
                                  const Aws::Client::AWSError<Aws::KMS::KMSErrors>& err);

/**
 * @brief A KMS-backed signer: the signing closure plus its one-shot startup
 *        probe.
 *
 * `make_kms_signature_provider` returns this pair so a caller can choose to
 * validate the KMS key eagerly at startup instead of lazily on the first sign.
 * Both members share the same underlying state, so the public-key pinning
 * check runs at most once regardless of which one triggers it.
 */
struct kms_signer {
   /// Signing closure, usable wherever `fc::crypto::sign_fn` is expected. Each
   /// call issues one `KMS::Sign`; the first call also runs the public-key
   /// pinning check, unless `warm_up` has already run it. A transient KMS
   /// failure throws `sysio::chain::signing_transient_exception` (safe to retry
   /// with backoff); a permanent one throws
   /// `sysio::chain::plugin_config_exception` (fatal — fix the configuration).
   fc::crypto::sign_fn sign;

   /// Eagerly run the startup probe: a single `KMS::GetPublicKey`
   /// that resolves AWS credentials, warms the client, and verifies the KMS
   /// key matches the pinned public key — without signing. Optional; if never
   /// called, the same check happens lazily on the first `sign`. Idempotent —
   /// it shares the closure's one-shot guard, so calling it never makes the
   /// check run twice. A permanent misconfiguration (missing credential, bad
   /// region, absent IAM grant, mismatched key) throws
   /// `sysio::chain::plugin_config_exception`; a transient failure (throttle,
   /// timeout, `KMSInternal`) throws `sysio::chain::signing_transient_exception`
   /// and the check is left to run again on the first `sign`.
   std::function<void()> warm_up;
};

/**
 * @brief Build a KMS-backed signer (closure + startup probe) for a key.
 *
 * Validates `key_type` and the public-key variant, resolves the shared
 * `KMSClient` for `ref.region` via `get_kms_client`, and captures the client,
 * key id, and expected public key into the returned closure. No network I/O
 * happens here; the first KMS request occurs only when the closure — or the
 * returned `warm_up` probe — is invoked.
 *
 * On its first invocation the closure performs public-key pinning: it calls
 * `KMSClient::GetPublicKey` exactly once, decodes the returned X.509
 * SubjectPublicKeyInfo via `spki_der_to_public_key`, and asserts the KMS
 * key's public key matches `expected_pubkey`. A mismatch
 * throws `chain::plugin_config_exception` immediately — before any billable
 * `Sign` — so a spec that pins the wrong `<public-key>` fails fast with a
 * direct error. The pinning check runs once on success; a transient
 * `GetPublicKey` failure is retried on the next `Sign`.
 *
 * Each invocation sends an `ECDSA_SHA_256` `Sign` request with
 * `MessageType=DIGEST` (so KMS treats the 32-byte input as already hashed
 * rather than re-hashing with SHA-256), decodes KMS's DER signature,
 * normalises it to low-S, recovers the Ethereum `v` byte by trying both
 * parities and matching against `expected_pubkey`, and returns a 65-byte
 * compact signature. If neither parity recovers to the expected key the call
 * throws `chain::plugin_config_exception`; once pinning has passed this is a
 * defence-in-depth check that should never fire.
 *
 * A failed `Sign` or `GetPublicKey` call is classified by retryability: a
 * transient failure (throttle, `KMSInternal`, timeout) throws
 * `chain::signing_transient_exception` and may be retried with backoff; a
 * permanent one throws `chain::plugin_config_exception`. See `throw_kms_error`.
 *
 * v1 only supports secp256k1 keys held as Ethereum public keys
 * (`chain_key_type_ethereum` + `fc::em::public_key_shim`). Other key types
 * raise `chain::pending_impl_exception` at construction; a `public_key`
 * variant that does not hold the Ethereum shim raises
 * `chain::plugin_config_exception`.
 *
 * @param ref parsed `(region, key_id)` pair
 * @param key_type chain key type; must be `chain_key_type_ethereum`
 * @param expected_pubkey public key the operator pinned in the spec; used at
 *        each sign call to recover the `v` byte and to assert that the
 *        signature KMS produced matches that key
 * @return a `kms_signer` bundling the signing closure and the startup probe
 */
kms_signer make_kms_signature_provider(
   const kms_key_ref&            ref,
   fc::crypto::chain_key_type_t  key_type,
   const fc::crypto::public_key& expected_pubkey);

} // namespace sysio::sigprov::kms

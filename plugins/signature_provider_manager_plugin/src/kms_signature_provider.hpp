#pragma once

/**
 * AWS KMS-backed signature provider — plugin-private header.
 *
 * The KMS provider extends the existing `KEY:` / `KIOD:` spec grammar with a
 * third form, `KMS:<key-ref>`, where the signing key never leaves AWS. See
 * `KMS_SIGNING_DESIGN.md` (this directory) for the full design.
 *
 * This header is consumed only by the plugin's own translation units and by
 * its tests; it is intentionally not installed under `include/`.
 */

#include <aws/kms/KMSClient.h>

#include <fc/crypto/chain_types_reflect.hpp>
#include <fc/crypto/elliptic_em.hpp>
#include <fc/crypto/keccak256.hpp>
#include <fc/crypto/public_key.hpp>
#include <fc/crypto/signature_provider.hpp>

#include <array>
#include <memory>
#include <span>
#include <string>
#include <string_view>

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
 * @brief Build a `sign_fn` closure that signs digests with AWS KMS.
 *
 * Stub in this revision: throws `chain::pending_impl_exception` on first
 * invocation. Implementation lands in step 6 of `KMS_SIGNING_DESIGN.md` §10
 * (DER → raw, low-S normalisation, v-recovery, and `KMSClient` lifecycle).
 *
 * @param ref the parsed `(region, key_id)` pair
 * @param key_type chain key type (must be ethereum / wire k1 — secp256k1)
 * @param expected_pubkey public key the operator placed in the spec; the
 *        provider validates it against `KMSClient::GetPublicKey` on first sign
 * @return a `sign_fn` matching `fc::crypto::sign_fn`'s signature
 */
fc::crypto::sign_fn make_kms_signature_provider(
   const kms_key_ref&            ref,
   fc::crypto::chain_key_type_t  key_type,
   const fc::crypto::public_key& expected_pubkey);

} // namespace sysio::sigprov::kms

#pragma once
#include <fc/crypto/private_key.hpp>
#include <fc/crypto/public_key.hpp>

#include <atomic>
#include <memory>

namespace fc::crypto {
using signature_provider_id_t  = std::variant<std::string, fc::crypto::public_key>;

/// Wire default signing function (sha256 digest)
//  NOTE: Really this is a 256-bit hash value, that is either a sha256 hash or keccak256 hash
//        the choice was made earlier to treat the payload as a 256-bit hash and changing all
//        of the plumbing was considered out of scope.
//
//        Because `sha256` is reused to carry keccak-256 bytes, the type system no
//        longer distinguishes the two digests at this boundary -- the Ethereum
//        signing path (`fc::crypto::detail::em_sign_keccak` in signer.hpp)
//        deliberately constructs a `sha256` from a keccak digest's bytes and hands
//        it to this closure. What keeps that safe is the self-verify in
//        `em_sign_keccak`: it recovers the public key from the produced signature
//        and asserts it matches the provider's pinned key, so a closure wired to
//        the wrong bytes (wrong digest, framing, or key) is rejected before its
//        output is used. That recover-and-compare is the compensating control for
//        the lost compile-time digest distinction -- DO NOT remove it as
//        "redundant." It is distinct from the KMS provider's own `recover_v`,
//        which only picks the recovery id inside the closure; this validates, from
//        outside, that the returned signature recovers to the pinned key.
//
//        It runs once per provider: a closure's key and framing are fixed, so the
//        first signature is representative. `signature_provider_t::self_verified`
//        gates it to the first sign and caches the result. The flag is set only
//        AFTER the checks pass, so a transiently bad signer throws and is
//        re-checked on the next sign. A plain atomic flag is used deliberately,
//        NOT `std::call_once`: the checks throw on failure, and on glibc an
//        exception unwinding through `call_once`'s `pthread_once` aborts the
//        process. Concurrent first-signs may each run the cheap, side-effect-free
//        recover, which is harmless.
using sign_fn        = std::function<fc::crypto::signature(const sha256&)>;

/**
 * `signature_provider_entry` constructed provider
 */
struct signature_provider_t  {

   /** The chain/key type */
   fc::crypto::chain_kind_t target_chain;

   /** The chain/key type */
   fc::crypto::chain_key_type_t key_type;

   /** The alias or name assigned to identify this key pair */
   std::string key_name;

   /** The public key component of this key pair */
   fc::crypto::public_key public_key;

   std::optional<fc::crypto::private_key> private_key;

   /// Wire default signing (always set for all key types)
   sign_fn        sign;

   /// One-shot guard for the remote-signer self-verify in
   /// `detail::em_sign_keccak` (signer.hpp). See note above for more detail.
   std::shared_ptr<std::atomic<bool>> self_verified = std::make_shared<std::atomic<bool>>(false);
};

using signature_provider_ptr = std::shared_ptr<signature_provider_t>;

/**
 * Creates a signature provider specification string from individual components.
 *
 * ethereum examples:
 *  ethereum-key-01,ethereum,ethereum,0xfc5422471c9e31a6cd6632a2858eeaab39f9a7eec5f48eedecf53b8398521af1c86c9fce17312900cbb11e2e2ec1fb706598065f855c2f8f2067e1fbc1ba54c8,KEY:0x8f2cdaeb8e036865421c79d4cc42c7704af5cef0f592b2e5c993e2ba7d328248
 *  ethereum-key-02,ethereum,ethereum,0x3a0d6f5e4e7f3a8ce6d5f5c1f3e6e8b9c1d2e3f4a5b6c7d8e9f0a1b2c3d4e5f6a7b8c9d0e1f2030405060708090a0b0c0d,KEY:0x4c0883a69102937d6231471b5dbb6204fe5129617082792ae468d01a3f362318
 *
 * @param key_name Name identifier for the key
 * @param target_chain Target blockchain protocol identifier
 * @param key_type Type of the key (e.g., k1, r1)
 * @param public_key_text Public key in text format
 * @param private_key_provider_spec Private key or provider specification
 * @return A formatted specification string in the format "key_name:chain:key_type:public_key:private_key_spec"
 */
std::string to_signature_provider_spec(const std::string& key_name, fc::crypto::chain_kind_t target_chain,
                                       fc::crypto::chain_key_type_t key_type, const std::string& public_key_text,
                                       const std::string& private_key_provider_spec);
}

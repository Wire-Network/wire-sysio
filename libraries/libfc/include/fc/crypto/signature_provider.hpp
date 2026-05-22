#pragma once
#include <fc/crypto/private_key.hpp>
#include <fc/crypto/public_key.hpp>
#include <fc/crypto/sha256.hpp>
#include <fc/crypto/keccak256.hpp>
#include <fc/exception/exception.hpp>

#include <span>

namespace fc::crypto {
using signature_provider_id_t  = std::variant<std::string, fc::crypto::public_key>;

/// A 32-byte digest tagged with the hash algorithm that produced it. A signing
/// closure receives one of these and can dispatch on the active alternative --
/// e.g. an Ethereum remote signer expects `keccak256`, block signing `sha256`.
using hash256        = std::variant<sha256, keccak256>;

/// Signing closure stored on every `signature_provider_t`. Takes the digest by
/// value: a `hash256` is two 32-byte hashes in a variant -- trivially cheap to
/// copy -- and the closure owns its copy.
using sign_fn        = std::function<fc::crypto::signature(hash256)>;

/// The raw 32 digest bytes of whichever hash `h` carries. For closures that
/// sign an opaque 32-byte digest (AWS KMS, kiod) regardless of hash algorithm.
inline std::span<const uint8_t, 32> digest_span(const hash256& h) {
   return std::visit([](const auto& d) -> std::span<const uint8_t, 32> {
      return d.to_uint8_span();
   }, h);
}

/// The `sha256` alternative of `h`. Throws if `h` carries a `keccak256`. For
/// closures that feed a SHA-256-only API (`private_key::sign`) and are only
/// ever handed a SHA-256 digest -- the assertion catches a wrong-hash bug.
inline const sha256& as_sha256(const hash256& h) {
   const sha256* s = std::get_if<sha256>(&h);
   FC_ASSERT(s, "expected a sha256 digest, but the hash256 carries a keccak256");
   return *s;
}

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

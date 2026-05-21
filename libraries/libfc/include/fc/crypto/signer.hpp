#pragma once

#include <fc-lite/crypto/chain_types.hpp>
#include <fc/exception/exception.hpp>
#include <fc/crypto/signature_provider.hpp>
#include <fc/crypto/ethereum/ethereum_utils.hpp>
#include <fc/crypto/keccak256.hpp>
#include <fc/crypto/sha256.hpp>

#include <magic_enum/magic_enum.hpp>

#include <span>

namespace fc::crypto {

// ===========================================================================
// signer_traits -- compile-time signing dispatch per (TargetChain, KeyType)
// ===========================================================================

/// Primary template -- must be specialized for each valid (TargetChain, KeyType) pair
template<chain_kind_t TargetChain, chain_key_type_t KeyType>
struct signer_traits;

namespace detail {

/// Sign a 32-byte keccak digest with an ethereum (secp256k1) key, transparently
/// supporting both `signature_provider_t` shapes:
///
///  - A provider with a local `em` private key signs in-process.
///  - A provider with no local key -- a remote signer such as AWS KMS -- has no
///    key to sign with directly, so the digest is handed to its `sign` closure.
///    `sign` is typed on `fc::sha256`, but the closure treats that argument as
///    an opaque 32-byte digest, so the keccak digest is carried across
///    byte-for-byte and raw-signed.
///
/// Digest preparation (plain keccak vs. EIP-191 framing) is the caller's job --
/// it has already happened in `signer_traits::prepare` -- so the remote signer
/// must raw-sign the digest as given. The remote path self-verifies this:
/// it recovers the public key from the returned signature and asserts it
/// matches the provider's key. A remote signer that applied its own message
/// framing instead of raw-signing, or that signed with the wrong key, recovers
/// to a different key and is rejected here -- before an invalid transaction can
/// be emitted -- rather than yielding a silently bad signature.
inline signature em_sign_keccak(const signature_provider_t& p, const keccak256& digest) {
   if (p.private_key) {
      FC_ASSERT(p.private_key->contains<em::private_key_shim>(),
                "ETH signing requires an EM private key");
      const auto& em_key = p.private_key->get<em::private_key_shim>();
      return signature(signature::storage_type(em_key.sign_keccak256(digest)));
   }

   FC_ASSERT(static_cast<bool>(p.sign),
             "signature provider has neither a local private key nor a sign function");

   // Carry the keccak digest into the sha256-typed closure argument unchanged.
   const auto digest_bytes = digest.to_char_span();
   const signature sig     = p.sign(sha256(digest_bytes.data(), digest_bytes.size()));

   // Self-verify: the remote signer must have raw-signed `digest` with the
   // provider's key. Recover and compare; reject anything else loudly.
   FC_ASSERT(sig.contains<em::signature_shim>(),
             "remote signer returned a non-Ethereum signature");
   const auto recovered = public_key(public_key::storage_type(
      sig.get<em::signature_shim>().recover_eth(digest)));
   FC_ASSERT(recovered == p.public_key,
             "remote signer produced a signature that does not recover to the "
             "provider's public key -- the signer's key or digest framing is wrong");
   return sig;
}

} // namespace detail

// ---------------------------------------------------------------------------
// (ethereum, ethereum) -- ETH client transaction signing
// Signs: keccak256(raw_bytes) via EM (secp256k1)
// ---------------------------------------------------------------------------
template<>
struct signer_traits<chain_kind_ethereum, chain_key_type_ethereum> {
   using input_type    = std::span<const uint8_t>;
   using prepared_type = keccak256;
   static constexpr bool recoverable = true;

   static prepared_type prepare(input_type data) {
      return keccak256::hash(data);
   }

   static signature do_sign(const signature_provider_t& p, const prepared_type& digest) {
      // Sign with a local em key, or delegate to a remote signer's closure.
      return detail::em_sign_keccak(p, digest);
   }

   static public_key do_recover(const signature& sig, const prepared_type& digest) {
      FC_ASSERT(sig.contains<em::signature_shim>(), "ETH recovery requires an EM signature");
      auto& em_sig = sig.get<em::signature_shim>();
      return public_key(public_key::storage_type(em_sig.recover_eth(digest)));
   }
};

// ---------------------------------------------------------------------------
// (wire, ethereum) -- Wire transactions signed via MetaMask / personal_sign
// Signs: keccak256(EIP-191 prefix + sha256_raw) via EM (secp256k1)
// ---------------------------------------------------------------------------
template<>
struct signer_traits<chain_kind_wire, chain_key_type_ethereum> {
   using input_type    = sha256;
   using prepared_type = keccak256;
   static constexpr bool recoverable = true;

   static prepared_type prepare(const sha256& digest) {
      return ethereum::hash_user_message(digest.to_uint8_span());
   }

   static signature do_sign(const signature_provider_t& p, const prepared_type& digest) {
      // Sign with a local em key, or delegate to a remote signer's closure.
      return detail::em_sign_keccak(p, digest);
   }

   static public_key do_recover(const signature& sig, const prepared_type& digest) {
      FC_ASSERT(sig.contains<em::signature_shim>(), "ETH recovery requires an EM signature");
      auto& em_sig = sig.get<em::signature_shim>();
      return public_key(public_key::storage_type(em_sig.recover_eth(digest)));
   }
};

// ---------------------------------------------------------------------------
// (solana, solana) -- Solana client transaction signing
// Signs: raw_bytes via ED25519
// ---------------------------------------------------------------------------
template<>
struct signer_traits<chain_kind_solana, chain_key_type_solana> {
   using input_type    = std::span<const uint8_t>;
   using prepared_type = std::span<const uint8_t>;
   static constexpr bool recoverable = false;

   static prepared_type prepare(input_type data) {
      return data;
   }

   static signature do_sign(const signature_provider_t& p, prepared_type data) {
      FC_ASSERT(p.private_key, "Solana signing requires a local private key");
      FC_ASSERT(p.private_key->contains<ed::private_key_shim>(), "Solana signing requires an ED private key");
      auto& ed_key = p.private_key->get<ed::private_key_shim>();
      return signature(signature::storage_type(ed_key.sign_raw(data.data(), data.size())));
   }

   static bool do_verify(const public_key& pub, const signature& sig, prepared_type data) {
      FC_ASSERT(sig.contains<ed::signature_shim>(), "Solana verification requires an ED signature");
      FC_ASSERT(pub.contains<ed::public_key_shim>(), "Solana verification requires an ED public key");
      auto& ed_sig = sig.get<ed::signature_shim>();
      auto& ed_pub = pub.get<ed::public_key_shim>();
      return ed_sig.verify_solana(data.data(), data.size(), ed_pub);
   }
};

// ===========================================================================
// signer<TargetChain, KeyType> -- typed cross-chain signing wrapper
// ===========================================================================

template<chain_kind_t TargetChain, chain_key_type_t KeyType>
struct signer {
   using traits = signer_traits<TargetChain, KeyType>;

   const signature_provider_t& provider;

   explicit signer(const signature_provider_t& p) : provider(p) {
      FC_ASSERT(p.key_type == KeyType,
         "signer: provider key_type mismatch (expected {}, got {})",
         magic_enum::enum_name(KeyType), magic_enum::enum_name(p.key_type));
   }

   signature sign(typename traits::input_type data) const {
      auto prepared = traits::prepare(data);
      return traits::do_sign(provider, prepared);
   }

   /// Recover public key from signature (recoverable key types only)
   public_key recover(const signature& sig, typename traits::input_type data) const
      requires (traits::recoverable)
   {
      auto prepared = traits::prepare(data);
      return traits::do_recover(sig, prepared);
   }

   /// Verify signature against provider's public key (non-recoverable key types only)
   bool verify(const signature& sig, typename traits::input_type data) const
      requires (!traits::recoverable)
   {
      auto prepared = traits::prepare(data);
      return traits::do_verify(provider.public_key, sig, prepared);
   }
};

// ===========================================================================
// wire_signer -- key-type agnostic Wire transaction signing
// Passes through to provider.sign(sha256); handles K1/EM/ED polymorphically
// ===========================================================================

/**
 * @brief Key-type-agnostic Wire transaction signer.
 *
 * Hands the 32-byte digest straight to `provider.sign(...)`, which dispatches
 * polymorphically over K1 / EM / ED keys inside the provider's closure.
 *
 * Unlike `signer<>` (and its `wire_eth_signer` alias), `wire_signer` has no
 * `prepare()` hook: it raw-signs exactly the 32 bytes it is given and applies
 * no EIP-191 framing. For an Ethereum key that framing
 * (`ethereum::hash_user_message`) lives in
 * `signer_traits<chain_kind_wire, chain_key_type_ethereum>::prepare`, not in
 * the signature provider -- a `KEY:` provider's `em::private_key_shim` and a
 * `KMS:` provider alike raw-sign the digest as handed in. Consequently
 * `wire_signer` and `wire_eth_signer` are NOT interchangeable even for an
 * identical provider: swapping one for the other changes which bytes are
 * signed. Use `wire_eth_signer` when the Wire transaction must carry a
 * MetaMask-compatible `personal_sign` signature; use `wire_signer` only when
 * the caller has already produced the exact digest to be signed.
 */
struct wire_signer {
   const signature_provider_t& provider;

   explicit wire_signer(const signature_provider_t& p) : provider(p) {}

   signature sign(const sha256& digest) const {
      return provider.sign(digest);
   }
};

// ===========================================================================
// Type aliases
// ===========================================================================

/// Signs Ethereum transactions (keccak256 hash of raw bytes)
using eth_client_signer = signer<chain_kind_ethereum, chain_key_type_ethereum>;

/// Signs Wire transactions with EM keys (EIP-191 prefixed keccak256)
using wire_eth_signer = signer<chain_kind_wire, chain_key_type_ethereum>;

/// Signs Solana transactions (ED25519 over raw bytes)
using sol_client_signer = signer<chain_kind_solana, chain_key_type_solana>;

} // namespace fc::crypto

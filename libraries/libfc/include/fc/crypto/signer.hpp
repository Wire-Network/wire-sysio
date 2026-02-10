#pragma once

#include <fc-lite/crypto/chain_types.hpp>
#include <fc/exception/exception.hpp>
#include <fc/crypto/signature_provider.hpp>
#include <fc/crypto/ethereum/ethereum_utils.hpp>
#include <fc/crypto/keccak256.hpp>
#include <fc/crypto/sha256.hpp>

#include <span>

namespace fc::crypto {

// ===========================================================================
// signer_traits — compile-time signing dispatch per (TargetChain, KeyType)
// ===========================================================================

/// Primary template — must be specialized for each valid (TargetChain, KeyType) pair
template<chain_kind_t TargetChain, chain_key_type_t KeyType>
struct signer_traits;

// ---------------------------------------------------------------------------
// (ethereum, ethereum) — ETH client transaction signing
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
      FC_ASSERT(p.private_key, "ETH signing requires a local private key");
      FC_ASSERT(p.private_key->contains<em::private_key_shim>(), "ETH signing requires an EM private key");
      auto& em_key = p.private_key->get<em::private_key_shim>();
      return signature(signature::storage_type(em_key.sign_keccak256(digest)));
   }

   static public_key do_recover(const signature& sig, const prepared_type& digest) {
      FC_ASSERT(sig.contains<em::signature_shim>(), "ETH recovery requires an EM signature");
      auto& em_sig = sig.get<em::signature_shim>();
      return public_key(public_key::storage_type(em_sig.recover_eth(digest)));
   }
};

// ---------------------------------------------------------------------------
// (wire, ethereum) — Wire transactions signed via MetaMask / personal_sign
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
      FC_ASSERT(p.private_key, "ETH signing requires a local private key");
      FC_ASSERT(p.private_key->contains<em::private_key_shim>(), "ETH signing requires an EM private key");
      auto& em_key = p.private_key->get<em::private_key_shim>();
      return signature(signature::storage_type(em_key.sign_keccak256(digest)));
   }

   static public_key do_recover(const signature& sig, const prepared_type& digest) {
      FC_ASSERT(sig.contains<em::signature_shim>(), "ETH recovery requires an EM signature");
      auto& em_sig = sig.get<em::signature_shim>();
      return public_key(public_key::storage_type(em_sig.recover_eth(digest)));
   }
};

// ---------------------------------------------------------------------------
// (solana, solana) — Solana client transaction signing
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
// signer<TargetChain, KeyType> — typed cross-chain signing wrapper
// ===========================================================================

template<chain_kind_t TargetChain, chain_key_type_t KeyType>
struct signer {
   using traits = signer_traits<TargetChain, KeyType>;

   const signature_provider_t& provider;

   explicit signer(const signature_provider_t& p) : provider(p) {
      FC_ASSERT(p.key_type == KeyType,
         "signer: provider key_type mismatch (expected {}, got {})",
         static_cast<int>(KeyType), static_cast<int>(p.key_type));
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
// wire_signer — key-type agnostic Wire transaction signing
// Passes through to provider.sign(sha256); handles K1/EM/ED polymorphically
// ===========================================================================

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

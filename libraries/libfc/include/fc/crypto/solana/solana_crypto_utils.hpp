#pragma once

#include <fc/crypto/elliptic_ed.hpp>
#include <fc/crypto/public_key.hpp>
#include <array>
#include <cstdint>
#include <string>

/**
 * Solana-flavored aliases and helpers over fc::crypto::ed.
 *
 * A Solana "pubkey" is just a 32-byte ed25519 public key — structurally identical
 * to fc::crypto::ed::public_key_shim. A Solana-wire "signature" is the raw 64-byte
 * ed25519 signature without fc's self-contained 96-byte (pubkey+sig) envelope.
 *
 * Domain-general properties of ed25519 keys (on-curve / zero checks) live in
 * fc::crypto::ed and are reachable here via ADL.
 */
namespace fc::crypto::solana {

/**
 * Solana pubkey — reuses the ed25519 shim directly (32 bytes).
 * Named `solana_public_key` (rather than bare `public_key`) to avoid
 * clashing with `fc::crypto::public_key` in files that do
 * `using namespace fc::crypto;`.
 */
using solana_public_key = fc::crypto::ed::public_key_shim;

/** Solana wire-format signature (64 bytes, no embedded pubkey). */
using solana_signature = std::array<uint8_t, 64>;

// NOTE: Base58 encode/decode for `solana_public_key` uses the shim's own methods:
//   `pk.to_string(fc::yield_function_t{})` and `solana_public_key::from_base58_string(s)`.
// No Solana-specific wrappers are provided for pubkeys to avoid duplicating that API.

/** Base58 encode a Solana wire-format signature. */
std::string to_base58(const solana_signature& sig);

/** Decode a Solana wire-format signature from base58. Throws on wrong length. */
solana_signature signature_from_base58(const std::string& str);

/** Convert the ED variant of fc::crypto::public_key into a Solana pubkey. */
solana_public_key from_fc_public_key(const fc::crypto::public_key& pk);

/**
 * Strip the 32-byte embedded pubkey prefix from fc's ed signature_shim to
 * obtain the 64-byte Solana wire-format signature.
 */
solana_signature from_ed_signature(const fc::crypto::ed::signature_shim& sig);

} // namespace fc::crypto::solana

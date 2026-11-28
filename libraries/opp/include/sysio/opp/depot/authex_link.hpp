#pragma once

#include <sysio/opp/types/types.pb.h>

#include <fc/crypto/private_key.hpp>
#include <fc/crypto/public_key.hpp>
#include <fc/crypto/sha256.hpp>
#include <fc/crypto/signature.hpp>

#include <cstdint>
#include <string>
#include <string_view>

/**
 * @file
 * Host-side builder for the ownership proof that `sysio.authex::createlink` checks.
 *
 * createlink links an external (Ethereum or Solana) key to a Wire account only with a signature, made by that
 * key, over the message
 *
 *     <public key>|<account>|<chain kind>|<nonce>|createlink auth
 *
 * - `<public key>` is `PUB_EM_` plus the lowercase hex of the 33-byte compressed secp256k1 point for an EM key,
 *   or `PUB_ED_` plus the base58 of the raw 32-byte ed25519 point for an ED key. This is the contract's own
 *   spelling of the key, not fc's display form (fc prints an EM key as its uncompressed `PUB_EM_04...` point).
 * - `<chain kind>` is the decimal value of the `ChainKind` enum: EVM for an EM key, SVM for an ED key.
 * - `<nonce>` is milliseconds since the Unix epoch. The contract accepts it only when it is at or before the
 *   head block time and at most `nonce_max_age_ms` older.
 *
 * What the key signs depends on its curve, and matches what the external wallet produces:
 * - EM: the 32-byte keccak256 of the message. fc's EM signer wraps those bytes in the EIP-191 personal_sign
 *   envelope, as MetaMask's `personal_sign` does.
 * - ED: sha256 of the message with every byte mapped into printable ASCII (`byte % 94 + 33`). fc's ED signer
 *   signs the lowercase hex of those 32 bytes, as a Solana wallet's `signMessage` does over that hex text.
 *
 * Keep in lockstep with `authex::createlink` in `contracts/sysio.authex/src/sysio.authex.cpp`, and with
 * `prepareCreateLink` / `createLinkSigningPayload` in wire-libraries-ts `packages/sdk-core`.
 */
namespace sysio::opp::depot::authex_link {

/// Fixed last field of every createlink message.
inline constexpr std::string_view auth_suffix = "createlink auth";

/// Separator between the message's fields.
inline constexpr char field_separator = '|';

/// Prefix of an EM key in the message; the compressed point follows as lowercase hex.
inline constexpr std::string_view em_key_prefix = "PUB_EM_";

/// Prefix of an ED key in the message; the raw point follows as base58.
inline constexpr std::string_view ed_key_prefix = "PUB_ED_";

/// First character of the printable-ASCII range an ED digest byte is mapped into.
inline constexpr unsigned ed_digest_printable_min = 33;

/// Number of characters in the printable-ASCII range an ED digest byte is mapped into (33 to 126).
inline constexpr unsigned ed_digest_printable_range = 94;

/// Oldest nonce the contract accepts, relative to the head block time, in milliseconds (10 minutes).
inline constexpr uint64_t nonce_max_age_ms = 10 * 60 * 1000;

/**
 * The chain a key links on: an EM key links EVM, an ED key links SVM.
 *
 * @param key the external key to link.
 * @return    `CHAIN_KIND_EVM` or `CHAIN_KIND_SVM`.
 * @throws fc::invalid_arg_exception for any other key type; createlink accepts no other pairing.
 */
types::ChainKind chain_kind_for(const fc::crypto::public_key& key);

/**
 * The key as the contract spells it inside the message: `PUB_EM_` + hex of the compressed point, or
 * `PUB_ED_` + base58 of the raw point.
 *
 * @throws fc::invalid_arg_exception for a key that is neither EM nor ED.
 */
std::string message_public_key(const fc::crypto::public_key& key);

/**
 * The full createlink message.
 *
 * The chain kind is a parameter rather than derived from the key, so a test can build the message the contract
 * would reconstruct for a mismatched pairing. For a valid proof pass `chain_kind_for(key)`.
 *
 * @param key        the external key to link (EM or ED).
 * @param account    the Wire account, in its canonical name spelling; the caller validates it.
 * @param chain_kind the chain kind written into the message.
 * @param nonce      milliseconds since the Unix epoch.
 */
std::string build_message(const fc::crypto::public_key& key, std::string_view account, types::ChainKind chain_kind,
                          uint64_t nonce);

/**
 * The 32 bytes an external key of this type signs for `message`: keccak256 for EM, the printable-mapped sha256
 * for ED. fc's EM and ED signers add the wallet envelope (EIP-191, or the hex text) on top.
 *
 * @throws fc::invalid_arg_exception for a key that is neither EM nor ED.
 */
fc::sha256 signing_digest(std::string_view message, const fc::crypto::public_key& key);

/**
 * Sign a createlink proof.
 *
 * @param key        the external private key (EM or ED).
 * @param account    the Wire account, in its canonical name spelling.
 * @param chain_kind the chain kind written into the message; `chain_kind_for(key.get_public_key())` for a
 *                   proof the contract accepts.
 * @param nonce      milliseconds since the Unix epoch.
 * @return the `SIG_EM_` or `SIG_ED_` signature for createlink's `sig` field.
 */
fc::crypto::signature sign(const fc::crypto::private_key& key, std::string_view account, types::ChainKind chain_kind,
                           uint64_t nonce);

/// Sign a createlink proof for the key's own chain kind (`chain_kind_for`).
fc::crypto::signature sign(const fc::crypto::private_key& key, std::string_view account, uint64_t nonce);

} // namespace sysio::opp::depot::authex_link

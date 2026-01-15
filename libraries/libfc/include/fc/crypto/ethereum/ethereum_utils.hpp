#pragma once

#include <string>
#include <vector>

#include <fc/crypto/public_key.hpp>

// Forward declarations in the case of circular dependencies
namespace fc::em {
   class public_key;
   class private_key;
}

/**
 * Utilities for interacting with ethereum keys
 */
namespace fc::crypto::ethereum {

/**
 * Keccak-256 hash type used for Ethereum cryptography storage
 */
using keccak256_hash_t = std::array<std::uint8_t, 32>;

/**
 * Create a 32-byte keccak256 hash
 *
 * @param data to digest
 * @param size number of bytes to digest
 * @return 32 byte keccak256 hash
 */
keccak256_hash_t keccak256(const uint8_t* data, size_t size);

keccak256_hash_t keccak256(const std::string& digest);

/**
 * Public key lengths
 */
constexpr std::array public_key_string_lengths = {64,66,128,130};

/**
 * Standard Ethereum message prefix used for signing according to EIP-155
 */
constexpr std::string_view crc155_message_prefix{"\x19" "Ethereum Signed Message:\n"};

/**
 * Removes '0x' prefix and leading zeros from a hex string
 * @param hex The hex string to trim
 * @return The trimmed hex string
 */
std::string trim(const std::string& hex);

/**
 * Calls `trim` and then checks if the size is 66 or 130, in
 * which case it removes the first hex byte (2 characters),
 * used to denote the public key data type,
 * 0x02 - compressed, Y = even
 * 0x03 - compressed, Y = odd
 * 0x04 - uncompressed
 *
 * @param hex The public key hex string to trim
 * @return The trimmed public key hex string
 */
std::string trim_public_key(const std::string& hex);

/**
 * Converts a byte vector to its hexadecimal string representation
 * @param bytes The byte vector to convert
 * @return Hexadecimal string representation
 */
std::string bytes_to_hex(const std::vector<uint8_t>& bytes);

/**
 * Converts a hexadecimal string to its byte vector representation
 * @param hex The hex string to convert
 * @return Byte vector representation
 */
std::vector<uint8_t> hex_to_bytes(const std::string& hex);

/**
 * Creates an Ethereum signed message hash from a message payload
 * @param payload The message payload to hash
 * @return The message hash
 */
em::message_hash_type hash_message(const em::message_body_type& payload);

em::message_hash_type hash_user_message(const em::message_body_type& payload);

/**
 * Parses a hexadecimal public key string into a public key object
 * @param pubkey_hex The public key in hexadecimal format
 * @return The parsed public key object
 */
fc::em::public_key to_em_public_key(const std::string& pubkey_hex);

fc::em::private_key to_em_private_key(const std::string& privkey_hex);
fc::em::compact_signature to_em_signature(const std::string& signature_hex);
}

#include <fc/crypto/elliptic_em.hpp>

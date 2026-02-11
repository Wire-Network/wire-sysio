#pragma once

#include <fc/crypto/public_key.hpp>
#include <string>
#include <vector>

// Forward declarations in the case of circular dependencies
namespace fc::ed {
class public_key;
class private_key;
} // namespace fc::ed

/**
 * Utilities for interacting with ethereum keys
 */
namespace fc::crypto::solana {

// Forward declarations
struct solana_public_key;
struct solana_signature;

/**
 * @brief 32-byte Solana public key (base58 encoded in Solana)
 */
struct solana_public_key {
   static constexpr size_t SIZE = 32;
   std::array<uint8_t, SIZE> data{};

   solana_public_key() = default;
   explicit solana_public_key(const std::array<uint8_t, SIZE>& d)
      : data(d) {}
   explicit solana_public_key(const uint8_t* d) { std::copy_n(d, SIZE, data.begin()); }

   /**
    * @brief Convert public key to base58 string
    */
   std::string to_base58() const;

   /**
    * @brief Create public key from base58 string
    */
   static solana_public_key from_base58(const std::string& str);

   /**
    * @brief Create public key from fc::crypto::public_key (ED25519)
    */
   static solana_public_key from_public_key(const fc::crypto::public_key& pk);

   /**
    * @brief Create public key from ED25519 public key shim
    */
   static solana_public_key from_ed_public_key(const fc::crypto::ed::public_key_shim& pk);

   /**
    * @brief Check if the public key is all zeros (default/uninitialized)
    */
   bool is_zero() const;

   bool operator==(const solana_public_key& other) const { return data == other.data; }
   bool operator!=(const solana_public_key& other) const { return data != other.data; }
   bool operator<(const solana_public_key& other) const { return data < other.data; }
};

/**
 * @brief 64-byte Solana signature
 */
struct solana_signature {
   static constexpr size_t SIZE = 64;
   std::array<uint8_t, SIZE> data{};

   solana_signature() = default;
   explicit solana_signature(const std::array<uint8_t, SIZE>& d)
      : data(d) {}
   explicit solana_signature(const uint8_t* d) { std::copy_n(d, SIZE, data.begin()); }

   /**
    * @brief Convert signature to base58 string
    */
   std::string to_base58() const;

   /**
    * @brief Create signature from base58 string
    */
   static solana_signature from_base58(const std::string& str);

   /**
    * @brief Create signature from ED25519 signature shim
    */
   static solana_signature from_ed_signature(const fc::crypto::ed::signature_shim& sig);

   bool operator==(const solana_signature& other) const { return data == other.data; }
   bool operator!=(const solana_signature& other) const { return data != other.data; }
};

/**
 * @brief Check if an address is on the ed25519 curve
 *
 * @param address The address to check
 * @return True if the address is on the curve, false otherwise
 */
bool is_on_curve(const solana_public_key& address);
} // namespace fc::crypto::solana

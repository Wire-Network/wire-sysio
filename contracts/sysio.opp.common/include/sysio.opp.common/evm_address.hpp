#pragma once
/**
 * @file evm_address.hpp
 * @brief Shared validation and derivation helpers for canonical EVM addresses.
 */

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include <sysio/crypto_ext.hpp>

namespace sysio::opp {

/** Number of bytes in a canonical EVM address. */
inline constexpr std::size_t evm_address_size = 20;

/** Number of bytes in an uncompressed SEC1 secp256k1 public key. */
inline constexpr std::size_t evm_uncompressed_public_key_size = 65;

/** SEC1 prefix identifying an uncompressed public key. */
inline constexpr std::uint8_t evm_uncompressed_public_key_prefix = 0x04;

/**
 * Derive the canonical EVM address from an uncompressed SEC1 secp256k1 public key.
 *
 * The address is the trailing 20 bytes of keccak256(X || Y). Invalid length or prefix returns
 * `std::nullopt` so trust-OPP callers can soft-drop malformed input without aborting dispatch.
 *
 * @param public_key Pointer to the SEC1-encoded public-key bytes.
 * @param public_key_size Number of bytes available at `public_key`.
 * @return The canonical 20-byte address, or `std::nullopt` when the key is not uncompressed SEC1.
 */
inline std::optional<std::vector<char>> evm_address_from_uncompressed_key(
   const char* public_key, std::size_t public_key_size) {
   if (public_key_size != evm_uncompressed_public_key_size
       || static_cast<std::uint8_t>(public_key[0]) != evm_uncompressed_public_key_prefix) {
      return std::nullopt;
   }

   const auto hash = keccak(public_key + 1, public_key_size - 1).extract_as_byte_array();
   return std::vector<char>(hash.end() - evm_address_size, hash.end());
}

} // namespace sysio::opp

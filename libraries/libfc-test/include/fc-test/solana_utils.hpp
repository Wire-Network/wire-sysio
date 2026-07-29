#pragma once

#include <fc/crypto/base58.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace fc::test {

/**
 * Return a deterministic canonical base58 value with Solana's 32-byte hash shape.
 *
 * This helper is suitable for genesis hashes, recent blockhashes, and public
 * keys in tests that do not need cryptographically meaningful contents.
 */
inline std::string solana_hash(uint8_t seed) {
   const std::vector<char> bytes(32, static_cast<char>(seed));
   return fc::to_base58(bytes, fc::yield_function_t{});
}

} // namespace fc::test

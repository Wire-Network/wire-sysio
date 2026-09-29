#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace sysio::finalizer_registration {

/// Versioned envelope containing a standard PoP and an account-bound signature.
inline constexpr std::string_view proof_prefix = "REG_BLS_V1:";
/// Ordinary BLS signature encoding: prefix plus base64url(192 affine bytes + checksum).
inline constexpr std::string_view signature_prefix = "SIG_BLS_";
inline constexpr size_t signature_text_size = 270;
inline constexpr size_t public_key_size = 96;
inline constexpr size_t account_size = sizeof(uint64_t);
inline constexpr size_t proof_size = proof_prefix.size() + 2 * signature_text_size + 1;
/// Distinguishes registration signatures from other uses of the ordinary BLS signing suite.
inline constexpr std::string_view message_domain = "WIRE:sysio.system:regfinkey:v1";

/**
 * Build the registration message without a prehash or serialization length prefixes.
 * The bytes are domain ASCII (no NUL), account uint64 little-endian, then the canonical
 * 96-byte affine little-endian public key. Key containers must have exactly 96 bytes.
 */
template<typename Byte>
std::string message(uint64_t account, const std::array<Byte, public_key_size>& public_key) {
   static_assert(sizeof(Byte) == 1, "finalizer public key elements must be bytes");
   std::string result(message_domain);
   result.reserve(message_domain.size() + account_size + public_key_size);
   for (size_t i = 0; i < account_size; ++i)
      result.push_back(static_cast<char>((account >> (8 * i)) & 0xff));
   result.append(reinterpret_cast<const char*>(public_key.data()), public_key.size());
   return result;
}

/// Check the fixed envelope framing before decoding either independently checksummed signature.
inline bool has_valid_format(std::string_view proof) {
   return proof.size() == proof_size && proof.substr(0, proof_prefix.size()) == proof_prefix &&
          proof.substr(proof_prefix.size(), signature_prefix.size()) == signature_prefix &&
          proof[proof_prefix.size() + signature_text_size] == ':' &&
          proof.substr(proof_prefix.size() + signature_text_size + 1, signature_prefix.size()) == signature_prefix;
}

} // namespace sysio::finalizer_registration

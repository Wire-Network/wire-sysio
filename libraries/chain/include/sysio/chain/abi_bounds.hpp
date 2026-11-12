#pragma once

#include <cstddef>
#include <cstdint>

namespace sysio::chain {

/// Deterministic publication limits for the known packed ABI fields, independent of ABI semantics.
namespace abi_limits {
inline constexpr uint32_t max_packed_bytes = 1024 * 1024;
inline constexpr uint32_t max_decoded_bytes = 16 * 1024 * 1024;
inline constexpr uint32_t max_elements = 1024 * 1024;
inline constexpr uint32_t max_depth = 32;
} // namespace abi_limits

/** Check packed ABI bounds without allocating decoded containers or validating type references.
 * Empty input clears an ABI. Opaque trailing bytes count toward the packed-size limit.
 * Decoded storage is bounded using fixed charges: 256 bytes per non-byte vector slot and twice (string length + 1).
 */
void check_abi_bounds(const char* data, size_t size);

} // namespace sysio::chain

#pragma once
#include <cstdint>

/// Shared contract/driver identifiers for the deadline and rollback probes.
namespace ed25519_test {
inline constexpr uint32_t marker_table = 123;
inline constexpr char marker_key = 'e';
inline constexpr char marker_value = 'v';
inline constexpr char ready_message[] = "ed25519 batch ready";
inline constexpr uint32_t stress_count = 32'768;
inline constexpr uint32_t wasm_page_size = 65'536;
} // namespace ed25519_test

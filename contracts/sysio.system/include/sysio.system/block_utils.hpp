#pragma once

#include <sysio/crypto.hpp>

#include <cstdint>

namespace sysiosystem::block_info {

inline uint32_t block_height_from_id(const sysio::checksum256& block_id) {
   auto arr = block_id.extract_as_byte_array();
   return (static_cast<uint32_t>(arr[0]) << 0x18) | (static_cast<uint32_t>(arr[1]) << 0x10) |
          (static_cast<uint32_t>(arr[2]) << 0x08) | static_cast<uint32_t>(arr[3]);
}

} // namespace sysiosystem::block_info
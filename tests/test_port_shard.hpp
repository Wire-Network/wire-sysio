#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>

namespace sysio::testing {

inline constexpr const char* test_port_offset_env_var = "SYSIO_TEST_PORT_OFFSET";
inline constexpr uint16_t default_state_history_port = 8080;
inline constexpr uint32_t compact_shard_anchor_port = 8888;
inline constexpr uint32_t compact_wallet_first_port = 9899;
inline constexpr uint32_t compact_wallet_last_port = 9999;
inline constexpr uint32_t compact_alternate_first_port = 9776;
inline constexpr uint32_t compact_alternate_last_port = 9822;
inline constexpr uint32_t compact_p2p_first_port = 9876;
inline constexpr uint32_t compact_p2p_last_port = 9898;
inline constexpr uint32_t compact_wallet_slot = 0;
inline constexpr uint32_t compact_bios_http_slot = 100;
inline constexpr uint32_t compact_ship_slot = 150;
inline constexpr uint32_t compact_state_history_slot = 151;
inline constexpr uint32_t compact_alternate_service_slot = 152;
inline constexpr uint32_t compact_alternate_p2p_slot = 153;
inline constexpr uint32_t compact_http_slot = 200;
inline constexpr uint32_t compact_p2p_slot = 225;

/** Return the current test's port shard offset from the CTest environment. */
inline uint32_t test_port_offset() {
   const char* raw_offset = std::getenv(test_port_offset_env_var);
   if(raw_offset == nullptr || raw_offset[0] == '\0')
      return 0;

   try {
      return static_cast<uint32_t>(std::stoul(raw_offset));
   } catch(const std::exception& ex) {
      throw std::runtime_error(std::string(test_port_offset_env_var) + " must be an unsigned integer: " + ex.what());
   }
}

/** Apply the current test's compact port shard mapping to a base port. */
inline uint16_t shard_port(uint16_t port) {
   const uint32_t offset = test_port_offset();
   if(offset == 0)
      return port;

   const uint32_t shard_base = compact_shard_anchor_port + offset;
   uint32_t shifted_port = static_cast<uint32_t>(port) + offset;

   switch(port) {
   case 8788:
      shifted_port = shard_base + compact_bios_http_slot;
      break;
   case 7899:
      shifted_port = shard_base + compact_ship_slot;
      break;
   case default_state_history_port:
      shifted_port = shard_base + compact_state_history_slot;
      break;
   case 9011:
      shifted_port = shard_base + compact_alternate_service_slot;
      break;
   case 8888:
      shifted_port = shard_base + compact_http_slot;
      break;
   default:
      if(compact_alternate_first_port <= port && port <= compact_alternate_last_port) {
         shifted_port = shard_base + compact_alternate_p2p_slot + (port - compact_alternate_first_port);
      } else if(compact_p2p_first_port <= port && port <= compact_p2p_last_port) {
         shifted_port = shard_base + compact_p2p_slot + (port - compact_p2p_first_port);
      } else if(compact_wallet_first_port <= port && port <= compact_wallet_last_port) {
         shifted_port = shard_base + compact_wallet_slot + std::min<uint32_t>(port - compact_wallet_first_port, 99);
      }
      break;
   }

   if(shifted_port > std::numeric_limits<uint16_t>::max())
      throw std::runtime_error(std::string(test_port_offset_env_var) + " shifts the requested port outside uint16_t");
   return static_cast<uint16_t>(shifted_port);
}

/** Return the default state-history websocket endpoint for this test shard. */
inline std::string default_state_history_endpoint() {
   return std::string("127.0.0.1:") + std::to_string(shard_port(default_state_history_port));
}

} // namespace sysio::testing

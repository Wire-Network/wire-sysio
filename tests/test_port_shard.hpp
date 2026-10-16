#pragma once

#include <charconv>
#include <array>
#include <set>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace sysio::testing {

inline constexpr const char* test_port_offset_env_var = "SYSIO_TEST_PORT_OFFSET";
inline constexpr uint32_t default_state_history_port = 8080;
inline constexpr uint32_t compact_shard_anchor_port = 8888;
inline constexpr uint32_t compact_http_first_port = 8888;
inline constexpr uint32_t compact_http_last_port = 8975;
inline constexpr uint32_t compact_bios_p2p_port = 9776;
inline constexpr uint32_t compact_alternate_first_port = 9777;
inline constexpr uint32_t compact_alternate_last_port = 9822;
inline constexpr uint32_t compact_p2p_first_port = 9876;
inline constexpr uint32_t compact_p2p_last_port = 9898;
inline constexpr uint32_t compact_wallet_first_port = 9899;
inline constexpr uint32_t compact_wallet_last_port = 9903;
// These unsharded transaction-only defaults intentionally preserve the legacy overlap with wallet ports 9902/9903.
inline constexpr uint32_t compact_transaction_only_first_port = 9902;
inline constexpr uint32_t compact_transaction_only_last_port = 9903;
inline constexpr uint32_t compact_ipv6_probe_first_port = 9997;
inline constexpr uint32_t compact_ipv6_probe_last_port = 10000;
inline constexpr uint32_t compact_ship_slot = 0;
inline constexpr uint32_t compact_state_history_slot = 1;
inline constexpr uint32_t compact_bios_http_slot = 2;
inline constexpr uint32_t compact_http_slot = 3;
inline constexpr uint32_t compact_alternate_service_slot = 91;
inline constexpr uint32_t compact_plugin_http_peer_slot = 92;
inline constexpr uint32_t compact_plugin_http_local_slot = 93;
inline constexpr uint32_t compact_bios_p2p_slot = 94;
inline constexpr uint32_t compact_alternate_p2p_slot = 95;
inline constexpr uint32_t compact_p2p_slot = 141;
inline constexpr uint32_t compact_wallet_base_slot = 164;
inline constexpr uint32_t wallet_port_count = 5;
inline constexpr uint32_t transaction_only_port_count =
    compact_transaction_only_last_port - compact_transaction_only_first_port + 1;
inline constexpr uint32_t compact_transaction_only_slot = compact_wallet_base_slot + wallet_port_count;
inline constexpr uint32_t compact_ipv6_probe_slot = compact_transaction_only_slot + transaction_only_port_count;

/** Logical listener classes inside a test's compact port shard. */
enum class port_category {
   ship,
   state_history,
   bios_http,
   bios_p2p,
   node_http,
   alternate_service,
   plugin_http_peer,
   plugin_http_local,
   alternate_p2p,
   p2p,
   wallet,
   transaction_only,
   ipv6_probe
};

/** Return the current test's port shard offset from the CTest environment. */
inline uint32_t test_port_offset() {
   const char* raw_offset = std::getenv(test_port_offset_env_var);
   if(raw_offset == nullptr || raw_offset[0] == '\0')
      return 0;

   std::string_view offset_view{raw_offset};
   if(offset_view.front() == '-')
      throw std::runtime_error(std::string(test_port_offset_env_var) + " must be non-negative");

   uint32_t offset = 0;
   const auto* begin = offset_view.data();
   const auto* end = begin + offset_view.size();
   auto [ptr, ec] = std::from_chars(begin, end, offset);
   if(ec != std::errc{} || ptr != end)
      throw std::runtime_error(std::string(test_port_offset_env_var) + " must be an unsigned 32-bit integer");

   return offset;
}

/** Validate the full issued lease before any port can be used. */
inline std::array<uint16_t, 192> parse_registered_ports(std::string_view value) {
   std::array<uint16_t, 192> result;
   std::set<uint16_t> unique;
   for(auto& port : result) {
      const auto delimiter = value.find(',');
      const auto field = value.substr(0, delimiter);
      uint32_t number = 0;
      const auto [end, error] = std::from_chars(field.data(), field.data() + field.size(), number);
      if(error != std::errc{} || end != field.data() + field.size() || number == 0 || number > 65535 ||
         !unique.insert(static_cast<uint16_t>(number)).second)
         throw std::runtime_error("Invalid or duplicate port in BindConfigProvider lease");
      port = static_cast<uint16_t>(number);
      if(delimiter == std::string_view::npos) {
         if(&port != &result.back())
            throw std::runtime_error("BindConfigProvider lease must contain exactly 192 ports");
         value = {};
      } else {
         value.remove_prefix(delimiter + 1);
         if(&port == &result.back())
            throw std::runtime_error("BindConfigProvider lease contains extra ports");
      }
   }
   return result;
}

/** Return the registry-issued port for a listener class and index. */
inline uint16_t get_port(port_category category, uint32_t index = 0) {
   uint32_t slot_start = 0;
   uint32_t slot_count = 1;

   switch(category) {
   case port_category::ship:
      slot_start = compact_ship_slot;
      break;
   case port_category::state_history:
      slot_start = compact_state_history_slot;
      break;
   case port_category::bios_http:
      slot_start = compact_bios_http_slot;
      break;
   case port_category::bios_p2p:
      slot_start = compact_bios_p2p_slot;
      break;
   case port_category::node_http:
      slot_start = compact_http_slot;
      slot_count = compact_http_last_port - compact_http_first_port + 1;
      break;
   case port_category::alternate_service:
      slot_start = compact_alternate_service_slot;
      break;
   case port_category::plugin_http_peer:
      slot_start = compact_plugin_http_peer_slot;
      break;
   case port_category::plugin_http_local:
      slot_start = compact_plugin_http_local_slot;
      break;
   case port_category::alternate_p2p:
      slot_start = compact_alternate_p2p_slot;
      slot_count = compact_alternate_last_port - compact_alternate_first_port + 1;
      break;
   case port_category::p2p:
      slot_start = compact_p2p_slot;
      slot_count = compact_p2p_last_port - compact_p2p_first_port + 1;
      break;
   case port_category::wallet:
      slot_start = compact_wallet_base_slot;
      slot_count = wallet_port_count;
      break;
   case port_category::transaction_only:
      slot_start = compact_transaction_only_slot;
      slot_count = transaction_only_port_count;
      break;
   case port_category::ipv6_probe:
      slot_start = compact_ipv6_probe_slot;
      slot_count = compact_ipv6_probe_last_port - compact_ipv6_probe_first_port + 1;
      break;
   }

   if(index >= slot_count)
      throw std::runtime_error("port category index is outside the compact test shard range");

   static const auto ports = [] {
      const char* raw = std::getenv("SYSIO_TEST_PORTS");
      if(raw == nullptr)
         throw std::runtime_error("Run network tests through tests/bind_registry.py to obtain a BindConfigProvider lease");
      return parse_registered_ports(raw);
   }();
   return ports.at(slot_start + index);
}

/** Return the default state-history websocket endpoint for this test shard. */
inline std::string default_state_history_endpoint() {
   return std::string("127.0.0.1:") + std::to_string(get_port(port_category::state_history));
}

} // namespace sysio::testing

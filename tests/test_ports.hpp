#pragma once

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

/**
 * Listener ports for native network tests, taken from the test's bind-registry lease.
 *
 * CTest runs every network test under `tests/bind_registry.py`, which leases one block of consecutive ports from the
 * host's bind registry and passes it in `SYSIO_TEST_PORTS`. Test code asks for a port by listener category and index;
 * the category picks a fixed slot range inside the lease. The slot layout matches `Utils.getPort` in
 * `tests/TestHarness/testUtils.py`.
 */
namespace sysio::testing {

/** Environment variable carrying the comma-separated lease. */
inline constexpr const char* test_ports_env_var = "SYSIO_TEST_PORTS";
inline constexpr char test_ports_separator = ',';

/** Logical listener classes inside a test's lease. */
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
   ipv6_probe,
   raw_client // not listeners: source ports of the raw P2P sessions tests/TestHarness/raw_p2p.py dials
};

/** The slots a category occupies in the lease: `[start, start + count)`. */
struct port_slots {
   uint32_t start;
   uint32_t count;
};

/** Return the slot range of a listener category. */
constexpr port_slots slots_of(port_category category) {
   switch(category) {
   case port_category::ship:              return {0, 1};
   case port_category::state_history:     return {1, 1};
   case port_category::bios_http:         return {2, 1};
   case port_category::node_http:         return {3, 88};
   case port_category::alternate_service: return {91, 1};
   case port_category::plugin_http_peer:  return {92, 1};
   case port_category::plugin_http_local: return {93, 1};
   case port_category::bios_p2p:          return {94, 1};
   case port_category::alternate_p2p:     return {95, 46};
   case port_category::p2p:               return {141, 23};
   case port_category::wallet:            return {164, 5};
   case port_category::transaction_only:  return {169, 2};
   case port_category::ipv6_probe:        return {171, 4};
   case port_category::raw_client:        return {175, 16};
   }
   throw std::logic_error("unknown port category");
}

/**
 * Parse a `SYSIO_TEST_PORTS` value: a non-empty, comma-separated list of consecutive ports, each in 1..65535.
 * @throws std::runtime_error when the value is malformed.
 */
inline std::vector<uint16_t> parse_test_ports(std::string_view value) {
   std::vector<uint16_t> ports;
   while(true) {
      const auto separator = value.find(test_ports_separator);
      const auto field = value.substr(0, separator);
      uint32_t port = 0;
      const auto [end, error] = std::from_chars(field.data(), field.data() + field.size(), port);
      if(error != std::errc{} || end != field.data() + field.size() || port == 0 ||
         port > std::numeric_limits<uint16_t>::max())
         throw std::runtime_error(std::string(test_ports_env_var) + " holds an invalid port: '" + std::string(field) +
                                  "'");
      if(!ports.empty() && port != ports.back() + 1u)
         throw std::runtime_error(std::string(test_ports_env_var) + " must hold consecutive ports");
      ports.push_back(static_cast<uint16_t>(port));
      if(separator == std::string_view::npos)
         return ports;
      value.remove_prefix(separator + 1);
   }
}

/**
 * Return this test's leased port for a listener class and index.
 * @throws std::runtime_error when the test runs without a lease, or the index is outside the category.
 */
inline uint16_t get_port(port_category category, uint32_t index = 0) {
   const port_slots slots = slots_of(category);
   if(index >= slots.count)
      throw std::runtime_error("port index " + std::to_string(index) + " is outside its category's " +
                               std::to_string(slots.count) + " slots");
   const char* lease = std::getenv(test_ports_env_var);
   if(lease == nullptr)
      throw std::runtime_error(std::string(test_ports_env_var) +
                               " is unset: run this test through ctest or `python3 tests/bind_registry.py <test>`");
   const auto ports = parse_test_ports(lease);
   if(slots.start + index >= ports.size())
      throw std::runtime_error(std::string(test_ports_env_var) + " holds " + std::to_string(ports.size()) +
                               " ports, too few for slot " + std::to_string(slots.start + index));
   return ports[slots.start + index];
}

/** Return the state-history websocket endpoint for this test's lease. */
inline std::string default_state_history_endpoint() {
   return std::string("127.0.0.1:") + std::to_string(get_port(port_category::state_history));
}

} // namespace sysio::testing

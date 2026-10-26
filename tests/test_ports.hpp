#pragma once

#include <array>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <ostream>
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
   ipv6_probe
};

/** The slots a category occupies in the lease: `[start, start + count)`. */
struct port_slots {
   uint32_t start;
   uint32_t count;
};

/** One port for a listener category that has no indexed peers. */
inline constexpr uint32_t single_port_count = 1;
/** Node HTTP ports reserved for nodes and local HTTP fixtures. */
inline constexpr uint32_t node_http_port_count = 88;
/** Alternate P2P ports reserved for alternate-topology fixtures. */
inline constexpr uint32_t alternate_p2p_port_count = 46;
/** Primary P2P ports reserved for node peers and local P2P fixtures. */
inline constexpr uint32_t p2p_port_count = 23;
/** Wallet ports reserved for kiod processes. */
inline constexpr uint32_t wallet_port_count = 5;
/** Transaction-only ports reserved for producer connections. */
inline constexpr uint32_t transaction_only_port_count = 2;
/** IPv6 probe ports reserved for address-family tests. */
inline constexpr uint32_t ipv6_probe_port_count = 4;

/** A listener category and the number of consecutive lease slots it owns. */
struct port_layout_entry {
   port_category category;
   uint32_t count;
};

/** Authoritative listener layout in lease-slot order. */
inline constexpr std::array port_layout{
   port_layout_entry{port_category::ship,              single_port_count          },
   port_layout_entry{port_category::state_history,     single_port_count          },
   port_layout_entry{port_category::bios_http,         single_port_count          },
   port_layout_entry{port_category::node_http,         node_http_port_count       },
   port_layout_entry{port_category::alternate_service, single_port_count          },
   port_layout_entry{port_category::plugin_http_peer,  single_port_count          },
   port_layout_entry{port_category::plugin_http_local, single_port_count          },
   port_layout_entry{port_category::bios_p2p,          single_port_count          },
   port_layout_entry{port_category::alternate_p2p,     alternate_p2p_port_count   },
   port_layout_entry{port_category::p2p,               p2p_port_count             },
   port_layout_entry{port_category::wallet,            wallet_port_count          },
   port_layout_entry{port_category::transaction_only,  transaction_only_port_count},
   port_layout_entry{port_category::ipv6_probe,        ipv6_probe_port_count      },
};

/** Return the slot range of a listener category. */
constexpr port_slots slots_of(port_category category) {
   uint32_t start = 0;
   for (const auto& entry : port_layout) {
      if (entry.category == category)
         return {.start = start, .count = entry.count};
      start += entry.count;
   }
   throw std::logic_error("unknown port category");
}

/**
 * Parse a `SYSIO_TEST_PORTS` value: a non-empty, comma-separated list of consecutive ports, each in 1..65535.
 * @throws std::runtime_error when the value is malformed.
 */
inline std::vector<uint16_t> parse_test_ports(std::string_view value) {
   std::vector<uint16_t> ports;
   while (true) {
      const auto separator = value.find(test_ports_separator);
      const auto field = value.substr(0, separator);
      uint32_t port = 0;
      const auto [end, error] = std::from_chars(field.data(), field.data() + field.size(), port);
      if (error != std::errc{} || end != field.data() + field.size() || port == 0 ||
          port > std::numeric_limits<uint16_t>::max())
         throw std::runtime_error(std::string(test_ports_env_var) + " holds an invalid port: '" + std::string(field) +
                                  "'");
      if (!ports.empty() && port != ports.back() + 1u)
         throw std::runtime_error(std::string(test_ports_env_var) + " must hold consecutive ports");
      ports.push_back(static_cast<uint16_t>(port));
      if (separator == std::string_view::npos)
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
   if (index >= slots.count)
      throw std::runtime_error("port index " + std::to_string(index) + " is outside its category's " +
                               std::to_string(slots.count) + " slots");
   const char* lease = std::getenv(test_ports_env_var);
   if (lease == nullptr)
      throw std::runtime_error(std::string(test_ports_env_var) +
                               " is unset: run this test through ctest or `python3 tests/bind_registry.py <test>`");
   const auto ports = parse_test_ports(lease);
   if (slots.start + index >= ports.size())
      throw std::runtime_error(std::string(test_ports_env_var) + " holds " + std::to_string(ports.size()) +
                               " ports, too few for slot " + std::to_string(slots.start + index));
   return ports[slots.start + index];
}

/** Allocate successive ports from one category of this process's registry lease. */
class test_port_allocator {
public:
   /** Select the listener category this allocator consumes. */
   explicit test_port_allocator(port_category category)
      : category(category) {}

   /** Return the next reserved port in the selected category. */
   uint16_t next() {
      std::lock_guard lock(mutex);
      return get_port(category, next_index++);
   }

   /** Return an HTTP loopback URL using the next reserved port. */
   std::string next_loopback_url() { return std::string(loopback_http_prefix) + std::to_string(next()); }

private:
   /** Canonical HTTP loopback prefix used by local test servers. */
   inline static constexpr std::string_view loopback_http_prefix = "http://127.0.0.1:";

   /** Listener category whose slots this allocator owns. */
   port_category category;
   /** Next category-relative slot to return. */
   uint32_t next_index = 0;
   /** Serializes allocation when test servers start concurrently. */
   std::mutex mutex;
};

/** Return the state-history websocket endpoint for this test's lease. */
inline std::string default_state_history_endpoint() {
   return std::string("127.0.0.1:") + std::to_string(get_port(port_category::state_history));
}

/**
 * Resolve an omitted state-history endpoint from this process's registry lease.
 *
 * @return false after reporting a missing/invalid lease; true when `endpoint` is ready to use.
 */
inline bool resolve_state_history_endpoint(std::string& endpoint, std::ostream& error) {
   if (!endpoint.empty())
      return true;
   try {
      endpoint = default_state_history_endpoint();
      return true;
   } catch (const std::runtime_error& exception) {
      error << exception.what() << " (or pass --socket-address)" << std::endl;
      return false;
   }
}

} // namespace sysio::testing

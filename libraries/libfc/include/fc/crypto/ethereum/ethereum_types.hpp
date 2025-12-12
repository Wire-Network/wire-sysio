#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <span>
#include <array>
#include <sstream>
#include <iomanip>

#include <fc/int256.hpp>
#include <fc/crypto/hex.hpp>

namespace fc::crypto::ethereum {

constexpr std::uint8_t ethereum_eip1559_tx_type = 0x02;

using bytes = std::vector<std::uint8_t>;
using bytes32 = std::array<std::uint8_t, 32>;
using bytes_span = std::span<std::uint8_t>;
using address = std::array<std::uint8_t, 20>;
using address_compat_type = std::variant<std::string, address>;

inline address to_address(const address_compat_type& addr_var) {
   if (std::holds_alternative<address>(addr_var))
      return std::get<address>(addr_var);
   auto addr_str = fc::trim_hex_prefix(std::get<std::string>(addr_var));
   FC_ASSERT(addr_str.size() == 40);
   address addr;

   FC_ASSERT(fc::from_hex(addr_str, reinterpret_cast<char*>(addr.data()), addr.size()) == addr.size());
   return addr;
}

inline bytes address_to_bytes(const address_compat_type& addr_var) {
   auto addr = to_address(addr_var);
   return bytes(addr.begin(), addr.end());
}
struct access_list_entry {
   address addr;               // 20 bytes
   std::vector<bytes32> storage_keys;  // each 32 bytes
};

struct eip1559_tx {
   fc::uint256 chain_id;
   fc::uint256 nonce;
   fc::uint256 max_priority_fee_per_gas;
   fc::uint256 max_fee_per_gas;
   fc::uint256 gas_limit;
   bytes to;
   fc::uint256 value;
   bytes data;
   std::vector<access_list_entry> access_list;
   fc::uint256 v;
   bytes32 r;
   bytes32 s;
};

}

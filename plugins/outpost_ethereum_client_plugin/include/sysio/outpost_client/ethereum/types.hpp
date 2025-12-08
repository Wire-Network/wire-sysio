#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <span>
#include <array>
#include <sstream>
#include <iomanip>

#include <fc/int256.hpp>

namespace sysio::outpost_client::ethereum {
using bytes = std::vector<std::uint8_t>;
using bytes32 = std::array<std::uint8_t, 32>;
using address = std::array<std::uint8_t, 20>;

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
};

}

#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <span>
#include <array>
#include <sstream>
#include <iomanip>
#include <ethash/keccak.hpp>

#include <fc/int256.hpp>
#include <fc/crypto/elliptic_em.hpp>
#include <fc/crypto/hex.hpp>
#include <fc/crypto/public_key.hpp>

namespace fc::crypto::ethereum {

constexpr std::uint8_t ethereum_eip1559_tx_type = 0x02;

using bytes = std::vector<std::uint8_t>;
using bytes32 = std::array<std::uint8_t, 32>;
using bytes_span = std::span<std::uint8_t>;
using address = std::array<std::uint8_t, 20>;
using address_compat_type = std::variant<std::string, address, fc::crypto::public_key, fc::em::public_key, fc::em::public_key_data>;

inline ethereum::address to_address(const address_compat_type& addr_var) {
   if (std::holds_alternative<address>(addr_var))
      return std::get<address>(addr_var);

   if (std::holds_alternative<std::string>(addr_var)) {
      auto addr_str = fc::trim_hex_prefix(std::get<std::string>(addr_var));
      FC_ASSERT(addr_str.size() == 40);
      address addr;

      FC_ASSERT(fc::from_hex(addr_str, reinterpret_cast<char*>(addr.data()), addr.size()) == addr.size());
      return addr;
   }
   em::public_key_data_uncompressed pub_key_data;
   if (std::holds_alternative<fc::crypto::public_key>(addr_var)) {
      const auto& pub_key = std::get<fc::crypto::public_key>(addr_var);

      FC_ASSERT(pub_key.contains<em::public_key_shim>());

      auto em_pub_key_shim = pub_key.get<em::public_key_shim>();
      pub_key_data = em_pub_key_shim.unwrapped().serialize_uncompressed();
   } else if (std::holds_alternative<fc::em::public_key>(addr_var)) {
      auto em_pub_key = std::get<fc::em::public_key>(addr_var);
      pub_key_data = em_pub_key.serialize_uncompressed();
   } else if (std::holds_alternative<fc::em::public_key_data>(addr_var)) {
      em::public_key em_pub_key(std::get<fc::em::public_key_data>(addr_var));
      pub_key_data = em_pub_key.serialize_uncompressed();
   } else {
      FC_THROW_EXCEPTION(fc::invalid_arg_exception, "Unsupported address type");
   }
   // NOTE: Computing the public key hash requires starting at data[1], so
   //  The source data pointer is incremented by 1 as the size is decremented by 1
   auto pub_key_hash = ethash::keccak256(reinterpret_cast<const uint8_t*>(pub_key_data.data() + 1), pub_key_data.size() - 1);

   // Construct the address from the last 20 bytes of the public key hash
   ethereum::address addr;
   static_assert(addr.size() == 20);

   constexpr auto pub_key_hash_size = sizeof(pub_key_hash.bytes);
   static_assert(pub_key_hash_size >= addr.size());

   // Copy the last 20 bytes, which is the address
   std::copy_n(pub_key_hash.bytes + pub_key_hash_size - addr.size(), addr.size(), addr.data());
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
   address to;
   fc::uint256 value;
   bytes data;
   std::vector<access_list_entry> access_list;
   fc::uint256 v;
   bytes32 r;
   bytes32 s;
};

}

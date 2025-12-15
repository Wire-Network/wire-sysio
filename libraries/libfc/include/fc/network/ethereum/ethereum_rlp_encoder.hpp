#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <span>
#include <array>

#include <fc/exception/exception.hpp>
#include <fc/crypto/ethereum/ethereum_types.hpp>

namespace fc::network::ethereum::rlp {
using namespace fc::crypto;
using namespace fc::crypto::ethereum;


// ---------------------------------------------------------
// Helpers
// ---------------------------------------------------------

// using rlp_input_data = std::variant<bytes, bytes32, bytes_span>;
//
// using rlp_input_data_items = std::vector<rlp_input_data>;

// 1. Forward declare the recursive struct
struct rlp_input_data;

// 2. Define the container for the recursive case
//    std::vector is guaranteed to support incomplete types since C++17
using rlp_input_data_items = std::vector<rlp_input_data>;

// 3. Define the underlying variant type, including the recursive container
using rlp_input_variant = std::variant<bytes, bytes32, bytes_span, address, rlp_input_data_items>;

// 4. Define the struct inheriting from the variant
struct rlp_input_data : rlp_input_variant {
   // Inherit constructors (allows implicit construction from inner types)
   using rlp_input_variant::rlp_input_variant;

   // Inherit assignment operators
   using rlp_input_variant::operator=;
};


void append(bytes& out, std::vector<rlp_input_variant>& in_vars);
void append(bytes& out, const std::uint8_t* data, std::size_t len);

template <typename T>
concept not_rlp_input_vector = !std::is_same_v<std::decay_t<T>, std::vector<rlp::rlp_input_variant>>;

template <typename... Args>
   requires (not_rlp_input_vector<std::tuple_element_t<0, std::tuple<Args...>>> &&
             !std::is_pointer_v<std::tuple_element_t<0, std::tuple<Args...>>>)
void append(bytes& out, Args&&... args) {
   std::vector<rlp_input_variant> items = {std::forward<Args>(args)...};
   append(out, items);
}

void append_byte(bytes& out, std::uint8_t b);

bytes encode_length(std::size_t len, std::size_t offset);

// ---------------------------------------------------------
// Core RLP encoders
// ---------------------------------------------------------

bytes encode_bytes(const address& data);
bytes encode_bytes(bytes32& b);
// bytes encode_bytes(std::span<std::uint8_t> data);
// bytes encode_bytes(std::span<std::uint8_t>& data);
bytes encode_bytes(const std::span<std::uint8_t>& data);
bytes encode_bytes(const bytes32& b);
bytes encode_bytes(const bytes& b);
bytes encode_bytes(const std::uint8_t* data, std::size_t len);

bytes encode_string(std::string& s);

// Ethereum-style uint (0 => empty byte string => 0x80)

// inline rlp_input_data encode_uint(fc::uint256 value) {
//    if (value == 0) {
//       bytes empty;
//       return encode_bytes(empty);
//    }
//
//    bytes buf;
//    bool started = false;
//    auto size = value.backend().size();
//    for (int shift = (size - 8); shift >= 0; shift -= 8) {
//       std::uint8_t byte = static_cast<std::uint8_t>((value >> shift) & 0xff);
//       if (byte == 0 && !started) continue;
//       started = true;
//       buf.push_back(byte);
//    }
//
//    return encode_bytes(buf);
// }

// Ethereum-style uint (0 => empty byte string => 0x80)
template <typename T>
bytes encode_uint(T value) {
   if constexpr (std::is_same_v<T, std::uint64_t>) {
      return encode_uint<fc::uint256>(value);
   } else if constexpr (std::is_same_v<T, fc::uint256>) {
      if (value == 0) {
         bytes empty;
         return encode_bytes(empty);
      }

      bytes buf;
      bool  started = false;

      for (int shift = 56; shift >= 0; shift -= 8) {
         std::uint8_t byte = static_cast<std::uint8_t>((value >> shift) & 0xff);
         if (byte == 0 && !started)
            continue;
         started = true;
         buf.push_back(byte);
      }

      return encode_bytes(buf);
   }
   FC_THROW_EXCEPTION(fc::exception, "Unsupported type for encode_uint");
}

bytes encode_access_list(const std::vector<access_list_entry>& access_list);
bytes encode_list(std::vector<rlp_input_variant> items);

// ---------------------------------------------------------
// Generic encode(T)
// ---------------------------------------------------------

template <typename T>
bytes encode(T& value);

template <>
inline bytes encode<bytes>(bytes& value) {
   return encode_bytes(value);
}

template <>
inline bytes encode<std::string>(std::string& value) {
   return encode_string(value);
}

template <>
inline bytes encode<std::uint64_t>(std::uint64_t& value) {
   return encode_uint(value);
}

template <typename... Ts>
bytes make_list(const Ts&... args) {
   std::vector<rlp_input_variant> items;
   items.reserve(sizeof...(Ts));
   (items.push_back(encode(args)), ...);
   return encode_list(items);
}

// ---------------------------------------------------------
// Hex helpers
// ---------------------------------------------------------

std::string to_hex(const bytes& b, bool prefixed = true);
std::string to_hex(const bytes32& b, bool prefixed = true);

std::string to_hex(std::size_t num, bool prefixed = false);

bytes from_hex_no_prefix(const std::string& hex);

bytes from_hex_any(const std::string& hex);


// RLP of the unsigned tx body (for signing):
// [chainId, nonce, maxPriorityFeePerGas, maxFeePerGas,
//  gasLimit, to, value, data, accessList]
bytes encode_eip1559_unsigned(const eip1559_tx& tx);

bytes encode_eip1559_unsigned_typed(const eip1559_tx& tx);
// RLP of the *signed* tx body (EIP-1559):
// [chainId, nonce, maxPriorityFeePerGas, maxFeePerGas,
//  gasLimit, to, value, data, accessList, yParity, r, s]
bytes encode_eip1559_signed(const eip1559_tx& tx);

// Final typed-transaction wire encoding: 0x02 || rlp(signed_body)
bytes encode_eip1559_signed_typed(const eip1559_tx& tx);


} // namespace eth
/**
 * @file ethereum_abi.cpp
 * @brief Implementation of Ethereum ABI encoding and decoding utilities
 *
 * This file provides functionality to encode and decode Ethereum smart contract calls
 * according to the Ethereum ABI specification. It handles various data types including
 * numeric types, addresses, bytes, strings, arrays, and tuples, supporting both static
 * and dynamic encoding.
 */

#include <algorithm>
#include <boost/multiprecision/cpp_int.hpp>
#include <cctype>
#include <fc/crypto/ethereum/ethereum_utils.hpp>
#include <fc/crypto/hex.hpp>
#include <fc/io/json.hpp>
#include <fc/network/ethereum/ethereum_abi.hpp>
#include <fc/string.hpp>
#include <optional>
#include <ranges>
#include <regex>
#include <sstream>

namespace fc::network::ethereum {

using namespace fc::crypto;
using namespace fc::crypto::ethereum;
using boost::multiprecision::cpp_int;

// Anonymous namespace for internal helpers (instead of marking them static)
namespace {


/**
 * @brief Pads a byte vector to 32 bytes with leading zeros (big-endian)
 *
 * @param in Input byte vector to pad
 * @return 32-byte vector with the input data right-aligned and zero-padded on the left
 */
std::vector<uint8_t> be_pad_left_32(const std::vector<uint8_t>& in) {
   std::vector<uint8_t> out(32, 0);
   if (in.size() >= 32) {
      std::ranges::copy(in.end() - 32, in.end(), out.begin());
   } else {
      std::ranges::copy(in, out.begin() + (32 - in.size()));
   }
   return out;
}

/**
 * @brief Converts a decimal or hexadecimal string to a 32-byte big-endian representation
 *
 * @param dec Decimal string or hexadecimal string (with "0x" or "0X" prefix)
 * @return 32-byte vector containing the big-endian representation of the number
 * @throws fc::exception if the decimal string contains invalid characters
 */
std::vector<uint8_t> be_uint_from_decimal(const std::string& dec) {
   // Supports decimal or hex (starting with 0x)
   if (dec.rfind("0x", 0) == 0 || dec.rfind("0X", 0) == 0) {
      auto bytes = fc::crypto::ethereum::hex_to_bytes(dec);
      return be_pad_left_32(bytes);
   }
   cpp_int v = 0;
   for (char c : dec) {
      FC_ASSERT(::isdigit(static_cast<unsigned char>(c)), "Invalid decimal number: {}", dec);
      v *= 10;
      v += static_cast<unsigned>(c - '0');
   }
   std::vector<uint8_t> tmp;
   while (v > 0) {
      uint8_t b = static_cast<uint8_t>(v & 0xff);
      tmp.push_back(b);
      v >>= 8;
   }
   std::ranges::reverse(tmp);
   return be_pad_left_32(tmp);
}

/**
 * @brief Encodes a static ABI value (fixed-size type) to 32-byte representation
 *
 * Handles encoding of numeric types (uint/int variants), booleans, addresses,
 * and fixed-size bytes (bytes1..bytes32).
 *
 * @param component ABI component type descriptor
 * @param value Variant containing the value to encode
 * @return 32-byte encoded representation
 * @throws fc::exception if the type is unsupported or value format is invalid
 */
std::vector<uint8_t> encode_static_value(const abi::component_type& component, const fc::variant& value) {
   using dt = abi::data_type;
   const auto type = component.type;

   // Numeric types (uint/int variants)
   if (abi::is_data_type_numeric(type)) {
      FC_ASSERT(value.is_numeric(), "Integer value expected for ABI encoding, got {}", value.as_string());
      if (type == dt::uint256 || type == dt::int256 || type == dt::uint128 || type == dt::int128) {
         return be_uint_from_decimal(value.as_string());
      }

      return be_uint_from_decimal(value.as_uint256().str());
   }

   switch (type) {
   case dt::boolean:
      return be_uint_from_decimal((value == "true" || value == "1") ? "1" : "0");

   case dt::address: {
      auto bytes = fc::crypto::ethereum::hex_to_bytes(value.as_string());
      FC_ASSERT(bytes.size() == 20, "Address must be 20 bytes, got {}", bytes.size());
      return be_pad_left_32(bytes);
   }

   default:
      break;
   }

   // Fixed-size bytes (bytes1..bytes32)
   if (abi::is_data_type_bytes(type)) {
      auto type_name = ethereum_abi_data_type_reflector::to_fc_string(type);
      auto sz = std::stoul(type_name.substr(5));
      auto bytes = fc::crypto::ethereum::hex_to_bytes(value.as_string());
      FC_ASSERT(bytes.size() == sz, "{} expects {} bytes, got {}", type_name, sz, bytes.size());
      std::vector<uint8_t> out(32, 0);
      std::ranges::copy(bytes, out.begin());
      return out;
   }

   FC_THROW_EXCEPTION(fc::unsupported_exception, "Unsupported static type for ABI encoding: {}",
                      ethereum_abi_data_type_reflector::to_fc_string(type));
}

/**
 * @brief Encodes a dynamic ABI value (variable-size type) with length prefix and padding
 *
 * Handles encoding of strings and dynamic bytes. The output includes a 32-byte length
 * prefix, followed by the data, padded to a 32-byte boundary.
 *
 * @param component ABI component type descriptor
 * @param value Variant containing the value to encode
 * @return Encoded byte vector with length prefix and padding
 * @throws fc::exception if the type is unsupported or value format is invalid
 */
std::vector<uint8_t> encode_dynamic_data(const abi::component_type& component, const fc::variant& value) {
   using dt = abi::data_type;
   const auto type = component.type;

   std::vector<uint8_t> data;
   switch (type) {
   case dt::string:
      FC_ASSERT(value.is_string(), "String value expected for ABI encoding, got {}", value.as_string());
      {
         const auto& s = value.as_string();
         data.assign(s.begin(), s.end());
      }
      break;

   case dt::bytes:
      FC_ASSERT(value.is_string(), "Bytes value expected for ABI encoding, got {}", value.as_string());
      data = fc::crypto::ethereum::hex_to_bytes(value.as_string());
      break;

   default:
      FC_THROW_EXCEPTION(fc::unsupported_exception, "Unsupported dynamic type for ABI encoding: {}",
                         ethereum_abi_data_type_reflector::to_fc_string(type));
   }

   std::vector<uint8_t> out;
   out.reserve(32 + data.size() + 32);

   // Length prefix (32 bytes big-endian)
   auto len_bytes = be_uint_from_decimal(std::to_string(data.size()));
   out.insert(out.end(), len_bytes.begin(), len_bytes.end());

   // Data
   out.insert(out.end(), data.begin(), data.end());

   // Padding to 32-byte boundary
   if (size_t pad = (32 - (data.size() % 32)) % 32; pad > 0) {
      out.insert(out.end(), pad, 0);
   }

   return out;
}

/**
 * @brief Encodes an ABI value according to its component type (forward declaration)
 *
 * @param component ABI component type descriptor
 * @param value Variant containing the value to encode
 * @return Encoded byte vector
 */
std::vector<uint8_t> encode_value(const abi::component_type& component, const fc::variant& value);

/**
 * @brief Encodes an ABI array (fixed or dynamic) with proper head/tail structure
 *
 * For fixed-size arrays, validates element count. For dynamic arrays, prepends the
 * array length. Handles both static and dynamic element types using offset encoding
 * when elements are dynamic.
 *
 * @param component ABI component type descriptor with list configuration
 * @param value Variant containing the array value
 * @return Encoded byte vector with head/tail structure
 * @throws fc::exception if array size mismatches or value is not an array
 */
std::vector<uint8_t> encode_list(const abi::component_type& component, const fc::variant& value) {
   FC_ASSERT(value.is_array(), "Array value expected for list encoding");
   const auto& arr = value.get_array();
   const auto& lc = component.list_config;

   if (lc.is_fixed_list()) {
      FC_ASSERT(arr.size() == lc.size, "Fixed-size list expects {} elements, got {}", lc.size, arr.size());
   }

   // Create element component (same as parent but without list_config)
   abi::component_type elem_comp = component;
   elem_comp.list_config = {};

   std::vector<uint8_t> out;

   // Dynamic lists prepend length
   if (lc.is_dynamic_list()) {
      auto len_bytes = be_uint_from_decimal(std::to_string(arr.size()));
      out.insert(out.end(), len_bytes.begin(), len_bytes.end());
   }

   // Check if elements are dynamic
   bool elements_dynamic = elem_comp.is_dynamic();

   if (elements_dynamic) {
      // Encode with head/tail structure
      std::vector<std::vector<uint8_t>> encoded_elements;
      encoded_elements.reserve(arr.size());
      for (const auto& elem : arr) {
         encoded_elements.push_back(encode_value(elem_comp, elem));
      }

      size_t head_size = arr.size() * 32;
      size_t offset = head_size;

      for (const auto& enc : encoded_elements) {
         auto off_bytes = be_uint_from_decimal(std::to_string(offset));
         out.insert(out.end(), off_bytes.begin(), off_bytes.end());
         offset += enc.size();
      }
      for (const auto& enc : encoded_elements) {
         out.insert(out.end(), enc.begin(), enc.end());
      }
   } else {
      // Static elements: encode sequentially
      for (const auto& elem : arr) {
         auto enc = encode_value(elem_comp, elem);
         out.insert(out.end(), enc.begin(), enc.end());
      }
   }

   return out;
}

/**
 * @brief Encodes an ABI tuple (struct) with proper head/tail structure
 *
 * Accepts tuple data as either an array (positional) or an object (named fields).
 * Static fields are encoded directly in the head, while dynamic fields use offset
 * pointers to tail data.
 *
 * @param component ABI component type descriptor with nested components
 * @param value Variant containing the tuple value (array or object)
 * @return Encoded byte vector with head/tail structure
 * @throws fc::exception if tuple structure mismatches or required fields are missing
 */
std::vector<uint8_t> encode_tuple(const abi::component_type& component, const fc::variant& value) {
   FC_ASSERT(value.is_array() || value.is_object(), "Tuple value must be array or object");

   const auto& children = component.components;
   std::vector<fc::variant> values;

   if (value.is_array()) {
      values = value.get_array();
   } else {
      const auto& obj = value.get_object();
      for (const auto& child : children) {
         FC_ASSERT_FMT(obj.contains(child.name.c_str()), "Missing tuple field: {}", child.name);
         values.push_back(obj[child.name]);
      }
   }
   FC_ASSERT(values.size() == children.size(), "Tuple size mismatch");

   std::vector<std::vector<uint8_t>> heads, tails;
   heads.reserve(children.size());
   tails.reserve(children.size());

   for (size_t i = 0; i < children.size(); ++i) {
      const auto& child = children[i];
      auto encoded = encode_value(child, values[i]);

      if (child.is_dynamic()) {
         heads.emplace_back(32, 0); // placeholder
         tails.push_back(std::move(encoded));
      } else {
         heads.push_back(std::move(encoded));
         tails.emplace_back();
      }
   }

   std::vector<uint8_t> out;
   auto head_sizes = heads | std::views::transform([](const auto& h) { return h.size(); });
   size_t head_size =
      std::ranges::fold_left(head_sizes, 0UL, std::plus<>{});
   size_t offset = head_size;

   for (size_t i = 0; i < heads.size(); ++i) {
      if (!tails[i].empty()) {
         auto off_bytes = be_uint_from_decimal(std::to_string(offset));
         out.insert(out.end(), off_bytes.begin(), off_bytes.end());
         offset += tails[i].size();
      } else {
         out.insert(out.end(), heads[i].begin(), heads[i].end());
      }
   }
   for (const auto& tail : tails) {
      out.insert(out.end(), tail.begin(), tail.end());
   }

   return out;
}

/**
 * @brief Main dispatcher for encoding an ABI value based on its component type
 *
 * Routes the encoding to the appropriate specialized function based on whether
 * the component is a list, tuple, dynamic type, or static type.
 *
 * @param component ABI component type descriptor
 * @param value Variant containing the value to encode
 * @return Encoded byte vector
 */
std::vector<uint8_t> encode_value(const abi::component_type& component, const fc::variant& value) {
   if (component.is_list()) {
      return encode_list(component, value);
   }

   if (component.is_container()) {
      return encode_tuple(component, value);
   }

   if (component.is_dynamic()) {
      return encode_dynamic_data(component, value);
   }

   return encode_static_value(component, value);
}

/**
 * @brief Reads a big-endian unsigned integer from a 32-byte slice
 *
 * @param data Pointer to the start of the 32-byte data
 * @return String representation of the decimal number
 */
std::string be_uint_to_decimal(const uint8_t* data) {
   boost::multiprecision::uint256_t value = 0;
   for (size_t i = 0; i < 32; ++i) {
      value = (value << 8) | data[i];
   }
   return value.str();
}

/**
 * @brief Reads a big-endian signed integer from a 32-byte slice
 *
 * @param data Pointer to the start of the 32-byte data
 * @return String representation of the decimal number
 */
std::string be_int_to_decimal(const uint8_t* data) {
   boost::multiprecision::int256_t value = 0;
   // Check sign bit
   bool is_negative = (data[0] & 0x80) != 0;
   
   if (is_negative) {
      // Two's complement: invert bits and add 1
      boost::multiprecision::uint256_t temp = 0;
      for (size_t i = 0; i < 32; ++i) {
         temp = (temp << 8) | (~data[i] & 0xFF);
      }
      temp += 1;
      value = -static_cast<boost::multiprecision::int256_t>(temp);
   } else {
      for (size_t i = 0; i < 32; ++i) {
         value = (value << 8) | data[i];
      }
   }
   return value.str();
}

/**
 * @brief Forward declaration for decode_value
 */
fc::variant decode_value(const abi::component_type& component, const uint8_t* data, size_t data_size, size_t& offset);

/**
 * @brief Decodes a static ABI value from 32-byte representation
 *
 * @param component ABI component type descriptor
 * @param data Pointer to the encoded data
 * @param offset Current offset in the data (will be advanced by 32)
 * @return Decoded value as fc::variant
 */
fc::variant decode_static_value(const abi::component_type& component, const uint8_t* data, size_t& offset) {
   using dt = abi::data_type;
   const auto type = component.type;
   
   FC_ASSERT(offset + 32 <= SIZE_MAX, "Offset overflow");
   const uint8_t* value_data = data + offset;
   offset += 32;

   // Numeric types
   if (abi::is_data_type_numeric(type)) {
      if (abi::is_data_type_numeric_signed(type)) {
         return fc::variant(be_int_to_decimal(value_data));
      } else {
         return fc::variant(be_uint_to_decimal(value_data));
      }
   }

   switch (type) {
   case dt::boolean: {
      auto val = be_uint_to_decimal(value_data);
      return fc::variant(val != "0");
   }

   case dt::address: {
      // Address is right-aligned in 32 bytes, take last 20 bytes
      std::vector<uint8_t> addr_bytes(value_data + 12, value_data + 32);
      return fc::variant("0x" + fc::to_hex(addr_bytes));
   }

   default:
      break;
   }

   // Fixed-size bytes (bytes1..bytes32)
   if (abi::is_data_type_bytes(type)) {
      auto type_name = ethereum_abi_data_type_reflector::to_fc_string(type);
      auto sz = std::stoul(type_name.substr(5));
      std::vector<uint8_t> bytes_data(value_data, value_data + sz);
      return fc::variant("0x" + fc::to_hex(bytes_data));
   }

   FC_THROW_EXCEPTION(fc::unsupported_exception, "Unsupported static type for ABI decoding: {}",
                      ethereum_abi_data_type_reflector::to_fc_string(type));
}

/**
 * @brief Decodes a dynamic ABI value (string or bytes)
 *
 * @param component ABI component type descriptor
 * @param data Pointer to the full encoded data
 * @param data_size Total size of the encoded data
 * @param offset Current offset pointing to the length prefix
 * @return Decoded value as fc::variant
 */
fc::variant decode_dynamic_data(const abi::component_type& component, const uint8_t* data, size_t data_size, size_t& offset) {
   using dt = abi::data_type;
   const auto type = component.type;

   // Read length (32 bytes)
   FC_ASSERT(offset + 32 <= data_size, "Not enough data for dynamic length");
   auto length_str = be_uint_to_decimal(data + offset);
   offset += 32;
   
   size_t length = std::stoull(length_str);
   FC_ASSERT(offset + length <= data_size, "Not enough data for dynamic content");

   switch (type) {
   case dt::string: {
      std::string str(reinterpret_cast<const char*>(data + offset), length);
      // Advance offset to next 32-byte boundary
      size_t padded_length = ((length + 31) / 32) * 32;
      offset += padded_length;
      return fc::variant(str);
   }

   case dt::bytes: {
      std::vector<uint8_t> bytes_data(data + offset, data + offset + length);
      // Advance offset to next 32-byte boundary
      size_t padded_length = ((length + 31) / 32) * 32;
      offset += padded_length;
      return fc::variant("0x" + fc::to_hex(bytes_data));
   }

   default:
      FC_THROW_EXCEPTION(fc::unsupported_exception, "Unsupported dynamic type for ABI decoding: {}",
                         ethereum_abi_data_type_reflector::to_fc_string(type));
   }
}

/**
 * @brief Decodes an ABI array (fixed or dynamic)
 *
 * @param component ABI component type descriptor with list configuration
 * @param data Pointer to the full encoded data
 * @param data_size Total size of the encoded data
 * @param offset Current offset in the data
 * @return Decoded array as fc::variant
 */
fc::variant decode_list(const abi::component_type& component, const uint8_t* data, size_t data_size, size_t& offset) {
   const auto& lc = component.list_config;
   size_t array_length;

   // Read array length for dynamic arrays
   if (lc.is_dynamic_list()) {
      FC_ASSERT(offset + 32 <= data_size, "Not enough data for array length");
      auto length_str = be_uint_to_decimal(data + offset);
      offset += 32;
      array_length = std::stoull(length_str);
   } else {
      array_length = lc.size;
   }

   // Create element component (same as parent but without list_config)
   abi::component_type elem_comp = component;
   elem_comp.list_config = {};

   fc::variants result;
   result.reserve(array_length);

   bool elements_dynamic = elem_comp.is_dynamic();

   if (elements_dynamic) {
      // Read offsets from head
      std::vector<size_t> offsets;
      offsets.reserve(array_length);
      size_t base_offset = offset;
      
      for (size_t i = 0; i < array_length; ++i) {
         FC_ASSERT(offset + 32 <= data_size, "Not enough data for array element offset");
         auto offset_str = be_uint_to_decimal(data + offset);
         offsets.push_back(base_offset + std::stoull(offset_str));
         offset += 32;
      }

      // Decode elements from tail
      for (size_t i = 0; i < array_length; ++i) {
         size_t elem_offset = offsets[i];
         result.push_back(decode_value(elem_comp, data, data_size, elem_offset));
      }
      
      // Update offset to end of last element
      if (!offsets.empty()) {
         offset = offsets.back();
         // The last decode_value call updated the offset, but we need to ensure we're past all data
         // This is handled by decode_value updating the offset parameter
      }
   } else {
      // Static elements: decode sequentially
      for (size_t i = 0; i < array_length; ++i) {
         result.push_back(decode_value(elem_comp, data, data_size, offset));
      }
   }

   return fc::variant(std::move(result));
}

/**
 * @brief Decodes an ABI tuple (struct) - returns as fc::variant_object
 *
 * @param component ABI component type descriptor with nested components
 * @param data Pointer to the full encoded data
 * @param data_size Total size of the encoded data
 * @param offset Current offset in the data
 * @return Decoded tuple as fc::variant_object
 */
fc::variant decode_tuple(const abi::component_type& component, const uint8_t* data, size_t data_size, size_t& offset) {
   const auto& children = component.components;
   fc::mutable_variant_object result;

   size_t base_offset = offset;
   std::vector<std::pair<std::string, size_t>> dynamic_fields; // name, absolute offset

   // First pass: decode static fields and collect dynamic field offsets
   for (const auto& child : children) {
      if (child.is_dynamic()) {
         // Read offset pointer (32 bytes)
         FC_ASSERT(offset + 32 <= data_size, "Not enough data for tuple field offset");
         auto offset_str = be_uint_to_decimal(data + offset);
         size_t field_offset = base_offset + std::stoull(offset_str);
         dynamic_fields.emplace_back(child.name, field_offset);
         offset += 32;
      } else {
         // Decode static field directly
         result[child.name] = decode_value(child, data, data_size, offset);
      }
   }

   // Second pass: decode dynamic fields
   for (const auto& [name, field_offset] : dynamic_fields) {
      size_t temp_offset = field_offset;
      // Find the child component by name
      auto it = std::ranges::find_if(children, [&name](const auto& c) { return c.name == name; });
      FC_ASSERT(it != children.end(), "Dynamic field not found in components");
      result[name] = decode_value(*it, data, data_size, temp_offset);
   }

   return fc::variant(std::move(result));
}

/**
 * @brief Main dispatcher for decoding an ABI value based on its component type
 *
 * @param component ABI component type descriptor
 * @param data Pointer to the full encoded data
 * @param data_size Total size of the encoded data
 * @param offset Current offset in the data (will be updated)
 * @return Decoded value as fc::variant
 */
fc::variant decode_value(const abi::component_type& component, const uint8_t* data, size_t data_size, size_t& offset) {
   if (component.is_list()) {
      return decode_list(component, data, data_size, offset);
   }

   if (component.is_container()) {
      return decode_tuple(component, data, data_size, offset);
   }

   if (component.is_dynamic()) {
      return decode_dynamic_data(component, data, data_size, offset);
   }

   return decode_static_value(component, data, offset);
}

} // anonymous namespace

/**
 * @brief Generates the canonical type signature string for an ABI component
 *
 * Recursively constructs the signature for tuples and arrays according to the
 * Ethereum ABI specification format (e.g., "uint256", "(uint256,address)", "uint256[]").
 *
 * @param component ABI component type descriptor
 * @return Canonical type signature string
 */
std::string abi::to_contract_component_signature(const component_type& component) {
   std::stringstream ss;
   if (!component.is_container()) {
      auto dt_str = ethereum_abi_data_type_reflector::to_fc_string(component.type);
      ss << dt_str;
   }
   if (component.is_container()) {
      ss << '(';

      std::ranges::for_each(component.components | std::views::enumerate, [&](auto&& enum_item) {
         auto& [i, child_comp] = enum_item;
         if (i) {
            ss << ',';
         }

         ss << abi::to_contract_component_signature(child_comp);
      });

      ss << ')';
   }

   if (component.is_list()) {
      auto& lc = component.list_config;
      ss << '[';
      if (lc.is_fixed_list()) {
         FC_ASSERT(lc.size > 0, "Fixed-size list must have size > 0");
         ss << lc.size;
      }
      ss << ']';
   }

   return ss.str();
}

/**
 * @brief Generates the full function signature string for an ABI contract
 *
 * Creates the canonical function signature used for computing the function selector,
 * in the format "functionName(type1,type2,...)".
 *
 * @param contract ABI contract descriptor (must be of type function)
 * @return Function signature string
 * @throws fc::exception if contract is not a function type
 */
std::string abi::to_contract_function_signature(const contract& contract) {
   FC_ASSERT(contract.type == invoke_target_type::function, "ABI contract must be a function");
   std::stringstream ss;
   ss << contract.name << '(';
   std::ranges::for_each(contract.inputs | std::views::enumerate, [&](auto&& enum_item) {
      auto& [i, input_comp] = enum_item;
      if (i) {
         ss << ',';
      }

      ss << abi::to_contract_component_signature(input_comp);
   });
   ss << ')';
   return ss.str();
}

/**
 * @brief Computes the 4-byte function selector for an ABI contract
 *
 * Calculates the Keccak-256 hash of the function signature. The first 4 bytes
 * of this hash serve as the function selector in Ethereum contract calls.
 *
 * @param contract ABI contract descriptor (must be of type function)
 * @return 32-byte Keccak-256 hash (first 4 bytes used as selector)
 * @throws fc::exception if contract is not a function type
 */
keccak256_hash_t abi::to_contract_function_selector(const contract& contract) {
   FC_ASSERT(contract.type == invoke_target_type::function, "ABI contract must be a function");
   auto signature = abi::to_contract_function_signature(contract);
   return fc::crypto::ethereum::keccak256(signature);
}

/**
 * @brief Parses multiple ABI contracts from a JSON file
 *
 * Reads and parses an Ethereum ABI JSON file containing an array of contract
 * definitions (functions, events, etc.).
 *
 * @param json_abi_file Path to the JSON ABI file
 * @return Vector of parsed ABI contract descriptors
 * @throws fc::exception if file cannot be read or does not contain an array
 */
std::vector<abi::contract> abi::parse_contracts(const std::filesystem::path& json_abi_file) {
   auto json_var = fc::json::from_file(json_abi_file);
   FC_ASSERT(json_var.is_array(), "ABI file must contain an array of contracts");
   std::vector<abi::contract> contracts;
   for (const auto& c : json_var.get_array()) {
      contracts.push_back(abi::parse_contract(c.get_object()));
   }
   return contracts;
}

/**
 * @brief Parses a single ABI contract from a variant
 *
 * Converts a variant representation (typically from JSON) into an ABI contract structure.
 *
 * @param v Variant containing the contract definition
 * @return Parsed ABI contract descriptor
 */
abi::contract abi::parse_contract(const fc::variant& v) {
   return v.as<abi::contract>();
}



/**
 * @brief Encodes function parameters for an Ethereum contract call
 *
 * Encodes the provided parameters according to the ABI contract specification,
 * prepending the 4-byte function selector. The result is formatted as a hex string
 * suitable for use as the `data` field in an Ethereum transaction.
 *
 * @param contract ABI contract descriptor defining the function signature and inputs
 * @param params Vector of parameter values to encode
 * @return Hex string of encoded call data (selector + encoded parameters)
 * @throws fc::exception if parameter count mismatches or encoding fails
 */
std::string contract_encode_data(const abi::contract& contract, const std::vector<fc::variant>& params) {
   const auto& inputs = contract.inputs;
   FC_ASSERT_FMT(inputs.size() == params.size(), "Parameter count mismatch (expected={}, provided={})", inputs.size(),
                 params.size());

   auto selector = abi::to_contract_function_selector(contract);

   std::vector<uint8_t> out;
   out.insert(out.end(), selector.begin(), selector.begin() + 4);

   std::vector<std::vector<uint8_t>> heads, tails;
   heads.reserve(inputs.size());
   tails.reserve(inputs.size());

   for (size_t i = 0; i < inputs.size(); ++i) {
      const auto& comp = inputs[i];
      auto encoded = encode_value(comp, params[i]);

      if (comp.is_dynamic() || comp.is_list()) {
         heads.emplace_back(32, 0);
         tails.push_back(std::move(encoded));
      } else {
         heads.push_back(std::move(encoded));
         tails.emplace_back();
      }
   }

   size_t head_size = heads.size() * 32;
   size_t offset = head_size;

   for (size_t i = 0; i < heads.size(); ++i) {
      if (!tails[i].empty()) {
         auto off_bytes = be_uint_from_decimal(std::to_string(offset));
         out.insert(out.end(), off_bytes.begin(), off_bytes.end());
         offset += tails[i].size();
      } else {
         out.insert(out.end(), heads[i].begin(), heads[i].end());
      }
   }

   for (const auto& tail : tails) {
      out.insert(out.end(), tail.begin(), tail.end());
   }

   return fc::to_hex(out);
}


/**
 * @brief Decodes encoded contract call data into structured parameters
 *
 * Decodes the provided data according to the ABI contract specification.
 * Optionally verifies and strips the 4-byte function selector. Returns decoded
 * parameters as either an array (positional) or objects for tuple/error/event types.
 *
 * @param contract ABI contract descriptor defining the function signature and inputs
 * @param encoded_invoke_data Hex-encoded call data string
 * @param use_inputs_instead_of_outputs it true, use contract.inputs to parse data (in which case, skip the first 4 bytes, the function selector), otherwise the default logic uses contract.outputs
 * @return Variant containing decoded parameters (array or object based on contract type)
 * @throws fc::exception if encoded data is too short or decoding fails
 */
fc::variant contract_decode_data(const abi::contract& contract, const std::string& encoded_invoke_data, bool use_inputs_instead_of_outputs) {
   auto bytes = fc::crypto::ethereum::hex_to_bytes(encoded_invoke_data);
   
   // Determine which component list to use
   const auto& components = use_inputs_instead_of_outputs ? contract.inputs : contract.outputs;
   
   // Determine starting offset
   size_t offset = 0;
   if (use_inputs_instead_of_outputs) {
      // Skip the 4-byte function selector when decoding inputs
      FC_ASSERT(bytes.size() >= 4, "Encoded call data too short for function selector");
      offset = 4;
   }
   
   const uint8_t* data = bytes.data();
   size_t data_size = bytes.size();
   
   // If there's only one component, return it directly (not wrapped in an array)
   if (components.size() == 1) {
      const auto& comp = components[0];
      // For dynamic or list components, we need to read the offset pointer first
      if (comp.is_dynamic() || comp.is_list()) {
         FC_ASSERT(offset + 32 <= data_size, "Not enough data for parameter offset");
         auto offset_str = be_uint_to_decimal(data + offset);
         size_t actual_offset = offset + std::stoull(offset_str);
         return decode_value(comp, data, data_size, actual_offset);
      }
      return decode_value(comp, data, data_size, offset);
   }
   
   // Decode multiple parameters
   fc::variants results;
   results.reserve(components.size());
   
   // Process each component using head/tail structure
   std::vector<size_t> offsets;
   offsets.reserve(components.size());
   size_t base_offset = offset;
   
   // First pass: read heads (either static values or offset pointers)
   for (const auto& comp : components) {

      if (comp.is_dynamic()) {
         // Dynamic component: read offset pointer
         FC_ASSERT(offset + 32 <= data_size, "Not enough data for parameter offset");
         auto offset_str = be_uint_to_decimal(data + offset);
         offsets.push_back(base_offset + std::stoull(offset_str));
         offset += 32;
      } else {
         // Static component: decode directly
         offsets.push_back(offset);
         offset += 32; // Static values are always 32 bytes
      }
   }
   
   // Second pass: decode each parameter
   for (size_t i = 0; i < components.size(); ++i) {
      const auto& comp = components[i];
      size_t param_offset = offsets[i];
      
      if (comp.is_dynamic()) {
         // Decode from tail using the offset
         results.push_back(decode_value(comp, data, data_size, param_offset));
      } else {
         // Decode static value (offset already points to the right place)
         results.push_back(decode_static_value(comp, data, param_offset));
      }
   }
   
   return fc::variant(std::move(results));
}

} // namespace fc::network::ethereum

/**
 * @brief Deserializes an ABI component type from a variant (typically JSON)
 *
 * Parses the variant object to extract component name, type, array dimensions,
 * internal type, and nested components (for tuples). Supports both fixed and
 * dynamic arrays through bracket notation (e.g., "uint256[]", "address[5]").
 *
 * @param var Variant containing the component definition
 * @param vo Output component type structure to populate
 * @throws fc::exception if variant format is invalid or required fields are missing
 */
void fc::from_variant(const fc::variant& var, fc::network::ethereum::abi::component_type& vo) {
   using namespace fc::network::ethereum;
   using namespace fc::network::ethereum::abi;

   FC_ASSERT(var.is_object(), "Variant must be an object to deserialize ABI component");
   auto& obj = var.get_object();
   auto data_type_str = obj["type"].as_string();

   std::regex data_type_regex(R"(([a-zA-Z0-9_]+)(\[(\d*)\])?)");
   std::smatch data_type_match;
   FC_ASSERT(std::regex_match(data_type_str, data_type_match, data_type_regex), "Invalid type format: {}", data_type_str);

   auto base_type_str = data_type_match[1].str();
   vo.name = obj["name"].as_string();
   vo.type = fc::reflector<data_type>::from_string(base_type_str);
   bool is_list = data_type_match[2].str().starts_with("[");
   if (is_list) {
      vo.list_config.is_list = true;
      auto list_size_str = data_type_match[3].str();
      FC_ASSERT_FMT(list_size_str.empty() || all_digits(list_size_str), "Invalid list size format: {}", list_size_str);
      vo.list_config.size = list_size_str.empty() ? 0 : std::stoul(list_size_str);
   }

   if (obj.contains("internalType")) {
      vo.internal_type = obj["internalType"].as_string();
   }

   if (obj.contains("components")) {
      vo.components.clear();

      auto& vo_components_var = obj["components"];
      FC_ASSERT(vo_components_var.is_array(), "ABI components must be an array if specified");
      auto& vo_comp_list = vo_components_var.get_array();
      for (auto& comp_var : vo_comp_list) {
         FC_ASSERT_FMT(comp_var.is_object(), "ABI component {} must be an object", vo.name);
         auto comp = comp_var.as<component_type>();
         vo.components.push_back(comp);
      }
   }

   dlog("name={},type={},internal_type={},components={}", vo.name, base_type_str, vo.internal_type,
        vo.components.size());
}
/**
 * @brief Deserializes an ABI contract from a variant (typically JSON)
 *
 * Parses the variant object to extract contract name, type (function, event, etc.),
 * and lists of input and output components. Supports optional fields that may not
 * be present in all ABI entries.
 *
 * @param var Variant containing the contract definition
 * @param vo Output contract structure to populate
 * @throws fc::exception if variant format is invalid or required fields are missing
 */
void fc::from_variant(const fc::variant& var, fc::network::ethereum::abi::contract& vo) {
   using namespace fc::network::ethereum;
   using namespace fc::network::ethereum::abi;

   FC_ASSERT(var.is_object(), "Variant must be an object to deserialize ABI contract");
   auto& obj = var.get_object();
   vo.name = obj["name"].as_string();
   auto type_str = obj["type"].as_string();
   vo.type = fc::reflector<fc::network::ethereum::abi::invoke_target_type>::from_string(type_str.c_str());

   auto parse_components = [&](std::vector<component_type>& vo_list, const std::string& list_name) {
      auto list_prop_exists = obj.contains(list_name.c_str());
      if (!list_prop_exists || !obj[list_name].is_array()) {
         dlog("ABI property is not set or not array (name={},exists={},is_array={})", list_name, list_prop_exists,
              obj[list_name].is_array(), "ABI contract inputs must be an array");
         return;
      }

      auto& vo_component_list = obj[list_name].get_array();
      vo_list.reserve(vo_component_list.size());

      for (auto& vo_component_var : vo_component_list) {
         FC_ASSERT_FMT(vo_component_var.is_object(), "ABI contract {} must be an object", list_name);
         auto comp = vo_component_var.as<fc::network::ethereum::abi::component_type>();
         vo_list.push_back(std::move(comp));
      }
   };

   parse_components(vo.inputs, "inputs");
   parse_components(vo.outputs, "outputs");
}
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
      FC_ASSERT(::isdigit(static_cast<unsigned char>(c)), "Invalid decimal number: ${n}", ("n", dec));
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
      FC_ASSERT_FMT(value.is_numeric(), "Integer value expected for ABI encoding, got {}", value.as_string());
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
      FC_ASSERT_FMT(bytes.size() == 20, "Address must be 20 bytes, got {}", bytes.size());
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
      FC_ASSERT_FMT(bytes.size() == sz, "{} expects {} bytes, got {}", type_name, sz, bytes.size());
      std::vector<uint8_t> out(32, 0);
      std::ranges::copy(bytes, out.begin());
      return out;
   }

   FC_THROW_EXCEPTION_FMT(fc::exception, "Unsupported static type for ABI encoding: {}",
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
      FC_ASSERT_FMT(value.is_string(), "String value expected for ABI encoding, got {}", value.as_string());
      {
         const auto& s = value.as_string();
         data.assign(s.begin(), s.end());
      }
      break;

   case dt::bytes:
      FC_ASSERT_FMT(value.is_string(), "Bytes value expected for ABI encoding, got {}", value.as_string());
      data = fc::crypto::ethereum::hex_to_bytes(value.as_string());
      break;

   default:
      FC_THROW_EXCEPTION_FMT(fc::exception, "Unsupported dynamic type for ABI encoding: {}",
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
      FC_ASSERT_FMT(arr.size() == lc.size, "Fixed-size list expects {} elements, got {}", lc.size, arr.size());
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

      // TODO: is needed? @jglanz
      // || child.is_list()
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


} // anonymous namespace

// bool abi::is_data_type_numeric(data_type type) {
//    std::string dts{magic_enum::enum_name(type)};
//    for (auto& prefix : data_type_numeric_prefixes) {
//       if (dts.starts_with(prefix))
//          return true;
//    }
//    return false;
// }

// bool abi::is_data_type_numeric_signed(data_type type) {
//    std::string dts{magic_enum::enum_name(type)};
//    for (auto& prefix : data_type_numeric_signed_prefixes) {
//       if (dts.starts_with(prefix))
//          return true;
//    }
//    return false;
// }

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
std::string contract_invoke_encode(const abi::contract& contract, const std::vector<fc::variant>& params) {
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
 * @brief Decodes encoded contract call data into selector and parameter words
 *
 * Performs minimal decoding by extracting the 4-byte function selector and splitting
 * the remaining data into 32-byte words. Returns a contract structure with the selector
 * as the name and a vector of hex-encoded parameter words.
 *
 * @param encoded_invoke_data Hex-encoded call data string
 * @return Pair of ABI contract (with selector as name) and vector of hex-encoded parameter words
 * @throws fc::exception if encoded data is too short (< 4 bytes)
 */
std::pair<abi::contract, std::vector<std::string>> contract_invoke_decode(const std::string& encoded_invoke_data) {
   // Minimal decoding: return selector as "0x...." in abi.name/signature and split payload into 32-byte words as hex
   auto bytes = fc::crypto::ethereum::hex_to_bytes(encoded_invoke_data);
   FC_ASSERT(bytes.size() >= 4, "Encoded call too short");

   abi::contract abi{};
   abi.type = abi::invoke_target_type::function;
   // selector
   std::vector<uint8_t> selector(bytes.begin(), bytes.begin() + 4);
   auto selector_hex = std::string("0x") + fc::to_hex(selector);
   abi.name = selector_hex; // unknown function name; set to selector
   // abi.signature     = selector_hex;

   std::vector<std::string> params;
   size_t offset = 4;
   while (offset + 32 <= bytes.size()) {
      std::vector<uint8_t> word(bytes.begin() + offset, bytes.begin() + offset + 32);
      params.emplace_back("0x" + fc::to_hex(word));
      offset += 32;
   }
   // If there is remaining tail data not multiple of 32, append as last param
   if (offset < bytes.size()) {
      std::vector<uint8_t> rem(bytes.begin() + offset, bytes.end());
      params.emplace_back("0x" + fc::to_hex(rem));
   }
   return {abi, params};
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
   FC_ASSERT(std::regex_match(data_type_str, data_type_match, data_type_regex), "Invalid type format: ${t}",
             ("t", data_type_str));

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
      auto& vo_components_var = obj["components"];
      FC_ASSERT(vo_components_var.is_array(), "ABI components must be an array if specified");
      auto& vo_comp_list = vo_components_var.get_array();
      vo.components.reserve(vo_comp_list.size());
      for (auto& comp_var : vo_comp_list) {
         FC_ASSERT_FMT(comp_var.is_object(), "ABI component {} must be an object", vo.name);
         auto comp = comp_var.as<component_type>();
         vo.components.push_back(comp);
      }
   }

   dlogf("name={},type={},internal_type={},components={}", vo.name, base_type_str, vo.internal_type,
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
         dlogf("ABI property is not set or not array (name={},exists={},is_array={})", list_name, list_prop_exists,
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
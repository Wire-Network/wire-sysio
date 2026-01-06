#include <algorithm>
#include <boost/multiprecision/cpp_int.hpp>
#include <cctype>
#include <ethash/keccak.hpp>
#include <fc/crypto/ethereum/ethereum_utils.hpp>
#include <fc/crypto/hex.hpp>
#include <fc/io/json.hpp>
#include <fc/network/ethereum/ethereum_abi.hpp>
#include <fc/string.hpp>
#include <optional>
#include <ranges>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace fc::network::ethereum {

using namespace fc::crypto;
using namespace fc::crypto::ethereum;
using boost::multiprecision::cpp_int;

// Anonymous namespace for internal helpers (instead of marking them static)
namespace {

std::vector<std::string> split_types(const std::string& inside) {
   std::vector<std::string> out;
   std::string cur;
   int depth = 0;
   for (char c : inside) {
      if (c == '(')
         depth++;
      if (c == ')')
         depth--;
      if (c == ',' && depth == 0) {
         if (!cur.empty()) {
            out.emplace_back(cur);
            cur.clear();
         }
      } else {
         cur.push_back(c);
      }
   }
   if (!cur.empty())
      out.emplace_back(cur);
   // remove whitespace from each type using ranges
   for (auto& s : out) {
      std::string cleaned;
      std::ranges::copy_if(s, std::back_inserter(cleaned), [](unsigned char ch) { return !std::isspace(ch); });
      s = std::move(cleaned);
   }
   return out;
}

std::vector<uint8_t> be_pad_left_32(const std::vector<uint8_t>& in) {
   std::vector<uint8_t> out(32, 0);
   if (in.size() >= 32) {
      std::copy(in.end() - 32, in.end(), out.begin());
   } else {
      std::copy(in.begin(), in.end(), out.begin() + (32 - in.size()));
   }
   return out;
}

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
   std::reverse(tmp.begin(), tmp.end());
   return be_pad_left_32(tmp);
}

std::vector<uint8_t> encode_static_value(const std::string& type, const fc::variant& value) {
   auto t = type;
   // Normalize
   std::ranges::transform(t, t.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

   if (t == "uint" || t.rfind("uint", 0) == 0) {
      if (t == "uint256" || t == "int256") {
         FC_ASSERT(value.is_string(), "Integer value expected for ABI encoding, got ${v}", ("v", value));
         return be_uint_from_decimal(value.as_string());
      }
      FC_ASSERT(value.is_numeric(), "Integer value expected for ABI encoding, got ${v}", ("v", value));
      return be_uint_from_decimal(value.as_uint256().str());
   }
   if (t == "bool") {
      return be_uint_from_decimal((value == "true" || value == "1") ? std::string("1") : std::string("0"));
   }
   if (t == "address") {
      auto bytes = fc::crypto::ethereum::hex_to_bytes(value.as_string());
      FC_ASSERT(bytes.size() == 20, "Address must be 20 bytes, got ${n}", ("n", bytes.size()));
      return be_pad_left_32(bytes);
   }
   if (t == "bytes32") {
      auto bytes = fc::crypto::ethereum::hex_to_bytes(value.as_string());
      FC_ASSERT(bytes.size() <= 32, "bytes32 too long: ${n}", ("n", bytes.size()));
      std::vector<uint8_t> out(32, 0);
      std::copy(bytes.begin(), bytes.end(), out.begin());
      return out;
   }
   // bytesN where 1<=N<=32
   if (t.rfind("bytes", 0) == 0 && t.size() > 5) {
      auto sz = std::stoul(t.substr(5));
      if (sz >= 1 && sz <= 32) {
         auto bytes = fc::crypto::ethereum::hex_to_bytes(value.as_string());
         FC_ASSERT(bytes.size() == sz, "${t} expects ${sz} bytes, got ${n}", ("t", t)("sz", sz)("n", bytes.size()));
         std::vector<uint8_t> out(32, 0);
         std::copy(bytes.begin(), bytes.end(), out.begin());
         return out;
      }
   }
   FC_THROW_EXCEPTION(fc::exception, "Unsupported static type for ABI encoding: ${t}", ("t", t));
}

bool is_dynamic(const std::string& type) {
   auto t = type;
   std::ranges::transform(t, t.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
   if (t == "string" || t == "bytes")
      return true;
   // arrays and dynamic bytes not implemented beyond simple types
   return false;
}

std::vector<uint8_t> encode_dynamic_data(const std::string& type, const fc::variant& value) {
   auto t = type;
   std::ranges::transform(t, t.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
   std::vector<uint8_t> data;
   if (t == "string") {
      FC_ASSERT(value.is_string(), "String value expected for ABI encoding, got ${v}", ("v", value));
      const auto& s = value.as_string();
      data.assign(s.begin(), s.end());
   } else if (t == "bytes") {
      FC_ASSERT(value.is_string(), "Bytes value expected for ABI encoding, got ${v}", ("v", value));
      data = fc::crypto::ethereum::hex_to_bytes(value.as_string());
   } else {
      FC_THROW_EXCEPTION(fc::exception, "Unsupported dynamic type for ABI encoding: ${t}", ("t", t));
   }

   // length (32 bytes) + data + padding to 32-byte multiple
   std::vector<uint8_t> out;
   // length as 32-byte big-endian
   {
      std::vector<uint8_t> len_be = be_uint_from_decimal(std::to_string(data.size()));
      out.insert(out.end(), len_be.begin(), len_be.end());
   }
   // data
   out.insert(out.end(), data.begin(), data.end());
   // padding
   size_t pad = (32 - (data.size() % 32)) % 32;
   out.insert(out.end(), pad, 0);
   return out;
}

} // anonymous namespace

std::string abi::to_contract_component_signature(const component_type& component) {
   std::stringstream ss;
   auto dt_str = ethereum_abi_data_type_reflector::to_fc_string(component.type);
   ss << dt_str;
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
      if (lc.is_fixed()) {
         FC_ASSERT(lc.size > 0, "Fixed-size list must have size > 0");
         ss << lc.size;
      }
      ss << ']';
   }

   return ss.str();
}

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

keccak256_hash_t abi::to_contract_function_selector(const contract& contract) {
   FC_ASSERT(contract.type == invoke_target_type::function, "ABI contract must be a function");
   auto signature = abi::to_contract_function_signature(contract);
   return fc::crypto::ethereum::keccak256(signature);
}

std::vector<abi::contract> abi::parse_contracts(const std::filesystem::path& json_abi_file) {
   auto json_var = fc::json::from_file(json_abi_file);
   FC_ASSERT(json_var.is_array(), "ABI file must contain an array of contracts");
   std::vector<abi::contract> contracts;
   for (const auto& c : json_var.get_array()) {
      contracts.push_back(abi::parse_contract(c.get_object()));
   }
   return contracts;
}
abi::contract abi::parse_contract(const fc::variant& v) {
   return v.as<abi::contract>();
}

std::string contract_invoke_encode(const abi::contract& abi, const std::vector<fc::variant>& params) {
   // Obtain ABI struct
   // abi::contract a{};
   // if (std::holds_alternative<std::string>(abi)) {
   //    auto& sig = std::get<std::string>(abi);
   //    a        = abi::parse_contract(sig);
   // } else {
   //    a = std::get<abi::contract>(abi);
   //    if (a.signature.empty()) {
   //       // Build signature from name and input types
   //       std::ostringstream oss;
   //       oss << a.name << '(';
   //       for (size_t i = 0; i < a.inputs.size(); ++i) {
   //          if (i) oss << ',';
   //          oss << a.inputs[i].second;
   //       }
   //       oss << ')';
   //       a.signature = oss.str();
   //    }
   // }
   //
   // FC_ASSERT(params.size() == a.inputs.size(), "Parameter count mismatch: expected ${e}, got ${g}",
   //           ("e", a.inputs.size())("g", params.size()));
   //
   // // Function selector
   // auto sig_bytes = std::vector<uint8_t>(a.signature.begin(), a.signature.end());
   // auto h         = ethash::keccak256(sig_bytes.data(), sig_bytes.size());
   // std::vector<uint8_t> out;
   // out.insert(out.end(), h.bytes, h.bytes + 4);
   //
   // // Prepare head and tail for dynamic params
   // std::vector<std::vector<uint8_t>> heads;
   // std::vector<std::vector<uint8_t>> tails;
   // heads.reserve(a.inputs.size());
   // tails.reserve(a.inputs.size());
   //
   // // First pass: build heads placeholders and tails
   // for (size_t i = 0; i < a.inputs.size(); ++i) {
   //    const auto& type  = a.inputs[i].second;
   //    const auto& value = params[i];
   //    if (is_dynamic(type)) {
   //       // placeholder for offset, will compute later
   //       heads.emplace_back(32, 0);
   //       tails.push_back(encode_dynamic_data(type, value));
   //    } else {
   //       heads.push_back(encode_static_value(type, value));
   //       tails.emplace_back();
   //    }
   // }
   //
   // // Compute starting offset of tail (after selector + head section)
   // size_t head_size = heads.size() * 32;
   // size_t current_offset = head_size; // offsets are from start of head section (not counting 4-byte selector)
   //
   // // Append heads, computing offsets for dynamic
   // for (size_t i = 0; i < heads.size(); ++i) {
   //    if (!tails[i].empty()) {
   //       // dynamic param: write offset
   //       // offset from start of head
   //       std::vector<uint8_t> off_be = be_uint_from_decimal(std::to_string(current_offset));
   //       out.insert(out.end(), off_be.begin(), off_be.end());
   //       current_offset += tails[i].size();
   //    } else {
   //       out.insert(out.end(), heads[i].begin(), heads[i].end());
   //    }
   // }
   //
   // // Append tails for dynamic params in order
   // for (size_t i = 0; i < tails.size(); ++i) {
   //    if (!tails[i].empty()) {
   //       out.insert(out.end(), tails[i].begin(), tails[i].end());
   //    }
   // }
   //
   // return fc::to_hex(out);
}

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
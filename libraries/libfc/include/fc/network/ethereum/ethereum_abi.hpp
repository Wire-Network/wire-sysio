#pragma once
#include <iostream>
#include <mutex>
#include <ranges>
#include <string>
#include <vector>

#include <magic_enum/magic_enum.hpp>

#include <fc-lite/lut.hpp>
#include <fc/exception/exception.hpp>
#include <fc/filesystem.hpp>
#include <fc/crypto/ethereum/ethereum_utils.hpp>
#include <fc/reflect/reflect.hpp>
#include <fc/variant.hpp>

namespace fc::network::ethereum {

namespace abi {
enum class invoke_target_type { function, constructor, event, error };

enum class data_type : int64_t {
   boolean,
   int8,
   int16,
   int32,
   int64,
   int128,
   int256,
   uint8,
   uint16,
   uint32,
   uint64,
   uint128,
   uint256,
   fixed32,
   fixed64,
   fixed128,
   fixed256,
   ufixed32,
   ufixed64,
   ufixed128,
   ufixed256,
   bytes1,
   bytes2,
   bytes3,
   bytes4,
   bytes5,
   bytes6,
   bytes7,
   bytes8,
   bytes9,
   bytes10,
   bytes11,
   bytes12,
   bytes13,
   bytes14,
   bytes15,

   bytes16,
   bytes17,
   bytes18,
   bytes19,
   bytes20,
   bytes21,
   bytes22,
   bytes23,
   bytes24,
   bytes25,
   bytes26,
   bytes27,
   bytes28,
   bytes29,
   bytes30,
   bytes31,
   bytes32,
   address,
   function,
   bytes,
   string,
   tuple,
   event,
   error
};

constexpr std::array data_type_numeric_prefixes{"uint", "int", "ufixed", "fixed"};
constexpr std::array data_type_numeric_signed_prefixes{"int", "fixed"};
constexpr std::string_view data_type_bytes_prefix{"bytes"};

constexpr bool is_data_type_bytes(data_type type) {
   return magic_enum::enum_name(type).starts_with(data_type_bytes_prefix);
}

constexpr bool is_data_type_numeric(data_type type) {
   auto dts = magic_enum::enum_name(type);
   for (auto& prefix : data_type_numeric_prefixes) {
      if (dts.starts_with(prefix))
         return true;
   }
   return false;
}

constexpr bool is_data_type_numeric_signed(data_type type) {
   auto dts = magic_enum::enum_name(type);
   for (auto& prefix : data_type_numeric_signed_prefixes) {
      if (dts.starts_with(prefix))
         return true;
   }
   return false;
}

// bool is_data_type_numeric(data_type type);

// bool is_data_type_numeric_signed(data_type type);

enum class padding_type { none, left, right };

constexpr std::array data_dynamic_types{data_type::bytes, data_type::string};

constexpr std::array data_dynamic_or_fixed_types{data_type::tuple, data_type::error, data_type::event};

constexpr bool data_type_requires_dynamic_size(data_type type) {
   return std::ranges::contains(data_dynamic_types, type);
}

constexpr bool data_type_supports_dynamic_size(data_type type) {
   return data_type_requires_dynamic_size(type) || std::ranges::contains(data_dynamic_or_fixed_types, type);
}

struct component_type {
   struct list_config_type {
      bool is_list{false};
      std::size_t size{0};

      bool is_fixed_list() const { return is_list && size > 0; }
      bool is_dynamic_list() const { return is_list && !is_fixed_list(); }
   };

   std::string name{""};
   data_type type{data_type::uint8};
   std::string internal_type{""};
   list_config_type list_config{};
   std::vector<component_type> components{};
   bool is_dynamic() const {
      // IF CONTAINER WITH DYNAMIC CHILD (AT ANY DEPTH)
      if (is_container() && std::ranges::any_of(components, [](const auto& c) { return c.is_dynamic(); }))
         return true;

      return data_type_requires_dynamic_size(type) || list_config.is_dynamic_list();
   };

   bool is_container() const {
      return type == data_type::error || type == data_type::event || type == data_type::tuple;
   };

   bool is_tuple() const { return type == data_type::tuple; };

   bool is_error() const { return type == data_type::error; };

   bool is_event() const { return type == data_type::event; };

   bool is_list() const { return list_config.is_list; };

   component_type() = default;
   explicit component_type(std::string name, data_type type, std::vector<component_type> components = {},
                           const std::optional<list_config_type>& list_config = std::nullopt)
      : name(std::move(name))
      , type(type)
      , list_config(list_config.value_or(list_config_type{}))
      , components(std::move(components)) {}
};

struct contract {
   std::string name;
   abi::invoke_target_type type;

   // [ { name: "param1", type: "uint256" }, { name: "param2", type: "string" }
   std::vector<component_type> inputs;

   // [ { name: "return1", type: "uint256" }, { name: "return2", type: "string" }]
   std::vector<component_type> outputs;
};

std::string to_contract_component_signature(const component_type& component);
std::string to_contract_function_signature(const contract& contract);
fc::crypto::ethereum::keccak256_hash_t to_contract_function_selector(const contract& contract);

std::vector<contract> parse_contracts(const std::filesystem::path& json_abi_file);

contract parse_contract(const fc::variant& v);

} // namespace abi

using contract_invoke_data = fc::variant;

using contract_invoke_data_items = std::vector<contract_invoke_data>;

/**
 * Encode a contract call
 * @return hex string of encoded call `data` field in RLP format
 */
std::string contract_encode_data(const abi::contract& contract, const contract_invoke_data_items& params);

template <typename T>
concept not_abi_data_params_t = !std::is_same_v<std::decay_t<T>, contract_invoke_data_items>;

template <typename... Args>
   requires(not_abi_data_params_t<std::tuple_element_t<0, std::tuple<Args...>>> &&
            !std::is_pointer_v<std::tuple_element_t<0, std::tuple<Args...>>>)

std::string contract_encode_data(const abi::contract& abi, const Args&... args) {
   contract_invoke_data_items params = {args...};
   return contract_encode_data(abi, params);
}

/**
 * Decode a contract call
 * @return decoded function name and parameters
 */
fc::variant contract_decode_data(const abi::contract& contract, const std::string& encoded_invoke_data, bool use_inputs_instead_of_outputs = false);
} // namespace fc::network::ethereum

namespace fc {
template <>
struct reflector<fc::network::ethereum::abi::data_type> {
   using data_t = fc::network::ethereum::abi::data_type;
   using is_defined = fc::true_type;
   using is_enum = fc::true_type;
   static const char* to_string(data_t elem) {
      static std::mutex mutex;
      static std::map<data_t,std::string_view> data_type_name_map{};
      if (!data_type_name_map.contains(elem)) {
         std::scoped_lock lock(mutex);
         if (!data_type_name_map.contains(elem)) {
            switch (elem) {
            case fc::network::ethereum::abi::data_type::boolean: {
               data_type_name_map[elem] = "bool";
               break;
            }
            default:
               data_type_name_map[elem] = magic_enum::enum_name(elem);
               break;
            }
         }
      }

      return data_type_name_map[elem].data();
   }

   static const char* to_string(int64_t i) {
      return to_string(magic_enum::enum_cast<fc::network::ethereum::abi::data_type>(i).value());
   }

   static std::string to_fc_string(fc::network::ethereum::abi::data_type elem) {
      switch (elem) {
      case fc::network::ethereum::abi::data_type::boolean:
         return std::string{"bool"};
      default:
         return std::string{magic_enum::enum_name(elem)};
      }
   }
   static std::string to_fc_string(int64_t i) { return to_fc_string(magic_enum::enum_cast<fc::network::ethereum::abi::data_type>(i).value()); }
   static fc::network::ethereum::abi::data_type from_int(int64_t i) {
      fc::network::ethereum::abi::data_type e = magic_enum::enum_cast<fc::network::ethereum::abi::data_type>(i).value();
      switch (e) {
         BOOST_PP_SEQ_FOR_EACH(
            FC_REFLECT_ENUM_FROM_STRING_CASE, fc::network::ethereum::abi::data_type,
            (boolean)(int8)(int16)(int32)(int64)(int128)(int256)(uint8)(uint16)(uint32)(uint64)(uint128)(uint256)(bytes1)(bytes2)(bytes4)(bytes8)(bytes16)(bytes32)(bytes)(string)(address)(tuple)(event)(error))
         break;
      default:
         fc::throw_bad_enum_cast(i, BOOST_PP_STRINGIZE(fc::network::ethereum::abi::data_type) );
      }
      return e;
   }
   static fc::network::ethereum::abi::data_type from_string(const char* s) {
      if (strcmp(s, "boolean") == 0 ||
         strcmp(s, "bool") == 0 ||
         strcmp(s, "fc::network::ethereum::abi::data_type_boolean") == 0 ||
         strcmp(s, "fc::network::ethereum::abi::data_type_bool") == 0)
         return fc::network::ethereum::abi::data_type::boolean;

      auto e = magic_enum::enum_cast<fc::network::ethereum::abi::data_type>(s);
      if (e.has_value()) return e.value();
      FC_THROW_EXCEPTION_FMT(fc::invalid_arg_exception, "Invalid Ethereum ABI data type: {}", std::string(s));
   }
   static fc::network::ethereum::abi::data_type from_string(const std::string& str) {
      return from_string(str.c_str());
   }

   template <typename Visitor>
   static void visit(Visitor& v) {
      // NOT IMPLEMENTED AS IT IS NOT NEEDED FOR CURRENT USE CASES
   }
};
template <>
struct get_typename<fc::network::ethereum::abi::data_type> {
   static const char* name() {
      return BOOST_PP_STRINGIZE(fc::network::ethereum::abi::data_type);
   }
};
}; // namespace fc

FC_REFLECT_ENUM(fc::network::ethereum::abi::invoke_target_type, (function)(constructor)(event)(error));

FC_REFLECT(fc::network::ethereum::abi::component_type::list_config_type, (is_list)(size));
FC_REFLECT(fc::network::ethereum::abi::component_type, (name)(type)(list_config)(components)(internal_type));
FC_REFLECT(fc::network::ethereum::abi::contract, (name)(type)(inputs)(outputs));

namespace fc {
void from_variant(const fc::variant& var, fc::network::ethereum::abi::component_type& vo);
void from_variant(const fc::variant& var, fc::network::ethereum::abi::contract& vo);

/**
 * Simple alias to the ethereum data type reflector
 */
using ethereum_abi_data_type_reflector = reflector<fc::network::ethereum::abi::data_type>;
} // namespace fc
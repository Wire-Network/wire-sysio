#pragma once

#include <fc/variant.hpp>
#include <iostream>
#include <string>
#include <variant>
#include <vector>

namespace fc::network::ethereum {
// using namespace fc::crypto::ethereum;

enum class ethereum_contract_abi_type { function, constructor, event };

struct ethereum_contract_abi {
   std::string name;
   ethereum_contract_abi_type type; // e.g., "function", "constructor", "event"
   std::vector<std::pair<std::string, std::string>> inputs;
   // [ { name: "param1", type: "uint256" }, { name: "param2", type: "string" }
   std::vector<std::pair<std::string, std::string>> outputs;
   // [ { name: "return1", type: "uint256" }, { name: "return2", type: "string" }]
   std::string signature; // "setNumber(uint256,string)"
};

ethereum_contract_abi parse_ethereum_contract_abi_signature(const std::string& sig);

using abi_params_vector = std::vector<fc::variant>;
/**
 * Encode a contract call
 * @return hex string of encoded call `data` field in RLP format
 */
std::string ethereum_contract_call_encode(const std::variant<ethereum_contract_abi, std::string>& abi,
                                          const abi_params_vector& params);

template <typename T>
concept not_abi_params_vector = !std::is_same_v<std::decay_t<T>, abi_params_vector>;

template <typename... Args>
   requires(not_abi_params_vector<std::tuple_element_t<0, std::tuple<Args...>>> &&
            !std::is_pointer_v<std::tuple_element_t<0, std::tuple<Args...>>>)

std::string ethereum_contract_call_encode(const std::variant<ethereum_contract_abi, std::string>& abi,
                                          const Args&... args) {
   ;
   abi_params_vector params = {args...};
   return ethereum_contract_call_encode(abi, params);
}

/**
 * Decode a contract call
 * @return decoded function name and parameters
 */
std::pair<ethereum_contract_abi, std::vector<std::string>>
ethereum_contract_call_decode(const std::string& ethereum_encoded_call_hex);
} // namespace fc::network::ethereum

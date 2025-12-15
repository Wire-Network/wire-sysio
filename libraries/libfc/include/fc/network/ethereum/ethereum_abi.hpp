#pragma once

#include <iostream>
#include <string>
#include <vector>
#include <variant>
#include <fc/variant.hpp>

namespace fc::network::ethereum {

// using namespace fc::crypto::ethereum;

enum class ethereum_contract_abi_type {
   function,
   constructor,
   event
   };

struct ethereum_contract_abi {
   std::string name;
   ethereum_contract_abi_type type; // e.g., "function", "constructor", "event"
   std::vector<std::pair<std::string, std::string>> inputs; // [ { name: "param1", type: "uint256" }, { name: "param2", type: "string" }
   std::vector<std::pair<std::string, std::string>> outputs; // [ { name: "return1", type: "uint256" }, { name: "return2", type: "string" }]
   std::string signature; // "setNumber(uint256,string)"
};

ethereum_contract_abi parse_ethereum_contract_abi_signature(const std::string& sig);

/**
 * Encode a contract call
 * @return hex string of encoded call `data` field in RLP format
 */
std::string ethereum_contract_call_encode(const std::variant<ethereum_contract_abi,std::string>& abi, const std::vector<fc::variant>& params);

template<typename ...Args>
std::string ethereum_contract_call_encode(const std::variant<ethereum_contract_abi,std::string>& abi, const Args&... args) {;
   std::vector<std::string> params = {args...};
   return ethereum_contract_call_encode(abi, params);
}

/**
 * Decode a contract call
 * @return decoded function name and parameters
 */
std::pair<ethereum_contract_abi,std::vector<std::string>> ethereum_contract_call_decode(const std::string& ethereum_encoded_call_hex);

}
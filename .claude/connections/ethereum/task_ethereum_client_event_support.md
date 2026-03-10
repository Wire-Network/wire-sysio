# Modify Ethereum Client Support for Events

## Overview

Modify Ethereum Client (`fc::network::ethereum::ethereum_client`) Support for Events to provide an ABI driven concrete
type decoding template.

### Current data structure for events

```c++
using namespace fc::network::ethereum;

struct ethereum_event_data {
   fc::crypto::ethereum::address contract_address;
   std::string event_name;
   fc::crypto::ethereum::bytes data;
   fc::uint256 block_number;
   std::string transaction_hash;
   uint32_t log_index{0};
   uint32_t transaction_index{0};
   std::vector<std::string> topics;
   fc::variant decoded_data;
};

```

### Refactor to use ABI decoding template

```c++
using namespace fc::network::ethereum;

// Add new exception code to `fc::exception_code`, `ethereum_abi_decode_exception_code`
FC_DECLARE_EXCEPTION(ethereum_abi_decode_exception, ethereum_abi_decode_exception_code,"ethereum_abi_decode_exception" )

struct ethereum_event_data {
   fc::crypto::ethereum::address contract_address;
   std::string event_name;
   fc::crypto::ethereum::bytes data;
   fc::uint256 block_number;
   std::string transaction_hash;
   uint32_t log_index{0};
   uint32_t transaction_index{0};
   std::vector<std::string> topics;

   // Special handling when `T == fc::variant|fc::variant_object`; simply decode to `fc::variant_object`
   template<typename T>
   std::expected<T,fc::ethereum_abi_decode_exception> decode() const {
       try {
           T value = ...; // decode data to `T`
           return value;
       } catch (auto& e) {
           return ethereum_abi_decode_exception(e.what());
       }
   };
};

```

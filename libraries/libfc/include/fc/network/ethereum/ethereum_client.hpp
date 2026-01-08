// SPDX-License-Identifier: MIT
#pragma once

#include <fc/int256.hpp>
#include <fc/network/json_rpc/json_rpc_client.hpp>
#include <fc/crypto/signature_provider.hpp>
#include <fc/network/ethereum/ethereum_abi.hpp>
#include <fc/crypto/ethereum/ethereum_types.hpp>

namespace fc::network::ethereum {
using namespace fc::crypto;
using namespace fc::crypto::ethereum;
using namespace fc::network::json_rpc;

class ethereum_client;
using ethereum_client_ptr = std::shared_ptr<ethereum_client>;

template<typename ... Args>
using ethereum_contract_call_fn = std::function<fc::variant(
   const std::string& block_tag,Args&...)>;
template<typename ... Args>
using ethereum_contract_tx_fn = std::function<fc::variant(Args&...)>;

class ethereum_contract_client : public std::enable_shared_from_this<ethereum_contract_client> {
public:
   const address contract_address;
   const std::string contract_address_hex;
   const ethereum_client_ptr client;
   ethereum_contract_client() = delete;
   ethereum_contract_client(const ethereum_client_ptr& client, const address_compat_type& contract_address_compat):
   contract_address(ethereum::to_address(contract_address_compat)),
   contract_address_hex(fc::to_hex(contract_address)),
   client(client) {};

   virtual ~ethereum_contract_client() = default;
protected:
   std::map<std::string, ethereum_contract_abi> abi_map{};

   template<typename ... Args>
   ethereum_contract_call_fn<Args...> create_call(const std::string& abi_signature);

   template<typename ... Args>
   ethereum_contract_tx_fn<Args...> create_tx(const std::string& abi_signature);

private:
   std::mutex _abi_mutex{};

};


/**
 * @class ethereum_client
 * @brief A class to interact with an Ethereum or Ethereum-compatible node.
 *
 * This class provides methods to send RPC requests to an Ethereum node,
 * parse the responses, and process the results. It supports common Ethereum RPC methods
 * such as retrieving block data, transaction details, estimating gas, and more.
 */
class ethereum_client : public std::enable_shared_from_this<ethereum_client> {
public:
   ethereum_client() = delete;

   /**
    * @brief Constructs an EthereumClient instance.
    * @param sig_provider `signature_provider` shared pointer
    * @param url_source The URL of the Ethereum node (e.g., Infura, local node).
    * @param chain_id optional uint256 encapsulating the chain id
    */
   ethereum_client(const signature_provider_ptr& sig_provider, const std::variant<std::string, fc::url>& url_source, std::optional<fc::uint256> chain_id = std::nullopt);

   /**
    * @brief General method to send RPC requests.
    * @param method The name of the RPC method to call (e.g., "eth_blockNumber").
    * @param params The parameters for the RPC method (as a JSON object).
    * @return The raw JSON response as a string, wrapped in std::optional.
    */
   fc::variant execute(const std::string& method, const fc::variant& params);

   fc::variant execute_contract_call(
      const address& contract_address,
      const ethereum_contract_abi & abi,
      const std::string& block_tag,
      const fc::variants& params);

   fc::variant execute_contract_tx(
      const eip1559_tx& tx,
      const ethereum_contract_abi & abi,
      const fc::variants& params = {},
      bool sign = true);


   /**
    * @brief Parses the response from the Ethereum node.
    * @param response The raw response from the Ethereum node.
    * @return The parsed JSON response, or an empty std::optional in case of failure.
    */
   fc::variant parse_json_response(const std::string& response) = delete;

   /**
    * @brief Processes the parsed JSON response and outputs relevant information.
    * @param result_json The parsed JSON response from the node.
    */
   void process_result(const fc::variant& result_json) = delete;

   // Ethereum RPC Methods

   /**
    * @brief Retrieves the latest block number.
    * @return The block number as a string in hexadecimal format, or an empty std::optional if an error occurs.
    */
   fc::uint256 get_block_number();

   /**
    * @brief Retrieves block information by block number.
    * @param block_number The block number (hexadecimal).
    * @param full_transaction_data Flag to determine whether to fetch full transaction data.
    * @return The block data in JSON format, or an empty std::optional if an error occurs.
    */
   fc::variant get_block_by_number(const std::string& block_number, bool full_transaction_data);

   /**
    * @brief Retrieves block information by block hash.
    * @param block_hash The block hash (hexadecimal).
    * @param full_transaction_data Flag to determine whether to fetch full transaction data.
    * @return The block data in JSON format, or an empty std::optional if an error occurs.
    */
   fc::variant get_block_by_hash(const std::string& block_hash, bool full_transaction_data);

   /**
    * @brief Retrieves transaction information by transaction hash.
    * @param tx_hash The transaction hash.
    * @return The transaction data in JSON format, or an empty std::optional if an error occurs.
    */
   fc::variant get_transaction_by_hash(const std::string& tx_hash);

   /**
    * @brief Estimates the gas required for a transaction.
    * @param from The sender address.
    * @param to The recipient address.
    * @param value The value to be sent (in hexadecimal).
    * @return The estimated gas (in hexadecimal), or an empty std::optional if an error occurs.
    */
   std::string estimate_gas(const std::string& from, const std::string& to, const std::string& value);

   /**
    * @brief Retrieves the current gas price.
    * @return The current gas price in hexadecimal format, or an empty std::optional if an error occurs.
    */
   std::string get_gas_price();

   /**
    * @brief Sends an encoded transaction.
    * @param raw_tx_data The raw transaction data.
    * @return The transaction hash (if successful), or an empty std::optional if an error occurs.
    */
   std::string send_transaction(const std::string& raw_tx_data);

   /**
    * @brief Sends a raw encoded transaction.
    * @param raw_tx_data The raw transaction data.
    * @return The transaction hash (if successful),
    * @throws fc::network::json_rpc::json_rpc_exception if the RPC call fails or returns an error.
    */
   std::string send_raw_transaction(const std::string& raw_tx_data);

   /**
    * @brief Retrieves logs based on filter parameters.
    * @param params The filter parameters for fetching logs.
    * @return The logs in JSON format, or an empty std::optional if an error occurs.
    */
   fc::variant get_logs(const fc::variant& params);

   /**
    * @brief Retrieves the transaction receipt by transaction hash.
    * @param tx_hash The transaction hash.
    * @return The transaction receipt data in JSON format, or an empty std::optional if an error occurs.
    */
   fc::variant get_transaction_receipt(const std::string& tx_hash);

   // Additional Methods

   /**
    * @brief Retrieves the transaction count (nonce) for an address.
    * @param address The address for which to fetch the transaction count.
    * @return The transaction count (nonce) as a string, or an empty std::optional if an error occurs.
    */
   fc::uint256 get_transaction_count(const address_compat_type& address, const std::string& block_tag = "pending");

   /**
    * @brief Retrieves the chain ID of the connected Ethereum network.
    * @return The chain ID as a string, or an empty std::optional if an error occurs.
    */
   fc::uint256 get_chain_id();

   /**
    * @brief Retrieves the version of the connected Ethereum network.
    * @return The network version as a string, or an empty std::optional if an error occurs.
    */
   fc::uint256 get_network_version();

   /**
    * @brief Checks if the Ethereum node is syncing.
    * @return Syncing status in JSON format, or an empty std::optional if an error occurs.
    */
   fc::variant get_syncing_status();

   ethereum::address get_signer_address();
   eip1559_tx create_default_tx(const address_compat_type& to);

   template<typename C>
   std::shared_ptr<C> get_contract(const address_compat_type& address_compat) {
      std::scoped_lock<std::mutex> lock(_contracts_map_mutex);
      auto addr = ethereum::to_address(address_compat);
      if (!_contracts_map.contains(addr)) {
         _contracts_map[addr] = std::make_shared<C>(shared_from_this(), addr);
      }
      return std::dynamic_pointer_cast<C>(_contracts_map[addr]);
   }

private:
   signature_provider_ptr _signature_provider; ///< Reference to the network adapter used for sending requests.
   json_rpc_client _client;
   std::optional<fc::uint256> _chain_id;
   std::mutex _contracts_map_mutex{};
   std::map<address, std::shared_ptr<ethereum_contract_client>> _contracts_map{};
};


template <typename ... Args>
ethereum_contract_call_fn<Args...> ethereum_contract_client::create_call(const std::string& abi_signature) {
   std::scoped_lock<std::mutex> lock(_abi_mutex);
   if (!abi_map.contains(abi_signature)) {
      abi_map[abi_signature] = parse_ethereum_contract_abi_signature(abi_signature);
   }
   ethereum_contract_abi& abi = abi_map[abi_signature];
   return [this, abi](const std::string& block_tag, Args&... args) -> fc::variant {
      fc::variants params = {args...};
      return client->execute_contract_call(contract_address, abi, block_tag,params);

   };
}


template <typename ... Args>
ethereum_contract_tx_fn<Args...> ethereum_contract_client::create_tx(const std::string& abi_signature) {
   std::scoped_lock<std::mutex> lock(_abi_mutex);
   if (!abi_map.contains(abi_signature)) {
      abi_map[abi_signature] = parse_ethereum_contract_abi_signature(abi_signature);
   }
   ethereum_contract_abi& abi = abi_map[abi_signature];
   return [this, abi](const Args&... args) -> fc::variant {
      fc::variants params = {args...};
      auto tx = client->create_default_tx(contract_address);
      return client->execute_contract_tx(tx, abi, params);
   };
}

} // namespace sysio::outpost_client::ethereum
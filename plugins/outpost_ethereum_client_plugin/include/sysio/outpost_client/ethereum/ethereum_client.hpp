// SPDX-License-Identifier: MIT
#pragma once

#include <fc/network/json_rpc/json_rpc_client.hpp>
#include <sysio/signature_provider_manager_plugin/signature_provider_manager_plugin.hpp>

namespace sysio::outpost_client::ethereum {
using namespace fc::network::json_rpc;

class ethereum_client;
using ethereum_client_ptr = std::shared_ptr<ethereum_client>;

/**
 * @class ethereum_client
 * @brief A class to interact with an Ethereum or Ethereum-compatible node.
 *
 * This class provides methods to send RPC requests to an Ethereum node,
 * parse the responses, and process the results. It supports common Ethereum RPC methods
 * such as retrieving block data, transaction details, estimating gas, and more.
 */
class ethereum_client {
public:
   /**
    * @brief Constructs an EthereumClient instance.
    * @param sig_provider `signature_provider` shared pointer
    * @param url_source The URL of the Ethereum node (e.g., Infura, local node).
    */
   ethereum_client(const signature_provider_ptr& sig_provider, const std::variant<std::string, fc::url>& url_source);

   /**
    * @brief General method to send RPC requests.
    * @param method The name of the RPC method to call (e.g., "eth_blockNumber").
    * @param params The parameters for the RPC method (as a JSON object).
    * @return The raw JSON response as a string, wrapped in std::optional.
    */
   fc::variant execute(const std::string& method, const fc::variant& params);

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
   std::string get_block_number();

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
    * @brief Sends a raw transaction.
    * @param raw_tx_data The raw transaction data.
    * @return The transaction hash (if successful), or an empty std::optional if an error occurs.
    */
   std::string send_transaction(const std::string& raw_tx_data);

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
   std::string get_transaction_count(const std::string& address);

   /**
    * @brief Retrieves the chain ID of the connected Ethereum network.
    * @return The chain ID as a string, or an empty std::optional if an error occurs.
    */
   std::string get_chain_id();

   /**
    * @brief Retrieves the version of the connected Ethereum network.
    * @return The network version as a string, or an empty std::optional if an error occurs.
    */
   std::string get_network_version();

   /**
    * @brief Checks if the Ethereum node is syncing.
    * @return Syncing status in JSON format, or an empty std::optional if an error occurs.
    */
   fc::variant get_syncing_status();

private:
   signature_provider_ptr _signature_provider; ///< Reference to the network adapter used for sending requests.
   json_rpc_client _client;
};

} // namespace sysio::outpost_client::ethereum
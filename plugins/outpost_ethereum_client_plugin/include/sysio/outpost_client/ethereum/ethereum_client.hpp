// SPDX-License-Identifier: MIT
#pragma once

#include <sysio/outpost_client/ethereum/network_adapter.hpp>

#include <json/json.h>

namespace sysio::outpost_client::ethereum {

/**
 * @class ethereum_client
 * @brief A class to interact with an Ethereum or Ethereum-compatible node.
 *
 * This class provides methods to send RPC requests to an Ethereum node,
 * parse the responses, and process the results. It supports common Ethereum RPC methods
 * such as retrieving block data, transaction details, estimating gas, and more.
 */
class LIB_EXPORT ethereum_client {
public:
    /**
     * @brief Constructs an EthereumClient instance.
     * @param node_url The URL of the Ethereum node (e.g., Infura, local node).
     * @param net_adapter A reference to the NetworkAdapter used for sending requests.
     */
    ethereum_client(const std::string& node_url, network_adapter& net_adapter);

    /**
     * @brief General method to send RPC requests.
     * @param method The name of the RPC method to call (e.g., "eth_blockNumber").
     * @param params The parameters for the RPC method (as a JSON object).
     * @return The raw JSON response as a string, wrapped in std::optional.
     */
    std::optional<std::string> execute(const std::string& method, const Json::Value& params);

    /**
     * @brief Parses the response from the Ethereum node.
     * @param response The raw response from the Ethereum node.
     * @return The parsed JSON response, or an empty std::optional in case of failure.
     */
    std::optional<Json::Value> parse_json_response(const std::string& response);

    /**
     * @brief Processes the parsed JSON response and outputs relevant information.
     * @param result_json The parsed JSON response from the node.
     */
    void process_result(const Json::Value& result_json);

    // Ethereum RPC Methods

    /**
     * @brief Retrieves the latest block number.
     * @return The block number as a string in hexadecimal format, or an empty std::optional if an error occurs.
     */
    std::optional<std::string> get_block_number();

    /**
     * @brief Retrieves block information by block number.
     * @param blockNumber The block number (hexadecimal).
     * @param fullTransactionData Flag to determine whether to fetch full transaction data.
     * @return The block data in JSON format, or an empty std::optional if an error occurs.
     */
    std::optional<Json::Value> get_block_by_number(const std::string& blockNumber, bool fullTransactionData);

    /**
     * @brief Retrieves block information by block hash.
     * @param block_hash The block hash (hexadecimal).
     * @param full_transaction_data Flag to determine whether to fetch full transaction data.
     * @return The block data in JSON format, or an empty std::optional if an error occurs.
     */
    std::optional<Json::Value> get_block_by_hash(const std::string& block_hash, bool full_transaction_data);

    /**
     * @brief Retrieves transaction information by transaction hash.
     * @param tx_hash The transaction hash.
     * @return The transaction data in JSON format, or an empty std::optional if an error occurs.
     */
    std::optional<Json::Value> get_transaction_by_hash(const std::string& tx_hash);

    /**
     * @brief Estimates the gas required for a transaction.
     * @param from The sender address.
     * @param to The recipient address.
     * @param value The value to be sent (in hexadecimal).
     * @return The estimated gas (in hexadecimal), or an empty std::optional if an error occurs.
     */
    std::optional<std::string> estimate_gas(const std::string& from, const std::string& to, const std::string& value);

    /**
     * @brief Retrieves the current gas price.
     * @return The current gas price in hexadecimal format, or an empty std::optional if an error occurs.
     */
    std::optional<std::string> get_gas_price();

    /**
     * @brief Sends a raw transaction.
     * @param raw_tx_data The raw transaction data.
     * @return The transaction hash (if successful), or an empty std::optional if an error occurs.
     */
    std::optional<std::string> send_transaction(const std::string& raw_tx_data);

    /**
     * @brief Retrieves logs based on filter parameters.
     * @param params The filter parameters for fetching logs.
     * @return The logs in JSON format, or an empty std::optional if an error occurs.
     */
    std::optional<Json::Value> get_logs(const Json::Value& params);

    /**
     * @brief Retrieves the transaction receipt by transaction hash.
     * @param tx_hash The transaction hash.
     * @return The transaction receipt data in JSON format, or an empty std::optional if an error occurs.
     */
    std::optional<Json::Value> get_transaction_receipt(const std::string& tx_hash);

    // Additional Methods

    /**
     * @brief Retrieves the transaction count (nonce) for an address.
     * @param address The address for which to fetch the transaction count.
     * @return The transaction count (nonce) as a string, or an empty std::optional if an error occurs.
     */
    std::optional<std::string> get_transaction_count(const std::string& address);

    /**
     * @brief Retrieves the chain ID of the connected Ethereum network.
     * @return The chain ID as a string, or an empty std::optional if an error occurs.
     */
    std::optional<std::string> get_chain_id();

    /**
     * @brief Retrieves the version of the connected Ethereum network.
     * @return The network version as a string, or an empty std::optional if an error occurs.
     */
    std::optional<std::string> get_network_version();

    /**
     * @brief Checks if the Ethereum node is syncing.
     * @return Syncing status in JSON format, or an empty std::optional if an error occurs.
     */
    std::optional<std::string> get_syncing_status();

 private:
    std::string      node_url_;         ///< The URL of the Ethereum node.
    network_adapter& net_adapter_; ///< Reference to the network adapter used for sending requests.
};

} // namespace sysio::outpost_client::ethereum

#include <ethash/keccak.hpp>
#include <fc/crypto/ethereum/ethereum_utils.hpp>
#include <fc/log/logger.hpp>
#include <fc/network/ethereum/ethereum_client.hpp>
#include <fc/network/ethereum/ethereum_rlp_encoder.hpp>
#include <iostream>

namespace fc::network::ethereum {

namespace {
using namespace fc::crypto;
using namespace fc::crypto::ethereum;
using namespace fc::network::json_rpc;
} // namespace

/**
 * @brief Checks if an ABI definition exists for the given contract name
 * 
 * @param contract_name Name of the contract function/event
 * @return true if ABI exists, false otherwise
 */
bool ethereum_contract_client::has_abi(const std::string& contract_name) {
   return _abi_map.readable().contains(contract_name);
}

/**
 * @brief Retrieves the ABI definition for the given contract name
 * 
 * @param contract_name Name of the contract function/event
 * @return Reference to the ABI contract definition
 * @throws std::out_of_range if contract_name not found
 */
const abi::contract& ethereum_contract_client::get_abi(const std::string& contract_name) {
   return _abi_map.readable().at(contract_name);
}

/**
 * @brief Constructs an ethereum_client instance
 * 
 * Initializes the client with a signature provider for transaction signing,
 * a JSON-RPC endpoint URL, and an optional chain ID. The Ethereum address
 * is derived from the signature provider's public key.
 * 
 * @param sig_provider Signature provider for signing transactions
 * @param url_source URL of the Ethereum node (string or fc::url)
 * @param chain_id Optional chain ID (if not provided, will be fetched from the node)
 */
ethereum_client::ethereum_client(const signature_provider_ptr& sig_provider,
                                 const std::variant<std::string, fc::url>& url_source,
                                 std::optional<fc::uint256> chain_id)
   : _signature_provider(sig_provider)
   , _address(to_address(_signature_provider->public_key))
   , _client(json_rpc_client::create(url_source))
   , _chain_id(chain_id) {}

/**
 * @brief Executes a JSON-RPC method call on the Ethereum node
 * 
 * @param method The RPC method name (e.g., "eth_blockNumber", "eth_call")
 * @param params The parameters for the RPC method as a variant
 * @return The response from the Ethereum node as a variant
 * @throws fc::network::json_rpc::json_rpc_exception if the RPC call fails
 */
fc::variant ethereum_client::execute(const std::string& method, const fc::variant& params) {
   return _client.call(method, params);
}

/**
 * @brief Executes a contract view function (read-only call)
 * 
 * Encodes the function call according to the ABI, executes it via eth_call,
 * and returns the result. This does not create a transaction or modify state.
 * 
 * @param contract_address The address of the smart contract
 * @param abi The ABI definition of the function to call
 * @param block_tag The block at which to execute the call (e.g., "latest", "pending")
 * @param params The parameters to pass to the contract function
 * @return The result of the contract call as a variant
 * @throws fc::network::json_rpc::json_rpc_exception if the call fails
 */
fc::variant ethereum_client::execute_contract_view_fn(const address& contract_address, const abi::contract& abi,
                                                      const std::string& block_tag,
                                                      const contract_invoke_data_items& params) {
   auto abi_call_encoded = contract_encode_data(abi, params);
   auto to_data_mvo = fc::mutable_variant_object("to", to_hex(contract_address, true))("data", abi_call_encoded);
   fc::variants rpc_params = {to_data_mvo, fc::variant(block_tag)};
   return execute("eth_call", rpc_params);
}

/**
 * @brief Executes a contract transaction function (state-changing call)
 * 
 * Encodes the function call according to the ABI, creates a signed transaction,
 * and submits it to the network via eth_sendRawTransaction.
 * 
 * @param source_tx The base EIP-1559 transaction (with gas, nonce, etc.)
 * @param abi The ABI definition of the function to call
 * @param params The parameters to pass to the contract function
 * @param sign If true, signs the transaction with the signature provider
 * @return The transaction hash as a variant
 * @throws fc::network::json_rpc::json_rpc_exception if the transaction submission fails
 */
fc::variant ethereum_client::execute_contract_tx_fn(const eip1559_tx& source_tx, const abi::contract& abi,
                                                    const contract_invoke_data_items& params, bool sign) {
   eip1559_tx tx = source_tx;
   tx.data = from_hex(contract_encode_data(abi, params));
   auto tx_encoded = rlp::encode_eip1559_unsigned_typed(tx);
   if (sign) {
      // auto tx_encoded_unsigned     = rlp::encode_eip1559_unsigned_typed(tx);
      auto tx_hash_data = fc::crypto::ethereum::hash_message(tx_encoded);
      fc::sha256 tx_hash(reinterpret_cast<const char*>(tx_hash_data.data()), tx_hash_data.size());
      auto tx_sig = _signature_provider->sign(tx_hash);
      FC_ASSERT(tx_sig.contains<fc::em::signature_shim>());
      auto& tx_sig_shim = tx_sig.get<fc::em::signature_shim>();
      auto& tx_sig_data = tx_sig_shim.serialize();
      std::copy_n(tx_sig_data.begin(), 32, tx.r.begin());
      std::copy_n(tx_sig_data.begin() + 32, 32, tx.s.begin());
      tx.v = tx_sig_data[64] - 27; // recovery id
      tx_encoded = rlp::encode_eip1559_signed_typed(tx);
   }

   return send_raw_transaction(to_hex(tx_encoded));
}


// JSON parsing helpers removed in favor of fc::variant returned by json_rpc_client

/**
 * @brief Retrieves the transaction count (nonce) for an address
 * 
 * Gets the number of transactions sent from the specified address at the given block.
 * This is commonly used to determine the nonce for the next transaction.
 * 
 * @param address The Ethereum address to query
 * @param block_tag The block at which to query (e.g., "latest", "pending")
 * @return The transaction count as a uint256
 * @throws fc::network::json_rpc::json_rpc_exception if the RPC call fails
 */
fc::uint256 ethereum_client::get_transaction_count(const address_compat_type& address, const std::string& block_tag) {
   auto from_addr = fc::crypto::ethereum::to_address(address);
   auto from_addr_hex = to_hex(from_addr, true);
   fc::variants params{from_addr_hex, block_tag};
   auto res = execute("eth_getTransactionCount", params);
   dlogf("tx_count: {}", res.as_string());
   return to_uint256(res);
}

/**
 * @brief Retrieves the chain ID of the connected Ethereum network
 * 
 * Fetches the chain ID from the node on first call and caches it for subsequent calls.
 * The chain ID is used in EIP-155 replay protection for transactions.
 * 
 * @return The chain ID as a uint256
 * @throws fc::network::json_rpc::json_rpc_exception if the RPC call fails
 */
fc::uint256 ethereum_client::get_chain_id() {
   static std::mutex mutex;
   std::scoped_lock<std::mutex> lock(mutex);
   if (_chain_id.has_value()) {
      return *_chain_id;
   }

   fc::variants params; // empty array
   _chain_id = to_uint256(execute("eth_chainId", params));

   return _chain_id.value();
}

/**
 * @brief Retrieves the network version of the connected Ethereum network
 * 
 * Gets the current network ID (e.g., 1 for mainnet, 3 for Ropsten).
 * 
 * @return The network version as a uint256
 * @throws fc::network::json_rpc::json_rpc_exception if the RPC call fails
 */
fc::uint256 ethereum_client::get_network_version() {
   fc::variants params; // Empty params array
   return to_uint256(execute("net_version", params));
}

/**
 * @brief Checks if the Ethereum node is currently syncing
 * 
 * Returns false if the node is fully synced, or an object with sync status
 * information (startingBlock, currentBlock, highestBlock) if syncing.
 * 
 * @return Variant containing false or sync status object
 * @throws fc::network::json_rpc::json_rpc_exception if the RPC call fails
 */
fc::variant ethereum_client::get_syncing_status() {
   fc::variants params; // empty
   return execute("eth_syncing", params);
}

/**
 * @brief Creates a default EIP-1559 transaction with estimated gas and current fees
 * 
 * Constructs a complete EIP-1559 transaction by:
 * - Fetching current gas configuration (base fee, priority fee)
 * - Encoding the contract call data according to the ABI
 * - Estimating gas usage and adding a 20% buffer
 * - Setting the nonce from the pending transaction count
 * 
 * @param to The recipient address (contract address for contract calls)
 * @param contract The ABI contract definition for encoding the call data
 * @param params The parameters to pass to the contract function
 * @return A configured eip1559_tx ready for signing and submission
 * @throws fc::network::json_rpc::json_rpc_exception if any RPC call fails
 */
eip1559_tx ethereum_client::create_default_tx(const address_compat_type& to, const abi::contract& contract,
                                              const fc::variants& params) {
   auto gc = get_gas_config();
   auto data = contract_encode_data(contract, params);

   auto estimated_gas = estimate_gas(to, contract, data, gc);
   // add 20% buffer - same as ceil(x * 1.2), but integer division only
   auto gas_limit = (estimated_gas * 6) /5;

   return eip1559_tx{.chain_id = get_chain_id(),
                     .nonce = get_transaction_count(get_signer_address(), "pending"),
                     .max_priority_fee_per_gas = gc.tip,
                     .max_fee_per_gas = gc.max_fee_per_gas,
                     .gas_limit = gas_limit,
                     .to = to_address(to),
                     .value = 0,
                     .data = from_hex(data),
                     .access_list = {}};
}

/**
 * @brief Converts contract parameters to hex-encoded data string
 * 
 * Handles both raw hex string data and structured parameter variants.
 * If params contains fc::variants, encodes them according to the contract ABI.
 * If params contains a string, uses it directly as the data.
 * 
 * @param contract ABI contract definition for encoding
 * @param params Either a raw hex string or structured parameters (fc::variants)
 * @param add_prefix If true, ensures the result has "0x" prefix
 * @return Hex-encoded data string suitable for transaction data field
 */
std::string to_data_from_params(const abi::contract& contract, const data_or_params_t& params, bool add_prefix) {
   std::string data;
   if (std::holds_alternative<fc::variants>(params)) {
      data = contract_encode_data(contract, std::get<fc::variants>(params));
   } else {
      data = std::get<std::string>(params);
   }

   if (add_prefix && !data.starts_with("0x")) {
      data = "0x" + data;
   }
   return data;
}

/**
 * @brief Retrieves the latest block number
 * 
 * Gets the number of the most recent block in the blockchain.
 * 
 * @return The latest block number as a uint256
 * @throws fc::network::json_rpc::json_rpc_exception if the RPC call fails
 */
fc::uint256 ethereum_client::get_block_number() {
   fc::variants params; // empty

   // ilogf("Block number: {}", fc::json::to_pretty_string(resp));
   return to_uint256(execute("eth_blockNumber", params));
}

/**
 * @brief Retrieves block information by block number or tag
 * 
 * Fetches detailed information about a block identified by its number or a tag
 * like "latest", "earliest", or "pending".
 * 
 * @param block_number_or_tag The block number (as string) or tag (e.g., "latest")
 * @param full_transaction_data If true, returns full transaction objects; if false, only transaction hashes
 * @return Block data as a variant_object
 * @throws fc::network::json_rpc::json_rpc_exception if the RPC call fails
 */
fc::variant_object ethereum_client::get_block_by_number(const block_tag_t& block_number_or_tag,
                                                        bool full_transaction_data) {
   auto block_number = to_block_tag(block_number_or_tag);
   fc::variants params{block_number, full_transaction_data};
   return execute("eth_getBlockByNumber", params).get_object();
}

/**
 * @brief Retrieves block information by block hash
 * 
 * Fetches detailed information about a block identified by its hash.
 * 
 * @param block_hash The block hash (hexadecimal string with "0x" prefix)
 * @param full_transaction_data If true, returns full transaction objects; if false, only transaction hashes
 * @return Block data as a variant_object
 * @throws fc::network::json_rpc::json_rpc_exception if the RPC call fails
 */
fc::variant_object ethereum_client::get_block_by_hash(const std::string& block_hash, bool full_transaction_data) {
   fc::variants params{block_hash, full_transaction_data};
   return execute("eth_getBlockByHash", params).get_object();
}

/**
 * @brief Retrieves transaction information by transaction hash
 * 
 * Fetches detailed information about a transaction identified by its hash.
 * 
 * @param tx_hash The transaction hash (hexadecimal string with "0x" prefix)
 * @return Transaction data as a variant
 * @throws fc::network::json_rpc::json_rpc_exception if the RPC call fails
 */
fc::variant ethereum_client::get_transaction_by_hash(const std::string& tx_hash) {
   fc::variants params{tx_hash};
   return execute("eth_getTransactionByHash", params);
}

/**
 * @brief Retrieves the base fee per gas from the latest block
 * 
 * Gets the base fee per gas (in wei) from the latest block. This is part of
 * EIP-1559 and represents the minimum fee required for transaction inclusion.
 * 
 * @return The base fee per gas as a uint256
 * @throws fc::exception if the block doesn't contain baseFeePerGas field
 * @throws fc::network::json_rpc::json_rpc_exception if the RPC call fails
 */
fc::uint256 ethereum_client::get_base_fee_per_gas() {
   auto block = get_block_by_number(block_tag_latest);
   FC_ASSERT_FMT(block.contains("baseFeePerGas"), "Block {} does not contain baseFeePerGas", block_tag_latest);
   return block["baseFeePerGas"].as_uint256();
}

/**
 * @brief Retrieves the suggested maximum priority fee per gas
 * 
 * Gets the suggested tip (priority fee) to pay to miners/validators for
 * transaction inclusion. This is part of EIP-1559 fee mechanism.
 * 
 * @return The suggested priority fee per gas as a uint256
 * @throws fc::network::json_rpc::json_rpc_exception if the RPC call fails
 */
fc::uint256 ethereum_client::get_max_priority_fee_per_gas() {
   fc::variants params; // empty
   auto resp = execute("eth_maxPriorityFeePerGas", params);
   return resp.as_uint256();
}

/**
 * @brief Estimates the gas required for a simple value transfer
 * 
 * Estimates the amount of gas needed to execute a transaction that transfers
 * value from one address to another.
 * 
 * @param to The recipient address
 * @param value The amount of wei to transfer (optional, defaults to 0)
 * @param gas_config Optional gas configuration (unused in this overload)
 * @return The estimated gas amount as a uint256
 * @throws fc::network::json_rpc::json_rpc_exception if the RPC call fails
 */
fc::uint256 ethereum_client::estimate_gas(const address_compat_type& to, const std::optional<fc::uint256>& value, const std::optional<gas_config_t>& gas_config) {
   fc::mutable_variant_object tx;
   tx("from", get_address())("to", to_address(to))("value", value);
   fc::variants params{fc::variant(tx)};
   auto resp = execute("eth_estimateGas", params);
   return resp.as_uint256();
}

/**
 * @brief Retrieves current gas configuration for EIP-1559 transactions
 * 
 * Fetches the current base fee and priority fee, then calculates the
 * recommended max fee per gas as (base_fee * 2) + tip. This provides
 * a buffer for base fee increases between transaction submission and inclusion.
 * 
 * @return gas_config_t structure containing tip, base_fee, and max_fee_per_gas
 * @throws fc::network::json_rpc::json_rpc_exception if any RPC call fails
 */
ethereum_client::gas_config_t ethereum_client::get_gas_config() {
   auto tip = get_max_priority_fee_per_gas();
   auto base_fee = get_base_fee_per_gas();
   auto max_fee_per_gas = (base_fee * 2) + tip;

   return gas_config_t{
      .tip = tip,
      .base_fee = base_fee,
      .max_fee_per_gas = max_fee_per_gas
   };
}

/**
 * @brief Estimates the gas required for a contract function call
 * 
 * Estimates the amount of gas needed to execute a contract function call.
 * Encodes the call data according to the ABI and simulates the transaction
 * to determine gas usage.
 * 
 * @param to The contract address
 * @param contract The ABI contract definition for encoding the call data
 * @param data_or_params Either raw hex string data or structured parameters (fc::variants)
 * @param gas_config_opt Optional gas configuration (if not provided, fetches current config)
 * @return The estimated gas amount as a uint256
 * @throws fc::network::json_rpc::json_rpc_exception if the RPC call fails
 */
fc::uint256 ethereum_client::estimate_gas(const address_compat_type& to, const abi::contract& contract,
                                          const data_or_params_t& data_or_params, const std::optional<gas_config_t>& gas_config_opt) {
   fc::mutable_variant_object tx;

   gas_config_t gc = gas_config_opt.value_or(get_gas_config());

   std::string data = to_data_from_params(contract, data_or_params);;
   if (std::holds_alternative<fc::variants>(data_or_params)) {
      auto& params = std::get<fc::variants>(data_or_params);
      data = "0x" + contract_encode_data(contract, params);
   } else {
      data = "0x" + std::get<std::string>(data_or_params);
   }

   tx("from", to_hex(get_address(), true))
   ("to", to_hex(to_address(to), true))
   ("maxPriorityFeePerGas", to_hex(rlp::encode_uint(gc.max_fee_per_gas), true))
   ("maxFeePerGas", to_hex(rlp::encode_uint(gc.max_fee_per_gas), true))
   ("data", data)
   ("input", data);

   auto resp = execute("eth_estimateGas", fc::variants{tx});
   return resp.as_uint256();
}

/**
 * @brief Retrieves the current gas price
 * 
 * Gets the current gas price in wei. This is the legacy gas price mechanism
 * (pre-EIP-1559). For EIP-1559 transactions, use get_gas_config() instead.
 * 
 * @return The current gas price as a uint256
 * @throws fc::network::json_rpc::json_rpc_exception if the RPC call fails
 */
fc::uint256 ethereum_client::get_gas_price() {
   fc::variants params; // empty
   auto resp = execute("eth_gasPrice", params);
   return resp.as_uint256();
}

/**
 * @brief Sends a transaction using eth_sendTransaction
 * 
 * Submits a transaction to the network. The node will sign the transaction
 * using its own account. This method is typically used with nodes that manage
 * private keys (e.g., local development nodes).
 * 
 * @param raw_tx_data The transaction data (hex-encoded)
 * @return The transaction hash as a string
 * @throws fc::network::json_rpc::json_rpc_exception if the RPC call fails
 */
std::string ethereum_client::send_transaction(const std::string& raw_tx_data) {
   fc::variants params{raw_tx_data};
   auto resp = execute("eth_sendTransaction", params);
   return resp.as_string();
}

/**
 * @brief Sends a signed raw transaction using eth_sendRawTransaction
 * 
 * Submits a pre-signed transaction to the network. The transaction must be
 * RLP-encoded and signed before calling this method.
 * 
 * @param raw_tx_data The signed, RLP-encoded transaction data (hex string with "0x" prefix)
 * @return The transaction hash as a string
 * @throws fc::network::json_rpc::json_rpc_exception if the RPC call fails
 */
std::string ethereum_client::send_raw_transaction(const std::string& raw_tx_data) {
   fc::variants params{raw_tx_data};
   auto resp = execute("eth_sendRawTransaction", params);
   return resp.as_string();
}

/**
 * @brief Retrieves logs matching the specified filter criteria
 * 
 * Fetches event logs from the blockchain based on filter parameters such as
 * address, topics, fromBlock, and toBlock.
 * 
 * @param params Filter parameters as a variant (typically a variant_object)
 * @return Array of log entries as a variant
 * @throws fc::network::json_rpc::json_rpc_exception if the RPC call fails
 */
fc::variant ethereum_client::get_logs(const fc::variant& params) {
   return execute("eth_getLogs", params);
}

/**
 * @brief Retrieves the transaction receipt by transaction hash
 * 
 * Fetches the receipt for a transaction, which includes information about
 * the transaction execution such as status, gas used, logs, and contract address
 * (for contract creation transactions).
 * 
 * @param tx_hash The transaction hash (hexadecimal string with "0x" prefix)
 * @return Transaction receipt data as a variant
 * @throws fc::network::json_rpc::json_rpc_exception if the RPC call fails
 */
fc::variant ethereum_client::get_transaction_receipt(const std::string& tx_hash) {
   fc::variants params{tx_hash};
   return execute("eth_getTransactionReceipt", params);
}

} // namespace fc::network::ethereum
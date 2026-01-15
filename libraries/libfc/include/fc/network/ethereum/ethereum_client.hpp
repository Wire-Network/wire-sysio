
// SPDX-License-Identifier: MIT
#pragma once

#include <fc-lite/threadsafe_map.hpp>

#include <fc/crypto/ethereum/ethereum_types.hpp>
#include <fc/crypto/signature_provider.hpp>
#include <fc/int256.hpp>
#include <fc/network/ethereum/ethereum_abi.hpp>
#include <fc/network/json_rpc/json_rpc_client.hpp>

namespace fc::network::ethereum {
using namespace fc::crypto;
using namespace fc::crypto::ethereum;
using namespace fc::network::json_rpc;

/**
 * @brief Type alias for Ethereum block tag or block number
 * 
 * Can hold either a string (for block numbers) or string_view (for tags like "latest", "pending")
 */
using block_tag_t = std::variant<std::string, std::string_view>;

/**
 * @brief Block tag constant representing pending transactions
 */
constexpr std::string_view block_tag_pending = "pending";

/**
 * @brief Block tag constant representing the latest block
 */
constexpr std::string_view block_tag_latest = "latest";

/**
 * @brief Converts a block_tag_t variant to a string
 * 
 * @param tag Block tag variant (either string or string_view)
 * @return String representation of the block tag
 */
constexpr std::string to_block_tag(block_tag_t tag) {
   if (std::holds_alternative<std::string>(tag)) {
      return std::get<std::string>(tag);
   }
   return std::string(std::get<std::string_view>(tag));
}

/**
 * @brief Type alias for contract call data - either raw hex string or structured parameters
 */
using data_or_params_t  = std::variant<std::string, fc::variants>;

class ethereum_client;

/**
 * @brief Shared pointer type for ethereum_client
 */
using ethereum_client_ptr = std::shared_ptr<ethereum_client>;

/**
 * @brief Function type for Ethereum contract view/call functions
 * 
 * @tparam RT Return type of the contract function
 * @tparam Args Argument types for the contract function
 */
template <typename RT, typename... Args>
using ethereum_contract_call_fn = std::function<RT(const std::string& block_tag, Args&...)>;

/**
 * @brief Function type for Ethereum contract transaction functions
 * 
 * @tparam RT Return type of the contract function
 * @tparam Args Argument types for the contract function
 */
template <typename RT, typename... Args>
using ethereum_contract_tx_fn = std::function<RT(Args&...)>;

/**
 * @class ethereum_contract_client
 * @brief Base class for interacting with Ethereum smart contracts
 * 
 * Provides functionality to create typed contract call and transaction functions
 * based on ABI definitions. Manages contract ABIs and provides methods to execute
 * view functions and transactions.
 */
class ethereum_contract_client : public std::enable_shared_from_this<ethereum_contract_client> {

public:
   /**
    * @brief The Ethereum address of the contract
    */
   const address contract_address;
   
   /**
    * @brief Hexadecimal string representation of the contract address
    */
   const std::string contract_address_hex;
   
   /**
    * @brief Shared pointer to the ethereum_client used for RPC calls
    */
   const ethereum_client_ptr client;
   
   ethereum_contract_client() = delete;
   
   /**
    * @brief Constructs an ethereum_contract_client instance
    * 
    * @param client Shared pointer to the ethereum_client for RPC communication
    * @param contract_address_compat Contract address (can be address or hex string)
    * @param contracts Optional vector of ABI contract definitions to preload
    */
   ethereum_contract_client(const ethereum_client_ptr& client, const address_compat_type& contract_address_compat,
                            const std::vector<abi::contract>& contracts = {})
      : contract_address(ethereum::to_address(contract_address_compat))
      , contract_address_hex(fc::to_hex(contract_address))
      , client(client) {
      auto abi_map = _abi_map.writeable();
      for (const auto& contract : contracts) {
         abi_map[contract.name] = contract;
      }
   };

   virtual ~ethereum_contract_client() = default;

   /**
    * @brief Checks if an ABI definition exists for the given contract name
    * 
    * @param contract_name Name of the contract function/event
    * @return true if ABI exists, false otherwise
    */
   bool has_abi(const std::string& contract_name);

   /**
    * @brief Retrieves the ABI definition for the given contract name
    * 
    * @param contract_name Name of the contract function/event
    * @return Reference to the ABI contract definition
    * @throws std::out_of_range if contract_name not found
    */
   const abi::contract& get_abi(const std::string& contract_name);


protected:


   /**
    * @brief Creates a typed contract call function (view/pure function)
    * 
    * Generates a callable function object that encodes parameters, executes
    * an eth_call RPC, and decodes the result according to the ABI.
    * 
    * @tparam RT Return type of the contract function
    * @tparam Args Argument types for the contract function
    * @param contract ABI contract definition
    * @return Callable function object for executing the contract call
    */
   template <typename RT, typename... Args>
   ethereum_contract_call_fn<RT, Args...> create_call(const abi::contract& contract);

   /**
    * @brief Creates a typed contract transaction function (state-changing function)
    * 
    * Generates a callable function object that encodes parameters, creates a signed
    * transaction, and submits it to the network.
    * 
    * @tparam RT Return type (typically transaction hash)
    * @tparam Args Argument types for the contract function
    * @param contract ABI contract definition
    * @return Callable function object for executing the contract transaction
    */
   template <typename RT, typename... Args>
   ethereum_contract_tx_fn<RT, Args...> create_tx(const abi::contract& contract);

private:
   /**
    * @brief Map of contract names to their ABI definitions
    */
   // std::map<std::string, abi::contract> abi_map{};
   fc::threadsafe_map<std::string, abi::contract> _abi_map{};
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
   /**
    * Holds gas related data
    */
   struct gas_config_t {
      /**
       * Alias to `max_priority_fee_per_gas`
       */
      fc::uint256 tip;
      fc::uint256 base_fee;
      fc::uint256 max_fee_per_gas;
   };

   ethereum_client() = delete;

   /**
    * @brief Constructs an EthereumClient instance.
    * @param sig_provider `signature_provider` shared pointer
    * @param url_source The URL of the Ethereum node (e.g., Infura, local node).
    * @param chain_id optional uint256 encapsulating the chain id
    */
   ethereum_client(const signature_provider_ptr& sig_provider, const std::variant<std::string, fc::url>& url_source,
                   std::optional<fc::uint256> chain_id = std::nullopt);

   /**
    * @brief General method to send RPC requests.
    * @param method The name of the RPC method to call (e.g., "eth_blockNumber").
    * @param params The parameters for the RPC method (as a JSON object).
    * @return The raw JSON response as a string, wrapped in std::optional.
    */
   fc::variant execute(const std::string& method, const fc::variant& params);

   fc::variant execute_contract_view_fn(const address& contract_address, const abi::contract& abi,
                                        const std::string& block_tag, const contract_invoke_data_items& params);

   fc::variant execute_contract_tx_fn(const eip1559_tx& tx, const abi::contract& abi,
                                      const contract_invoke_data_items& params = {}, bool sign = true);

   // Ethereum RPC Methods

   /**
    * @brief Retrieves the latest block number.
    * @return The block number as a string in hexadecimal format.
    */
   fc::uint256 get_block_number();

   /**
    * @brief Retrieves block information by block number.
    * @param block_number_or_tag block # or tag (e.g., "latest", "pending").
    * @param full_transaction_data Flag to determine whether to fetch full transaction data.
    * @return The block data in JSON format.
    */
   fc::variant_object get_block_by_number(const block_tag_t& block_number_or_tag = block_tag_latest,
                                          bool full_transaction_data = false);

   /**
    * @brief Retrieves block information by block hash.
    * @param block_hash The block hash (hexadecimal).
    * @param full_transaction_data Flag to determine whether to fetch full transaction data.
    * @return The block data in JSON format.
    */
   fc::variant_object get_block_by_hash(const std::string& block_hash, bool full_transaction_data = false);

   /**
    * @brief Retrieves transaction information by transaction hash.
    * @param tx_hash The transaction hash.
    * @return The transaction data in JSON format.
    */
   fc::variant get_transaction_by_hash(const std::string& tx_hash);

   fc::uint256 get_base_fee_per_gas();

   fc::uint256 get_max_priority_fee_per_gas();

   /**
    * Gas configuration based on latest block and chain data
    *
    * @return Gas configuration based on latest block and chain data
    */
   gas_config_t get_gas_config();

   /**
    * @brief Estimates the gas required for a transaction.
    * @param to The recipient address.
    * @param value the value to be sent.
    * @param gas_config
    * @return The estimated gas.
    */
   fc::uint256 estimate_gas(const address_compat_type& to,
      const std::optional<fc::uint256>& value = {0}, const std::optional<gas_config_t>& gas_config = std::nullopt);

   fc::uint256 estimate_gas(const address_compat_type& to, const abi::contract& contract, const data_or_params_t& params, const std::optional<gas_config_t>& gas_config = std::nullopt);

   /**
    * @brief Retrieves the current gas price.
    * @return The current gas price in hexadecimal format, if an error occurs, it throws.
    */
   fc::uint256 get_gas_price();

   /**
    * @brief Sends an encoded transaction.
    * @param raw_tx_data The raw transaction data.
    * @return The transaction hash (if successful).
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
    * @return The logs in JSON format.
    */
   fc::variant get_logs(const fc::variant& params);

   /**
    * @brief Retrieves the transaction receipt by transaction hash.
    * @param tx_hash The transaction hash.
    * @return The transaction receipt data in JSON format.
    */
   fc::variant get_transaction_receipt(const std::string& tx_hash);

   // Additional Methods

   /**
    * @brief Retrieves the transaction count (nonce) for an address.
    * @param address The address for which to fetch the transaction count.
    * @param block_tag
    * @return The transaction count (nonce).
    */
   fc::uint256 get_transaction_count(const address_compat_type& address, const std::string& block_tag = "pending");

   /**
    * @brief Retrieves the chain ID of the connected Ethereum network.
    * @return The chain ID.
    */
   fc::uint256 get_chain_id();

   /**
    * @brief Retrieves the version of the connected Ethereum network.
    * @return The network version.
    */
   fc::uint256 get_network_version();

   /**
    * @brief Checks if the Ethereum node is syncing.
    * @return Syncing status in JSON format.
    */
   fc::variant get_syncing_status();

   /**
    * @brief Gets the Ethereum address associated with this client
    * 
    * @return The Ethereum address derived from the signature provider's public key
    */
   ethereum::address get_address() const { return _address; };
   
   /**
    * @brief Gets the signer address (same as get_address)
    * 
    * @return The Ethereum address used for signing transactions
    */
   ethereum::address get_signer_address() const { return _address; };
   
   /**
    * @brief Creates a default EIP-1559 transaction with estimated gas and current fees
    * 
    * @param to Recipient address (contract address for contract calls)
    * @param contract ABI contract definition for encoding the call data
    * @param params Parameters to pass to the contract function
    * @return Configured eip1559_tx ready for signing and submission
    */
   eip1559_tx create_default_tx(const address_compat_type& to, const abi::contract& contract,
                                const fc::variants& params = {});

   /**
    * @brief Gets or creates a typed contract client instance
    * 
    * Maintains a cache of contract clients by address. If a client for the given
    * address doesn't exist, creates a new instance of type C.
    * 
    * @tparam C Contract client type (must derive from ethereum_contract_client)
    * @param address_compat Contract address (can be address or hex string)
    * @param contracts Optional vector of ABI contract definitions to preload
    * @return Shared pointer to the contract client instance
    */
   template <typename C>
   std::shared_ptr<C> get_contract(const address_compat_type& address_compat,
                                   const std::vector<abi::contract>& contracts = {}) {
      std::scoped_lock<std::mutex> lock(_contracts_map_mutex);
      auto addr = ethereum::to_address(address_compat);
      if (!_contracts_map.contains(addr)) {
         _contracts_map[addr] = std::make_shared<C>(shared_from_this(), addr, contracts);
      }
      return std::dynamic_pointer_cast<C>(_contracts_map[addr]);
   }

private:
   /**
    * @brief Signature provider for signing transactions
    */
   const signature_provider_ptr _signature_provider;
   
   /**
    * @brief Ethereum address derived from the signature provider's public key
    */
   const ethereum::address _address;
   
   /**
    * @brief JSON-RPC client for communicating with the Ethereum node
    */
   json_rpc_client _client;
   
   /**
    * @brief Cached chain ID (fetched once and reused)
    */
   std::optional<fc::uint256> _chain_id;
   
   /**
    * @brief Mutex for thread-safe access to _contracts_map
    */
   std::mutex _contracts_map_mutex{};
   
   /**
    * @brief Cache of contract client instances by address
    */
   std::map<address, std::shared_ptr<ethereum_contract_client>> _contracts_map{};
};

/**
 * @brief Converts contract parameters to hex-encoded data string
 * 
 * Handles both raw hex string data and structured parameter variants.
 * If params is a string, uses it directly; if it's variants, encodes them
 * according to the contract ABI.
 * 
 * @param contract ABI contract definition for encoding
 * @param params Either a raw hex string or structured parameters (fc::variants)
 * @param add_prefix If true, ensures the result has "0x" prefix
 * @return Hex-encoded data string suitable for transaction data field
 */
std::string to_data_from_params(const abi::contract& contract, const data_or_params_t& params, bool add_prefix = false);

template <typename RT, typename... Args>
ethereum_contract_call_fn<RT, Args...> ethereum_contract_client::create_call(const abi::contract& contract) {
   auto abi_map = _abi_map.writeable();
   if (!abi_map.contains(contract.name)) {
      abi_map[contract.name] = contract;
   }

   abi::contract& abi = abi_map[contract.name];
   return [this, &abi](const std::string& block_tag, Args&... args) -> RT {
      contract_invoke_data_items params = {args...};
      auto res_var = client->execute_contract_view_fn(contract_address, abi, block_tag, params);

      if constexpr (std::is_same_v<std::decay_t<RT>, fc::variant>) {
         return res_var;
      }

      return res_var.as<RT>();
   };
}


template <typename RT, typename... Args>
ethereum_contract_tx_fn<RT, Args...> ethereum_contract_client::create_tx(const abi::contract& contract) {
   auto abi_map = _abi_map.writeable();
   if (!abi_map.contains(contract.name)) {
      abi_map[contract.name] = contract;
   }

   abi::contract& abi = abi_map[contract.name];
   return [this, &abi](const Args&... args) -> RT {
      contract_invoke_data_items params = {args...};
      auto tx = client->create_default_tx(contract_address, abi, params);
      auto res_var = client->execute_contract_tx_fn(tx, abi, params);

      if constexpr (std::is_same_v<std::decay_t<RT>, fc::variant>) {
         return res_var;
      }

      return res_var.as<RT>();
   };
}

} // namespace fc::network::ethereum
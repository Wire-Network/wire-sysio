#include <print>
#include <sysio/chain/application.hpp>
#include <fc/network/ethereum/ethereum_client.hpp>
#include <fc/io/json.hpp>
#include <fc/time.hpp>
#include <fc/network/ethereum/ethereum_rlp_encoder.hpp>

#include <fc/crypto/ethereum/ethereum_utils.hpp>
#include <fc/log/logger_config.hpp>
#include <sysio/outpost_client_plugin.hpp>
#include <sysio/outpost_ethereum_client_plugin.hpp>
#include <sysio/version/version.hpp>

using namespace appbase;
using namespace sysio;

void configure_logging(const std::filesystem::path& config_path) {
   try {
      try {
         fc::configure_logging(config_path);
      } catch (...) {
         elog("Error reloading logging.json");
         throw;
      }
   } catch (const fc::exception& e) {
      //
      elog("{}", e.to_detail_string());
   } catch (const boost::exception& e) {
      elog("{}", boost::diagnostic_information(e));
   } catch (const std::exception& e) {
      //
      elog("{}", e.what());
   } catch (...) {
      // empty
   }
}

void logging_conf_handler() {
   auto config_path = app().get_logging_conf();
   if (std::filesystem::exists(config_path)) {
      ilog("Received HUP.  Reloading logging configuration from {}.", config_path.string());
   } else {
      ilog("Received HUP.  No log config found at {}, setting to default.", config_path.string());
   }
   configure_logging(config_path);
//todo   fc::log_config::initialize_appenders();
}


void initialize_logging() {
   auto config_path = app().get_logging_conf();
   // if (std::filesystem::exists(config_path))
   //    fc::configure_logging(config_path); // intentionally allowing exceptions to escape
   //fc::log_config::initialize_appenders();

   app().set_sighup_callback(logging_conf_handler);
}

using namespace fc::network::ethereum;

struct ethereum_contract_test_counter_client : fc::network::ethereum::ethereum_contract_client {

   ethereum_contract_tx_fn<fc::variant, fc::uint256> set_number;
   ethereum_contract_call_fn<fc::variant> get_number;
   ethereum_contract_test_counter_client(const ethereum_client_ptr& client,
                                         const address_compat_type& contract_address_compat,
                                         const std::vector<fc::network::ethereum::abi::contract>& contracts)
      : ethereum_contract_client(client, contract_address_compat, contracts),
   set_number(create_tx<fc::variant, fc::uint256>(get_abi("setNumber"))),
   get_number(create_call<fc::variant>(get_abi("number"))) {

   };
};

/**
 * @brief Main function that demonstrates interaction with the Ethereum client.
 *
 * This example loads the configuration, creates an Ethereum client, and performs
 * several Ethereum RPC method calls to get block numbers, estimate gas, and more.
 *
 * Steps:
 * 1. Load configuration from the config file.
 * 2. Create a `NetworkAdapter` and `EthereumClient` instance.
 * 3. Call Ethereum RPC methods to get block number, block data, estimate gas, etc.
 * 4. Print the results to the console.
 *
 * @return int Exit code of the program.
 */
int main(int argc, char* argv[]) {
   using namespace fc::crypto::ethereum;
   try {
      appbase::scoped_app app;

      app->set_version_string(sysio::version::version_client());
      app->set_full_version_string(sysio::version::version_full());

      application::register_plugin<signature_provider_manager_plugin>();
      application::register_plugin<outpost_client_plugin>();
      application::register_plugin<outpost_ethereum_client_plugin>();

      if (!app->initialize<signature_provider_manager_plugin, outpost_client_plugin, outpost_ethereum_client_plugin>(
         argc, argv, initialize_logging)) {
         const auto& opts = app->get_options();
         if (opts.contains("help") || opts.contains("version") || opts.contains("full-version") ||
             opts.contains("print-default-config")) {
            return 0;
         }
         return 1;
      }

      // auto& sig_plug = app->get_plugin<sysio::signature_provider_manager_plugin>();
      auto& eth_plug = app->get_plugin<sysio::outpost_ethereum_client_plugin>();
      auto& eth_abi_files = eth_plug.get_abi_files();
      FC_ASSERT(eth_abi_files.size() == 1, "1 ABI file is required (--ethereum-abi-file <json-array-file>)");
      auto& [eth_abi_file, eth_abi_contracts] = eth_abi_files[0];
      ilog("Using ABI file contracts: {}", eth_abi_file.string());

      auto  client_entry = eth_plug.get_clients()[0];
      auto& client       = client_entry->client;

      auto chain_id = client->get_chain_id();


      ilog("Current chain id: {}", chain_id.str());

      // Example 1: Get the current block number
      /**
       * @brief Get the current block number using the `eth_blockNumber` method.
       *
       * The `getBlockNumber` method sends a request to the Ethereum node to retrieve the current block
       * number. The block number is returned as a string, which is printed to the console.
       */
      auto block_number = client->get_block_number();
      ilog("Current Block Number: {}", block_number.str());

      auto counter_contract = client->get_contract<ethereum_contract_test_counter_client>("0x5FbDB2315678afecb367f032d93F642f64180aa3",eth_abi_contracts);
      auto counter_contract_num_res = counter_contract->get_number("pending");
      auto counter_contract_num = fc::hex_to_number<fc::uint256>(counter_contract_num_res.as_string());
      ilog("Current counter value: {}", counter_contract_num.str());

      fc::uint256 new_num = counter_contract_num + 1;
      ilog("Setting New counter value: {}", new_num.str());
      auto counter_contract_set_num_receipt = counter_contract->set_number(new_num);
      ilog("Counter set number receipt: {}", counter_contract_set_num_receipt.as_string());

      counter_contract_num_res = counter_contract->get_number("pending");
      counter_contract_num = fc::hex_to_number<fc::uint256>(counter_contract_num_res.as_string());
      ilog("New counter value: {}", counter_contract_num.str());

      // Example 2: Get block information by block number
      /**
       * @brief Get block data by block number using the `eth_getBlockByNumber` method.
       *
       * The `getBlockByNumber` method sends a request to the Ethereum node to retrieve the block data
       * corresponding to a given block number (in hexadecimal format). It fetches the block's transactions
       * if `fullTransactionData` is true.
       */
      // std::string block_number_str = "0x5d5f"; ///< Example block number in hexadecimal format.
      // auto        block_data       = client->get_block_by_number(block_number_str, true); // Fetch full transaction data
      // std::println("Block Data: {}", fc::json::to_string(block_data, fc::time_point::maximum()));

      // Example 3: Estimate gas for a transaction
      /**
       * @brief Estimate the gas required for a transaction using the `eth_estimateGas` method.
       *
       * The `estimateGas` method estimates the gas required to send a transaction from one address to
       * another, with a specified value (in hexadecimal format). It returns the estimated gas as a string.
       */
      // std::string from         = "0x7960f1b90b257bff29d5164d16bca4c8030b7f6d"; ///< Example "from" address.
      // std::string to           = "0x7960f1b90b257bff29d5164d16bca4c8030b7f6d"; ///< Example "to" address.
      // std::string value        = "0x9184e72a"; ///< Example value in hexadecimal.
      // auto        gas_estimate = client->estimate_gas(from, to, value);
      // std::println("Estimated Gas: {}", gas_estimate);

      // Example 4: Get Ethereum network version
      /**
       * @brief Get the Ethereum network version using the `net_version` method.
       *
       * The `getNetworkVersion` method retrieves the version of the Ethereum network the node is connected to.
       * It returns the network version as a string.
       */
      auto protocol_version = client->get_network_version();
      ilog("Ethereum Protocol Version: {}", protocol_version.str());
   } catch (const fc::exception& e) {
      elog("{}", e.to_detail_string());
   } catch (const boost::exception& e) {
      elog("{}", boost::diagnostic_information(e));
   } catch (const std::exception& e) {
      elog("{}", e.what());
   } catch (...) {
      elog("unknown exception");
   }
   return 0; ///< Return 0 if the program executed successfully.
}
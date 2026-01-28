#include <print>
#include <sysio/chain/application.hpp>
#include <fc/network/solana/solana_client.hpp>
#include <fc/network/solana/solana_system_programs.hpp>
#include <fc/network/solana/solana_borsh.hpp>
#include <fc/io/json.hpp>
#include <fc/time.hpp>

#include <fc/log/logger_config.hpp>
#include <sysio/outpost_client_plugin.hpp>
#include <sysio/outpost_solana_client_plugin.hpp>
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
      elog("${e}", ("e", e.to_detail_string()));
   } catch (const boost::exception& e) {
      elog("${e}", ("e", boost::diagnostic_information(e)));
   } catch (const std::exception& e) {
      elog("${e}", ("e", e.what()));
   } catch (...) {
      // empty
   }
}

void logging_conf_handler() {
   auto config_path = app().get_logging_conf();
   if (std::filesystem::exists(config_path)) {
      ilog("Received HUP.  Reloading logging configuration from ${p}.", ("p", config_path.string()));
   } else {
      ilog("Received HUP.  No log config found at ${p}, setting to default.", ("p", config_path.string()));
   }
   configure_logging(config_path);
   fc::log_config::initialize_appenders();
}

void initialize_logging() {
   auto config_path = app().get_logging_conf();
   fc::log_config::initialize_appenders();
   app().set_sighup_callback(logging_conf_handler);
}

using namespace fc::network::solana;

/**
 * @brief Client for the raw (non-Anchor) counter program
 *
 * This program uses a PDA to store a u64 counter value. The instruction data
 * is simply an 8-byte little-endian u64 representing the increment amount.
 *
 * Program ID: Cdea2BCiWYBPTQJQq2oWjn5vCkfgENSHNG4GVnWqSvyw
 * PDA Seed: "counter"
 * Accounts:
 *   0: payer (signer, writable)
 *   1: counter_pda (writable)
 *   2: system_program (readonly)
 */
struct solana_program_test_counter_data_client : fc::network::solana::solana_program_data_client {
   static constexpr const char* COUNTER_SEED = "counter";

   // Derive the counter PDA address
   pubkey counter_pda;
   uint8_t counter_bump;

   solana_program_test_counter_data_client(const solana_client_ptr& client, const pubkey& program_id)
      : solana_program_data_client(client, program_id) {
      // Derive the counter PDA
      std::vector<std::vector<uint8_t>> seeds = {
         std::vector<uint8_t>(COUNTER_SEED, COUNTER_SEED + strlen(COUNTER_SEED))
      };
      std::tie(counter_pda, counter_bump) = system::find_program_address(seeds, program_id);
   }

   /**
    * @brief Get the current counter value by reading account data
    */
   uint64_t get_counter_value() {
      auto account_info = client->get_account_info(counter_pda);
      if (!account_info.has_value() || account_info->data.size() < 8) {
         return 0;  // Account doesn't exist or has no data
      }
      // Counter value is stored as little-endian u64
      uint64_t value = 0;
      std::memcpy(&value, account_info->data.data(), sizeof(uint64_t));
      return value;
   }

   /**
    * @brief Increment the counter by the specified amount
    *
    * @param increment_amount The amount to add to the counter
    * @return Transaction signature
    */
   std::string increment(uint64_t increment_amount) {
      // Build instruction data: 8-byte little-endian u64
      std::vector<uint8_t> data(8);
      std::memcpy(data.data(), &increment_amount, sizeof(uint64_t));

      // Build accounts
      std::vector<account_meta> accounts = {
         account_meta::signer(client->get_pubkey(), true),     // payer (signer, writable)
         account_meta::writable(counter_pda, false),            // counter_pda (writable)
         account_meta::readonly(system::program_ids::SYSTEM_PROGRAM, false)  // system_program
      };

      return send_and_confirm_tx(data, accounts);
   }
};

/**
 * @brief Client for the Anchor-based counter program
 *
 * This program uses Anchor IDL with two instructions:
 * - initialize: Creates and initializes the counter PDA
 * - increment: Increments the counter by a specified amount
 *
 * Program ID: 8qR5fPrG9YWSWc68NLArP8m4JhM4e1T3aJ4waV9RKYQb
 * PDA Seed: "counter"
 *
 * This client demonstrates the solana_program_client API using create_tx
 * to create typed callable functions that automatically resolve accounts
 * from IDL definitions - similar to ethereum_contract_client pattern.
 */
struct solana_program_test_counter_anchor_client : fc::network::solana::solana_program_client {
   static constexpr const char* COUNTER_SEED = "counter";

   // Derived counter PDA (for reading account data)
   pubkey counter_pda;
   uint8_t counter_bump;

   /**
    * @brief Initialize the counter account (creates the PDA)
    *
    * Uses create_tx which automatically resolves accounts from IDL:
    * - payer: resolved as signer (client's key)
    * - counter: resolved as PDA from seeds in IDL
    * - system_program: resolved from fixed address in IDL
    */
   solana_program_tx_fn<std::string> initialize;

   /**
    * @brief Increment the counter by the specified amount
    *
    * Uses create_tx which automatically resolves accounts from IDL:
    * - counter: resolved as PDA from seeds in IDL
    */
   solana_program_tx_fn<std::string, uint64_t> increment;

   /**
    * @brief Get the counter account data
    *
    * Uses create_account_data_get which creates a reusable getter function
    * that fetches and decodes account data using the IDL.
    */
   solana_program_account_data_fn<fc::variant> get_counter;

   solana_program_test_counter_anchor_client(const solana_client_ptr& client, const pubkey& program_id,
                                              const std::vector<idl::program>& idls = {})
      : solana_program_client(client, program_id, idls) {
      // Derive the counter PDA for account data reads
      std::vector<std::vector<uint8_t>> seeds = {
         std::vector<uint8_t>(COUNTER_SEED, COUNTER_SEED + strlen(COUNTER_SEED))
      };
      std::tie(counter_pda, counter_bump) = system::find_program_address(seeds, program_id);

      // Create typed transaction functions from IDL (Ethereum contract client pattern)
      initialize = create_tx<std::string>(get_idl("initialize"));
      increment = create_tx<std::string, uint64_t>(get_idl("increment"));

      // Create account data getter (similar to create_tx/create_call pattern)
      get_counter = create_account_data_get<fc::variant>("Counter", counter_pda);
   }

   /**
    * @brief Check if the counter account exists
    */
   bool is_initialized() {
      auto account_info = client->get_account_info(counter_pda);
      return account_info.has_value() && !account_info->data.empty();
   }

   /**
    * @brief Get the current counter value using the getter function
    *
    * Uses get_counter (created via create_account_data_get) to fetch and decode
    * the Counter account according to the IDL definition.
    */
   uint64_t get_counter_value() {
      if (!is_initialized()) {
         return 0;
      }
      return get_counter(commitment_t::finalized)["count"].as_uint64();
   }

   /**
    * @brief Get the full counter account data as variant (for debugging)
    *
    * Returns the complete decoded account including all fields.
    */
   fc::variant get_counter_data() {
      if (!is_initialized()) {
         return fc::variant();
      }
      return get_account_data<fc::variant>("Counter", counter_pda);
   }
};

/**
 * @brief Main function that demonstrates interaction with Solana programs
 *
 * This tool connects to a Solana RPC node and performs various operations:
 * 1. Query basic chain info (slot, block height, balance)
 * 2. Interact with a raw counter program (non-Anchor)
 * 3. Interact with an Anchor-based counter program
 */
int main(int argc, char* argv[]) {
   try {
      appbase::scoped_app app;

      app->set_version_string(sysio::version::version_client());
      app->set_full_version_string(sysio::version::version_full());

      application::register_plugin<signature_provider_manager_plugin>();
      application::register_plugin<outpost_client_plugin>();
      application::register_plugin<outpost_solana_client_plugin>();

      if (!app->initialize<signature_provider_manager_plugin, outpost_client_plugin, outpost_solana_client_plugin>(
         argc, argv, initialize_logging)) {
         const auto& opts = app->get_options();
         if (opts.contains("help") || opts.contains("version") || opts.contains("full-version") ||
             opts.contains("print-default-config")) {
            return 0;
         }
         return 1;
      }

      auto& sol_plug = app->get_plugin<sysio::outpost_solana_client_plugin>();

      // Get the first client
      auto clients = sol_plug.get_clients();
      FC_ASSERT(!clients.empty(), "No Solana clients configured");

      auto client_entry = clients[0];
      auto& client = client_entry->client;

      ilogf("Connected to Solana RPC: {}", client_entry->url);
      ilogf("Signer public key: {}", client->get_pubkey().to_base58());

      // Query basic chain information
      auto slot = client->get_slot();
      ilogf("Current slot: {}", slot);

      auto block_height = client->get_block_height();
      ilogf("Current block height: {}", block_height);

      auto balance = client->get_balance(client->get_pubkey());
      ilogf("Signer balance: {} lamports ({:.9f} SOL)",
            balance, static_cast<double>(balance) / 1e9);

      auto version = client->get_version();
      ilogf("Node version: {}", fc::json::to_string(version, fc::time_point::maximum()));

      // Get IDL files if provided
      auto& idl_files = sol_plug.get_idl_files();
      std::vector<idl::program> all_idls;
      for (auto& [file_path, programs] : idl_files) {
         ilogf("Loaded IDL from: {}", file_path.string());
         for (auto& prog : programs) {
            ilogf("  Program: {} v{}", prog.name, prog.version);
            all_idls.push_back(prog);
         }
      }

      // Test the raw counter program (non-Anchor)
      // const pubkey counter_program_id = pubkey::from_base58("Cdea2BCiWYBPTQJQq2oWjn5vCkfgENSHNG4GVnWqSvyw");
      // ilog("");
      // ilog("=== Testing Raw Counter Program ===");
      // ilogf("Program ID: {}", counter_program_id.to_base58());
      //
      // auto raw_counter = client->get_data_program<solana_program_test_counter_data_client>(counter_program_id);
      // ilogf("Counter PDA: {}", raw_counter->counter_pda.to_base58());
      //
      // uint64_t current_value = raw_counter->get_counter_value();
      // ilogf("Current counter value: {}", current_value);
      //
      // // Increment the counter by 1
      // ilog("Incrementing raw counter by 1...");
      // try {
      //    auto sig = raw_counter->increment(1);
      //    ilogf("Transaction signature: {}", sig);
      //
      //    uint64_t new_value = raw_counter->get_counter_value();
      //    ilogf("New counter value: {}", new_value);
      // } catch (const fc::exception& e) {
      //    wlogf("Failed to increment raw counter: {}", e.to_detail_string());
      // }

      // Test the Anchor counter program
      const pubkey anchor_counter_program_id = pubkey::from_base58("8qR5fPrG9YWSWc68NLArP8m4JhM4e1T3aJ4waV9RKYQb");
      ilog("");
      ilog("=== Testing Anchor Counter Program ===");
      ilogf("Program ID: {}", anchor_counter_program_id.to_base58());

      auto anchor_counter = client->get_program<solana_program_test_counter_anchor_client>(
         anchor_counter_program_id, all_idls);
      ilogf("Counter PDA: {}", anchor_counter->counter_pda.to_base58());

      // Check if initialized
      bool is_init = anchor_counter->is_initialized();
      ilogf("Counter initialized: {}", is_init ? "yes" : "no");

      if (!is_init) {
         ilog("Initializing anchor counter...");
         try {
            auto sig = anchor_counter->initialize();
            ilogf("Initialize transaction signature: {}", sig);
         } catch (const fc::exception& e) {
            wlogf("Failed to initialize anchor counter: {}", e.to_detail_string());
         }
      }

      uint64_t anchor_value = anchor_counter->get_counter_value();
      ilogf("Current anchor counter value: {}", anchor_value);

      // Increment the anchor counter by 5
      ilog("Incrementing anchor counter by 5...");

      auto sig = anchor_counter->increment(5);
      ilogf("Transaction signature: {}", sig);

      uint64_t new_anchor_value = anchor_counter->get_counter_value();
      ilogf("New anchor counter value: {}", new_anchor_value);
      // } catch (const fc::exception& e) {
      //    wlogf("Failed to increment anchor counter: {}", e.to_detail_string());
      // }

      ilog("");
      ilog("Solana client tool completed successfully.");

   } catch (const fc::exception& e) {
      elog("${e}", ("e", e.to_detail_string()));
      return 1;
   } catch (const boost::exception& e) {
      elog("${e}", ("e", boost::diagnostic_information(e)));
      return 1;
   } catch (const std::exception& e) {
      elog("${e}", ("e", e.what()));
      return 1;
   } catch (...) {
      elog("unknown exception");
      return 1;
   }
   return 0;
}

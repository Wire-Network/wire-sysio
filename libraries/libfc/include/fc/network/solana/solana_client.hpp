// SPDX-License-Identifier: MIT
#pragma once

#include <fc-lite/threadsafe_map.hpp>

#include <fc/crypto/signature_provider.hpp>
#include <fc/network/json_rpc/json_rpc_client.hpp>
#include <fc/network/solana/solana_borsh.hpp>
#include <fc/network/solana/solana_idl.hpp>
#include <fc/network/solana/solana_system_programs.hpp>
#include <fc/network/solana/solana_types.hpp>

#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace fc::network::solana {

using namespace fc::crypto;
using namespace fc::network::json_rpc;

class solana_client;
using solana_client_ptr = std::shared_ptr<solana_client>;

/**
 * @brief Type for raw instruction data input
 */
using instruction_data_t = std::vector<uint8_t>;

/**
 * @brief Type for program invocation data items (used with IDL)
 */
using program_invoke_data_items = std::vector<fc::variant>;

/**
 * @brief Function type for Solana program read-only calls
 */
template <typename RT, typename... Args>
using solana_program_call_fn = std::function<RT(commitment_t, Args&...)>;

/**
 * @brief Function type for Solana program transaction calls
 */
template <typename RT, typename... Args>
using solana_program_tx_fn = std::function<RT(Args&...)>;

/**
 * @brief Function type for raw data program read-only calls
 */
using solana_program_data_call_fn =
   std::function<std::vector<uint8_t>(const instruction_data_t&, const std::vector<account_meta>&, commitment_t)>;

/**
 * @brief Function type for raw data program transaction calls
 */
using solana_program_data_tx_fn =
   std::function<std::string(const instruction_data_t&, const std::vector<account_meta>&)>;

/**
 * @class solana_program_data_client
 * @brief Base class for interacting with Solana programs using raw data buffers
 *
 * This client does not use IDL definitions and works directly with raw byte buffers
 * for instruction data. Useful for programs without IDL or for low-level interactions.
 */
class solana_program_data_client : public std::enable_shared_from_this<solana_program_data_client> {
public:
   const pubkey program_id;
   const solana_client_ptr client;

   solana_program_data_client() = delete;

   /**
    * @brief Constructs a solana_program_data_client instance
    *
    * @param client Shared pointer to the solana_client for RPC communication
    * @param program_id Program address
    */
   solana_program_data_client(const solana_client_ptr& client, const pubkey& program_id);

   virtual ~solana_program_data_client() = default;

   /**
    * @brief Execute a program instruction via simulation and return raw data
    *
    * Builds an instruction with the provided data and accounts, simulates the
    * transaction, and returns the raw return data from the program.
    *
    * @param data Raw instruction data
    * @param accounts Account metadata for the instruction
    * @param commitment Commitment level for simulation
    * @return Raw return data from the simulated transaction
    */
   std::vector<uint8_t> call(const instruction_data_t& data, const std::vector<account_meta>& accounts,
                             commitment_t commitment = commitment_t::finalized);

   /**
    * @brief Execute a program instruction as a transaction
    *
    * Builds an instruction with the provided data and accounts, creates a
    * signed transaction, and submits it to the network.
    *
    * @param data Raw instruction data
    * @param accounts Account metadata for the instruction
    * @return Transaction signature
    */
   std::string send_tx(const instruction_data_t& data, const std::vector<account_meta>& accounts);

   /**
    * @brief Execute a program instruction and wait for confirmation
    *
    * @param data Raw instruction data
    * @param accounts Account metadata for the instruction
    * @param commitment Commitment level to wait for
    * @return Transaction signature
    */
   std::string send_and_confirm_tx(const instruction_data_t& data, const std::vector<account_meta>& accounts,
                                   commitment_t commitment = commitment_t::finalized);

protected:
   /**
    * @brief Create a call function for raw data operations
    *
    * Returns a function that simulates a transaction and returns raw return data.
    */
   solana_program_data_call_fn create_call();

   /**
    * @brief Create a transaction function for raw data operations
    *
    * Returns a function that sends a transaction and returns the signature.
    */
   solana_program_data_tx_fn create_tx();

   /**
    * @brief Build an instruction from raw data and accounts
    */
   instruction build_instruction(const instruction_data_t& data, const std::vector<account_meta>& accounts);
};

/**
 * @class solana_program_client
 * @brief Base class for interacting with Solana programs
 *
 * Provides functionality to create typed program call and transaction functions
 * based on IDL definitions.
 */
class solana_program_client : public std::enable_shared_from_this<solana_program_client> {
public:
   const pubkey program_id;
   const solana_client_ptr client;

   solana_program_client() = delete;

   /**
    * @brief Constructs a solana_program_client instance
    *
    * @param client Shared pointer to the solana_client for RPC communication
    * @param program_id Program address
    * @param idls Optional vector of IDL program definitions to preload
    */
   solana_program_client(const solana_client_ptr& client, const pubkey& program_id,
                         const std::vector<idl::program>& idls = {});

   virtual ~solana_program_client() = default;

   /**
    * @brief Checks if an IDL instruction definition exists
    */
   bool has_idl(const std::string& instruction_name);

   /**
    * @brief Retrieves the IDL instruction definition
    */
   const idl::instruction& get_idl(const std::string& instruction_name);

   /**
    * @brief Execute a program call via simulation
    *
    * @param instr IDL instruction definition
    * @param accounts Account metadata for the instruction
    * @param params Parameters for the instruction (as fc::variants)
    * @param commitment Commitment level for simulation
    * @return Simulation result as variant
    */
   fc::variant execute_call(const idl::instruction& instr, const std::vector<account_meta>& accounts,
                            const program_invoke_data_items& params, commitment_t commitment = commitment_t::finalized);

   /**
    * @brief Execute a program transaction
    *
    * @param instr IDL instruction definition
    * @param accounts Account metadata for the instruction
    * @param params Parameters for the instruction (as fc::variants)
    * @return Transaction signature
    */
   std::string execute_tx(const idl::instruction& instr, const std::vector<account_meta>& accounts,
                          const program_invoke_data_items& params = {});

protected:
   /**
    * @brief Creates a typed program call function (read-only/simulation)
    *
    * Generates a callable function object that encodes parameters using Borsh,
    * simulates the transaction, and decodes the result according to the IDL.
    *
    * @tparam RT Return type of the program call
    * @tparam Args Argument types for the program call
    * @param instr IDL instruction definition
    * @return Callable function object for executing the program call
    */
   template <typename RT, typename... Args>
   solana_program_call_fn<RT, Args...> create_call(const idl::instruction& instr);

   /**
    * @brief Creates a typed program transaction function (state-changing)
    *
    * Generates a callable function object that encodes parameters using Borsh,
    * creates a signed transaction, and submits it to the network.
    *
    * @tparam RT Return type (typically std::string for signature)
    * @tparam Args Argument types for the program transaction
    * @param instr IDL instruction definition
    * @return Callable function object for executing the program transaction
    */
   template <typename RT, typename... Args>
   solana_program_tx_fn<RT, Args...> create_tx(const idl::instruction& instr);

   /**
    * @brief Build instruction data from IDL and parameters
    *
    * Encodes the instruction discriminator and parameters using Borsh serialization.
    *
    * @param instr IDL instruction definition
    * @param params Parameters for the instruction
    * @return Encoded instruction data
    */
   std::vector<uint8_t> build_instruction_data(const idl::instruction& instr, const program_invoke_data_items& params);

   /**
    * @brief Build an instruction from IDL definition
    */
   instruction build_instruction(const idl::instruction& instr, const std::vector<account_meta>& accounts,
                                 const program_invoke_data_items& params);

private:
   fc::threadsafe_map<std::string, idl::instruction> _idl_map{};
};

/**
 * @class solana_client
 * @brief A class to interact with a Solana node via JSON-RPC
 *
 * This class provides methods to send RPC requests to a Solana node,
 * manage transactions, and interact with Solana programs.
 */
class solana_client : public std::enable_shared_from_this<solana_client> {
public:
   solana_client() = delete;

   /**
    * @brief Constructs a solana_client instance
    *
    * @param sig_provider Signature provider for signing transactions (ED25519)
    * @param url_source The URL of the Solana RPC node
    */
   solana_client(const signature_provider_ptr& sig_provider, const std::variant<std::string, fc::url>& url_source);

   /**
    * @brief Execute a raw JSON-RPC method call
    */
   fc::variant execute(const std::string& method, const fc::variant& params);

   /**
    * @brief Get the signer's public key
    */
   pubkey get_pubkey() const { return _pubkey; }

   //=========================================================================
   // Account Methods
   //=========================================================================

   /**
    * @brief Get account information
    *
    * @param address Account public key
    * @param commitment Commitment level
    * @return Account info or nullopt if account doesn't exist
    */
   std::optional<account_info> get_account_info(const pubkey_compat_t& address,
                                                 commitment_t commitment = commitment_t::finalized);

   /**
    * @brief Get account balance in lamports
    */
   uint64_t get_balance(const pubkey_compat_t& address, commitment_t commitment = commitment_t::finalized);

   /**
    * @brief Get multiple accounts in a single request
    */
   std::vector<std::optional<account_info>> get_multiple_accounts(const std::vector<pubkey>& addresses,
                                                                   commitment_t commitment = commitment_t::finalized);

   //=========================================================================
   // Block Methods
   //=========================================================================

   /**
    * @brief Get the current block height
    */
   uint64_t get_block_height(commitment_t commitment = commitment_t::finalized);

   /**
    * @brief Get block information by slot
    */
   fc::variant get_block(uint64_t slot, commitment_t commitment = commitment_t::finalized);

   /**
    * @brief Get block commitment information
    */
   fc::variant get_block_commitment(uint64_t slot);

   /**
    * @brief Get list of confirmed blocks between two slots
    */
   std::vector<uint64_t> get_blocks(uint64_t start_slot, std::optional<uint64_t> end_slot = std::nullopt);

   /**
    * @brief Get confirmed blocks starting at a slot with limit
    */
   std::vector<uint64_t> get_blocks_with_limit(uint64_t start_slot, uint64_t limit);

   /**
    * @brief Get estimated production time of a block
    */
   std::optional<int64_t> get_block_time(uint64_t slot);

   //=========================================================================
   // Blockhash Methods
   //=========================================================================

   /**
    * @brief Get the latest blockhash
    */
   blockhash_info get_latest_blockhash(commitment_t commitment = commitment_t::finalized);

   /**
    * @brief Check if a blockhash is still valid
    */
   bool is_blockhash_valid(const std::string& blockhash, commitment_t commitment = commitment_t::finalized);

   //=========================================================================
   // Cluster Methods
   //=========================================================================

   /**
    * @brief Get cluster nodes information
    */
   fc::variant get_cluster_nodes();

   /**
    * @brief Get genesis hash
    */
   std::string get_genesis_hash();

   /**
    * @brief Get node health status
    */
   std::string get_health();

   /**
    * @brief Get highest snapshot slot
    */
   fc::variant get_highest_snapshot_slot();

   /**
    * @brief Get node identity public key
    */
   std::string get_identity();

   /**
    * @brief Get leader schedule
    */
   fc::variant get_leader_schedule(std::optional<uint64_t> slot = std::nullopt);

   /**
    * @brief Get current slot
    */
   uint64_t get_slot(commitment_t commitment = commitment_t::finalized);

   /**
    * @brief Get current slot leader
    */
   std::string get_slot_leader(commitment_t commitment = commitment_t::finalized);

   /**
    * @brief Get slot leaders for a range
    */
   std::vector<std::string> get_slot_leaders(uint64_t start_slot, uint64_t limit);

   /**
    * @brief Get node version information
    */
   fc::variant get_version();

   //=========================================================================
   // Epoch Methods
   //=========================================================================

   /**
    * @brief Get epoch information
    */
   fc::variant get_epoch_info(commitment_t commitment = commitment_t::finalized);

   /**
    * @brief Get epoch schedule
    */
   fc::variant get_epoch_schedule();

   //=========================================================================
   // Fee Methods
   //=========================================================================

   /**
    * @brief Get fee for a message
    *
    * @param message_base64 Base64-encoded message
    * @param commitment Commitment level
    * @return Fee in lamports or nullopt if blockhash expired
    */
   std::optional<uint64_t> get_fee_for_message(const std::string& message_base64,
                                                commitment_t commitment = commitment_t::finalized);

   /**
    * @brief Get recent prioritization fees
    */
   std::vector<fc::variant> get_recent_prioritization_fees(const std::vector<pubkey>& accounts = {});

   //=========================================================================
   // Inflation Methods
   //=========================================================================

   fc::variant get_inflation_governor(commitment_t commitment = commitment_t::finalized);
   fc::variant get_inflation_rate();
   fc::variant get_inflation_reward(const std::vector<pubkey>& addresses, std::optional<uint64_t> epoch = std::nullopt);

   //=========================================================================
   // Supply Methods
   //=========================================================================

   fc::variant get_supply(commitment_t commitment = commitment_t::finalized);
   fc::variant get_largest_accounts(commitment_t commitment = commitment_t::finalized);

   //=========================================================================
   // Stake Methods
   //=========================================================================

   uint64_t get_stake_minimum_delegation(commitment_t commitment = commitment_t::finalized);

   //=========================================================================
   // Token Methods
   //=========================================================================

   fc::variant get_token_account_balance(const pubkey_compat_t& token_account,
                                          commitment_t commitment = commitment_t::finalized);
   fc::variant get_token_accounts_by_delegate(const pubkey_compat_t& delegate, const fc::variant& filter,
                                               commitment_t commitment = commitment_t::finalized);
   fc::variant get_token_accounts_by_owner(const pubkey_compat_t& owner, const fc::variant& filter,
                                            commitment_t commitment = commitment_t::finalized);
   fc::variant get_token_largest_accounts(const pubkey_compat_t& mint,
                                           commitment_t commitment = commitment_t::finalized);
   fc::variant get_token_supply(const pubkey_compat_t& mint, commitment_t commitment = commitment_t::finalized);

   //=========================================================================
   // Transaction Methods
   //=========================================================================

   /**
    * @brief Get transaction information by signature
    */
   fc::variant get_transaction(const std::string& signature, commitment_t commitment = commitment_t::finalized);

   /**
    * @brief Get total transaction count
    */
   uint64_t get_transaction_count(commitment_t commitment = commitment_t::finalized);

   /**
    * @brief Get signatures for an address
    */
   std::vector<fc::variant> get_signatures_for_address(const pubkey_compat_t& address,
                                                        std::optional<std::string> before = std::nullopt,
                                                        std::optional<std::string> until = std::nullopt,
                                                        size_t limit = 1000);

   /**
    * @brief Get signature statuses
    */
   rpc_response<std::vector<std::optional<signature_status>>>
   get_signature_statuses(const std::vector<std::string>& signatures, bool search_transaction_history = false);

   //=========================================================================
   // Transaction Submission
   //=========================================================================

   /**
    * @brief Send a signed transaction
    *
    * @param tx Signed transaction
    * @param skip_preflight Skip preflight checks
    * @param preflight_commitment Commitment level for preflight
    * @return Transaction signature
    */
   std::string send_transaction(const transaction& tx, bool skip_preflight = false,
                                commitment_t preflight_commitment = commitment_t::finalized);

   /**
    * @brief Send a base64-encoded transaction
    */
   std::string send_transaction(const std::string& tx_base64, bool skip_preflight = false,
                                commitment_t preflight_commitment = commitment_t::finalized);

   /**
    * @brief Simulate a transaction
    */
   fc::variant simulate_transaction(const transaction& tx, commitment_t commitment = commitment_t::finalized);

   /**
    * @brief Request an airdrop (devnet/testnet only)
    */
   std::string request_airdrop(const pubkey_compat_t& address, uint64_t lamports,
                               commitment_t commitment = commitment_t::finalized);

   //=========================================================================
   // Program Methods
   //=========================================================================

   /**
    * @brief Get all accounts owned by a program
    */
   fc::variant get_program_accounts(const pubkey_compat_t& program_id, const std::vector<fc::variant>& filters = {},
                                    commitment_t commitment = commitment_t::finalized);

   //=========================================================================
   // Vote Methods
   //=========================================================================

   fc::variant get_vote_accounts(commitment_t commitment = commitment_t::finalized);

   //=========================================================================
   // Rent Methods
   //=========================================================================

   /**
    * @brief Get minimum balance for rent exemption
    */
   uint64_t get_minimum_balance_for_rent_exemption(size_t data_length, commitment_t commitment = commitment_t::finalized);

   //=========================================================================
   // Performance Methods
   //=========================================================================

   fc::variant get_recent_performance_samples(size_t limit = 720);

   //=========================================================================
   // Ledger Methods
   //=========================================================================

   uint64_t get_first_available_block();
   uint64_t minimum_ledger_slot();
   uint64_t get_max_retransmit_slot();
   uint64_t get_max_shred_insert_slot();

   //=========================================================================
   // Block Production
   //=========================================================================

   fc::variant get_block_production(std::optional<uint64_t> first_slot = std::nullopt,
                                    std::optional<uint64_t> last_slot = std::nullopt);

   //=========================================================================
   // Transaction Building Helpers
   //=========================================================================

   /**
    * @brief Create a transaction from instructions
    *
    * Creates an unsigned transaction with the given instructions. The fee payer
    * will be the first required signer.
    *
    * @param instructions Instructions to include
    * @param fee_payer Fee payer account
    * @return Unsigned transaction
    */
   transaction create_transaction(const std::vector<instruction>& instructions, const pubkey& fee_payer);

   /**
    * @brief Sign a transaction
    *
    * Signs the transaction with the client's signature provider.
    *
    * @param tx Transaction to sign (modified in place)
    * @return Signed transaction
    */
   transaction sign_transaction(transaction& tx);

   /**
    * @brief Send and confirm a transaction
    *
    * Sends the transaction and waits for confirmation.
    *
    * @param tx Signed transaction
    * @param commitment Commitment level to wait for
    * @return Transaction signature
    */
   std::string send_and_confirm_transaction(const transaction& tx,
                                             commitment_t commitment = commitment_t::finalized);

   //=========================================================================
   // Program Client Support
   //=========================================================================

   /**
    * @brief Get or create a typed program client
    */
   template <typename C>
   std::shared_ptr<C> get_program(const pubkey& program_id, const std::vector<idl::program>& idls = {}) {
      auto programs = _programs_map.writeable();
      if (!programs.contains(program_id)) {
         programs[program_id] = std::make_shared<C>(shared_from_this(), program_id, idls);
      }
      return std::dynamic_pointer_cast<C>(programs[program_id]);
   }

   /**
    * @brief Get or create a raw data program client
    *
    * Returns a solana_program_data_client for low-level program interactions
    * without IDL support.
    */
   template <typename C = solana_program_data_client>
   std::shared_ptr<C> get_data_program(const pubkey& program_id) {
      auto data_programs = _data_programs_map.writeable();
      if (!data_programs.contains(program_id)) {
         data_programs[program_id] = std::make_shared<C>(shared_from_this(), program_id);
      }
      return std::dynamic_pointer_cast<C>(data_programs[program_id]);
   }

private:
   const signature_provider_ptr _signature_provider;
   const pubkey _pubkey;
   json_rpc_client _client;

   fc::threadsafe_map<pubkey, std::shared_ptr<solana_program_client>> _programs_map{};
   fc::threadsafe_map<pubkey, std::shared_ptr<solana_program_data_client>> _data_programs_map{};

   /**
    * @brief Build config object for RPC calls
    */
   fc::variant build_config(commitment_t commitment, const std::optional<fc::variant_object>& extra = std::nullopt);
};

//=============================================================================
// Template implementations for solana_program_client
//=============================================================================

template <typename RT, typename... Args>
solana_program_call_fn<RT, Args...> solana_program_client::create_call(const idl::instruction& instr) {
   auto idl_map = _idl_map.writeable();
   if (!idl_map.contains(instr.name)) {
      idl_map[instr.name] = instr;
   }

   idl::instruction& idl = idl_map[instr.name];
   return [this, &idl](commitment_t commitment, Args&... args) -> RT {
      program_invoke_data_items params = {fc::variant(args)...};

      auto res_var = execute_call(idl, {}, params, commitment);

      if constexpr (std::is_same_v<std::decay_t<RT>, fc::variant>) {
         return res_var;
      } else if constexpr (std::is_same_v<std::decay_t<RT>, std::vector<uint8_t>>) {
         // Return raw data buffer
         if (res_var.is_blob()) {
            auto blob = res_var.as_blob();
            return std::vector<uint8_t>(blob.data.begin(), blob.data.end());
         }
         return std::vector<uint8_t>{};
      } else {
         return res_var.as<RT>();
      }
   };
}

template <typename RT, typename... Args>
solana_program_tx_fn<RT, Args...> solana_program_client::create_tx(const idl::instruction& instr) {
   auto idl_map = _idl_map.writeable();
   if (!idl_map.contains(instr.name)) {
      idl_map[instr.name] = instr;
   }

   idl::instruction& idl = idl_map[instr.name];
   return [this, &idl](Args&... args) -> RT {
      program_invoke_data_items params = {fc::variant(args)...};

      auto res = execute_tx(idl, {}, params);

      if constexpr (std::is_same_v<std::decay_t<RT>, std::string>) {
         return res;
      } else if constexpr (std::is_same_v<std::decay_t<RT>, fc::variant>) {
         return fc::variant(res);
      } else {
         return RT{res};
      }
   };
}

}  // namespace fc::network::solana

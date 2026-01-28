// SPDX-License-Identifier: MIT
#pragma once

#include <fc-lite/threadsafe_map.hpp>

#include <fc/crypto/signature_provider.hpp>
#include <fc/network/json_rpc/json_rpc_client.hpp>
#include <fc/network/solana/solana_borsh.hpp>
#include <fc/network/solana/solana_idl.hpp>
#include <fc/network/solana/solana_system_programs.hpp>
#include <fc/network/solana/solana_types.hpp>

#include <map>
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
using solana_program_call_fn = std::function<RT(commitment_t, Args...)>;

/**
 * @brief Function type for Solana program transaction calls
 */
template <typename RT, typename... Args>
using solana_program_tx_fn = std::function<RT(Args...)>;

/**
 * @brief Function type for account data getter
 *
 * Takes optional commitment and returns decoded account data.
 */
template <typename RT>
using solana_program_account_data_fn = std::function<RT(commitment_t)>;

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
 * @brief Map of account name to pubkey for account overrides
 */
using account_overrides_t = std::map<std::string, pubkey>;

/**
 * @class solana_program_client
 * @brief Base class for interacting with Solana programs using IDL
 *
 * Provides functionality to create typed program call and transaction functions
 * based on IDL definitions. Similar to ethereum_contract_client pattern.
 *
 * The client automatically resolves accounts based on IDL definitions:
 * - Signer accounts: uses the client's payer key
 * - PDA accounts: derives address from seeds defined in IDL
 * - Fixed address accounts: uses the address specified in IDL (e.g., system_program)
 * - Other accounts: must be provided via account_overrides
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
    * @brief Gets the full IDL program definition (for type lookups)
    */
   const idl::program* get_program() const { return _program.get(); }

   /**
    * @brief Execute a program call via simulation with manual account specification
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
    * @brief Execute a program transaction with manual account specification
    *
    * @param instr IDL instruction definition
    * @param accounts Account metadata for the instruction
    * @param params Parameters for the instruction (as fc::variants)
    * @return Transaction signature
    */
   std::string execute_tx(const idl::instruction& instr, const std::vector<account_meta>& accounts,
                          const program_invoke_data_items& params = {});

   /**
    * @brief Resolve accounts for an instruction based on IDL
    *
    * Resolves account pubkeys and builds account_meta list from IDL definition.
    *
    * @param instr IDL instruction definition
    * @param params Instruction parameters (used for arg-based PDA seeds)
    * @param account_overrides Map of account name -> pubkey for explicit accounts
    * @return Vector of resolved account_meta
    */
   std::vector<account_meta> resolve_accounts(const idl::instruction& instr,
                                              const program_invoke_data_items& params = {},
                                              const account_overrides_t& account_overrides = {});

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
    * @brief Creates a typed account data getter function
    *
    * Generates a callable function object that fetches account data from the network
    * and decodes it using the IDL type definition. This is useful for creating
    * reusable getter functions for PDA accounts.
    *
    * @tparam RT Return type for the decoded account data (e.g., fc::variant or user struct)
    * @param idl_type_name Name of the type in the IDL (e.g., "Counter")
    * @param pda The PDA address to read from
    * @return Callable function object that fetches and decodes the account data
    */
   template <typename RT>
   solana_program_account_data_fn<RT> create_account_data_get(const std::string& idl_type_name, const pubkey& pda);

   /**
    * @brief Encode a value to Borsh format using IDL type information
    *
    * Encodes a single value to the encoder according to the IDL type.
    * Supports all IDL types including primitives, options, vecs, arrays,
    * and user-defined structs/enums.
    *
    * @param encoder Borsh encoder to write to
    * @param value Value to encode (fc::variant)
    * @param type IDL type definition
    */
   void encode_type(borsh::encoder& encoder, const fc::variant& value, const idl::idl_type& type);

   /**
    * @brief Encode a list of fields to Borsh format
    *
    * Encodes struct fields from an fc::variant_object according to IDL field definitions.
    *
    * @param encoder Borsh encoder to write to
    * @param value Value containing the fields (fc::variant_object)
    * @param fields List of field definitions
    */
   void encode_fields(borsh::encoder& encoder, const fc::variant& value, const std::vector<idl::field>& fields);

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

   /**
    * @brief Derive a PDA from seeds defined in the IDL
    *
    * @param pda_seeds Seeds from IDL instruction_account
    * @param params Instruction parameters for arg-type seeds
    * @return Pair of (pubkey, bump)
    */
   std::pair<pubkey, uint8_t> derive_pda(const std::vector<idl::pda_seed>& pda_seeds,
                                          const program_invoke_data_items& params = {});

   /**
    * @brief Decode a value from Borsh-encoded data using IDL type information
    *
    * Decodes a single value in-place from the decoder according to the IDL type.
    *
    * @param decoder Borsh decoder (position advances as data is read)
    * @param type IDL type definition
    * @return Decoded value as fc::variant
    */
   fc::variant decode_type(borsh::decoder& decoder, const idl::idl_type& type);

   /**
    * @brief Decode a list of fields from Borsh-encoded data
    *
    * @param decoder Borsh decoder (position advances as data is read)
    * @param fields List of field definitions
    * @return Decoded fields as variant object
    */
   fc::variant decode_fields(borsh::decoder& decoder, const std::vector<idl::field>& fields);

   /**
    * @brief Decode account data using IDL account definition
    *
    * Decodes Borsh-encoded account data according to the IDL account type.
    * Verifies the 8-byte Anchor discriminator and decodes all fields.
    *
    * @param data Raw account data bytes (including discriminator)
    * @param account_name Name of the account type in IDL
    * @return Decoded account data as fc::variant object
    */
   fc::variant decode_account_data(const std::vector<uint8_t>& data, const std::string& account_name);

   /**
    * @brief Extract return data from simulation result
    *
    * @param sim_result Raw simulation result from RPC
    * @return Decoded return data bytes, or empty vector if none
    */
   std::vector<uint8_t> extract_return_data(const fc::variant& sim_result);

public:
   /**
    * @brief Fetch and decode account data using IDL
    *
    * Fetches account info from the network and decodes the data according to
    * the IDL account type definition. Similar to Ethereum's contract view pattern.
    *
    * @tparam RT Return type for the decoded data
    * @param account_name Name of the account type in IDL
    * @param address Account address to fetch
    * @param commitment Commitment level for the query
    * @return Decoded account data as RT
    */
   template <typename RT>
   RT get_account_data(const std::string& account_name, const pubkey& address,
                       commitment_t commitment = commitment_t::finalized);

private:
   fc::threadsafe_map<std::string, idl::instruction> _idl_map{};
   std::shared_ptr<idl::program> _program;
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
   return [this, &idl](commitment_t commitment, Args... args) -> RT {
      program_invoke_data_items params = {fc::variant(args)...};

      // Execute call - this uses IDL to decode return data
      auto res_var = execute_call(idl, resolve_accounts(idl, params), params, commitment);

      // Return the result, converting as needed (matches Ethereum pattern)
      if constexpr (std::is_same_v<std::decay_t<RT>, fc::variant>) {
         return res_var;
      }

      return res_var.as<RT>();
   };
}

template <typename RT, typename... Args>
solana_program_tx_fn<RT, Args...> solana_program_client::create_tx(const idl::instruction& instr) {
   auto idl_map = _idl_map.writeable();
   if (!idl_map.contains(instr.name)) {
      idl_map[instr.name] = instr;
   }

   idl::instruction& idl = idl_map[instr.name];
   return [this, &idl](Args... args) -> RT {
      program_invoke_data_items params = {fc::variant(args)...};

      // Execute transaction - returns signature as string
      std::string signature = execute_tx(idl, resolve_accounts(idl, params), params);

      // Return the result, converting as needed (matches Ethereum pattern)
      if constexpr (std::is_same_v<std::decay_t<RT>, fc::variant>) {
         return fc::variant(signature);
      } else if constexpr (std::is_same_v<std::decay_t<RT>, std::string>) {
         return signature;
      } else {
         // For other types, convert via variant
         fc::variant res_var(signature);
         return res_var.as<RT>();
      }
   };
}

template <typename RT>
RT solana_program_client::get_account_data(const std::string& account_name, const pubkey& address,
                                            commitment_t commitment) {
   // Fetch account info from the network
   auto account_info = client->get_account_info(address, commitment);
   FC_ASSERT(account_info.has_value(), "Account not found: {}", address.to_base58());
   FC_ASSERT(!account_info->data.empty(), "Account has no data: {}", address.to_base58());

   // Decode using IDL
   auto res_var = decode_account_data(account_info->data, account_name);

   // Return the result, converting as needed (matches Ethereum Contract Client pattern)
   if constexpr (std::is_same_v<std::decay_t<RT>, fc::variant>) {
      return res_var;
   }

   return res_var.as<RT>();
}

template <typename RT>
solana_program_account_data_fn<RT> solana_program_client::create_account_data_get(const std::string& idl_type_name,
                                                                                    const pubkey& pda) {
   // Capture type name and address by value for the lambda
   std::string type_name = idl_type_name;
   pubkey address = pda;

   return [this, type_name, address](commitment_t commitment = commitment_t::finalized) -> RT {
      // Fetch account info from the network
      auto account_info = client->get_account_info(address, commitment);
      FC_ASSERT(account_info.has_value(), "Account not found: {}", address.to_base58());
      FC_ASSERT(!account_info->data.empty(), "Account has no data: {}", address.to_base58());

      // Decode using IDL
      auto res_var = decode_account_data(account_info->data, type_name);

      // Return the result, converting as needed (matches create_call/create_tx pattern)
      if constexpr (std::is_same_v<std::decay_t<RT>, fc::variant>) {
         return res_var;
      }

      return res_var.as<RT>();
   };
}

}  // namespace fc::network::solana

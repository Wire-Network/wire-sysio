// SPDX-License-Identifier: MIT
#pragma once

#include <fc/network/solana/solana_types.hpp>
#include <fc/network/solana/solana_borsh.hpp>

#include <vector>
#include <string>

namespace fc::network::solana::system {

//=============================================================================
// Well-known program IDs
//=============================================================================

namespace program_ids {

// System Program - native program for account creation and SOL transfers
inline const pubkey SYSTEM_PROGRAM = pubkey::from_base58("11111111111111111111111111111111");

// SPL Token Program - standard token implementation
inline const pubkey TOKEN_PROGRAM = pubkey::from_base58("TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA");

// SPL Token 2022 Program - extended token implementation
inline const pubkey TOKEN_2022_PROGRAM = pubkey::from_base58("TokenzQdBNbLqP5VEhdkAS6EPFLC1PHnBqCXEpPxuEb");

// Associated Token Account Program
inline const pubkey ASSOCIATED_TOKEN_PROGRAM = pubkey::from_base58("ATokenGPvbdGVxr1b2hvZbsiqW5xWH25efTNsLJA8knL");

// Memo Program
inline const pubkey MEMO_PROGRAM = pubkey::from_base58("MemoSq4gqABAXKb96qnH8TysNcWxMyWCqXgDLGmfcHr");

// Memo Program (old version)
inline const pubkey MEMO_PROGRAM_V1 = pubkey::from_base58("Memo1UhkJRfHyvLMcVucJwxXeuD728EqVDDwQDxFMNo");

// Compute Budget Program
inline const pubkey COMPUTE_BUDGET_PROGRAM = pubkey::from_base58("ComputeBudget111111111111111111111111111111");

// BPF Loader
inline const pubkey BPF_LOADER = pubkey::from_base58("BPFLoader2111111111111111111111111111111111");

// BPF Upgradeable Loader
inline const pubkey BPF_UPGRADEABLE_LOADER = pubkey::from_base58("BPFLoaderUpgradeab1e11111111111111111111111");

// Stake Program
inline const pubkey STAKE_PROGRAM = pubkey::from_base58("Stake11111111111111111111111111111111111111");

// Vote Program
inline const pubkey VOTE_PROGRAM = pubkey::from_base58("Vote111111111111111111111111111111111111111");

// Config Program
inline const pubkey CONFIG_PROGRAM = pubkey::from_base58("Config1111111111111111111111111111111111111");

// Address Lookup Table Program
inline const pubkey ADDRESS_LOOKUP_TABLE_PROGRAM = pubkey::from_base58("AddressLookupTab1e1111111111111111111111111");

// Ed25519 Signature Verification Program
inline const pubkey ED25519_PROGRAM = pubkey::from_base58("Ed25519SigVerify111111111111111111111111111");

// Secp256k1 Signature Recovery Program
inline const pubkey SECP256K1_PROGRAM = pubkey::from_base58("KeccakSecp256k11111111111111111111111111111");

}  // namespace program_ids

//=============================================================================
// SYSVAR addresses
//=============================================================================

namespace sysvars {

// Clock sysvar - current slot, epoch, and unix timestamp
inline const pubkey CLOCK = pubkey::from_base58("SysvarC1ock11111111111111111111111111111111");

// Rent sysvar - rent parameters
inline const pubkey RENT = pubkey::from_base58("SysvarRent111111111111111111111111111111111");

// Epoch Schedule sysvar
inline const pubkey EPOCH_SCHEDULE = pubkey::from_base58("SysvarEpochScheworker1111111111111111111111");

// Instructions sysvar - access to serialized instructions
inline const pubkey INSTRUCTIONS = pubkey::from_base58("Sysvar1nstructions1111111111111111111111111");

// Recent Blockhashes sysvar (deprecated)
inline const pubkey RECENT_BLOCKHASHES = pubkey::from_base58("SysvarRecentB1ockHashes11111111111111111111");

// Stake History sysvar
inline const pubkey STAKE_HISTORY = pubkey::from_base58("SysvarStakeHistory1111111111111111111111111");

// Slot Hashes sysvar
inline const pubkey SLOT_HASHES = pubkey::from_base58("SysvarS1otHashes111111111111111111111111111");

// Slot History sysvar
inline const pubkey SLOT_HISTORY = pubkey::from_base58("SysvarS1otHistory11111111111111111111111111");

// Fees sysvar (deprecated)
inline const pubkey FEES = pubkey::from_base58("SysvarFees111111111111111111111111111111111");

// Last Restart Slot sysvar
inline const pubkey LAST_RESTART_SLOT = pubkey::from_base58("SysvarLastRestartS1ot1111111111111111111111");

}  // namespace sysvars

//=============================================================================
// System Program Instructions
//=============================================================================

namespace instructions {

/**
 * @brief Create a new account
 *
 * @param from Funding account (signer)
 * @param new_account New account (signer)
 * @param lamports Number of lamports to transfer to the new account
 * @param space Number of bytes to allocate for the account data
 * @param owner Program that will own the new account
 */
instruction create_account(const pubkey& from, const pubkey& new_account,
                           uint64_t lamports, uint64_t space, const pubkey& owner);

/**
 * @brief Transfer SOL between accounts
 *
 * @param from Source account (signer)
 * @param to Destination account
 * @param lamports Number of lamports to transfer
 */
instruction transfer(const pubkey& from, const pubkey& to, uint64_t lamports);

/**
 * @brief Assign an account to a new owner program
 *
 * @param account Account to reassign (signer)
 * @param owner New owner program
 */
instruction assign(const pubkey& account, const pubkey& owner);

/**
 * @brief Allocate space for an account
 *
 * @param account Account to allocate space for (signer)
 * @param space Number of bytes to allocate
 */
instruction allocate(const pubkey& account, uint64_t space);

/**
 * @brief Create account with seed
 *
 * @param from Funding account (signer)
 * @param to New account address (derived from base + seed)
 * @param base Base account used to derive the address (signer)
 * @param seed Seed used to derive the address
 * @param lamports Number of lamports to transfer
 * @param space Number of bytes to allocate
 * @param owner Program that will own the new account
 */
instruction create_account_with_seed(const pubkey& from, const pubkey& to,
                                     const pubkey& base, const std::string& seed,
                                     uint64_t lamports, uint64_t space,
                                     const pubkey& owner);

/**
 * @brief Transfer with seed
 *
 * @param from Source account
 * @param from_base Base address used to derive the source (signer)
 * @param seed Seed used to derive the source
 * @param from_owner Owner program of the source
 * @param to Destination account
 * @param lamports Number of lamports to transfer
 */
instruction transfer_with_seed(const pubkey& from, const pubkey& from_base,
                               const std::string& seed, const pubkey& from_owner,
                               const pubkey& to, uint64_t lamports);

/**
 * @brief Advance nonce account
 *
 * @param nonce_account Nonce account
 * @param authority Nonce authority (signer)
 */
instruction advance_nonce(const pubkey& nonce_account, const pubkey& authority);

/**
 * @brief Withdraw from nonce account
 *
 * @param nonce_account Nonce account
 * @param authority Nonce authority (signer)
 * @param to Destination account
 * @param lamports Number of lamports to withdraw
 */
instruction withdraw_nonce(const pubkey& nonce_account, const pubkey& authority,
                           const pubkey& to, uint64_t lamports);

/**
 * @brief Initialize nonce account
 *
 * @param nonce_account Nonce account
 * @param authority Nonce authority
 */
instruction initialize_nonce(const pubkey& nonce_account, const pubkey& authority);

/**
 * @brief Authorize nonce account
 *
 * @param nonce_account Nonce account
 * @param authority Current nonce authority (signer)
 * @param new_authority New nonce authority
 */
instruction authorize_nonce(const pubkey& nonce_account, const pubkey& authority,
                            const pubkey& new_authority);

/**
 * @brief Upgrade nonce account
 *
 * @param nonce_account Nonce account
 */
instruction upgrade_nonce_account(const pubkey& nonce_account);

}  // namespace instructions

//=============================================================================
// Compute Budget Instructions
//=============================================================================

namespace compute_budget {

/**
 * @brief Set compute unit limit
 *
 * @param units Maximum compute units for the transaction
 */
instruction set_compute_unit_limit(uint32_t units);

/**
 * @brief Set compute unit price (priority fee)
 *
 * @param micro_lamports Price per compute unit in micro-lamports
 */
instruction set_compute_unit_price(uint64_t micro_lamports);

/**
 * @brief Request heap frame size
 *
 * @param bytes Requested heap frame size in bytes
 */
instruction request_heap_frame(uint32_t bytes);

/**
 * @brief Set loaded accounts data size limit
 *
 * @param bytes Maximum total size of accounts data that can be loaded
 */
instruction set_loaded_accounts_data_size_limit(uint32_t bytes);

}  // namespace compute_budget

//=============================================================================
// PDA (Program Derived Address) Utilities
//=============================================================================

/**
 * @brief Find a valid program derived address and bump seed
 *
 * Searches for a valid PDA by iterating through bump seeds from 255 to 0.
 *
 * @param seeds The seeds to derive the address from
 * @param program_id The program ID that will own the PDA
 * @return Pair of (PDA address, bump seed)
 */
std::pair<pubkey, uint8_t> find_program_address(const std::vector<std::vector<uint8_t>>& seeds,
                                                 const pubkey& program_id);

/**
 * @brief Create a program derived address
 *
 * Creates a PDA from the given seeds and program ID. The seeds must include
 * a valid bump seed that results in a valid PDA (off the ed25519 curve).
 *
 * @param seeds The seeds including bump seed
 * @param program_id The program ID
 * @return The PDA address
 * @throws If the derived address is on the ed25519 curve
 */
pubkey create_program_address(const std::vector<std::vector<uint8_t>>& seeds,
                               const pubkey& program_id);

/**
 * @brief Get associated token account address
 *
 * Derives the associated token account address for a given owner and mint.
 *
 * @param owner The wallet address that owns the token account
 * @param mint The token mint address
 * @param token_program The token program (defaults to SPL Token)
 * @return The associated token account address
 */
pubkey get_associated_token_address(const pubkey& owner, const pubkey& mint,
                                     const pubkey& token_program = program_ids::TOKEN_PROGRAM);

/**
 * @brief Check if an address is on the ed25519 curve
 *
 * @param address The address to check
 * @return True if the address is on the curve, false otherwise
 */
bool is_on_curve(const pubkey& address);

//=============================================================================
// Address derivation helpers
//=============================================================================

/**
 * @brief Create address with seed
 *
 * Derives a new address from a base address, seed string, and program ID.
 *
 * @param base Base address
 * @param seed Seed string
 * @param program_id Program ID
 * @return Derived address
 */
pubkey create_with_seed(const pubkey& base, const std::string& seed, const pubkey& program_id);

}  // namespace fc::network::solana::system
